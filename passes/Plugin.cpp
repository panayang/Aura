//===- Plugin.cpp - rust-hpc-opt pass plugin entry point ----------------===//
//
// Single registration point for all rust-hpc-opt passes. Each pass lives in
// its own subdirectory and exposes a `register*Passes(PassBuilder &)`
// function declared in its own header; this file just wires them all into
// one PassPluginLibraryInfo so the whole set loads via one
// `-load-pass-plugin=RustHpcPasses.dll`.
//
//===----------------------------------------------------------------------===//

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"

#include "BoundsCheckElimination/BoundsCheckElimination.h"
#include "ColdPathOutlining/ColdPathOutlining.h"
#include "GEPStrengthReduction/GEPStrengthReduction.h"
#include "HeapToStackPromotion/HeapToStackPromotion.h"

using namespace llvm;

namespace {
void registerCallbacks(PassBuilder &PB) {
  rust_hpc::registerGEPStrengthReductionPasses(PB);
  rust_hpc::registerBoundsCheckEliminationPasses(PB);
  rust_hpc::registerColdPathOutliningPasses(PB);
  rust_hpc::registerHeapToStackPromotionPasses(PB);
}
} // namespace

#if defined(_WIN32) && defined(RUST_HPC_BUILDING_PLUGIN_DLL)
#define RUST_HPC_PLUGIN_EXPORT __declspec(dllexport)
#else
#define RUST_HPC_PLUGIN_EXPORT
#endif

extern "C" RUST_HPC_PLUGIN_EXPORT ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RustHpcPasses", "0.1", registerCallbacks};
}
