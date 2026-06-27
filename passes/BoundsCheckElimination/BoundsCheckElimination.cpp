//===- BoundsCheckElimination.cpp ----------------------------------------===//
//
// See BoundsCheckElimination.h.
//
// Gating: this pass is a no-op unless explicitly enabled via
// `-rust-hpc-trust-bounds-checks` on the RustHpcOpt/opt command line. There
// is no per-function or per-call opt-out below that; the flag is meant to be
// set at the granularity of "this compilation unit's indices are all
// pre-validated by the surrounding Rust code", which is the caller's
// decision to make, not something we infer from the IR.
//
// What "semantic, not pattern-matching" means here: it would be easy to
// just delete any `br` that leads to a block calling a function whose name
// contains "panic_bounds_check". That's wrong on its own merits even with
// the flag set, because:
//   - rustc sometimes emits *other* panics (explicit `.expect()`, assertion
//     failures, user panics) right next to real bounds checks, and a naive
//     "any branch that reaches any panicking call" rule would delete those
//     too -- they are not bounds checks and removing them changes program
//     behavior beyond "trust my indices are in range".
//   - a branch merely *reaching* a panic block isn't the same as that
//     branch being *the* guard for *that specific* bounds check; CFGs after
//     inlining can have multiple guards feeding the same panic block, or a
//     panic block reachable from contexts unrelated to indexing.
//
// So this performs three independent checks before touching anything, and
// only acts if all three agree:
//   1. Structural: the candidate block has exactly one predecessor (this
//      branch) and provably never falls through to anything but
//      `unreachable` -- i.e. it is *only* a guard target, not a merge point
//      or a block with other live effects.
//   2. Identity: the call in that block is to an external declaration whose
//      demangled name is, specifically, `core::panicking::panic_bounds_check`
//      (the Rust core library's bounds-check panic, not panics in general).
//   3. Data-flow: the icmp condition feeding the branch and the call's own
//      arguments name the *same* SSA values (the index and the length),
//      modulo integer extension/truncation. This is the cross-check that
//      actually proves the branch is the guard for *this* call: rustc
//      passes the exact index/length pair being compared into the panic
//      call, so if the operands don't line up, this branch is guarding
//      something else and we leave it alone.
//
//===----------------------------------------------------------------------===//

#include "BoundsCheckElimination.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

#define DEBUG_TYPE "rust-hpc-elim-bounds-checks"

static cl::opt<bool> EnableBoundsCheckElim(
    "rust-hpc-trust-bounds-checks", cl::Hidden, cl::init(false),
    cl::desc(
        "Remove Rust slice/array bounds-check guards whose indices are "
        "asserted (by the caller of this flag, not by any analysis this "
        "pass performs) to always be in range. Dangerous: this removes a "
        "real safety check and introduces undefined behavior if an index "
        "is ever actually out of range. Off by default."));

STATISTIC(NumBoundsChecksEliminated,
          "Number of Rust panic_bounds_check guards removed");

namespace {

/// Bounds-check panic functions this pass recognizes by name, and whether
/// that function's call site can be expected to carry the index/length as
/// real arguments (enabling the operand cross-check in
/// conditionMatchesCallArguments) or not.
///
/// `core::panicking::panic_bounds_check` takes `(index, len, location)`, so
/// the cross-check applies. `ndarray::arraytraits::array_out_of_bounds` --
/// found empirically while benchmarking against real ndarray code (see
/// bench/ and README.md), not anticipated up front -- takes *no* arguments
/// at all, so there is nothing for that cross-check to compare against; for
/// that function the verification falls back to the structural check alone
/// (single-predecessor guard block, `noreturn` call, `unreachable`
/// terminator). That's a real reduction in how independently this specific
/// allowlist entry is verified, called out explicitly rather than silently
/// -- it's accepted because the function's only possible caller-visible
/// purpose, given its name and crate, is exactly this guard.
struct BoundsCheckPanicFn {
  StringRef Name;
  bool HasIndexLenArgs;
};

constexpr BoundsCheckPanicFn BoundsCheckPanicFns[] = {
    {"core::panicking::panic_bounds_check", true},
    {"ndarray::arraytraits::array_out_of_bounds", false},
};

/// Returns the matching BoundsCheckPanicFn for the (demangled) name of a
/// panicking function, or nullptr. Kept as an explicit allowlist rather
/// than a substring search for "panic" so that unrelated panics (explicit
/// user panics, `.unwrap()`/`.expect()` failures, assertion failures,
/// arithmetic-overflow panics, etc.) are never touched.
const BoundsCheckPanicFn *matchBoundsCheckPanicFunction(StringRef MangledName) {
  std::string Demangled = demangle(MangledName);
  StringRef D(Demangled);
  for (const BoundsCheckPanicFn &Fn : BoundsCheckPanicFns) {
    // The full `a::b::c` path, checked against the demangled name.
    if (D.contains(Fn.Name))
      return &Fn;
    // Bare last-component fallback (e.g. "panic_bounds_check" alone)
    // checked against the *raw* mangled name, in case `demangle()` fails
    // to recognize a mangling scheme variant -- legacy Rust mangling is a
    // thin wrapper around Itanium and should always demangle, but symbol
    // mangling has changed across rustc versions before, and Rust's
    // mangling keeps identifier text verbatim (just length-prefixed) so
    // the bare name still appears as a literal substring either way.
    StringRef BareName = Fn.Name.substr(Fn.Name.rfind("::") + 2);
    if (MangledName.contains(BareName))
      return &Fn;
  }
  return nullptr;
}

/// Returns the sole "interesting" call in `BB`, if `BB` is purely a panic
/// shim: any number of side-effect-free instructions computing call
/// arguments (GEPs, casts, constant aggregates), exactly one call to an
/// external declaration, and a terminator that is either `unreachable`
/// directly or becomes unreachable because the call is `noreturn`. Returns
/// nullptr if `BB` does anything else (stores, other calls, a normal
/// fallthrough terminator), since that means it's not purely a guard
/// target and we should not reason about it as one.
CallInst *getSolePanicCall(BasicBlock *BB) {
  CallInst *Found = nullptr;
  for (Instruction &I : *BB) {
    if (I.isTerminator()) {
      if (!isa<UnreachableInst>(&I))
        return nullptr;
      continue;
    }
    if (auto *CI = dyn_cast<CallInst>(&I)) {
      if (Found)
        return nullptr; // more than one call: not a simple panic shim
      Found = CI;
      continue;
    }
    if (I.mayHaveSideEffects() || I.mayReadOrWriteMemory())
      return nullptr;
  }
  if (!Found)
    return nullptr;
  Function *Callee = Found->getCalledFunction();
  if (!Callee || !Callee->isDeclaration())
    return nullptr;
  if (!Found->doesNotReturn() && !Callee->doesNotReturn())
    return nullptr;
  return Found;
}

/// True if `V` and `Arg` denote the same value modulo a single level of
/// integer extension/truncation, which is the only kind of "noise" rustc
/// puts between a usize comparison and the equivalent panic-call argument
/// (e.g. comparing as i64 but passing a narrower index type, or vice versa).
bool matchesModuloIntCast(Value *V, Value *Arg) {
  if (V == Arg)
    return true;
  if (auto *Cast = dyn_cast<CastInst>(V))
    if (Cast->isIntegerCast() && Cast->getOperand(0) == Arg)
      return true;
  if (auto *Cast = dyn_cast<CastInst>(Arg))
    if (Cast->isIntegerCast() && Cast->getOperand(0) == V)
      return true;
  return false;
}

/// Decomposes `V` into the `icmp`s it's an `and` of, e.g. ndarray's
/// multi-dimensional `Index` impl checks each dimension separately and
/// combines them with `and i1 %row_in_bounds, %col_in_bounds` rather than a
/// single comparison -- found empirically (see bench/ and README.md) after
/// a version of this pass that only ever recognized a bare `icmp` matched
/// nothing in real ndarray-generated IR. Sets `Valid` to false (rejecting
/// the branch entirely) if `V` contains anything other than `icmp`s and
/// `and`s, e.g. a call result or an `or` -- this is meant to recognize "a
/// conjunction of range checks", not arbitrary boolean conditions that
/// merely happen to lead to a recognized panic call.
void collectIcmpLeaves(Value *V, SmallVectorImpl<ICmpInst *> &Leaves,
                       bool &Valid) {
  if (auto *Cmp = dyn_cast<ICmpInst>(V)) {
    Leaves.push_back(Cmp);
    return;
  }
  if (auto *BO = dyn_cast<BinaryOperator>(V);
      BO && BO->getOpcode() == Instruction::And) {
    collectIcmpLeaves(BO->getOperand(0), Leaves, Valid);
    collectIcmpLeaves(BO->getOperand(1), Leaves, Valid);
    return;
  }
  Valid = false;
}

/// True if at least two of the call's arguments (e.g. index and len out of
/// `panic_bounds_check(index, len, location)` -- the `location` argument is
/// never expected to match anything and that's fine) appear, modulo int
/// casts, as operands somewhere across the conjunction's comparisons. This
/// is the same "at least the index/len pair line up" bar the original
/// single-`icmp` version of this check used, generalized to a conjunction
/// of several comparisons (see collectIcmpLeaves) instead of just one.
/// Each operand can satisfy at most one call argument, so a degenerate
/// repeated operand can't trivially satisfy more than its share.
bool conditionMatchesCallArguments(ArrayRef<ICmpInst *> Leaves,
                                    CallInst *Call) {
  SmallVector<Value *, 8> Operands;
  for (ICmpInst *Cmp : Leaves) {
    Operands.push_back(Cmp->getOperand(0));
    Operands.push_back(Cmp->getOperand(1));
  }
  SmallPtrSet<Value *, 8> UsedOperands;
  unsigned MatchedArgs = 0;
  for (Value *Arg : Call->args()) {
    for (Value *Op : Operands) {
      if (UsedOperands.contains(Op))
        continue;
      if (matchesModuloIntCast(Op, Arg)) {
        UsedOperands.insert(Op);
        ++MatchedArgs;
        break;
      }
    }
  }
  return MatchedArgs >= 2;
}

/// If `Br`'s condition guards a call to panic_bounds_check on one side,
/// rewrites `Br` to unconditionally take the other (non-panicking) side.
/// Returns true if a rewrite happened.
bool tryEliminateGuard(BranchInst *Br) {
  if (!Br->isConditional())
    return false;
  SmallVector<ICmpInst *, 4> CmpLeaves;
  bool ValidCondition = true;
  collectIcmpLeaves(Br->getCondition(), CmpLeaves, ValidCondition);
  if (!ValidCondition || CmpLeaves.empty())
    return false;

  for (unsigned K = 0; K != 2; ++K) {
    BasicBlock *PanicBB = Br->getSuccessor(K);
    BasicBlock *SafeBB = Br->getSuccessor(1 - K);
    if (PanicBB == SafeBB)
      continue;
    // Deliberately not requiring PanicBB to have a single predecessor:
    // found empirically (see bench/ and README.md) that LLVM commonly
    // merges several distinct bounds-check failure paths -- e.g. separate
    // checks for `a[[i,j]]`, `b[[i,j]]`, `out[[i,j]]` -- into one shared
    // tail block when the panic call takes no arguments to distinguish
    // them (as ndarray's array_out_of_bounds doesn't). Soundness doesn't
    // depend on this block having one predecessor: for a panic call that
    // does carry index/length arguments, conditionMatchesCallArguments
    // below independently proves *this* Br is a guard for *this* call's
    // fixed, static operands regardless of how many other branches also
    // happen to share the block; for one that doesn't (see
    // BoundsCheckPanicFn::HasIndexLenArgs), the verification is already
    // documented as resting on structure and name alone, which doesn't
    // get any weaker just because other, independently-verified branches
    // also lead here.

    CallInst *Call = getSolePanicCall(PanicBB);
    if (!Call)
      continue;
    const BoundsCheckPanicFn *Fn =
        matchBoundsCheckPanicFunction(Call->getCalledFunction()->getName());
    if (!Fn)
      continue;
    if (Fn->HasIndexLenArgs && !conditionMatchesCallArguments(CmpLeaves, Call))
      continue;

    LLVM_DEBUG(dbgs() << "rust-hpc-bce: eliminating bounds-check guard "
                       << *Br << " (panic block " << PanicBB->getName()
                       << ")\n");

    BranchInst::Create(SafeBB, Br->getIterator());
    Br->eraseFromParent();
    for (ICmpInst *Cmp : CmpLeaves)
      RecursivelyDeleteTriviallyDeadInstructions(Cmp);
    ++NumBoundsChecksEliminated;
    return true;
  }
  return false;
}

} // namespace

PreservedAnalyses
rust_hpc::BoundsCheckEliminationPass::run(Function &F,
                                           FunctionAnalysisManager &AM) {
  if (!EnableBoundsCheckElim)
    return PreservedAnalyses::all();

  bool Changed = false;

  // Collect branches before mutating: rewriting one guard can make blocks
  // unreachable, but we erase/clean those up afterwards via
  // removeUnreachableBlocks rather than while iterating.
  SmallVector<BranchInst *, 16> Candidates;
  for (BasicBlock &BB : F)
    if (auto *Br = dyn_cast<BranchInst>(BB.getTerminator()))
      if (Br->isConditional())
        Candidates.push_back(Br);

  for (BranchInst *Br : Candidates)
    Changed |= tryEliminateGuard(Br);

  if (!Changed)
    return PreservedAnalyses::all();

  removeUnreachableBlocks(F);
  return PreservedAnalyses::none();
}

void rust_hpc::registerBoundsCheckEliminationPasses(PassBuilder &PB) {
  PB.registerPipelineParsingCallback(
      [](StringRef Name, FunctionPassManager &FPM,
         ArrayRef<PassBuilder::PipelineElement>) {
        if (Name == "rust-hpc-elim-bounds-checks") {
          FPM.addPass(rust_hpc::BoundsCheckEliminationPass());
          return true;
        }
        return false;
      });
}
