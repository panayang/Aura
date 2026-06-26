# The pass implementations are shared between two consumers:
#  - RustHpcPasses: a loadable `-load-pass-plugin` DLL, for use with an LLVM
#    build that has LLVM_EXPORT_SYMBOLS_FOR_PLUGINS=ON (e.g. a self-built
#    LLVM). Kept for that future case; do not use it against this repo's
#    distributed LLVM, see RustHpcOpt below for why.
#  - RustHpcOpt: a standalone driver that statically links the same passes
#    into one binary together with LLVM itself. This distribution's LLVM was
#    packaged with LLVM_ENABLE_PLUGINS=OFF and
#    LLVM_EXPORT_SYMBOLS_FOR_PLUGINS=OFF, so opt.exe does not export the
#    symbols a loaded plugin DLL needs. Worse, a plugin DLL that statically
#    links its own copy of LLVM ends up with a *second*, address-distinct
#    instantiation of AnalysisManager's template machinery: AnalysisKey
#    identity is the in-process address of a static per-analysis-type
#    object, so the host's FunctionAnalysisManager (populated using the
#    host's copy of e.g. LoopAnalysis::ID()) cannot find results requested
#    via the plugin's copy of the same template, and getResult() dereferences
#    a missing entry -> segfault. This is reproducible here even for a
#    no-op pass. RustHpcOpt avoids the whole problem by never crossing a
#    module boundary: passes, PassBuilder, and all AnalysisManagers live in
#    one binary.
add_library(RustHpcPassesCore STATIC
  GEPStrengthReduction/GEPStrengthReduction.cpp
  BoundsCheckElimination/BoundsCheckElimination.cpp
  ColdPathOutlining/ColdPathOutlining.cpp
  HeapToStackPromotion/HeapToStackPromotion.cpp
)
target_include_directories(RustHpcPassesCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(RustHpcPassesCore PUBLIC ${RUST_HPC_LLVM_LIBS})

add_library(RustHpcPasses MODULE Plugin.cpp)
target_link_libraries(RustHpcPasses PRIVATE RustHpcPassesCore)
if(WIN32)
  set_target_properties(RustHpcPasses PROPERTIES SUFFIX ".dll" PREFIX "")
  target_compile_definitions(RustHpcPasses PRIVATE RUST_HPC_BUILDING_PLUGIN_DLL)
endif()
set_target_properties(RustHpcPasses PROPERTIES CXX_VISIBILITY_PRESET hidden)

add_subdirectory(tools)
