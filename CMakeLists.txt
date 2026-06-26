cmake_minimum_required(VERSION 3.20)
project(RustHpcLLVMOpt LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(LLVM 22.1 REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "LLVM_DIR: ${LLVM_DIR}")

list(APPEND CMAKE_MODULE_PATH ${LLVM_CMAKE_DIR})
include(AddLLVM)
include(HandleLLVMOptions)

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})

# LLVMExports.cmake bakes in the DIA SDK path from whatever VS edition/path
# built this LLVM distribution (VS Enterprise), which doesn't match our local
# VS Community install. Repoint LLVMDebugInfoPDB's link interface at our
# local diaguids.lib rather than patching the installed cmake files.
if(WIN32 AND TARGET LLVMDebugInfoPDB)
  get_target_property(_dipdb_libs LLVMDebugInfoPDB INTERFACE_LINK_LIBRARIES)
  set(_local_diaguids "D:/Microsoft Visual Studio/2022/Community/DIA SDK/lib/amd64/diaguids.lib")
  if(_dipdb_libs AND EXISTS "${_local_diaguids}")
    list(TRANSFORM _dipdb_libs REPLACE
         "^.*diaguids\\.lib$" "${_local_diaguids}")
    set_target_properties(LLVMDebugInfoPDB PROPERTIES
      INTERFACE_LINK_LIBRARIES "${_dipdb_libs}")
  endif()
endif()

# This distribution of LLVM is a static-library package built with
# LLVM_ENABLE_PLUGINS=OFF / LLVM_EXPORT_SYMBOLS_FOR_PLUGINS=OFF, so the host
# tools (opt.exe/clang.exe) do not export the symbols a loadable plugin would
# normally resolve against on Windows. The plugin therefore links its own
# copy of the needed static LLVM component libraries; it talks to the host
# only through the PassBuilder/PassPlugin C++ ABI passed in at load time, so
# duplicate static globals in the plugin's copy do not collide with the host.
set(RUST_HPC_LLVM_COMPONENTS Core Support TransformUtils Analysis Demangle)
llvm_map_components_to_libnames(RUST_HPC_LLVM_LIBS ${RUST_HPC_LLVM_COMPONENTS})

add_subdirectory(passes)

enable_testing()
add_subdirectory(test)
