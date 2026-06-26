//===- ColdPathOutlining.cpp ----------------------------------------------===//
//
// See ColdPathOutlining.h.
//
// Why this is built on llvm::CodeExtractor rather than a hand-rolled region
// splitter: pulling a chunk of a function's CFG out into a new function
// while correctly handling live-in/live-out values, PHI nodes at the
// region's entry and exits, lifetime markers, and exception-handling
// constraints is exactly what CodeExtractor already does, and is exactly
// the utility -hotcoldsplit (lib/Transforms/IPO/HotColdSplitting.cpp) uses
// for the same kind of transform. Reimplementing that logic here would only
// add a second, less battle-tested copy of it. Our job is narrower and
// Rust-specific: deciding *which* code is "defensive overhead that
// shouldn't be inline in a hot loop" in the first place, which is not
// something -hotcoldsplit can do on its own -- it relies on profile data or
// existing `cold`/`unlikely` markup, and Rust's println!/Mutex/OnceLock
// runtime calls in the hot path have neither: they're called unconditionally,
// not behind a branch some profiler marked rare.
//
// v2 design: a real slow path is rarely just the one block containing a
// recognizable call. In this project's own output.s, the println-adjacent
// code is a *chain* -- a TLS-slot check, a CAS retry loop allocating a
// thread id, a mutex-contended wait, and only then the actual fmt::write
// call -- and the blocks in between have no recognizable name at all (raw
// TLS pointer-chasing, an inlined atomic compare-exchange loop). Anchoring
// purely on named calls, as v1 did, leaves all of that connective tissue
// behind in the hot function. So this version only uses the name allowlist
// to find a *seed* call; the *extent* of what gets outlined around that
// seed is found structurally, with no naming involved:
//
//   1. Climb the seed block's *dominator-tree* ancestor chain (idom, idom
//      of idom, ...) up to, but not including, the loop header's direct
//      dominator-tree child. Any block's dominated set is automatically a
//      valid single-entry region (an edge can't enter it except through
//      that block, or something it dominates wouldn't be dominated), so
//      this never needs to separately prove the climb is "legal" --
//      only how far up is still the *same* slow path.
//   2. That "how far" question is settled by checking, at each step before
//      absorbing an ancestor P, whether every successor of P can still
//      reach the seed. If they all can, P's own branching (if any) is
//      purely internal to the slow path -- a retry loop, an if/else that
//      both arms feed back into more cold code -- and it's safe to keep
//      climbing through it. If some successor of P escapes without ever
//      reaching the seed, P is the actual hot/cold boundary, and climbing
//      stops one step below it. This is what keeps a block of ordinary,
//      always-executed hot setup code that merely happens to sit right
//      before the real fork from being swept into the region: it has the
//      same idom chain as the fork, but the fork's "skip this entirely"
//      successor can't reach the seed, so climbing correctly stops there.
//   3. Take every block in the loop dominated by the final entry. This
//      pulls in whatever sits between it and the seed, including nested
//      sub-control-flow like a retry loop, without needing to recognize
//      any of it by name.
//
// This is the same "isEligible() is the real gate" philosophy as v1: step 1
// only needs to propose a plausible single-entry root, not prove it's
// legal. CodeExtractor's own checks reject anything that isn't.
//
// Platform note: the Rust-side names below (std::sync::*, std::sys::sync::*,
// core::fmt::*, core::panicking::*) are the same across every target Rust
// std supports -- they're paths through libstd's own source, not anything
// platform-specific, so this allowlist needs no per-OS branching for that
// part. What *is* platform-specific is the bottom-most primitive a contended
// lock or thread-id allocation eventually calls into (Win32 vs. pthreads vs.
// a raw Linux futex syscall); see the comment by the allowlist for how that
// asymmetry is handled instead of just listing every OS's syscall wrapper.
//
//===----------------------------------------------------------------------===//

#include "ColdPathOutlining.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/CodeExtractor.h"

using namespace llvm;

#define DEBUG_TYPE "rust-hpc-cold-path-outline"

static cl::opt<bool> EnableColdPathOutlining(
    "rust-hpc-cold-path-outline", cl::init(true),
    cl::desc("Outline calls to known synchronization/IO/formatting overhead "
             "out of hot loops into separate cold functions. Semantics-"
             "preserving (unlike -rust-hpc-trust-bounds-checks); on by "
             "default."));

STATISTIC(NumColdRegionsOutlined,
          "Number of defensive sync/IO/formatting regions outlined out of "
          "hot loops");

namespace {

/// Demangled-name substrings used only to find a *seed* instruction inside
/// a slow path; the region actually outlined around that seed is found
/// structurally (see file header), not by extending this list to cover
/// every instruction in the path. Grouped by category; deliberately
/// specific (not just "fmt" or "sync") to avoid catching legitimate
/// Display/atomic code that's part of the real algorithm.
constexpr StringRef ColdCallNameAllowlist[] = {
    // println!/print!/write! runtime. Platform-independent: libstd's
    // formatting machinery is the same Rust source on every target.
    "std::io::stdio::_print",
    "std::io::stdio::stdout",
    "std::io::Stdout",
    "core::fmt::write",
    // Mutex/RwLock/OnceLock runtime (contention slow paths, lazy init).
    // Also platform-independent at this level: libstd's `futex` module name
    // is used on every target that has *a* futex-like primitive (Linux,
    // Windows via WaitOnAddress, etc.); only the syscall underneath it
    // differs, see below.
    "std::sync::Mutex",
    "std::sync::RwLock",
    "std::sync::OnceLock",
    "std::sync::once_lock",
    "std::sys::sync::mutex",
    "std::sys::sync::rwlock",
    "std::sys::sync::once",
    "std::sys::sync::condvar",
    // Thread-id allocation slow path (TLS miss -> global counter CAS loop).
    "std::thread::id",
    // Formatting-based panics (distinct from panic_bounds_check, which
    // BoundsCheckElimination.cpp handles under its own, much stricter,
    // opt-in gate; panic_fmt is always a genuine panic, always cold).
    "core::panicking::panic_fmt",
    // Win32 synchronization primitives libstd's futex-based sync calls into
    // directly; plain C symbol names, not Rust-mangled.
    "WakeByAddressSingle",
    "WaitOnAddress",
    "AcquireSRWLockExclusive",
    "ReleaseSRWLockExclusive",
    "EnterCriticalSection",
    "LeaveCriticalSection",
    // pthreads-based sync, used by libstd on targets without a native futex
    // (and by any crate that calls into libc's pthread API directly rather
    // than through libstd). Not needed on a target where libstd's own
    // `std::sys::sync::mutex::futex` entries above already anchor the
    // region, but kept as a fallback anchor for when that layer got fully
    // inlined and only the libc call remains visible.
    "pthread_mutex_lock",
    "pthread_mutex_unlock",
    "pthread_rwlock_rdlock",
    "pthread_rwlock_wrlock",
    "pthread_cond_wait",
    "pthread_cond_signal",
    // Deliberately NOT matching bare "syscall" or "futex" as substrings:
    // on Linux, libstd's futex wait/wake goes through a direct `syscall(2)`
    // call (or even an inlined `syscall` instruction with no call at all,
    // depending on how it's invoked), and matching that name would also
    // match any legitimate direct syscall a hot loop makes on purpose. The
    // structural region growth below means we don't need a name for that
    // layer anyway: once a libstd-side name above anchors the region, the
    // syscall sitting downstream of it gets pulled in by dominance whether
    // or not we can name it.
};

bool isColdOverheadCall(StringRef MangledName) {
  std::string Demangled = demangle(MangledName);
  for (StringRef Needle : ColdCallNameAllowlist)
    if (StringRef(Demangled).contains(Needle) || MangledName.contains(Needle))
      return true;
  return false;
}

/// Marks the outlined function and the call site left behind the same way
/// HotColdSplitting.cpp does for a profile-identified cold region: cold
/// calling convention (when the target finds that profitable), the Cold
/// and NoInline attributes, and the Cold function attribute on the callee
/// itself so nothing downstream second-guesses this back into the hot path.
void markExtractedFunctionCold(Function &OldF, CallInst &CI, Function &NewF) {
  if (!NewF.hasFnAttribute(Attribute::Cold))
    NewF.addFnAttr(Attribute::Cold);
  NewF.addFnAttr(Attribute::NoInline);
  CI.addFnAttr(Attribute::Cold);
  CI.setIsNoInline();

  if (OldF.hasSection())
    NewF.setSection(OldF.getSection());
}

/// Climbs the *dominator tree* from `Seed` (not the CFG's predecessor
/// edges directly -- see below) up to, but not including, the loop
/// header's immediate dominator-tree child on the path to `Seed`.
///
/// Any block A's dominated set Dom(A) is automatically a valid single-entry
/// region with sole entry A: by definition of dominance, any edge entering
/// Dom(A) from outside it must target A itself (if some other block in
/// Dom(A) were reachable without passing through A, A wouldn't dominate
/// it). So the only real question is how far up the idom chain to climb,
/// not whether climbing is "legal" -- it always is. Climbing all the way to
/// the header's direct child is the natural stopping point: that child's
/// idom (the header) is exactly where this slow path's hot-path sibling
/// (the loop's normal continuation) branches away, and going any higher
/// would mean folding the header itself into the region.
///
/// Note this deliberately uses the *dominator-tree* parent at each step,
/// not "the unique CFG predecessor": a slow path's own internal branches
/// (e.g. a retry loop with two ways to reach the block after it) mean a
/// block can have more than one direct CFG predecessor while still having
/// a single well-defined region entry further up -- dominance, not raw
/// predecessor counting, is what correctly collapses that back to one
/// answer.
///
/// Before absorbing `P` into the region (climbing past the current entry
/// candidate `A` up to `P`), check that *every* successor of `P` can still
/// reach `Seed`. If they all can, `P`'s branching (if it has any) is purely
/// internal to this slow path -- e.g. a retry loop, or an if/else where
/// both arms lead deeper into the same cold code -- and absorbing it is
/// correct. If some successor of `P` *cannot* reach `Seed`, that successor
/// is a way to skip the slow path entirely, which makes `P` itself the
/// real hot/cold boundary: stop, and don't absorb it.
///
/// This is what distinguishes "a block whose branching is incidental
/// internal structure of the cold region" from "a block of ordinary,
/// unconditionally-executed hot setup code that happens to sit right
/// before the real fork" -- both have the same idom-chain shape, but only
/// the first should be pulled into the outlined function. Plain dominance
/// can't tell them apart on its own (that's what the first version of this
/// function got wrong); reachability from each successor is what's
/// actually being asked: "does taking this edge still lead to the cold
/// code, or does it bypass it?"
///
/// The reachability check excludes `L`'s header: without that, "can Succ
/// reach Seed" is trivially true for *any* two blocks inside the loop, by
/// going around the back-edge and reaching Seed on a later iteration. What
/// we actually need to know is reachability *within this same iteration*,
/// i.e. without going around the loop again -- cutting the header out of
/// the search is what enforces that (every cross-iteration path has to
/// pass through it).
bool allSuccessorsCanReachSeed(BasicBlock *P, BasicBlock *Seed, const Loop *L,
                                DominatorTree &DT, LoopInfo &LI) {
  SmallPtrSet<BasicBlock *, 1> ExclusionSet{L->getHeader()};
  for (BasicBlock *Succ : successors(P))
    if (Succ != Seed &&
        !isPotentiallyReachable(Succ, Seed, &ExclusionSet, &DT, &LI))
      return false;
  return true;
}

BasicBlock *findColdRegionEntry(BasicBlock *Seed, const Loop *L,
                                 DominatorTree &DT, LoopInfo &LI) {
  BasicBlock *A = Seed;
  while (true) {
    DomTreeNode *Node = DT.getNode(A);
    DomTreeNode *IDomNode = Node ? Node->getIDom() : nullptr;
    BasicBlock *P = IDomNode ? IDomNode->getBlock() : nullptr;
    if (!P || !L->contains(P) || P == L->getHeader())
      break;
    if (!allSuccessorsCanReachSeed(P, Seed, L, DT, LI))
      break;
    A = P;
  }
  return A;
}

/// Collects every block of `L` dominated by `Entry`, excluding `L`'s own
/// header and latch (never extract the hot loop's own control skeleton --
/// see the call site for why). A self-contained sub-loop entirely inside
/// the slow path (its own header and latch included) is *not* excluded by
/// this: the check is specifically against the enclosing hot loop `L`, not
/// against whatever loop a given block might itself be the header of.
SmallVector<BasicBlock *, 8> collectColdRegion(BasicBlock *Entry, Loop *L,
                                                DominatorTree &DT) {
  SmallVector<BasicBlock *, 8> Region;
  for (BasicBlock *BB : L->blocks())
    if (BB != L->getHeader() && BB != L->getLoopLatch() &&
        DT.dominates(Entry, BB))
      Region.push_back(BB);
  return Region;
}

/// Attempts to outline the region rooted at `Entry`. Returns true if
/// extraction happened.
bool tryOutlineRegion(BasicBlock *Entry, Loop *L, DominatorTree &DT) {
  Function &F = *Entry->getParent();
  SmallVector<BasicBlock *, 8> Region = collectColdRegion(Entry, L, DT);
  if (Region.empty())
    return false;

  CodeExtractorAnalysisCache CEAC(F);
  CodeExtractor CE(Region, &DT, /*AggregateArgs=*/false);

  if (!CE.isEligible())
    return false;

  Function *NewF = CE.extractCodeRegion(CEAC);
  if (!NewF)
    return false;

  // extractCodeRegion leaves exactly one call to NewF behind, in the block
  // that replaced the extracted region.
  auto *CI = cast<CallInst>(*NewF->user_begin());
  markExtractedFunctionCold(F, *CI, *NewF);

  LLVM_DEBUG(dbgs() << "rust-hpc-cpo: outlined " << Region.size()
                     << " block(s) rooted at " << Entry->getName() << " into "
                     << NewF->getName() << '\n');
  ++NumColdRegionsOutlined;
  return true;
}

} // namespace

PreservedAnalyses
rust_hpc::ColdPathOutliningPass::run(Function &F,
                                      FunctionAnalysisManager &AM) {
  if (!EnableColdPathOutlining)
    return PreservedAnalyses::all();

  auto &LI = AM.getResult<LoopAnalysis>(F);
  if (LI.empty())
    return PreservedAnalyses::all();

  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);

  // Collect candidate region entries before mutating: extracting one region
  // doesn't change whether other, unrelated regions still qualify, but it's
  // simplest and safest to decide the whole candidate set against the
  // original CFG.
  SmallPtrSet<BasicBlock *, 16> Seen;
  SmallVector<std::pair<BasicBlock *, Loop *>, 8> CandidateEntries;
  for (Loop *L : LI.getLoopsInPreorder()) {
    // Loop::blocks() includes nested sub-loops' blocks, so the same block
    // can come up once per enclosing loop level; Seen keeps each block
    // considered as a *seed* at most once. (A block can still legitimately
    // appear in more than one extracted *region* if regions are processed
    // outermost-first and an inner one was already consumed -- handled by
    // re-checking Entry's parent function membership at extraction time via
    // CodeExtractor's own legality checks.)
    for (BasicBlock *BB : L->blocks()) {
      if (!Seen.insert(BB).second)
        continue;
      if (BB == L->getHeader() || BB == L->getLoopLatch())
        continue;

      bool IsSeed = any_of(*BB, [](Instruction &I) {
        auto *CI = dyn_cast<CallInst>(&I);
        if (!CI || CI->isIndirectCall())
          return false;
        Function *Callee = CI->getCalledFunction();
        return Callee && isColdOverheadCall(Callee->getName());
      });
      if (!IsSeed)
        continue;

      BasicBlock *Entry = findColdRegionEntry(BB, L, DT, LI);
      if (Entry == L->getHeader() || Entry == L->getLoopLatch())
        continue;
      CandidateEntries.emplace_back(Entry, L);
    }
  }

  // Multiple seeds in the same slow path (e.g. a TLS-miss block and the
  // fmt::write block it leads to) climb to the same Entry; only attempt
  // each entry once.
  SmallPtrSet<BasicBlock *, 8> ProcessedEntries;
  bool Changed = false;
  for (auto &[Entry, L] : CandidateEntries) {
    if (!ProcessedEntries.insert(Entry).second)
      continue;
    Changed |= tryOutlineRegion(Entry, L, DT);
  }

  if (!Changed)
    return PreservedAnalyses::all();

  return PreservedAnalyses::none();
}

void rust_hpc::registerColdPathOutliningPasses(PassBuilder &PB) {
  PB.registerPipelineParsingCallback(
      [](StringRef Name, FunctionPassManager &FPM,
         ArrayRef<PassBuilder::PipelineElement>) {
        if (Name == "rust-hpc-cold-path-outline") {
          FPM.addPass(rust_hpc::ColdPathOutliningPass());
          return true;
        }
        return false;
      });
}
