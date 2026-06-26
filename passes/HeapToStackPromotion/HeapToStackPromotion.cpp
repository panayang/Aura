//===- HeapToStackPromotion.cpp -------------------------------------------===//
//
// See HeapToStackPromotion.h.
//
// This is the inverse of the usual escape-analysis story (which proves a
// *stack* object never escapes so it can stay in registers): here the
// object already starts on the heap, via an explicit allocator call, and we
// prove the same "never escapes, lifetime fully contained in this
// function" property to justify moving it onto the stack instead. The
// proof obligation, for a matched alloc call A with returned pointer P:
//
//   1. The allocation size is either a compile-time constant, or a runtime
//      value ScalarEvolution can prove is bounded above by a constant --
//      either way, small enough to be safe as an `alloca` (capped by
//      -rust-hpc-max-promotable-alloca-size). A constant-size alloca is
//      hoisted to the entry block and reused across iterations if the
//      alloc call is inside a loop, same as before. A *provably bounded*
//      but non-constant size left inside a loop is instead bracketed with
//      `llvm.stacksave`/`llvm.stackrestore` around each logical instance
//      (save right where the alloc call was, restore right where each
//      dealloc call was): an `alloca` outside the entry block grows the
//      current stack frame every time it executes and isn't reclaimed
//      until the function returns, so without that bracket, one inside a
//      loop would leak stack space every iteration -- the same hazard as a
//      C VLA declared inside a loop body, and the same fix Clang itself
//      uses for exactly that case.
//   2. There is a set of calls D1..Dn, in the same function, to the paired
//      deallocator, such that every path from A to a function exit passes
//      through exactly one of them before either returning or looping back
//      to A itself (checked with a direct forward CFG search, see
//      pathsAllReachOneDealloc). A single dealloc site is the common case
//      and the only one v1 of this pass handled; multiple sites (e.g.
//      cleanup duplicated across an early-return error path) are now
//      supported directly by that same search.
//   3. Each Di's pointer argument resolves back to P, where "resolves back
//      to" means either (a) the same value modulo bitcasts/GEPs
//      (`getUnderlyingObject`), or (b) loaded from a local temporary that
//      has *exactly one* store anywhere in the function, dominating the
//      load, storing P -- the shape a Box/Vec's pointer field takes when
//      SROA/mem2reg hasn't (yet, or won't) turn it into P directly. Case
//      (b) is a verified structural fact (single store, proven dominance),
//      not an assumption that this pipeline position happens to look a
//      particular way.
//   4. P does not escape on any path from A to whichever Di it goes
//      through: checked with llvm::PointerMayBeCapturedBefore, the same
//      capture-tracking utility AliasAnalysis and the inliner's noalias
//      inference rely on, run once per Di (a use that escapes on the path
//      through one Di is caught by that Di's own check).
//
// Given all four hold, A and {Di} bracket P's *entire* useful lifetime with
// no way for it to be observed outside that bracket, which is exactly what
// `alloca` semantics provide for free (lifetime ends when the function
// returns; nothing outside the function can ever have seen the address).
//
// Allocator coverage: __rust_alloc[_zeroed]/__rust_dealloc are libstd's own
// portable allocator shim and so need no per-OS handling. HeapAlloc/HeapFree
// and malloc/free are matched too, both because output.s shows HeapFree
// surviving as a real call where the Rust-side alloc wrapper didn't, and so
// the same coverage exists on Linux (where libstd's allocator bottoms out
// in malloc/free, not a Windows-only API).
//
//===----------------------------------------------------------------------===//

#include "HeapToStackPromotion.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

#define DEBUG_TYPE "rust-hpc-heap-to-stack"

static cl::opt<bool> EnableHeapToStackPromotion(
    "rust-hpc-heap-to-stack-promote", cl::init(true),
    cl::desc("Replace non-escaping, bounded-size heap allocations with a "
             "stack alloca. Semantics-preserving when its checks succeed; "
             "on by default."));

static cl::opt<uint64_t> MaxPromotableAllocaSize(
    "rust-hpc-max-promotable-alloca-size", cl::init(8192),
    cl::desc("Upper bound (bytes) on the allocation size -- constant, or "
             "ScalarEvolution-provable as bounded -- this pass will turn "
             "into a stack alloca."));

STATISTIC(NumHeapAllocsPromoted,
          "Number of heap allocations promoted to a stack alloca");

namespace {

/// Describes one allocator/deallocator ABI pair this pass knows how to
/// promote. Matched by exact declared-function name -- these are all
/// plain, unmangled C symbols (libstd's own `__rust_*` shim included), not
/// Rust-mangled names, so no demangling is needed here unlike the other
/// passes in this project.
struct AllocatorPair {
  StringRef AllocName;
  int SizeArgIdx;
  int AlignArgIdx; // -1 if the ABI doesn't expose one.
  bool Zeroed;
  StringRef DeallocName;
  int DeallocPtrArgIdx;
};

constexpr AllocatorPair AllocatorPairs[] = {
    // libstd's portable allocator shim; size/align in usize args 0/1.
    {"__rust_alloc_zeroed", 0, 1, true, "__rust_dealloc", 0},
    {"__rust_alloc", 0, 1, false, "__rust_dealloc", 0},
    // Win32, when the alloc wrapper itself didn't survive as a named call
    // but HeapAlloc/HeapFree did (as seen in this project's output.s).
    {"HeapAlloc", 2, -1, false, "HeapFree", 2},
    // libc, for Linux/other Unix targets where libstd's allocator bottoms
    // out here instead.
    {"malloc", 0, -1, false, "free", 0},
};

constexpr unsigned DefaultAllocaAlignment = 16;

/// Returns the AllocatorPair matching `CI`'s callee, or nullptr.
const AllocatorPair *matchAllocCall(CallInst *CI) {
  Function *Callee = CI->getCalledFunction();
  if (!Callee || !Callee->isDeclaration())
    return nullptr;
  for (const AllocatorPair &Pair : AllocatorPairs)
    if (Callee->getName() == Pair.AllocName)
      return &Pair;
  return nullptr;
}

/// The allocation size this pass will promote: either a literal constant,
/// or a value ScalarEvolution can prove is bounded by
/// MaxPromotableAllocaSize, kept as the original Value so a dynamic
/// `alloca` can be sized with it directly.
struct PromotableSize {
  uint64_t ConstantValue = 0;
  Value *DynamicValue = nullptr;
  bool isConstant() const { return DynamicValue == nullptr; }
};

std::optional<PromotableSize> getPromotableSize(CallInst *CI,
                                                 const AllocatorPair &Pair,
                                                 ScalarEvolution &SE) {
  Value *SizeArg = CI->getArgOperand(Pair.SizeArgIdx);
  if (auto *SizeC = dyn_cast<ConstantInt>(SizeArg)) {
    uint64_t Size = SizeC->getZExtValue();
    if (Size == 0 || Size > MaxPromotableAllocaSize)
      return std::nullopt;
    return PromotableSize{Size, nullptr};
  }

  // Not a literal constant: ask ScalarEvolution for a provable upper bound
  // instead of giving up. This is the same reuse-not-reinvent move as the
  // GEPStrengthReduction pass -- SCEV already knows how to derive a sound
  // range for an expression like `and %x, 4095` or a value clamped by a
  // dominating `icmp`+`select`, which would otherwise require redoing
  // ValueTracking's range analysis by hand.
  if (!SE.isSCEVable(SizeArg->getType()))
    return std::nullopt;
  const SCEV *S = SE.getSCEV(SizeArg);
  APInt Max = SE.getUnsignedRangeMax(S);
  if (Max.isZero() || Max.ugt(MaxPromotableAllocaSize))
    return std::nullopt;
  return PromotableSize{0, SizeArg};
}

Align getConstantAlignOr(CallInst *CI, const AllocatorPair &Pair,
                          unsigned Default) {
  if (Pair.AlignArgIdx < 0)
    return Align(Default);
  auto *AlignC = dyn_cast<ConstantInt>(CI->getArgOperand(Pair.AlignArgIdx));
  if (!AlignC || !AlignC->getValue().isPowerOf2())
    return Align(Default);
  return Align(AlignC->getZExtValue());
}

/// Describes how a dealloc call's pointer argument was proven to resolve
/// back to a given alloc call, and what that proof still requires the
/// escape check to verify.
struct PointerResolution {
  bool Matched = false;
  /// AllocCI must not escape via any use reachable before this
  /// instruction. For a direct match this is the dealloc call itself
  /// (matching the original v1 check); for a reload-from-local-slot match
  /// it's that slot's *store* instead -- see below for why.
  Instruction *EscapeCheckBoundary = nullptr;
  /// If set, this value must independently never escape anywhere in the
  /// function (checked with the unbounded `PointerMayBeCaptured`, not the
  /// "before" variant).
  Value *MustNotEscape = nullptr;
};

/// Determines whether `DeallocArg` resolves back to `AllocCI`'s returned
/// pointer, by either being the same value modulo bitcasts/GEPs, or being
/// loaded from a local temporary ("Slot") that has exactly one store
/// anywhere in the function, dominating this load, storing `AllocCI`. Case
/// (b) is what a Box/Vec's single-pointer-field struct looks like when
/// SROA/mem2reg hasn't (yet, or won't) eliminated it into a direct SSA
/// value; verifying "exactly one store, and it dominates this load" is what
/// makes accepting it sound -- with only one store to that memory in the
/// whole function, nothing else could have changed it before this load
/// sees it.
///
/// Why case (b) needs its own escape-check boundary, not just "before the
/// dealloc call" like case (a): storing a pointer into memory at all is
/// treated by CaptureTracking as a potential capture by default, since on
/// its own it has no way to know that *this particular* destination (Slot)
/// is itself non-escaping -- that's exactly the extra fact this function
/// just verified, which the generic capture check can't see. So the escape
/// check for AllocCI itself only needs to run up to Slot's store (by which
/// point, in the shapes this matches, AllocCI has no other uses left to
/// worry about), and Slot's own non-capture is checked separately as an
/// independent condition.
PointerResolution resolvePointerToAlloc(Value *DeallocArg, CallInst *AllocCI,
                                         CallInst *DeallocCI,
                                         DominatorTree &DT) {
  if (getUnderlyingObject(DeallocArg) == AllocCI)
    return {true, DeallocCI, nullptr};

  auto *Load = dyn_cast<LoadInst>(DeallocArg);
  if (!Load)
    return {};
  auto *Slot =
      dyn_cast<AllocaInst>(getUnderlyingObject(Load->getPointerOperand()));
  if (!Slot)
    return {};

  StoreInst *UniqueStore = nullptr;
  for (User *U : Slot->users()) {
    auto *SI = dyn_cast<StoreInst>(U);
    if (!SI || SI->getPointerOperand() != Slot)
      continue; // Not a whole-slot store (a load, or a store through a
                // sub-offset GEP); conservatively still required to find
                // at most one *whole-slot* store below.
    if (UniqueStore)
      return {}; // More than one store to this slot: too risky to reason
                 // about which one (if any) reaches this load without
                 // redoing real memory-SSA analysis.
    UniqueStore = SI;
  }
  if (!UniqueStore ||
      getUnderlyingObject(UniqueStore->getValueOperand()) != AllocCI ||
      !DT.dominates(UniqueStore, Load))
    return {};

  return {true, UniqueStore, Slot};
}

struct DeallocMatch {
  CallInst *Call;
  PointerResolution Resolution;
};

/// Finds every call in `F` to `Pair.DeallocName` whose pointer argument
/// resolves back to `AllocCI` per resolvePointerToAlloc.
SmallVector<DeallocMatch, 2> findDeallocCalls(Function &F, CallInst *AllocCI,
                                               const AllocatorPair &Pair,
                                               DominatorTree &DT) {
  SmallVector<DeallocMatch, 2> Found;
  for (Instruction &I : instructions(F)) {
    auto *CI = dyn_cast<CallInst>(&I);
    if (!CI)
      continue;
    Function *Callee = CI->getCalledFunction();
    if (!Callee || Callee->getName() != Pair.DeallocName)
      continue;
    PointerResolution Resolution = resolvePointerToAlloc(
        CI->getArgOperand(Pair.DeallocPtrArgIdx), AllocCI, CI, DT);
    if (Resolution.Matched)
      Found.push_back({CI, Resolution});
  }
  return Found;
}

/// True if every path forward from `AllocCI` reaches one of `Deallocs`
/// before either reaching a return/unreachable terminator (a leak) or
/// looping back around to `AllocCI`'s own block (meaning `AllocCI` could
/// execute again before this allocation's instance was ever freed -- not
/// something well-formed Rust alloc/dealloc pairing should produce, so
/// conservatively rejected rather than reasoned about further).
///
/// This single forward search handles any number of dealloc sites
/// uniformly: each one simply ends the search along whichever paths reach
/// it, with no need to characterize the *set* of sites as a block via
/// PostDominatorTree (which doesn't directly answer "does this entire set,
/// jointly, post-dominate" in one query).
bool pathsAllReachOneDealloc(CallInst *AllocCI,
                              ArrayRef<CallInst *> Deallocs) {
  SmallPtrSet<CallInst *, 4> DeallocSet(Deallocs.begin(), Deallocs.end());
  BasicBlock *AllocBB = AllocCI->getParent();

  SmallPtrSet<BasicBlock *, 16> EnteredBlocks;
  SmallVector<Instruction *, 16> Worklist;

  auto pushSuccessors = [&](BasicBlock *BB, bool &LoopedBack) {
    for (BasicBlock *Succ : successors(BB)) {
      if (Succ == AllocBB) {
        LoopedBack = true;
        return;
      }
      if (EnteredBlocks.insert(Succ).second)
        Worklist.push_back(&Succ->front());
    }
  };

  Worklist.push_back(AllocCI->getNextNode());
  while (!Worklist.empty()) {
    Instruction *I = Worklist.pop_back_val();
    for (; I; I = I->getNextNode()) {
      if (auto *CI = dyn_cast<CallInst>(I); CI && DeallocSet.contains(CI))
        goto handled;
      if (I->isTerminator()) {
        if (isa<ReturnInst>(I) || isa<UnreachableInst>(I))
          return false; // Reached a function exit without freeing.
        bool LoopedBack = false;
        pushSuccessors(I->getParent(), LoopedBack);
        if (LoopedBack)
          return false;
        break;
      }
    }
  handled:
    continue;
  }
  return true;
}

/// Replaces the alloc/dealloc(s) with a stack alloca. `Size` is the
/// already-validated promotable size. `NeedsStackSaveRestore` is set when
/// this is a non-constant-sized allocation inside a loop: see the file
/// header for why that case needs `llvm.stacksave`/`llvm.stackrestore`
/// bracketing each logical instance rather than a plain `alloca`.
void promoteToStack(CallInst *AllocCI, ArrayRef<DeallocMatch> Deallocs,
                     const AllocatorPair &Pair, const PromotableSize &Size,
                     Align Alignment, bool InsertAtAllocSite,
                     bool NeedsStackSaveRestore) {
  Function &F = *AllocCI->getFunction();
  IRBuilder<> Builder(InsertAtAllocSite
                           ? AllocCI
                           : &*F.getEntryBlock().getFirstInsertionPt());

  // Captures the stack pointer right before the alloca grows the frame, so
  // each dealloc site can restore it afterwards and reclaim that space
  // before the next loop iteration's allocation runs.
  Value *StackSavePtr =
      NeedsStackSaveRestore ? Builder.CreateStackSave() : nullptr;

  AllocaInst *Slot;
  Value *ByteSize;
  if (Size.isConstant()) {
    Slot = Builder.CreateAlloca(
        ArrayType::get(Builder.getInt8Ty(), Size.ConstantValue), nullptr,
        "rust.hpc.h2s");
    ByteSize = Builder.getInt64(Size.ConstantValue);
  } else {
    Slot = Builder.CreateAlloca(Builder.getInt8Ty(), Size.DynamicValue,
                                 "rust.hpc.h2s");
    ByteSize = Size.DynamicValue;
  }
  Slot->setAlignment(Alignment);

  IRBuilder<> AtAlloc(AllocCI);
  if (Pair.Zeroed) {
    // Re-zero at the original call site, not just once where the alloca
    // was inserted: if the alloc call is inside a loop, each "logical"
    // allocation still needs its own zero-fill, since the stack slot now
    // persists (or, for the stacksave/stackrestore case, the freshly
    // re-grown region may coincidentally hold a previous iteration's
    // bytes) where a fresh heap allocation wouldn't have.
    AtAlloc.CreateMemSet(Slot, AtAlloc.getInt8(0), ByteSize, Alignment);
  }

  AllocCI->replaceAllUsesWith(Slot);
  AllocCI->eraseFromParent();

  for (const DeallocMatch &M : Deallocs) {
    CallInst *DeallocCI = M.Call;
    if (StackSavePtr)
      IRBuilder<>(DeallocCI).CreateStackRestore(StackSavePtr);
    Value *DeallocPtrArg = DeallocCI->getArgOperand(Pair.DeallocPtrArgIdx);
    DeallocCI->eraseFromParent();
    // The dealloc call's other arguments (handle, flags, size, align), and
    // any now-dead load/alloca temporary the pointer used to be reloaded
    // from, may now be dead; let DCE decide, we just make sure dropping
    // our own reference doesn't leave them artificially alive.
    if (auto *DeadArg = dyn_cast<Instruction>(DeallocPtrArg))
      RecursivelyDeleteTriviallyDeadInstructions(DeadArg);
  }

  ++NumHeapAllocsPromoted;
}

} // namespace

PreservedAnalyses
rust_hpc::HeapToStackPromotionPass::run(Function &F,
                                         FunctionAnalysisManager &AM) {
  if (!EnableHeapToStackPromotion)
    return PreservedAnalyses::all();

  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &LI = AM.getResult<LoopAnalysis>(F);
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);

  SmallVector<CallInst *, 8> AllocCandidates;
  for (Instruction &I : instructions(F))
    if (auto *CI = dyn_cast<CallInst>(&I))
      if (matchAllocCall(CI))
        AllocCandidates.push_back(CI);

  bool Changed = false;
  for (CallInst *AllocCI : AllocCandidates) {
    const AllocatorPair *Pair = matchAllocCall(AllocCI);
    if (!Pair)
      continue; // Already promoted as part of another candidate's cleanup.

    std::optional<PromotableSize> Size = getPromotableSize(AllocCI, *Pair, SE);
    if (!Size)
      continue;

    bool InLoop = LI.getLoopFor(AllocCI->getParent()) != nullptr;
    bool NeedsStackSaveRestore = !Size->isConstant() && InLoop;

    SmallVector<DeallocMatch, 2> Deallocs =
        findDeallocCalls(F, AllocCI, *Pair, DT);
    if (Deallocs.empty())
      continue;

    SmallVector<CallInst *, 2> DeallocCalls;
    for (const DeallocMatch &M : Deallocs)
      DeallocCalls.push_back(M.Call);
    if (!pathsAllReachOneDealloc(AllocCI, DeallocCalls))
      continue;

    bool Escapes = any_of(Deallocs, [&](const DeallocMatch &M) {
      if (PointerMayBeCapturedBefore(AllocCI, /*ReturnCaptures=*/true,
                                       M.Resolution.EscapeCheckBoundary, &DT,
                                       /*IncludeI=*/false))
        return true;
      return M.Resolution.MustNotEscape &&
             PointerMayBeCaptured(M.Resolution.MustNotEscape,
                                   /*ReturnCaptures=*/true);
    });
    if (Escapes)
      continue;

    Align Alignment = getConstantAlignOr(AllocCI, *Pair, DefaultAllocaAlignment);

    LLVM_DEBUG(dbgs() << "rust-hpc-h2s: promoting " << *AllocCI
                       << " to a stack alloca\n");

    promoteToStack(AllocCI, Deallocs, *Pair, *Size, Alignment,
                    /*InsertAtAllocSite=*/!Size->isConstant(),
                    NeedsStackSaveRestore);
    Changed = true;
  }

  if (!Changed)
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}

void rust_hpc::registerHeapToStackPromotionPasses(PassBuilder &PB) {
  PB.registerPipelineParsingCallback(
      [](StringRef Name, FunctionPassManager &FPM,
         ArrayRef<PassBuilder::PipelineElement>) {
        if (Name == "rust-hpc-heap-to-stack-promote") {
          FPM.addPass(rust_hpc::HeapToStackPromotionPass());
          return true;
        }
        return false;
      });
}
