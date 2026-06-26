# Minimal lit-RUN-line equivalent for a single test:
#   RustHpcOpt -passes=<passes> -S <input> | FileCheck <input>
#
# We use our own standalone driver, not `opt -load-pass-plugin=...`: this
# LLVM distribution was packaged with LLVM_EXPORT_SYMBOLS_FOR_PLUGINS=OFF, so
# a loaded plugin DLL ends up with its own address-distinct copy of
# AnalysisManager's template machinery and segfaults on the first
# AM.getResult<...>() call. See passes/CMakeLists.txt for the full
# explanation; RustHpcOpt links the passes and LLVM into one binary instead.
if(NOT DEFINED EXTRA_ARGS)
  set(EXTRA_ARGS "")
endif()
separate_arguments(EXTRA_ARGS_LIST UNIX_COMMAND "${EXTRA_ARGS}")

execute_process(
  COMMAND "${OPT_EXE}" -passes=${PASSES} ${EXTRA_ARGS_LIST} -S ${INPUT}
  OUTPUT_VARIABLE OPT_OUTPUT
  RESULT_VARIABLE OPT_RESULT
  ERROR_VARIABLE OPT_ERROR
)
if(NOT OPT_RESULT EQUAL 0)
  message(FATAL_ERROR "RustHpcOpt failed (${OPT_RESULT}):\n${OPT_ERROR}")
endif()

# CMake's execute_process can't pipe a variable directly into a child's
# stdin; INPUT_FILE only accepts a real file, so write opt's output to a
# temp file first.
set(TMP_LL "${INPUT}.opt-out.tmp.ll")
file(WRITE "${TMP_LL}" "${OPT_OUTPUT}")

execute_process(
  COMMAND "${FILECHECK_EXE}" ${INPUT}
  INPUT_FILE "${TMP_LL}"
  RESULT_VARIABLE FC_RESULT2
  ERROR_VARIABLE FC_ERROR2
)
file(REMOVE "${TMP_LL}")

if(NOT FC_RESULT2 EQUAL 0)
  message(FATAL_ERROR "FileCheck failed:\n${FC_ERROR2}\n--- opt output ---\n${OPT_OUTPUT}")
endif()
