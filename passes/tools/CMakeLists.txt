add_executable(RustHpcOpt RustHpcOpt.cpp)
target_link_libraries(RustHpcOpt PRIVATE RustHpcPassesCore)

llvm_map_components_to_libnames(RUST_HPC_OPT_EXTRA_LIBS
  IRReader
  BitWriter
  AsmParser
  Passes
  ScalarOpts
  InstCombine
  AggressiveInstCombine
  Vectorize
  ipo
  ObjCARCOpts
  Coroutines
  CFGuard
  Instrumentation
)
target_link_libraries(RustHpcOpt PRIVATE ${RUST_HPC_OPT_EXTRA_LIBS})
