//===- BoundsCheckElimination.h ------------------------------*- C++ -*-===//
//
// Pass #3 of rust-hpc-opt: structurally identify the IR shape rustc emits
// for a slice/array bounds check (a guard branch to a block that calls
// `core::panicking::panic_bounds_check` and never returns) and remove the
// guard, under an explicit opt-in flag. This is *not* a general "delete
// calls to panic" pass -- see the .cpp for the verification this performs
// before touching anything, and why blind name/pattern matching on its own
// would not be enough to do this safely.
//
//===----------------------------------------------------------------------===//

#ifndef RUST_HPC_PASSES_BOUNDSCHECKELIMINATION_H
#define RUST_HPC_PASSES_BOUNDSCHECKELIMINATION_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class PassBuilder;
}

namespace rust_hpc {

class BoundsCheckEliminationPass
    : public llvm::PassInfoMixin<BoundsCheckEliminationPass> {
public:
  llvm::PreservedAnalyses run(llvm::Function &F,
                               llvm::FunctionAnalysisManager &AM);
};

void registerBoundsCheckEliminationPasses(llvm::PassBuilder &PB);

} // namespace rust_hpc

#endif // RUST_HPC_PASSES_BOUNDSCHECKELIMINATION_H
