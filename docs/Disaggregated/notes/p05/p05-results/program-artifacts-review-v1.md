# p05-program-artifacts — adversarial review v1

Reviewed: tree `~/w7/p05-program-artifacts`, branch `p05/program-artifacts`, HEAD `77beb7d6` (3 commits on `6672778b`: `a180dc57`, `3ce1264c`, `77beb7d6`).
Against: `BRIEF-P05.md` (A, B.0, B.2, C, D) and `p05-results/program-artifacts-v1.md`.
Every command below was re-run by me in WSL Arch from the package tree (scripts `scratchpad/wf3/p05-review-pa/s1..s10.sh`, raw outputs under `~/w7/rv-pa/`). `~/w7/pipe` was used read-only (`ctest -N`, `nm`, one run of one existing integration binary). The tree was left exactly as found: `git status --short` (excluding `build-linux/`) empty before and after, HEAD unchanged, `~/w7/pipe` clean.

**Verdict: APPROVED — 0 majors, 6 minors.** The package is a pure move that meets the brief; every claim in the result file that matters reproduced, and every gate it adds goes red for its reason.

---

## 1. What I tried to refute, and what I found

### 1.1 "Pure move" — copied vs moved, verbatim bodies, semantics

| check | command | observed |
|---|---|---|
| Struct bodies verbatim | `git show 6672778b:ProgramObject.h` regions `:41-104`, `:1144-1171`, `:1173-1449` vs `git show a180dc57:ProgramArtifacts.h` `:28-400`, both with leading whitespace stripped and blank lines dropped, `diff` | 7 hunks, all declared: 5 comment lines `glslang::X` → `glslang X` (deviation 1 in the result file), the one added I5 sentence the brief itself prescribes (B.2 layout), and the closing `} // namespace` line. **No field added, removed, reordered, re-typed, no default changed.** |
| ODR: one definition per name | `grep -rn "^\s*struct \(TypeFacts\|ResourceReflection\|XfbVarying\|LinkArtifacts\|SpirvArtifacts\)\b" MobileGL` | exactly 5 hits, `ProgramArtifacts.h:31,63,95,160,359` |
| `kInvalidUniformOffset` one definition | `grep -rn "kInvalidUniformOffset\s*=" MobileGL` | `ProgramArtifacts.h:26` (`inline constexpr Uint = ~0u`) and `ProgramObject.h:496` defined *from* it; the old `= ~0u` in class is gone |
| Alias identity actually enforced | scratch farm `~/w7/rv-pa/neg4f`: replaced the `using TypeFacts = …` alias in a copy of `ProgramObject.h` with a second `struct TypeFacts {…}` body, compiled `ProgramArtifactsTest.cpp` by hand with the farm first on the include path | `ProgramArtifactsTest.cpp:29: error: static assertion failed … is_same_v<ProgramObject::TypeFacts, GLState::TypeFacts>` (plus `ProgramObject.h:464` no-viable-conversion). The copy-left-behind failure the brief's risk table names is caught at compile time. |
| Behaviour unchanged (strongest evidence) | own `symcmp.py` (`nm --defined-only -S` + `c++filt`, fold `GLState::ProgramObject::{5 names}` → `GLState::X`, multiset compare) on `~/w7/pipe/build-linux/libMobileGL.so` (built 22:06 at `6672778b`, commit time 22:13 — same tree state) vs `build-linux/libMobileGL.so` | `symbols 30511 -> 30511; 43 names folded on the before side, 0 on the after side; added 0 removed 0 resized 1`; `.text 10792579 -> 10792579`, `.rodata`, `.data`, `.bss`, `Total 17209819` all byte-identical. The 1 resized is `typeinfo name for std::_Sp_counted_ptr_inplace<…LinkArtifacts,…>` 122 → 107 bytes: the RTTI name *string* lost `13ProgramObject`. Fully attributed. Byte-identical `.text` also rules out any ADL/overload-resolution shift from de-nesting. |
| No mangled-name dependence anywhere | `grep -rn "typeid\|type_info\|__PRETTY_FUNCTION__\|__FUNCSIG__" MobileGL` (non-test) | empty |
| B.0 D5 (glslang members verbatim, count pinned) | `grep -o "glslang::" ProgramArtifacts.h \| wc -l` (occurrences, not lines) | `2` — `:165 SharedPtr<glslang::TProgram> program;`, `:232 Vector<glslang::TIntermediate::TUniformInitializer> uniformInitialValues;` |
| B.0 D6 (includes) | `grep -n "#include" ProgramArtifacts.h` | `:10 <Includes.h>`, `:15 <set>` — nothing else |
| B.0 D7 (sizes) | header `:179-203`; test prints `sizeof: TypeFacts=44 ResourceReflection=128 XfbVarying=128 LinkArtifacts=1056 SpirvArtifacts=88` | TypeFacts pinned unconditionally at 44 with `is_trivially_copyable_v`; the four container-bearing sizes under `__GLIBCXX__ && !_GLIBCXX_DEBUG && SIZE_MAX == UINT64_MAX`; libc++ `#elif` inert behind `MGL_ARTIFACT_SIZES_LIBCXX_PINNED`; MSVC unasserted. Exactly the brief's shape. |
| I5 untouched | `git diff --stat 6672778b..HEAD` | only 4 files: `ProgramArtifacts.h` (new), `ProgramObject.h`, `MG_Test/Program/CMakeLists.txt`, `ProgramArtifactsTest.cpp`. `MG_Backend/**`, `MG_Impl/**`, `ProgramObject.cpp`, `ProgramLinkTask.*`, `ProgramSpirvTask.*` untouched; `m_artifacts`/`m_spirv` and `Artifacts()`/`Spirv()` not in the diff. |
| Includers of `ProgramObject.h` | repo-wide grep (both spellings) | the 8 non-self includers the brief lists + `ProgramObject.cpp` + the new test; none edited; all compile through the in-class aliases. |

### 1.2 Tests exist, run under the right label, and can fail

| check | command | observed |
|---|---|---|
| Incremental build | `cmake --build build-linux -j 16` (`CCACHE_BASEDIR=/home/swung/w7`) | `ninja: no work to do.` |
| Unit suite | `ctest --test-dir build-linux -L unit --no-tests=error -j 12` | `100% tests passed, 0 tests failed out of 1463`; 3 `***Skipped` (`LogLevel.DebugBuildKeepsEverything`, `LogLevel.AssertIsLiveInDebugBuilds`, `XfbFrontendOrderInvarianceTest.DumpBSpirvForDisassembly`) — skip-by-design, same as base |
| No test name lost | `ctest -N` (pattern `'^\s+Test\s+#'` — the implementer is right that the brief's single-space pattern drops tests < #1000) on both trees, `comm -23 before after` | before 2326, after 2331, **lost: none**, added: exactly `ProgramArtifacts.{AliasesAreTheSameTypes,SizesArePinnedOnThisToolchain,TypeFactsIsPodOf44Bytes,VisitFieldsCoversEveryMember,VisitFieldsPassesConstnessThrough}` |
| Label reaches `ctest -L unit` | `ctest -L unit -N \| grep -c ProgramArtifacts` | `5` |
| CMake wiring executes | `MG_Test/CMakeLists.txt:85 add_subdirectory(Program)` unconditional; stanza `Program/CMakeLists.txt:280-297` = the `AsyncCompileTest` shape with `${LINK_LIBRARIES}`, `gtest_discover_tests(… DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)`; CI `test` job configures `-DMOBILEGL_BUILD_TEST=ON` (`test.yml:86`) and runs `ctest -L unit` | wired, discovered, runs |
| **Red-for-its-reason C1** — member added without a table entry | scratch copy `~/w7/rv-pa/neg1/…/ProgramArtifacts.h` with `Int addedWithoutATableEntry = 0;` appended to `LinkArtifacts`; `clang++ -I~/w7/rv-pa/neg1 -std=gnu++23 -Iinclude -IMobileGL … -fsyntax-only probe_pa.cpp` | exit 1: `error: static assertion failed … 'sizeof(LinkArtifacts) == 1056': LinkArtifacts changed size: add the field to VisitFields(LinkArtifacts) …` / `note: expression evaluates to '1064 == 1056'` |
| **Red-for-its-reason C2** — table entry dropped | symlink farm `~/w7/rv-pa/neg2f` (`cp -rs MobileGL`, header replaced by a copy minus `v("linkStatus", a.linkStatus);`); compiled `ProgramArtifactsTest.cpp` with its exact `compile_commands.json` line + `-I<farm>` first, linked with the ninja link line, ran | `ProgramArtifactsTest.cpp:84: Failure … Which is: 56 / Which is: 57`, plus `:110` (`linkStatus` write-through) fails; exit 1. Same hand pipeline on the **unmodified** header (`~/w7/rv-pa/pos2f`): `[ PASSED ] 2 tests` — so the failure is the dropped entry, not the pipeline. |
| **Red-for-its-reason C3** — purity | farm `~/w7/rv-pa/fake2` with `#include "ShaderObject.h"` inserted at header `:15`; (a) manual `-H` probe with the brief's grep; (b) the real gate from `~/w7/p05-include-gate/scripts/check_include_closure.py` copied under the farm's `scripts/` | (a) hits: `MobileGL/Config.h`, `MG_Backend/BackendObject.h`, `MG_Backend/BackendObjects.h`, `…/BackendObject_DirectGLES.h`, `…/BackendObject_DirectVulkan.h`, `BufferState/BufferObject.h`; (b) `artifacts-header FORBIDDEN 89 headers in closure, 16 forbidden (mode=text)` / `FORBIDDEN 678 headers, 16 forbidden (mode=clang)`, chain printed one hop per line, `::error::…`, exit 1 |
| Table completeness vs struct bodies (independent of the counting test) | python cross-check: member names parsed from each struct body vs `v("name", a.name)` entries | TypeFacts 20/20, ResourceReflection 14/14, XfbVarying 11/11, LinkArtifacts members 58 / table 57 (**only `program` missing**), SpirvArtifacts 8/8; every `"name"` string equals its member, every table in declaration order |

### 1.3 The closure gate, on the real tree

The gate lives in `p05/include-gate`; I ran that script (copied, unmodified) against this tree through a symlinked root `~/w7/rv-pa/fake/{MobileGL,include,3rdparty → tree}` so the tree itself was not touched:

```
include-closure: value-header SKIP  header not present yet: MobileGL/MG_Pipe/MGPipeValueTypes.h
include-closure: artifacts-header OK 65 headers in closure, 0 forbidden (mode=text, cxx=-)
include-closure: artifacts-header OK 653 headers in closure, 0 forbidden (mode=clang, cxx=/usr/sbin/clang++)
include-closure: artifacts-header SELF-CONTAINED OK (-fsyntax-only, cxx=/usr/sbin/clang++)
include-closure: wire-header OK …
include-closure:     self-test control-3 (ShaderObject.h added back to the artifacts header's TU) [text]: tripped, 16 violation(s) …  [clang]: tripped, 16 violation(s) …
include-closure:     self-test text limit: tripped as expected (glslang:: occurs 3 times, budget is 2)
include-closure: 3 probes, 1 skipped, 0 problem(s)        exit 0
--require-all: exit 1 (value-header absent — expected until p05/value-types lands)
--probe artifacts-header alone: OK/OK/SELF-CONTAINED OK, exit 0
```
Manual equivalent (brief B.2): `-H -E` probe → 653 closure lines, 0 matches for `ShaderObject.h|SpvcSession.h|MG_Util/ShaderTranspiler/|MobileGL/Config.h|MobileGL/MG_Backend/|BufferState/`; the only `MobileGL/` headers in the closure are `Defines.h`, `Includes.h`, `ProgramArtifacts.h`, `MG_Util/Debug/Log.h`, `MG_Util/GLExtensions.h`, `MG_Util/PlatformStubs.h`, `MG_Util/Types.h`. `-fsyntax-only` OK. `python3 scripts/gen_pipe.py --check` → `generated files are up to date`.

### 1.4 Lost transitive includes — TUs this configuration never compiled

`ProgramObject.h` sits behind `Core.h`, so commit 2 (dropping `SpvcSession.h`, which forwards `<spirv_reflect.h>` and `MG_Util/ShaderTranspiler/Types.h`) affects every `Core.h` consumer. The local build has `MOBILEGL_BUILD_BENCHMARK=OFF`; **CI's `test` job has it ON (`test.yml:87`)**, so the implementer's "no consumer needed an include" was proven only for the TUs in `compile_commands.json`. I enumerated every `.cpp` under `MobileGL/ tools/ android-plugin/` absent from `compile_commands.json` (48 files: benchmarks, MG_Remote transport, CGL/WGL/NSOpenGL, Wire tests, JNI, tools) and syntax-checked the ones that reach the program headers with the exact flag set of a compiled test TU plus `_deps/benchmark-src/include`:

```
OK   MobileGL/MG_Benchmark/Buffer/BufferBench.cpp
OK   MobileGL/MG_Benchmark/Container/UnorderedMapBench.cpp
OK   MobileGL/MG_Benchmark/Program/ProgramBench.cpp
OK   MobileGL/MG_Benchmark/SanityBench.cpp
OK   MobileGL/MG_Benchmark/ShaderCache/TranslationCacheBench.cpp
OK   MobileGL/MG_Benchmark/Transpile/TranspileProfile.cpp
OK   MobileGL/MG_Util/Converters/SPIRVCrossToGL/SpvcTypeConverter.cpp   (referenced by no CMakeLists; checked anyway)
```
Repo-wide, every TU that names `SpvReflect*`/`SpvcSession`/`SpvcMetadata` either includes `SpvcSession.h`/`spirv_reflect.h` itself or is in this build's compile set (and built). `MG_Test/Backend/DirectVulkan/SanityTest.cpp` fails only on `GLFW/glfw3.h` (not installed; unrelated). `DriverBenchJni.cpp` names none of those symbols. Conclusion: commit 2 is safe for the CI configuration as well; the binding check remains the integrator's `gh workflow run` (C.5).

### 1.5 The integration-gpu failure the result file reports

`DirectVulkan.IterationRPProgram203Scenario.FixedCompleteInputProducesFixedCompleteGoldenOutput`: one run each of the untouched base binary (`~/w7/pipe/build-linux/MobileGL/MG_IntegrationTest/MobileGLIntegrationTest`, DirectVulkan + lavapipe env as the ctest property injects) and the package's: **both `[ FAILED ]`**. Pre-existing on this machine, as the implementer's 3×3 A/B said; with byte-identical `.text` it cannot be this package.

---

## 2. Majors

None.

## 3. Minors (none blocks; listed for the integrator / a follow-up commit)

1. **File mode.** `MobileGL/MG_Test/Program/ProgramArtifactsTest.cpp` is committed as `100755`; its 15 siblings in that directory are `100644` (`git ls-files -s MobileGL/MG_Test/Program/`). Cosmetic; `git update-index --chmod=-x` in the integrator's merge or a follow-up.
2. **Test/header `#if` drift risk.** `ProgramArtifactsTest.cpp:297` re-spells the header's guard (`__GLIBCXX__ && !_GLIBCXX_DEBUG && SIZE_MAX == UINT64_MAX`) instead of `#ifdef MGL_LINKARTIFACTS_SIZE`. The brief literally asked for "under the same `#if`", so this is by the letter — but when C.4 pins the libc++ branch, the header will assert on Android while the runtime twin silently records `sizes_pinned: no` there. Recommend `#ifdef MGL_LINKARTIFACTS_SIZE` in the test (one-line change, integrator-owned since C.4 touches the header anyway).
3. **Macro leak.** `MGL_RESOURCEREFLECTION_SIZE`/`MGL_XFBVARYING_SIZE`/`MGL_LINKARTIFACTS_SIZE`/`MGL_SPIRVARTIFACTS_SIZE` (`ProgramArtifacts.h:187-190`) are `#define`d in a header reached by every `Core.h` consumer and never `#undef`'d. No collision today (`grep -rn MGL_.*_SIZE` shows only `MGL_RESIDUAL_BLOCK_SIZE`); the brief prescribed this shape. Note only.
4. **Transitive-include reliance inside the header.** `std::same_as` (`<concepts>`), `std::remove_const_t` (`<type_traits>`), `SIZE_MAX`/`UINT64_MAX` (`<cstdint>`) are not included by the header; D6 pins the include list, and the `-H` closure shows `concepts` and `cstdint` arrive via `<Includes.h>` on libstdc++ (`-dM -E` confirms `SIZE_MAX`, `UINT64_MAX` defined). Two things worth knowing: (a) if `SIZE_MAX`/`UINT64_MAX` were ever *not* defined at that point, the preprocessor evaluates `0 == 0` → true, i.e. the guard fails open rather than closed; `-Wundef` is not in the project flags. (b) libc++ and MSVC supply `std::same_as` through `<string>`/`<set>` in C++20 mode in practice. Not a defect under D6; a `<cstdint>`/`<concepts>` pair would cost nothing and is not forbidden by the gate — the plan owner's call.
5. **Result-file correction (deviation 6).** The implementer warns that the brief's C.4 recipe `--strip-scope 'MobileGL::MG_State::GLState::ProgramObject::'` would report "43 added / 43 removed". With the real `scripts/symbol_report.py` from `p05/include-gate` it does not: `--strip-scope 'MobileGL::MG_State::GLState::ProgramObject::'` → `27799 -> 27799 defined symbols: 0 added, 0 removed, 1 resized, 42 renamed`, identical to the `--rename-map` fold and to `--strip-scope 'ProgramObject::'`. The warning was true of the implementer's hand-rolled `symcmp.py`, not of the script the integrator will run. The C.4 recipe can be used as written; the expected line is `0 added, 0 removed, 1 resized, 42 renamed` with the one resized = the RTTI name string (`-15` bytes).
6. **Cosmetic.** `ProgramArtifacts.h:18` starts a sentence in lowercase ("…asserts that closure. the glslang scope token appears…").

## 4. Deviations declared by the implementer — verified

| # | claim | verdict |
|---|---|---|
| 1 | 5 comment respellings `glslang::X` → `glslang X` | exactly those 5 lines differ (§1.1 verbatim diff); no code token changed; needed because the gate counts occurrences in the whole file (the include-gate self-test confirms "budget is 2") |
| 2 | LinkArtifacts 57 of 58, not 52 of 53 | independent count 58 members (§1.2 cross-check); the brief's 53 was stale |
| 3 | `ctest -N` grep pattern | correct; used here |
| 4 | extra test `VisitFieldsPassesConstnessThrough` | additive; fails when the table is broken (§1.2 C2 also tripped it at `:110`) |
| 5 | CMake stanza appended at file end | content matches the `AsyncCompileTest` shape; wired (§1.2) |
| 6 | `symbol_report.py` `--strip-scope` warning | **incorrect for the real script** — see minor 5 |

## 5. Not verified here (by design)

- The libc++/NDK sizes (integrator C.4) and the MSVC build; nothing in the package is platform-specific beyond the per-STL `#if`.
- The full `integration-gpu` suite (only the reported failure was A/B'd, §1.5).
- CI dispatch of `test.yml` on `feat/disaggregated` (D10, integrator).

## 6. State of the trees after review

`~/w7/p05-program-artifacts`: HEAD `77beb7d6`, `git status --short` (minus `build-linux/`) empty, `build-linux/` rebuilt to no-op. `~/w7/pipe`: untouched, `6672778b`, clean. All scratch work under `~/w7/rv-pa/` (farms `fake`, `fake2`, `neg1`, `neg2f`, `pos2f`, `neg4f`, logs) and `scratchpad/wf3/p05-review-pa/`.
