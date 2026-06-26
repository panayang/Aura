//===- GEPStrengthReduction.h ---------------------------------*- C++ -*-===//
//
// Pass #4 of rust-hpc-opt: replace dynamically recomputed address arithmetic
// (Base + Stride * InductionVar, expressed in IR as a `mul` inside the loop
// feeding a `getelementptr`) with a strength-reduced, loop-carried pointer
// increment, in cases where ScalarEvolution can prove the index is an affine
// recurrence of the enclosing loop.
//
//===----------------------------------------------------------------------===//

#ifndef RUST_HPC_PASSES_GEPSTRENGTHREDUCTION_H
#define RUST_HPC_PASSES_GEPSTRENGTHREDUCTION_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class PassBuilder;
}

namespace rust_hpc {

class GEPStrengthReductionPass
    : public llvm::PassInfoMixin<GEPStrengthReductionPass> {
public:
  llvm::PreservedAnalyses run(llvm::Function &F,
                               llvm::FunctionAnalysisManager &AM);
};

/// Registers this pass (and any sibling passes added to this file group)
/// with the given PassBuilder, both under an explicit `-passes=` name and as
/// part of the relevant default pipeline extension points.
void registerGEPStrengthReductionPasses(llvm::PassBuilder &PB);

} // namespace rust_hpc

#endif // RUST_HPC_PASSES_GEPSTRENGTHREDUCTION_H
