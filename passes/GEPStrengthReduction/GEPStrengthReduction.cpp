//===- GEPStrengthReduction.cpp ------------------------------------------===//
//
// See GEPStrengthReduction.h for the pass description.
//
// Why this needs to exist as its own pass rather than relying solely on
// -loop-reduce: LSR runs late in the default pipeline and is conservative
// about pointer-typed candidates that come from `getelementptr` chains
// produced after inlining heap-allocation-removal/bounds-check-removal
// transforms run earlier in this plugin's pipeline. Those upstream passes
// can expose `mul`-based index recomputation that was previously hidden
// behind a call boundary; this pass re-canonicalizes it immediately so later
// passes (SLP/LoopVectorize, instruction scheduling) see clean affine
// pointer recurrences instead of a multiply on every iteration.
//
// v2 design: rather than hand-matching one specific IR shape (a single GEP
// index fed by `mul <canonical IV>, <invariant stride>`), this pass asks
// ScalarEvolution for the GEP's address SCEV directly. SCEV already
// linearizes multi-index GEPs (struct/array navigation through several
// dimensions) into one polynomial expression in terms of the enclosing
// loops' trip counts, and it normalizes away however a loop's IV happens to
// be expressed in IR (count up, count down, step by a non-unit constant,
// driven by a variable nobody would recognize as "the" induction variable
// by inspection). That covers multi-index GEPs, non-canonical IVs, and
// nested loops (a nested AddRecExpr) uniformly, instead of needing bespoke
// matching code for each shape.
//
// Expansion uses SCEVExpander in *non-canonical* mode, the same mode
// LoopStrengthReduce.cpp uses (see disableCanonicalMode()/enableLSRMode()
// there) for exactly this kind of rewrite. Canonical mode tries to reuse
// existing IR induction variables creatively and, for an insert point at a
// loop preheader, that was observed to synthesize a value referencing an
// LCSSA phi that doesn't dominate the use (a real, reproduced bug during
// development of this pass) -- non-canonical mode always builds a fresh,
// self-contained phi/add chain instead, which is what we want here anyway.
//
//===----------------------------------------------------------------------===//

#include "GEPStrengthReduction.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"

using namespace llvm;

#define DEBUG_TYPE "rust-hpc-gep-strength-reduce"

STATISTIC(NumGEPsStrengthReduced,
          "Number of GEPs rewritten from recomputed index arithmetic to "
          "loop-carried pointer increments");

namespace {

/// True if `GEP`'s pointer operand is already exactly the cheapest possible
/// per-iteration form: a phi that is itself this loop's running pointer,
/// stepped by a single loop-invariant index. There's nothing left to fold
/// here -- this is what our own output looks like, so this check is also
/// what keeps the pass idempotent instead of re-deriving the same
/// SCEVAddRecExpr and rebuilding an equivalent phi/add chain every run.
bool isAlreadyMinimalPointerRecurrence(GetElementPtrInst *GEP,
                                        const Loop *L) {
  auto *PtrPhi = dyn_cast<PHINode>(GEP->getPointerOperand());
  return PtrPhi && PtrPhi->getParent() == L->getHeader() &&
         GEP->getNumIndices() == 1 && L->isLoopInvariant(*GEP->idx_begin());
}

/// True if at least one of the GEP's indices actually varies across
/// iterations of \p L. This deliberately does not require an explicit
/// `mul`/`shl` in the IR: a multi-index GEP into a multi-dimensional
/// aggregate (`gep [1024 x [64 x float]], ptr, i64 0, i64 %row, i64 %col`)
/// implies a multiply by each dimension's stride without any multiply
/// instruction ever appearing in the IR -- the cost shows up later, at
/// lowering. Whether the recurrence is actually affine and worth rewriting
/// is ScalarEvolution's job in tryStrengthReduceGEP; this is only the cheap
/// pre-filter for "is there anything here for SCEV to even look at".
bool hasLoopVariantIndex(GetElementPtrInst *GEP, const Loop *L) {
  for (Value *Idx : GEP->indices())
    if (!isa<Constant>(Idx) && !L->isLoopInvariant(Idx))
      return true;
  return false;
}

/// Attempts to strength-reduce a single GEP whose address SCEV is an affine
/// recurrence of \p L. Returns true if the GEP was rewritten.
bool tryStrengthReduceGEP(GetElementPtrInst *GEP, Loop *L,
                           ScalarEvolution &SE) {
  if (isAlreadyMinimalPointerRecurrence(GEP, L) || !hasLoopVariantIndex(GEP, L))
    return false;

  if (!SE.isSCEVable(GEP->getType()))
    return false;

  const SCEV *AddrSCEV = SE.getSCEV(GEP);
  const auto *AR = dyn_cast<SCEVAddRecExpr>(AddrSCEV);
  if (!AR || AR->getLoop() != L || !AR->isAffine())
    return false;

  BasicBlock *Preheader = L->getLoopPreheader();
  if (!Preheader)
    return false;

  SCEVExpander Expander(SE, "rust-hpc-gsr");
  // Non-canonical mode: always synthesize a fresh phi/add chain for the
  // recurrence instead of trying to reuse/repurpose whatever induction
  // variable already exists in the loop. See the file header comment for
  // why canonical mode (the default) isn't safe to use the way we need to
  // here.
  Expander.disableCanonicalMode();

  if (!Expander.isSafeToExpand(AR))
    return false;

  // Insert at the GEP's own location, not the preheader: we want "the
  // address as of this iteration", which only means something inside the
  // loop. SCEVExpander places the recurrence's loop-invariant pieces (start
  // value, per-iteration increment) in the preheader on its own as part of
  // building the phi; it doesn't need to be told to do that separately.
  Expander.setInsertPoint(GEP);
  Value *NewAddr = Expander.expandCodeFor(AR, GEP->getType());

  if (NewAddr == GEP)
    return false;

  LLVM_DEBUG(dbgs() << "rust-hpc-gsr: strength-reducing " << *GEP << " -> "
                     << *NewAddr << '\n');

  GEP->replaceAllUsesWith(NewAddr);
  ++NumGEPsStrengthReduced;
  return true;
}

bool runOnLoop(Loop *L, ScalarEvolution &SE) {
  bool Changed = false;

  // Process inner loops first: an outer loop's AddRec is defined partly in
  // terms of the inner loop's trip count, so simplifying the inner loop's
  // own GEPs first can't invalidate anything we still need, whereas doing
  // the outer loop first and then mutating the inner loop could.
  for (Loop *Sub : *L)
    Changed |= runOnLoop(Sub, SE);

  SmallVector<GetElementPtrInst *, 8> Candidates;
  for (BasicBlock *BB : L->blocks())
    for (Instruction &I : *BB)
      if (auto *GEP = dyn_cast<GetElementPtrInst>(&I))
        Candidates.push_back(GEP);

  SmallVector<WeakTrackingVH, 8> DeadInsts;
  for (GetElementPtrInst *GEP : Candidates) {
    if (tryStrengthReduceGEP(GEP, L, SE)) {
      Changed = true;
      DeadInsts.push_back(GEP);
    }
  }

  RecursivelyDeleteTriviallyDeadInstructions(DeadInsts);
  return Changed;
}

} // namespace

PreservedAnalyses
rust_hpc::GEPStrengthReductionPass::run(Function &F,
                                         FunctionAnalysisManager &AM) {
  auto &LI = AM.getResult<LoopAnalysis>(F);
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);

  bool Changed = false;
  for (Loop *L : LI)
    Changed |= runOnLoop(L, SE);

  if (!Changed)
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}

void rust_hpc::registerGEPStrengthReductionPasses(PassBuilder &PB) {
  PB.registerPipelineParsingCallback(
      [](StringRef Name, FunctionPassManager &FPM,
         ArrayRef<PassBuilder::PipelineElement>) {
        if (Name == "rust-hpc-gep-strength-reduce") {
          FPM.addPass(rust_hpc::GEPStrengthReductionPass());
          return true;
        }
        return false;
      });
}
