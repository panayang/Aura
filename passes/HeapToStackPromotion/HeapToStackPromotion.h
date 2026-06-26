//===- HeapToStackPromotion.h ---------------------------------*- C++ -*-===//
//
// Pass #1 of rust-hpc-opt: the primary motivation for this whole project --
// Rust's borrow checker forces a heap allocation (Box/Vec/RawVec, routed
// through `__rust_alloc`/the platform allocator) in plenty of cases where
// the allocated buffer provably never leaves the function and has a
// compile-time-known size, e.g. a scratch buffer inside an `ndarray::Zip`
// closure. This pass identifies exactly those cases and replaces the
// alloc/dealloc pair with a single stack `alloca`, eliminating both calls
// and the heap traffic entirely. See the .cpp for the soundness conditions
// this checks before doing so.
//
//===----------------------------------------------------------------------===//

#ifndef RUST_HPC_PASSES_HEAPTOSTACKPROMOTION_H
#define RUST_HPC_PASSES_HEAPTOSTACKPROMOTION_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class PassBuilder;
}

namespace rust_hpc {

class HeapToStackPromotionPass
    : public llvm::PassInfoMixin<HeapToStackPromotionPass> {
public:
  llvm::PreservedAnalyses run(llvm::Function &F,
                               llvm::FunctionAnalysisManager &AM);
};

void registerHeapToStackPromotionPasses(llvm::PassBuilder &PB);

} // namespace rust_hpc

#endif // RUST_HPC_PASSES_HEAPTOSTACKPROMOTION_H
