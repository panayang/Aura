//===- ColdPathOutlining.h ------------------------------------*- C++ -*-===//
//
// Pass #2 of rust-hpc-opt: pull calls to known synchronization/IO/formatting
// "overhead" machinery (println!/write! internals, Mutex/RwLock/OnceLock
// runtime, thread-id TLS slow path, Win32 sync primitives, panic
// formatting) out of hot loops and into a separate, explicitly cold,
// noinline function, so the bulk of that machinery's code stops occupying
// space in the hot loop's instruction stream and stops being something the
// vectorizer/register allocator has to reason about. See the .cpp for why
// this is implemented on top of llvm::CodeExtractor rather than as a
// from-scratch region-outlining pass.
//
//===----------------------------------------------------------------------===//

#ifndef RUST_HPC_PASSES_COLDPATHOUTLINING_H
#define RUST_HPC_PASSES_COLDPATHOUTLINING_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class PassBuilder;
}

namespace rust_hpc {

class ColdPathOutliningPass
    : public llvm::PassInfoMixin<ColdPathOutliningPass> {
public:
  llvm::PreservedAnalyses run(llvm::Function &F,
                               llvm::FunctionAnalysisManager &AM);
};

void registerColdPathOutliningPasses(llvm::PassBuilder &PB);

} // namespace rust_hpc

#endif // RUST_HPC_PASSES_COLDPATHOUTLINING_H
