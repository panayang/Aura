# We don't have llvm-lit in this binary LLVM distribution, and building the
# full lit test-utils tree is out of scope for an out-of-tree plugin. Instead:
#  - Build FileCheck.exe directly from the LLVM source tree against the
#    LLVMFileCheck static library that does ship in this distribution.
#  - Drive each .ll test with a tiny CMake/CTest wrapper that runs
#    `RustHpcOpt -passes=... -S` and pipes it into FileCheck, mirroring what
#    lit would do for a single RUN line. We use our own RustHpcOpt driver
#    rather than `opt -load-pass-plugin=...`; see passes/CMakeLists.txt for
#    why the latter segfaults against this LLVM distribution.

set(LLVM_PROJECT_SRC_FILECHECK_DIR "" CACHE PATH
    "Path to llvm-project/llvm/utils/FileCheck (FileCheck.cpp), used to build a local FileCheck.exe")

if(LLVM_PROJECT_SRC_FILECHECK_DIR AND EXISTS "${LLVM_PROJECT_SRC_FILECHECK_DIR}/FileCheck.cpp")
  add_executable(FileCheck "${LLVM_PROJECT_SRC_FILECHECK_DIR}/FileCheck.cpp")
  llvm_map_components_to_libnames(FILECHECK_LLVM_LIBS FileCheck Support)
  target_link_libraries(FileCheck PRIVATE ${FILECHECK_LLVM_LIBS})
else()
  message(WARNING
    "LLVM_PROJECT_SRC_FILECHECK_DIR not set/found; 'FileCheck' target and "
    "tests will be unavailable. Configure with "
    "-DLLVM_PROJECT_SRC_FILECHECK_DIR=<llvm-project>/llvm/utils/FileCheck")
endif()

function(add_rust_hpc_ll_test name passes)
  cmake_parse_arguments(ARG "" "EXTRA_ARGS" "" ${ARGN})
  if(NOT TARGET FileCheck)
    return()
  endif()
  add_test(
    NAME ${name}
    COMMAND ${CMAKE_COMMAND}
            -DOPT_EXE=$<TARGET_FILE:RustHpcOpt>
            -DFILECHECK_EXE=$<TARGET_FILE:FileCheck>
            -DPASSES=${passes}
            -DEXTRA_ARGS=${ARG_EXTRA_ARGS}
            -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/${name}.ll
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_ll_test.cmake
  )
endfunction()

add_rust_hpc_ll_test(gep-strength-reduce "function(rust-hpc-gep-strength-reduce)")
add_rust_hpc_ll_test(gep-strength-reduce-multi-index "function(rust-hpc-gep-strength-reduce)")
add_rust_hpc_ll_test(gep-strength-reduce-nested-loop "function(rust-hpc-gep-strength-reduce)")
add_rust_hpc_ll_test(gep-strength-reduce-noncanonical-iv "function(rust-hpc-gep-strength-reduce)")

add_rust_hpc_ll_test(bounds-check-elim-basic "function(rust-hpc-elim-bounds-checks)"
  EXTRA_ARGS "-rust-hpc-trust-bounds-checks")
add_rust_hpc_ll_test(bounds-check-elim-inverted-branch "function(rust-hpc-elim-bounds-checks)"
  EXTRA_ARGS "-rust-hpc-trust-bounds-checks")
add_rust_hpc_ll_test(bounds-check-elim-disabled-by-default "function(rust-hpc-elim-bounds-checks)")
add_rust_hpc_ll_test(bounds-check-elim-ignores-user-panic "function(rust-hpc-elim-bounds-checks)"
  EXTRA_ARGS "-rust-hpc-trust-bounds-checks")

add_rust_hpc_ll_test(cold-path-outline-basic "function(rust-hpc-cold-path-outline)")
add_rust_hpc_ll_test(cold-path-outline-no-loop "function(rust-hpc-cold-path-outline)")
add_rust_hpc_ll_test(cold-path-outline-disabled "function(rust-hpc-cold-path-outline)"
  EXTRA_ARGS "-rust-hpc-cold-path-outline=0")
add_rust_hpc_ll_test(cold-path-outline-multi-block "function(rust-hpc-cold-path-outline)")
add_rust_hpc_ll_test(cold-path-outline-hot-preamble "function(rust-hpc-cold-path-outline)")

add_rust_hpc_ll_test(heap-to-stack-basic "function(rust-hpc-heap-to-stack-promote)")
add_rust_hpc_ll_test(heap-to-stack-zeroed-in-loop "function(rust-hpc-heap-to-stack-promote)")
add_rust_hpc_ll_test(heap-to-stack-escapes "function(rust-hpc-heap-to-stack-promote)")
add_rust_hpc_ll_test(heap-to-stack-multi-dealloc "function(rust-hpc-heap-to-stack-promote)")
add_rust_hpc_ll_test(heap-to-stack-dynamic-size "function(rust-hpc-heap-to-stack-promote)")
add_rust_hpc_ll_test(heap-to-stack-too-large "function(rust-hpc-heap-to-stack-promote)")
add_rust_hpc_ll_test(heap-to-stack-bounded-dynamic-size "function(rust-hpc-heap-to-stack-promote)")
add_rust_hpc_ll_test(heap-to-stack-bounded-dynamic-size-in-loop "function(rust-hpc-heap-to-stack-promote)")
add_rust_hpc_ll_test(heap-to-stack-relaxed-ssa "function(rust-hpc-heap-to-stack-promote)")
