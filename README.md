# rust-hpc-opt

Out-of-tree LLVM passes targeting Rust HPC code, built against LLVM 22.1.5.
The motivating case (`output.s`, a Binja HLIL decompile) is an `ndarray::Zip`
closure where the borrow checker's conservatism forces heap allocations for
small, provably-local scratch buffers, and where `println!`/`Mutex`/thread-id
machinery pulled in incidentally ends up inlined directly into the hot loop.

Four passes address that, in dependency order (each builds on the IR being
cleaner after the previous one runs):

1. **HeapToStackPromotion** -- replaces non-escaping, bounded-size
   heap allocations with a stack `alloca`, eliminating the alloc/dealloc
   calls entirely.
2. **BoundsCheckElimination** -- removes a Rust bounds-check guard under an
   explicit opt-in flag, with no semantic ambiguity about which guard it's
   removing.
3. **ColdPathOutlining** -- pulls synchronization/IO/formatting overhead
   (println!, Mutex/OnceLock runtime, panic formatting) out of hot loops
   into a separate, explicitly cold function.
4. **GEPStrengthReduction** -- turns recomputed `Base + Stride*Index`
   address arithmetic into a loop-carried pointer increment.

See each pass's `.cpp` file header for the full soundness argument; this
README covers building, running, and the project's non-obvious environment
constraints.

## Why this needs a custom driver, not `opt -load-pass-plugin`

This project's LLVM distribution (`D:\dev\llvm`, referenced by `LLVM_DIR`
below) was packaged with `LLVM_ENABLE_PLUGINS=OFF` and
`LLVM_EXPORT_SYMBOLS_FOR_PLUGINS=OFF`. Loading a plugin DLL built against it
into `opt.exe` **segfaults** on the first `AnalysisManager::getResult<...>()`
call: the plugin statically links its own copy of LLVM (the host doesn't
export symbols to link against instead), so `AnalysisKey` identity --
an in-process address per analysis type -- diverges between the host's copy
of the template machinery and the plugin's, and the host's
`FunctionAnalysisManager` can't find results keyed by the plugin's copy of
the key. This is reproducible, not theoretical.

The fix: **`RustHpcOpt`**, a standalone driver (`passes/tools/RustHpcOpt.cpp`)
that links the passes and LLVM into one binary, the same way Polly's
`LINK_INTO_TOOLS` mode avoids the same class of problem. Use it instead of
`opt -load-pass-plugin=...` against this LLVM build. (`RustHpcPasses.dll`,
the loadable-plugin target, is still built and kept in the tree for a future
LLVM built with `LLVM_EXPORT_SYMBOLS_FOR_PLUGINS=ON`, where it would work --
just not against today's distribution.)

## Building

Requires the VS2022 dev shell (for `clang-cl`/`lld-link` to resolve the
MSVC-ABI toolchain) and this repo's LLVM distribution. From PowerShell:

```powershell
$vsPath = "D:\Microsoft Visual Studio\2022\Community"
Import-Module "$vsPath\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64"

cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DCMAKE_C_COMPILER=D:/dev/llvm/bin/clang-cl.exe `
  -DCMAKE_CXX_COMPILER=D:/dev/llvm/bin/clang-cl.exe `
  -DCMAKE_LINKER=D:/dev/llvm/bin/lld-link.exe `
  -DLLVM_DIR=D:/dev/llvm/lib/cmake/llvm `
  -DLLVM_PROJECT_SRC_FILECHECK_DIR=D:/dev/llvm-project-22.1.5.src/llvm/utils/FileCheck

ninja -C build
ctest --test-dir build --output-on-failure
```

`LLVM_PROJECT_SRC_FILECHECK_DIR` is optional but needed for `ctest`: this
LLVM distribution doesn't ship `FileCheck.exe` (it's a release package, not
a build tree), so `test/CMakeLists.txt` builds one from source against the
distribution's own `LLVMFileCheck.lib`/`LLVMSupport.lib` instead of pulling
in the full `llvm-lit` test-utilities tree.

Two notable build-environment fixes already applied in the top-level
`CMakeLists.txt`, documented there in more detail:
- `find_package(LLVM 22.1 ...)`, not bare `22`: `LLVMConfigVersion.cmake`
  requires an exact major.minor match against the requested version.
- `LLVMDebugInfoPDB`'s link interface is patched at configure time to point
  at this machine's local `diaguids.lib`: the installed `LLVMExports.cmake`
  hardcodes the DIA SDK path from whatever VS edition built this LLVM
  distribution (Enterprise), which doesn't exist locally (Community).

## Running a pass

```
RustHpcOpt.exe -passes="function(<pass-name>)" -S input.ll -o -
```

Pass names and their gating flags:

| Pass | `-passes=` name | Gating |
|---|---|---|
| HeapToStackPromotion | `rust-hpc-heap-to-stack-promote` | on by default; `-rust-hpc-heap-to-stack-promote=0` to disable |
| BoundsCheckElimination | `rust-hpc-elim-bounds-checks` | **off** by default; `-rust-hpc-trust-bounds-checks` to enable |
| ColdPathOutlining | `rust-hpc-cold-path-outline` | on by default; `-rust-hpc-cold-path-outline=0` to disable |
| GEPStrengthReduction | `rust-hpc-gep-strength-reduce` | always runs when included in `-passes=` |

`HeapToStackPromotion` also takes `-rust-hpc-max-promotable-alloca-size`
(default 8192 bytes) capping how large an allocation it will move to the
stack.

To run the full intended pipeline on one module:

```
RustHpcOpt.exe -passes="function(rust-hpc-heap-to-stack-promote,rust-hpc-cold-path-outline,rust-hpc-gep-strength-reduce)" -S input.ll -o output.ll
```

`rust-hpc-elim-bounds-checks` is left out of that example deliberately --
add it explicitly, with `-rust-hpc-trust-bounds-checks`, only once you've
confirmed the indices involved really are always in range; see its
soundness note below.

## Testing

No `llvm-lit`/`FileCheck.exe` ship with this LLVM distribution, so
`test/run_ll_test.cmake` is a small lit-RUN-line equivalent: for each `.ll`
file, it runs `RustHpcOpt -passes=<...>` (plus any `EXTRA_ARGS` from
`test/CMakeLists.txt`, e.g. a gating flag) and pipes the output into
`FileCheck`, registered as a CTest case. Run all of them with
`ctest --test-dir build --output-on-failure`; 22 tests as of this writing,
covering both the positive transform shape and the negative cases each
pass's soundness argument depends on (escapes, multiple dealloc sites, a
hot preamble block that must *not* get swept into a cold region, etc.) --
see `test/*.ll` for the rationale behind each one, written as a comment at
the top of the file.

## Soundness notes worth reading before trusting a pass on new code

- **HeapToStackPromotion** reuses `llvm::PostDominatorTree`-style forward
  reachability, `llvm::PointerMayBeCapturedBefore`/`PointerMayBeCaptured`,
  and `ScalarEvolution::getUnsignedRangeMax` rather than hand-rolling escape
  or range analysis. It promotes constant-size allocations (hoisting the
  `alloca` to the entry block, reused across loop iterations) and
  ScalarEvolution-bounded non-constant sizes (bracketed with
  `llvm.stacksave`/`llvm.stackrestore` per iteration if the alloc is inside
  a loop, the same fix Clang uses for a C VLA declared inside a loop body).
  It accepts a dealloc call whose pointer argument is the alloc's SSA value
  modulo bitcasts/GEPs, *or* reloaded from a local temporary verified (not
  assumed) to have exactly one store in the whole function, dominating the
  reload, storing the alloc's value.
- **BoundsCheckElimination** is the one pass that changes program
  semantics on purpose (removing a real safety check) and is off by
  default for that reason. It only matches a structurally-verified guard
  (single-predecessor block, `noreturn` call, `unreachable` terminator)
  to a demangled-name allowlist entry for `core::panicking::panic_bounds_check`
  specifically, cross-checked against the icmp condition sharing operands
  with the panic call's own arguments -- never a generic "this branch leads
  to a panic" heuristic. Verify the indices involved are genuinely always
  in range before enabling it for a given crate.
- **ColdPathOutlining** is semantics-preserving (`llvm::CodeExtractor`
  handles the actual code motion, the same utility `-hotcoldsplit` uses);
  its only judgment call is *what* counts as overhead worth outlining, via
  a demangled-name allowlist used only to find a seed instruction, with the
  actual region grown structurally via the dominator tree and a
  loop-back-edge-excluded reachability check (so it doesn't accidentally
  sweep ordinary hot setup code into the outlined region just because it
  sits on the same dominator-tree path as the real fork point).
- **GEPStrengthReduction** delegates the actual rewrite to
  `llvm::SCEVExpander` in non-canonical mode (matching
  `LoopStrengthReduce.cpp`'s own usage), after ScalarEvolution proves the
  address is an affine recurrence of the enclosing loop; this is what makes
  it handle multi-index GEPs and non-canonical/nested induction variables
  uniformly instead of needing separate matching code for each IR shape.

## Known limitations (by design, not oversight)

- HeapToStackPromotion's reload-from-local-temporary match requires
  *exactly one* store to that temporary anywhere in the function; more than
  one is left on the heap rather than risk reasoning about which store
  reaches a given load without real memory-SSA analysis.
- ColdPathOutlining only finds a region by climbing from one seed call at a
  time; it doesn't yet merge regions found from different seeds that turn
  out to be nested inside each other, and doesn't use LLVM's
  RegionInfo/Program Structure Tree, which would let it characterize a
  region's boundary more precisely than the current dominator-tree-climb-
  plus-reachability-check heuristic.
- GEPStrengthReduction's profitability check (`hasLoopVariantIndex`) doesn't
  attempt to estimate whether strength-reducing a given GEP is actually a
  net win versus what LSR would later do anyway; it strength-reduces every
  affine candidate unconditionally.
