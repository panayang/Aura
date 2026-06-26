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

/// True if the (demangled) name of a Rust core panicking function is
/// specifically the bounds-check one. Kept as an explicit allowlist rather
/// than a substring search for "panic" so that unrelated panics (explicit
/// user panics, `.unwrap()`/`.expect()` failures, assertion failures,
/// arithmetic-overflow panics, etc.) are never touched.
bool isBoundsCheckPanicFunction(StringRef MangledName) {
  std::string Demangled = demangle(MangledName);
  StringRef D(Demangled);
  // v0 and legacy mangling both demangle to this same path; check the raw
  // mangled name too in case `demangle()` fails to recognize a mangling
  // scheme variant (defensive -- legacy Rust mangling is a thin wrapper
  // around Itanium and should always demangle, but symbol mangling has
  // changed across rustc versions before).
  return D.contains("core::panicking::panic_bounds_check") ||
         MangledName.contains("panic_bounds_check");
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

/// True if the icmp's two operands are, as a set, the same two values
/// (modulo int casts) as two of the call's arguments -- i.e. this
/// comparison and this call are talking about the same index/length pair.
bool conditionMatchesCallArguments(ICmpInst *Cmp, CallInst *Call) {
  Value *C0 = Cmp->getOperand(0), *C1 = Cmp->getOperand(1);
  bool Found0 = false, Found1 = false;
  for (Value *Arg : Call->args()) {
    if (!Found0 && matchesModuloIntCast(C0, Arg))
      Found0 = true;
    else if (!Found1 && matchesModuloIntCast(C1, Arg))
      Found1 = true;
  }
  return Found0 && Found1;
}

/// If `Br`'s condition guards a call to panic_bounds_check on one side,
/// rewrites `Br` to unconditionally take the other (non-panicking) side.
/// Returns true if a rewrite happened.
bool tryEliminateGuard(BranchInst *Br) {
  if (!Br->isConditional())
    return false;
  auto *Cmp = dyn_cast<ICmpInst>(Br->getCondition());
  if (!Cmp)
    return false;

  for (unsigned K = 0; K != 2; ++K) {
    BasicBlock *PanicBB = Br->getSuccessor(K);
    BasicBlock *SafeBB = Br->getSuccessor(1 - K);
    if (PanicBB == SafeBB || !PanicBB->hasNPredecessors(1))
      continue;

    CallInst *Call = getSolePanicCall(PanicBB);
    if (!Call)
      continue;
    if (!isBoundsCheckPanicFunction(Call->getCalledFunction()->getName()))
      continue;
    if (!conditionMatchesCallArguments(Cmp, Call))
      continue;

    LLVM_DEBUG(dbgs() << "rust-hpc-bce: eliminating bounds-check guard "
                       << *Br << " (panic block " << PanicBB->getName()
                       << ")\n");

    BranchInst::Create(SafeBB, Br->getIterator());
    Br->eraseFromParent();
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
