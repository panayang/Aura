//===- RustHpcOpt.cpp - standalone driver for rust-hpc-opt passes -------===//
//
// A minimal `opt`-equivalent that links this project's passes directly into
// the same binary as LLVM itself, rather than loading them as a plugin DLL.
// See passes/CMakeLists.txt for why that's necessary against this LLVM
// distribution. Usage mirrors the subset of `opt` we need:
//
//   RustHpcOpt -passes=<pipeline-text> [-S] [-o <output>] <input.ll|.bc>
//
//===----------------------------------------------------------------------===//

#include "BoundsCheckElimination/BoundsCheckElimination.h"
#include "ColdPathOutlining/ColdPathOutlining.h"
#include "GEPStrengthReduction/GEPStrengthReduction.h"
#include "HeapToStackPromotion/HeapToStackPromotion.h"

#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ToolOutputFile.h"

using namespace llvm;

namespace {
cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input .ll/.bc>"),
                                    cl::Required);
cl::opt<std::string> OutputFilename("o", cl::desc("Output filename"),
                                     cl::value_desc("filename"),
                                     cl::init("-"));
cl::opt<std::string>
    PassPipeline("passes",
                 cl::desc("New-PM pass pipeline text, e.g. "
                          "'function(rust-hpc-gep-strength-reduce)'"),
                 cl::Required);
cl::opt<bool> EmitTextual("S", cl::desc("Emit textual IR instead of bitcode"));
} // namespace

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv, "rust-hpc-opt standalone driver\n");

  LLVMContext Context;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }
  if (verifyModule(*M, &errs())) {
    errs() << argv[0] << ": input module is invalid\n";
    return 1;
  }

  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;

  PassBuilder PB;
  rust_hpc::registerGEPStrengthReductionPasses(PB);
  rust_hpc::registerBoundsCheckEliminationPasses(PB);
  rust_hpc::registerColdPathOutliningPasses(PB);
  rust_hpc::registerHeapToStackPromotionPasses(PB);

  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  ModulePassManager MPM;
  if (auto Err2 = PB.parsePassPipeline(MPM, PassPipeline)) {
    errs() << argv[0] << ": " << toString(std::move(Err2)) << '\n';
    return 1;
  }

  MPM.run(*M, MAM);

  if (verifyModule(*M, &errs())) {
    errs() << argv[0] << ": pass pipeline produced an invalid module\n";
    return 1;
  }

  std::error_code EC;
  ToolOutputFile Out(OutputFilename, EC,
                      EmitTextual ? sys::fs::OF_Text : sys::fs::OF_None);
  if (EC) {
    errs() << argv[0] << ": " << EC.message() << '\n';
    return 1;
  }

  if (EmitTextual)
    M->print(Out.os(), nullptr);
  else
    WriteBitcodeToFile(*M, Out.os());

  Out.keep();
  return 0;
}
