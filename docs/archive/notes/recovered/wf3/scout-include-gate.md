# P0.5 CI include-closure gate — scout report

Tree scouted: `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`, branch `feat/disaggregated`.
Every path below is repo-relative to that root unless written absolute. Every line number was opened.

Live measurements in section 5 were run in WSL against `/home/swung/w7/pipe` (the same branch, `a4baa9d4`, 12 commits ahead of the Windows worktree) because it has configured build trees. All of the headers involved are byte-identical in the two worktrees for the purposes of these measurements; re-run the commands after rebasing if you need exact numbers for a specific commit.

---

## 0. Two naming corrections before anything else

The task brief names two headers as if they exist. **Neither exists in the tree today** — P0.5 *creates* them:

- `MobileGL/MG_Pipe/MGPipeValueTypes.h` — does not exist. `find MobileGL -iname "*ValueType*"` returns nothing. The real, existing header is `MobileGL/MG_Pipe/MGPipeTypes.h`.
- `MobileGL/MG_State/GLState/ProgramState/ProgramArtifacts.h` — does not exist. `ls MobileGL/MG_State/GLState/ProgramState/` shows: `ProgramLinkTask.{cpp,h}`, `ProgramObject.{cpp,h}`, `ProgramPipelineObject.h`, `ProgramSpirvTask.{cpp,h}`, `ProgramState.{cpp,h}`, `ProgramTranslationCache.{cpp,h}`, `ShaderCompileAdoptionMap.{cpp,h}`, `ShaderCompileTask.{cpp,h}`, `ShaderObject.{cpp,h}`, `ShaderPreprocessCache.{cpp,h}`, `ShaderSourceKey.h`, `ShaderStage.h`. The types to extract live in `ProgramObject.h`.

So the gate is written **against headers that P0.5 introduces**. It must therefore be able to run in a "not yet extracted" state without crashing the CI for a missing file — decide explicitly: either land the gate in the same commit as the headers, or give the script a `--allow-missing` that prints SKIP. Recommendation below: land in the same commit, no skip mode (a gate that can silently skip is the "all-skip run indistinguishable from a pass" failure the integration lane comment at `.github/workflows/test.yml:222-225` was written about).

---

## 1. Existing gates in `.github/workflows/test.yml` (861 lines total)

Job list with line numbers (`grep -n "^  [a-zA-Z0-9_-]*:"`):

| line | job | runs-on | needs |
|---|---|---|---|
| 12 | `build-linux` | ubuntu-latest | — |
| 149 | `test` | ubuntu-latest | build-linux |
| 205 | `integration` | ubuntu-latest | build-linux |
| 304 | `flatc-check` | ubuntu-latest | — |
| 325 | `benchmark` | ubuntu-latest | build-linux |
| 381 | `build-retrace` | ubuntu-latest | — |
| 526 | `trace-cases` | ubuntu-latest | — |
| 546 | `trace-fixtures` | ubuntu-latest | — |
| 611 | `retrace` | ubuntu-latest | — |
| 717 | `retrace-summary` | ubuntu-latest | — |
| 758 | `remove-artifact-clutter` | ubuntu-latest | — |
| 809 | `pipe-gates` | ubuntu-latest | — (deliberately) |

Triggers, `test.yml:2-9`: `push` on `dev`, `Feat/Backend-Direct-GLES`, `Feat/Backend-Direct-Vulkan`, plus `workflow_dispatch`. **`feat/disaggregated` is not a trigger branch** — a new gate added here will only run on `workflow_dispatch` or after the branch merges, unless you add the branch to the `branches:` list. Flag this to the implementer; it is the single easiest way for a "green gate" to be green because it never ran.

### 1.1 `pipe-gates` (test.yml:809-861) — the job to extend

```yaml
809  pipe-gates:
810    name: MGPipe generators and hygiene gates
811    runs-on: ubuntu-latest
812    # Deliberately independent of build-linux: these are source-level gates, they take
813    # seconds, and a broken build must not hide a drifted interface.
814
815    steps:
816      - name: Checkout repo
817        uses: actions/checkout@v6
```

Note `actions/checkout@v6` at 816-817 with **no `with: submodules:`** — this job has an empty `3rdparty/` and an empty `include/ska/`. That is load-bearing for the design (section 5.4).

Steps:

- **819-827 "Regenerate the MGPipe interface (G1-G7)"**
  ```
  python3 scripts/gen_pipe.py
  git diff --exit-code -- MobileGL/MG_Pipe/generated
  ```
  Failure surfaces as a non-zero exit from `git diff --exit-code` (a diff printed to the log). Note it runs the *writing* mode and diffs, not `--check`.
- **829-846 "No stdio instrumentation in MG_Backend or MG_State"** — the stdio grep gate:
  ```bash
  if grep -rnE 'fprintf[[:space:]]*\((stderr|stdout)|(^|[^[:alnum:]_>.])printf[[:space:]]*\(|(^|[^[:alnum:]_>.:])puts[[:space:]]*\(|std::(cout|cerr)' \
      MobileGL/MG_Backend MobileGL/MG_State; then
    echo "::error::stdio instrumentation found; use MGLOG_D (compiled out in INFO builds)"
    exit 1
  fi
  echo "no fprintf(stderr/stdout / printf( / puts( / std::cout|cerr under MobileGL/MG_Backend or MobileGL/MG_State"
  ```
  Failure surfaces as a GitHub `::error::` annotation plus `exit 1`. **This is the house style for a new grep/scan gate**: annotate with `::error::`, then exit 1, and print an affirmative one-liner on success. Zero exceptions by policy (comment at 831-835: "this gate starts with no exceptions, and any addition to it needs a reason in the pull request rather than a quiet whitelist entry"). Copy that policy for the closure allow-list.
- **848 "MGPipe dirty-surface report"**: `python3 scripts/gen_pipe_dirty_surface.py --summary` — informational only (comment 846-847: "It becomes a gate in P1").
- **850-861 "Documentation citation lint"**:
  ```bash
  shopt -s nullglob
  documents=(docs/Disaggregated/*.md)
  if [ ${#documents[@]} -eq 0 ]; then echo "no disaggregation documents to check"; exit 0; fi
  python3 scripts/check_doc_citations.py "${documents[@]}" || true
  ```
  Warning-only via `|| true` (comment 849-851: becomes `--strict` when the documents settle). This is the last step of the file (file ends at 861).

### 1.2 `flatc-check` (test.yml:298-323) — the model for a compiler-needing, submodule-selective job

Header comment 298-303 explains why the generated header is committed and flatc kept out of the build graph. Steps:

- 306-307 `Checkout repo` / `actions/checkout@v6` (no submodules)
- 309-310 `Get CMake` / `lukka/get-cmake@v4.3.3`
- 312-315 **`Check out the FlatBuffers submodule only`**: `git submodule update --init 3rdparty/flatbuffers`, with the comment "Just this one: the schema check has nothing to do with glslang, SPIRV-Cross or the trace fixtures."
- 317-318 `Regenerate protocol_generated.h`: `python3 scripts/gen_protocol.py --build-dir "${{ runner.temp }}/flatc-build"`
- 320-321 `Fail if the committed header is stale`: `git diff --exit-code -- MobileGL/MG_Remote/Protocol/generated/protocol_generated.h`

**Use this job's selective-submodule pattern verbatim** for the include-closure gate (section 6): `git submodule update --init include/ska 3rdparty/xxHash 3rdparty/Vulkan-Headers` is the whole dependency set (proved in 5.4).

### 1.3 `build-linux` (test.yml:12-148) — toolchain and configure facts

- 59-60 installs the toolchain: `sudo apt-get install -y ccache clang-20 clang++-20 lld-20 libc++-20-dev libc++abi-20-dev libvulkan-dev libegl1-mesa-dev libgles2-mesa-dev libgl1-mesa-dri mesa-vulkan-drivers ninja-build`. So the project's CI compiler is **clang-20**, installed explicitly, not the image default.
- 52-54: `python update_glslang_sources.py` in `3rdparty/glslang` — a prerequisite for a *full* configure, not for the closure probe.
- 71-91 `Configure CMake`:
  ```
  cmake -S . -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_INFO \
    -DMOBILEGL_BUILD_TEST=ON -DMOBILEGL_BUILD_BENCHMARK=ON \
    -DMOBILEGL_BUILD_INTEGRATION_TEST=ON \
    -DMOBILEGL_ITEST_VK_ICD=/usr/share/vulkan/icd.d/lvp_icd.json \
    -DMOBILEGL_BUILD_TRACE_REPLAY=OFF \
    -DBENCHMARK_DOWNLOAD_DEPENDENCIES=ON -DBENCHMARK_ENABLE_TESTING=OFF \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  ```
  **`MOBILEGL_BUILD_DISAGGREGATED` is not passed** — CI's `build-linux` builds the OFF (shipping) configuration only. `BUILD_DIR=build-linux` (env, line 17).
- 94 `Build`: `cmake --build "${BUILD_DIR}" --parallel "$(nproc)"`
- 122-138 packages `ci-artifacts/mobilegl-linux-runtime.tgz` containing `CTestTestfile.cmake`, `MobileGL/MG_Test`, `MobileGL/MG_Benchmark`, `MobileGL/MG_IntegrationTest` and every `*.so` found — **note it excludes `*.a` and `*.o`, and `libMobileGL.so` IS in the `SHARED_LIBS` glob (127)**. That matters for the nm report: `libMobileGL.so` already leaves `build-linux` as an artifact (140-146, `actions/upload-artifact@v7`, name `mobilegl-linux-runtime`, `if-no-files-found: error`).
- 139-146 upload. 96-98 `ccache --show-stats` `if: always()`.
- 100-120: rolling ccache cache entry, only written on the default branch.

`test` job (149-203) downloads that artifact (176-180), unpacks (182-183), rewrites CTest command paths with an inline python heredoc (185-196) and runs `ctest --output-on-failure -L unit --no-tests=error` (198-203). **`--no-tests=error` is the house guard against a vacuous pass** — any new ctest must carry the right label (`unit`) or it will never run in CI.

### 1.4 `scripts/gen_pipe.py --check`

- Docstring 9-30; usage lines 25-26:
  ```
  python3 scripts/gen_pipe.py            # write the generated files, print the summary
  python3 scripts/gen_pipe.py --check    # fail if regenerating would change anything
  ```
- `REPO_ROOT` derived from `__file__` at line 37: `os.path.dirname(os.path.dirname(os.path.abspath(__file__)))` — **copy this idiom**, it makes the script cwd-independent.
- `write(path, text, check, changed)` at 618-626: compares against the existing file, appends `os.path.relpath(path, REPO_ROOT)` to `changed`, writes only when `not check`.
- `main()` 628-671: `argparse.ArgumentParser(description=__doc__)`, `--check` as `action="store_true"` (630-632). On drift with `--check` (663-666): `print("gen_pipe: OUT OF DATE: %s" % ", ".join(changed), file=sys.stderr); return 1`. Otherwise 669 `print("gen_pipe: generated files are up to date")`, `return 0`. Entry point 673-674 `sys.exit(main())`.
- Prints a summary block at 653-660 with a `gen_pipe:` prefix on every line. **House style: `<toolname>: <message>` prefix on every stdout line.**
- Hard errors use `sys.exit("message")` (lines 135, 153, 158, 178, 182, 193, 202, 208, 219, 519).

### 1.5 `scripts/check_doc_citations.py` (117 lines)

- Docstring 10-26 states the motivation and the two usages; last line 26: "Exits non-zero only with `--strict`, so it can be wired into CI as a warning first."
- `REPO_ROOT` at 33, same idiom.
- `CITATION_RE` 37-40 — a path-with-extension followed by `:line[-line]`.
- `git(args, rev_root=REPO_ROOT)` 43-45 shells `git -C <root> …` with `check=False`.
- `class Tree` 48-73: builds `Paths`, `ByBasename`, lazily counts lines from `git show rev:path`.
- `main()` 76-114: `documents nargs="+"`, `--rev` (default `HEAD`), `--strict`. Summary line 108-109 `check_doc_citations: %d citations in %d document(s) against %s, %d problem(s)`; problems printed indented; `return 1` only when `problems and args.strict` (112-113).

**Two reusable patterns for the new script**: (a) a `--strict`-style escalation switch so the gate can land as a warning and become blocking in a later commit; (b) a machine-countable summary line printed unconditionally.

---

## 2. Build configuration — CMake facts an implementer needs

Root `CMakeLists.txt` is 38,244 bytes.

### 2.1 Options

| line | option | default |
|---|---|---|
| 5 | `MOBILEGL_BUILD_TEST` | ON (forced OFF for `ANDROID` at line 29) |
| 6 | `MOBILEGL_BUILD_BENCHMARK` | ON (forced OFF for `ANDROID` at 30) |
| 11 | `MOBILEGL_BUILD_INTEGRATION_TEST` | OFF |
| 12 | `MOBILEGL_FORCE_RELEASE_OPT` | ON |
| 13 | `MOBILEGL_ENABLE_TRACY` | OFF |
| 14 | `MOBILEGL_BUILD_TRACE_REPLAY` | OFF |
| 15 | `MOBILEGL_TRACE_ANGLE_VARIANTS` | OFF |
| 16 | `MOBILEGL_IOS` | OFF |
| 23 | `MOBILEGL_BUILD_DISAGGREGATED` | OFF |
| 24 | `MOBILEGL_BUILD_SERVER_SPIKE` | OFF |
| 108 | `MOBILEGL_ENABLE_LTO` | OFF |

`CMakeLists.txt:17-22` is the comment that names the nm equality this whole family of gates protects:

> "OFF is the shipping default and OFF must stay byte-comparable to a tree without MG_Remote at all: nothing under `MobileGL/MG_Remote/` is compiled, no include path is added, and no library is linked, so `nm --defined-only libMobileGL.so | grep -i MG_Remote` is empty. That emptiness is one of the two byte-level equalities the plan's validation gates keep (section 10.3)."

Also: `MOBILEGL_LOG_ACTIVE_LEVEL` is a cache STRING, default `"MOBILEGL_LOG_LEVEL_INFO"` (line 25).

### 2.2 Compiler / language

- 155 `enable_language(CXX)`; 157 `set(CMAKE_CXX_STANDARD 23)`; 158 `CMAKE_CXX_STANDARD_REQUIRED ON`.
- **160 `set(CMAKE_EXPORT_COMPILE_COMMANDS ON)`** — unconditional. Every configured build tree has a `compile_commands.json`. Confirmed present at `~/w7/pipe/{build,build-linux,build-linux-split,build-retrace,build-android}/compile_commands.json`.
- 162 `CMAKE_POSITION_INDEPENDENT_CODE ON`. 165 MSVC `/EHsc`. 149-151 MSVC `/Zc:preprocessor` + `/Zc:__cplusplus`.
- 137-146: with `MOBILEGL_ENABLE_LTO`, `-O3 -ffunction-sections -fdata-sections` + `-Wl,--gc-sections`; off by default, so the default Release build does **not** use `--gc-sections`. Relevant to the nm/size report: section-splitting is off by default, so per-symbol sizes from `nm -S` are comparable between builds.
- 168-183: `MGGitHash.h` is `configure_file`d into `${CMAKE_BINARY_DIR}/generated` and 185 `include_directories(${CMAKE_BINARY_DIR}/generated)` — a *build-dir* include path. (Verified in section 5.4 that the closure probe does not need it.)
- 578-583 / 596-598 visibility presets: `hidden` in non-Debug, `default` in Debug. This directly changes the `nm --defined-only` output; **the nm report must compare two builds of the same `CMAKE_BUILD_TYPE`**.

### 2.3 Include directories — `MOBILEGL_INCLUDE_DIR` (`CMakeLists.txt:530-544`)

```cmake
set(MOBILEGL_INCLUDE_DIR
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/MobileGL
        # The MGPipe boundary headers. …
        ${CMAKE_SOURCE_DIR}/MobileGL/MG_Pipe          # line 535
        ${spirv-tools_SOURCE_DIR}
        ${spirv-tools_SOURCE_DIR}/include
        ${spirv-tools_BINARY_DIR}
        ${SPIRV-Headers_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/3rdparty/asio/include
)
```
and, only when disaggregated, `CMakeLists.txt:546-551` appends `${CMAKE_SOURCE_DIR}/3rdparty/flatbuffers/include`.

Applied to both targets: `target_include_directories(${CMAKE_PROJECT_NAME} PUBLIC ${MOBILEGL_INCLUDE_DIR})` at 579-581 (shared `MobileGL`) and 638-640 (static `MobileGL_s`, inside `if(NOT ANDROID)` at 619).

Note that `MG_Remote` gets **no include path of its own** — its headers are reached as `<MG_Remote/...>` through `${CMAKE_SOURCE_DIR}/MobileGL`, and internally by relative `"../Protocol/mg_protocol_base.h"` (`MobileGL/MG_Remote/Transport/ITransport.h:33`, `Framing.h:32`, `Ring.h:47`).

Sources: `MG_Remote` `.cpp` files are appended to `SOURCE_FILES` at `CMakeLists.txt:454-471` under `if (MOBILEGL_BUILD_DISAGGREGATED)`; the flatbuffers-submodule-missing fallback that shadows the option OFF for one configure is at 440-451; `-DMOBILEGL_BUILD_DISAGGREGATED=1` is appended to `MOBILEGL_COMPILE_DEF` at 523-525.

`MOBILEGL_COMPILE_DEF` (`CMakeLists.txt:512-521`): `-DVMA_STATIC_VULKAN_FUNCTIONS=0 -DVMA_DYNAMIC_VULKAN_FUNCTIONS=1 -DVMA_VULKAN_VERSION=1001000 -DASIO_STANDALONE -DASIO_NO_DEPRECATED`, plus `MOBILEGL_LOG_ACTIVE_LEVEL=${MOBILEGL_LOG_ACTIVE_LEVEL}` at 585-589 / 645-649 and `MOBILEGL_TRACE_ANGLE_VARIANTS` via a generator expression at 588.

### 2.4 Where the test targets are declared

- Root gate: `CMakeLists.txt:732-763`, inside `if (NOT ANDROID)`; 741 `enable_testing()` at top-level scope (comment 732-740 explains that this is what puts a `CTestTestfile.cmake` at the build root so `ctest -L unit` works from there); 744-746 `if (MOBILEGL_BUILD_TEST) add_subdirectory(MobileGL/MG_Test) endif()`; 750-752 integration; 754-756 benchmark; 758-760 trace_replay.
- `MobileGL/MG_Test/CMakeLists.txt` (103 lines):
  - 1-2 `cmake_minimum_required(VERSION 3.24)` / `project(MobileGLTest)`
  - 6-7 C++23
  - **9 `set(MGL_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)`** — the variable every sub-CMakeLists uses for include paths.
  - 15-24 `FetchContent` googletest v1.17.0 (`gtest_force_shared_crt ON`)
  - 26 `enable_testing()`
  - 32-47 the `SanityTest` target, include dirs `${MGL_ROOT}/include` and `${MGL_ROOT}/MobileGL`
  - **60-62 `set(LINK_LIBRARIES MobileGL_s)`** — the variable every sub-CMakeLists links
  - 64-65 `include(GoogleTest)` + `gtest_discover_tests(SanityTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)`
  - 67-74 MSVC `/WHOLEARCHIVE:MobileGL_s`
  - 76-99 the `add_subdirectory` list (`BackendLoader Buffer State EGLState Framebuffer Texture VertexArray Program Query Pipeline Pipe ShaderTranspiler Util SelfTest Backend/DirectGLES`, `Backend/DirectVulkan` under `ENABLE_INTEGRATION_TESTS`)
  - 100-103 `if (MOBILEGL_BUILD_DISAGGREGATED) add_subdirectory(Wire) endif()` — the disaggregated-only suite (this is the `MG_Test/CMakeLists.txt:93-95` the docs cite; the real lines are 100-103 in this worktree, a citation the doc lint would flag once ARCHITECTURE.md is re-pinned).
- **The model target — `MobileGL/MG_Test/Pipe/CMakeLists.txt` (28 lines)**:
  ```cmake
  1  cmake_minimum_required(VERSION 3.14)
  3  add_executable(PipeCatalogueTest PipeCatalogueTest.cpp)
  8  target_include_directories(PipeCatalogueTest PRIVATE
  9          ${MGL_ROOT}/include
 10          ${MGL_ROOT}/MobileGL
 11          ${MGL_ROOT}/MobileGL/MG_Pipe
 12          ${MGL_ROOT}/3rdparty/xxHash
 13          ${MGL_ROOT}/3rdparty/Vulkan-Headers/include
 14          ${MGL_ROOT}/3rdparty/SPIRV-Reflect
 15  )
 17  target_link_libraries(PipeCatalogueTest PRIVATE GTest::gtest_main ${LINK_LIBRARIES})
 23  if (MSVC) target_compile_options(PipeCatalogueTest PRIVATE /Zc:preprocessor) endif()
 27  include(GoogleTest)
 28  gtest_discover_tests(PipeCatalogueTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
  ```
  Note lines 9-14: **this target already spells out, by hand, almost exactly the minimal include set the closure probe needs** (section 5.4). It is the precedent for hand-listing include roots outside `MOBILEGL_INCLUDE_DIR`.
- `MobileGL/MG_Test/Wire/CMakeLists.txt` (36 lines) is the loop form: a `MOBILEGL_WIRE_TESTS` list (7-12), `FdPassingTest` appended only `if (NOT WIN32)` (14-17), and a `foreach` (21-36) that adds the executable, the include dirs (`include`, `MobileGL`, `3rdparty/flatbuffers/include`) and `gtest_discover_tests(... LABELS unit)`.
- Non-gtest `add_test` precedent (for a script-driven ctest): `tools/trace_replay/CMakeLists.txt:350-372` — `add_test(NAME MobileGLTraceReplay.${CASE_NAME}.${BACKEND} COMMAND "${CMAKE_COMMAND}" -D... -P .../run_trace_case.cmake)`, then `set_tests_properties(... PROPERTIES ENVIRONMENT "…")` at 373-380. `find_package(Python3 REQUIRED)` + `${Python3_EXECUTABLE}` appear at `tools/trace_replay/CMakeLists.txt:15` and `:390` and at `android-plugin/app/src/trace/cpp/CMakeLists.txt:9,51`. **There is no `find_package(Python3)` in the root `CMakeLists.txt` or under `MG_Test`** — a python-driven ctest under `MG_Test` must add its own.
- Benchmarks show the plain form: `MobileGL/MG_Benchmark/CMakeLists.txt:40 add_test(NAME SanityBench COMMAND SanityBench …)`.

### 2.5 Does a `compile_commands.json` / `clang -H` probe fit?

Yes, and better than expected — see section 5. Summary of the fit:

- `CMAKE_EXPORT_COMPILE_COMMANDS ON` is unconditional (`CMakeLists.txt:160`), so a local/ctest run can always read the *real* flags out of the build tree it lives in. 816 entries in `~/w7/pipe/build-linux-split/compile_commands.json`, 67 of them under `MG_State`.
- But a full configure is **not required**: a five-flag hand-rolled command reproduces the exact same 666-line closure (5.4). So the gate can also run in a source-only job in ~0.15 s.
- Both `clang++ -H` and `g++ -H` work and agree on the in-tree part of the closure (5.5).

---

## 3. What the docs require of the gate — verbatim quotes

### 3.1 `docs/Disaggregated/ROADMAP.md:16` — the P0.5 row (full row, `|`-separated)

> `| **P0.5** 值头与制品头抽取 | 6–9 | `MG_Pipe/MGPipeValueTypes.h`（`RenderStateParameters`、`SamplerParameters`、`PixelStoreParameters`、`VertexAttribute`… 不 include `MG_State/GLState`）；`MG_State/GLState/ProgramState/ProgramArtifacts.h`（五个反射类型，不 include `ShaderObject.h`/`SpvcSession.h`，更新 7 个 includer）；`Visit()` 归档 + `sizeof` 绊线；CI `-H` include 闭包断言 | 全套测试逐名不变（纯搬移）；两条闭包断言绿且人为加回一个 `MG_State` include 能变红；`nm`/`.text` 变化可逐符号归因 | P0；**P1 与 P7 的硬前置** |`

Decomposed acceptance gate (column 4):
1. **全套测试逐名不变（纯搬移）** — the whole suite unchanged *test-name by test-name*; the extraction is a pure move.
2. **两条闭包断言绿** — two closure assertions green.
3. **人为加回一个 `MG_State` include 能变红** — deliberately adding an `MG_State` include back must turn it red.
4. **`nm`/`.text` 变化可逐符号归因** — nm/.text changes must be attributable symbol by symbol.

### 3.2 `docs/Disaggregated/ROADMAP.md:7` — the per-commit discipline

> 默认 ALL target 必须完整构建；禁止提交热路径插桩（CI grep 门）；**每个门必须能因它存在的理由变红**；Windows 机器不是正确性门；…

"**Every gate must be able to turn red for the reason it exists**" is the sentence that makes the negative control mandatory and not optional.

### 3.3 `docs/Disaggregated/ARCHITECTURE.md:501` — §13.2 item 1, purity gate A

> 1. **接口纯度三道门**（只跑非 verify 构建）：**A 门 include 图**——disaggregated 配置编译 `MG_Backend` 时把 `MG_State/GLState` 从 include 搜索路径移除（`nm --undefined-only` 对"只 include 不调用"是瞎的，而 `RenderState.h → FramebufferObject.h → TextureObject.h` 正是这种耦合），依赖 P0.5；**B 门符号**——`nm --undefined-only libMobileGLServer.so | grep -E 'MG_State::GLState::|glslang'` 为空；**C 门未声明**——`grep -c 'pGLContext' MG_Backend/` == 0。…

Two things to carry into the design: (a) gate A's ultimate form is *removing `MG_State/GLState` from the include search path* when compiling `MG_Backend` — the `-H` closure assertion is the P0.5 stepping stone toward that; (b) the doc names the exact chain `RenderState.h → FramebufferObject.h → TextureObject.h` as the motivating coupling, and section 5.3 below **confirms that chain exists today, byte for byte**.

`ARCHITECTURE.md:499` (the sentence that opens §13.2):

> "改前改后 `nm --defined-only` 与 `.text` size 完全相等"的门在本方案里按构造死亡（不存在能让旧字节回来的配置）；替换是：

`ARCHITECTURE.md:507` (the two surviving byte-level equalities, and the drift-report requirement):

> 两条幸存的字节级等式：`MOBILEGL_BUILD_DISAGGREGATED=OFF` 时 `nm --defined-only libMobileGL.so | grep MG_Remote` 为空且链接行不增加库；`nm -D libMobileGL.so | grep mobilegl_server_main` 在 RelWithDebInfo 里命中。符号与 `.text` 漂移每阶段作为信息性指标发布。

("symbol and `.text` drift is published as an informational metric each phase" — that is the home for the nm/size report of section 8.)

### 3.4 `docs/Disaggregated/ARCHITECTURE.md:568` — §16 构建布局, the CI bullet

> - CI（`.github/workflows/test.yml:809` `pipe-gates`，P0 已落地）：`gen_pipe.py` 重生成 + diff；`MG_Backend`/`MG_State` 下禁止 stdio 插桩的 grep 门；`gen_pipe_dirty_surface.py --summary`（信息性，P1 成门）；`check_doc_citations.py`（警告级，文档定稿后 `--strict`）。独立 job `flatc-check`。后续：**`include-graph-check`（P0.5）**、`monolith-symbol-report`。

**The gate's name is already chosen by the docs: `include-graph-check`, and it is described as a *后续* (follow-on) item alongside `monolith-symbol-report`.** The natural reading — "独立 job `flatc-check`. 后续：`include-graph-check`(P0.5)、`monolith-symbol-report`" — is that these two are also standalone jobs. Section 6 proposes exactly that, and separately proposes reusing `monolith-symbol-report` as the name of the nm/.text job.

§16 also fixes the tree layout (`ARCHITECTURE.md:550-561`), naming `MobileGL/MG_Pipe/` as "永远进构建（monolith 的架构，不在任何 option 之后）  [P0]".

Other §16 lines the implementer needs:
- `ARCHITECTURE.md:563` — the CMake option paragraph, citing `CMakeLists.txt:23`, `454-469`, `440-451`, and `MobileGL/MG_Test/CMakeLists.txt:93-95`.
- `ARCHITECTURE.md:564` — `MOBILEGL_BUILD_DISAGGREGATED_INPROC` (does not exist yet) and the sentence "server 角色不再读 `pGLContext`（三道纯度门就是这个断言）".
- `ARCHITECTURE.md:567` — the ctest wiring traps: "ctest `ENVIRONMENT` 是替换而非追加、`;` 必须转义、property 覆盖 job env". Confirmed independently by the comment at `.github/workflows/test.yml:222-227`.

### 3.5 `docs/Disaggregated/ARCHITECTURE.md:260` — §7, the exact extraction contract

> - **P0.5 硬前置**：反射类型今天声明在 `ProgramObject.h` 里，而它 include `ShaderObject.h`（→ glslang）与 `SpvcSession.h`（→ spirv_reflect）。P0.5 把 `TypeFacts`、`ResourceReflection`、`XfbVarying`、`LinkArtifacts`、`SpirvArtifacts` 抽到 `MG_State/GLState/ProgramState/ProgramArtifacts.h`（只 include `<Includes.h>` 与容器），更新 7 个 includer，加 CI `-H` 闭包断言。同批抽取 `MG_Pipe/MGPipeValueTypes.h`（`MAX_DRAW_BUFFERS`、`PerBufferBlendState`、`StencilFaceState`、`PixelStoreParameters`、`RenderStateParameters`、`SamplerParameters`、`BorderColorForm`、`VertexAttribute`、`VertexBufferBindingPoint`），它不 include `MG_State/GLState` 任何东西；`MGPipeTypes.h` 今天为此临时 include 了 `BackendObject.h` 与 `RenderState.h`（文件头注明为 P0.5 债务）。没有这一步，P7 的 `nm -D | grep glslang` 判据不可达。

**This is the authoritative spec of the two closure assertions.** Note carefully:

- `ProgramArtifacts.h` is specified as "**只 include `<Includes.h>` 与容器**" — it *keeps* the umbrella header. Since `MobileGL/Includes.h:59` includes `<spirv_cross/spirv_cross_c.h>` and `:79-83` include five glslang headers, **the ProgramArtifacts assertion CANNOT be "no glslang / no spirv-cross in the closure"**. It must be "no `ShaderObject.h`, no `SpvcSession.h`" (and, in practice, none of the chain those two drag in). See section 5.6.
- `MGPipeValueTypes.h` is specified as "不 include `MG_State/GLState` 任何东西" — a *directory-prefix* assertion. `MGPipeTypes.h`'s own P0.5 debt note names both offending includes.

The file-level debt note is in the tree at `MobileGL/MG_Pipe/MGPipeTypes.h:25-33`:

> // P0.5 DEBT, recorded here so it is impossible to miss: two payloads reach into headers
> // this directory is eventually forbidden to see - MGPCaps embeds MG_Backend's
> // DynamicBackendParameters, and ResidualValueBlock embeds MG_State's RenderStateParameters
> // and PixelStoreParameters. … P0.5
> // extracts MGPipeValueTypes.h and both includes below go away; until then purity gate A
> // (section 10.3) cannot be armed for this header.
> `#include <MG_Backend/BackendObject.h>`      ← line 32
> `#include <MG_State/GLState/RenderState/RenderState.h>` ← line 33

And `MGPipeTypes.h:35-40` has the `using` declarations that must follow the types when they move:
```cpp
namespace MobileGL::MG_Pipe {
    using MG_Backend::DynamicBackendParameters;
    // Both live directly in namespace MobileGL today; P0.5 moves them into
    // MG_Pipe/MGPipeValueTypes.h.
    using MobileGL::PixelStoreParameters;
    using MobileGL::RenderStateParameters;
```

`ARCHITECTURE.md:259` gives the companion `Visit()`/`sizeof` tripwire requirement (`static_assert(sizeof(LinkArtifacts) == MGL_LINKARTIFACTS_SIZE)`) and enumerates the fields the archive must cover — out of scope for the include gate but same commit.

### 3.6 Precedent already in the tree — `MG_Remote/Transport` is umbrella-free by rule

`MobileGL/MG_Remote/Transport/WireLog.h:9-24` is the strongest existing statement of intent, and it names the mechanism this gate implements:

> // MG_Util/Debug/Log.h includes `<Includes.h>`, the GL frontend's umbrella
> // header - **661 headers, measured with `clang++ -H`**. That is fine inside a
> // .cpp … It is not fine in a header of this layer: ITransport.h states the rule
> // ("nothing about a byte pipe needs the GL frontend's umbrella header") … and
> // because the disaggregated build's include-graph purity gate (plan section
> // 10.3, gate A) asserts on `-H` output rather than on symbols. Framing.h was
> // the one header under Transport/ that broke the rule; it now calls this
> // instead, and the umbrella stays inside WireLog.cpp.

Their includes today (all in-tree-clean):
- `ITransport.h:33,35`: `"../Protocol/mg_protocol_base.h"`, `<cstdint>`
- `Framing.h:32,36,38-41`: `"../Protocol/mg_protocol_base.h"`, `"WireLog.h"`, `<cstddef> <cstdint> <cstring> <vector>`
- `Ring.h:47,49-51`: `"../Protocol/mg_protocol_base.h"`, `<atomic> <cstddef> <cstdint>`

**Recommendation: make this a third, free probe** (section 6.3). It is green today, it costs 30 ms, and it locks a rule that a single careless `#include <Includes.h>` in `Framing.h` has already broken once.

---

## 4. Ground truth about the headers to be extracted

### 4.1 Current include edges (all verified by opening the files)

```
MG_Pipe/MGPipe.h:10,12-15         <Includes.h>, "MGPipeCallbacks.h", "MGPipeHandles.h", "MGPipeHostSpan.h", "MGPipeTypes.h"
MG_Pipe/MGPipe.h:65,68,77,80,83,86,89,92   "PipeCalls.def" + the seven generated/*.inc
MG_Pipe/MGPipeCallbacks.h:10,12-13 <Includes.h>, "MGPipeHandles.h", "MGPipeTypes.h"
MG_Pipe/MGPipeHandles.h:10         <Includes.h>          ← already MG_State-free
MG_Pipe/MGPipeHostSpan.h:10        <Includes.h>          ← already MG_State-free
MG_Pipe/MGPipeTypes.h:10,12-13     <Includes.h>, "MGPipeHandles.h", "MGPipeHostSpan.h"
MG_Pipe/MGPipeTypes.h:32           <MG_Backend/BackendObject.h>                       ← P0.5 debt
MG_Pipe/MGPipeTypes.h:33           <MG_State/GLState/RenderState/RenderState.h>       ← P0.5 debt

MG_State/GLState/RenderState/RenderState.h:10-12   <Includes.h>, <MG_Util/Math/VectorTypes.h>,
                                                   <MG_State/GLState/FramebufferState/FramebufferObject.h>
MG_State/GLState/FramebufferState/FramebufferObject.h:10-13
        "MG_Util/Types.h", <Includes.h>,
        <MG_State/GLState/TextureState/TextureObject.h>,
        <MG_State/GLState/RenderbufferState/RenderbufferObject.h>
MG_Backend/BackendObject.h:10-11   <Includes.h>, "MG_State/GLState/TextureState/TextureEnum.h"

MG_State/GLState/ProgramState/ProgramObject.h:10,11,13,14
        <Includes.h>, "ShaderObject.h", <MG_Util/Metrics/BufferMetrics.h>,
        <MG_Util/ShaderTranspiler/SpvcSession.h>
```

`RenderState.h → FramebufferObject.h → TextureObject.h` — the coupling `ARCHITECTURE.md:501` names — is `RenderState.h:12` → `FramebufferObject.h:12`. Confirmed.

### 4.2 The types to move

Value types (target `MG_Pipe/MGPipeValueTypes.h`), all in `MobileGL/MG_State/GLState/RenderState/RenderState.h` (538 lines, `namespace MobileGL` at the outer level, closing at 538):
- `PixelStoreParameters` at **:191-200** (8 scalar members)
- `PerBufferBlendState` at **:202-210**
- `StencilFaceState` at **:212-220**
- `RenderStateParameters` at **:222-…**, with `static constexpr Uint MAX_VIEWPORTS = 16;` at **:227** and `Array<FloatVec4, MAX_VIEWPORTS> Viewports{}` at **:238**
- the state class's private data at **:527-537**: `Uint16 m_version`, `Uint16 m_pipelineStateVersion`, `RenderStateParameters m_parameters`, two `PixelStoreParameters`
`ARCHITECTURE.md:260` additionally names `MAX_DRAW_BUFFERS`, `SamplerParameters`, `BorderColorForm`, `VertexAttribute`, `VertexBufferBindingPoint` — those live in the sampler/vertex-array headers, not in `RenderState.h`; grep for them before writing the probe's expectations.

Reflection types (target `MG_State/GLState/ProgramState/ProgramArtifacts.h`), all nested inside `class ProgramObject` in `MobileGL/MG_State/GLState/ProgramState/ProgramObject.h`:
- `struct TypeFacts` **:44**
- `struct ResourceReflection` **:76**
- `struct LinkedShaderRef` **:150** (not in the doc's list of five)
- `struct XfbVarying` **:1146**
- `struct LinkArtifacts` **:1210**
- `struct SpirvArtifacts` **:1409**
- (also `PendingUniformWrite` **:1658**, `BackendHashMemoSlot` **:1767**, forward decls `ProgramLinkTask` **:20**, `ProgramSpirvTask` **:23**, `class ProgramObject` **:25**)

The doc's "five reflection types" = `TypeFacts`, `ResourceReflection`, `XfbVarying`, `LinkArtifacts`, `SpirvArtifacts` (`ARCHITECTURE.md:260`). **They are nested classes today** — extracting them out of `class ProgramObject` changes their qualified names (`ProgramObject::LinkArtifacts` → `ProgramArtifacts`-namespace name), which is what makes the "全套测试逐名不变（纯搬移）" and "nm 逐符号归因" acceptance gates non-trivial. Expect mangled-name churn on every function taking one of these by reference; that is exactly what the nm report of section 8 has to attribute.

### 4.3 The "7 includers" of `ProgramObject.h`

`grep -rln "ProgramState/ProgramObject.h\|\"ProgramObject.h\"" --include=*.h --include=*.cpp MobileGL` gives 10 files (one is the header itself):

```
MobileGL/MG_Backend/DirectVulkan/Renderer/ProgramFactory.h
MobileGL/MG_Backend/DirectVulkan/Renderer/UniformManager.cpp
MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp
MobileGL/MG_Impl/GLImpl/Program/ProgramInterface.cpp
MobileGL/MG_State/GLState/ProgramState/ProgramLinkTask.h
MobileGL/MG_State/GLState/ProgramState/ProgramObject.cpp
MobileGL/MG_State/GLState/ProgramState/ProgramObject.h      (self)
MobileGL/MG_State/GLState/ProgramState/ProgramPipelineObject.h
MobileGL/MG_State/GLState/ProgramState/ProgramState.h
MobileGL/MG_State/GLState/ProgramState/ProgramTranslationCache.h
```
i.e. 9 other files, of which the doc counts 7 as needing an update (the two `MG_Backend/DirectVulkan/Renderer/*.cpp` and the `.h` all consume the reflection types; work out the exact 7 during implementation — the discrepancy is worth a line in the commit message).

---

## 5. Measured facts about `-H` (all commands run, all output real)

Environment: WSL Arch, `clang version 22.1.6` (`/usr/sbin/clang++`), `g++ (GCC) 16.1.1`, GNU `nm`/`size` at `/usr/sbin`. `which clang++` on the **Windows** shell returns nothing — the Windows box has no clang, so all local verification of this gate must happen in WSL (consistent with `ROADMAP.md:7` "Windows 机器不是正确性门").

### 5.1 `-H` goes to stderr, with dot-depth prefixes

```
$ clang++ -std=c++23 -I/tmp/hprobe -H -fsyntax-only probe.cpp 2>err.txt 1>out.txt
exit=0
stdout bytes: 0            ← nothing on stdout
err.txt:
. ./outer.h
.. ./sub/inner.h
... /usr/bin/../lib64/gcc/.../include/c++/16/vector
.... .../bits/requires_hosted.h
```
Format: `^(\.+) (path)$`; the number of dots is the include depth, so the **full inclusion chain to any forbidden header is reconstructible** by walking backwards to the nearest line with depth−1. That is the whole reason to prefer `-H` over `-MM`.

Paths come back **relative when the `-I` was relative or the include was resolved relative to the including file** (`. ./outer.h`, and `. MobileGL/MG_Pipe/MGPipeTypes.h` in the real run). The script must `os.path.realpath(os.path.join(compiler_cwd, path))` before matching prefixes.

`-H` prints a header **only on its first inclusion** (include guards suppress the rest) — irrelevant for a closure test, but it means the output is not an inclusion *count*.

### 5.2 `-MM` alternative

```
$ clang++ -std=c++23 -I/tmp/hprobe -MM probe.cpp
probe.o: probe.cpp outer.h sub/inner.h
```
One make rule, easy to parse, but it drops system headers and, critically, **loses the depth/chain information**. Use it only as a cross-check.

### 5.3 The real closure of `MGPipeTypes.h` today

Probe TU: `printf "#include <MG_Pipe/MGPipeTypes.h>\n" > probe_pipe.cpp`, flags copied verbatim from `build-linux-split/compile_commands.json` (the entry for `MobileGL/MG_State/GLState/Core.cpp`; 816 entries in that file), run with cwd = the build dir:

```
exit=0
total header lines: 666
in-tree MobileGL/:   20
MG_State/:            9
MG_Impl/:             0
MG_Backend/:          1
glslang:             18
spirv_cross:          2
SPIRV-Reflect:        0
```
The nine `MG_State` lines, with their real depths — this is the violation set the gate must report:

```
...   MobileGL/MG_State/GLState/TextureState/TextureEnum.h            (via MG_Backend/BackendObject.h)
..    MobileGL/MG_State/GLState/RenderState/RenderState.h             ← MGPipeTypes.h:33, depth 2
...   MobileGL/MG_State/GLState/FramebufferState/FramebufferObject.h
....  MobileGL/MG_State/GLState/TextureState/TextureObject.h
..... MobileGL/MG_State/GLState/TextureState/MipmapUploadTargetArray.h
......MobileGL/MG_State/GLState/TextureState/MipmapStorage.h
....... MobileGL/MG_State/GLState/TextureState/TextureTypes.h
..... MobileGL/MG_State/GLState/TextureState/../SamplerState/SamplerObject.h
....  MobileGL/MG_State/GLState/RenderbufferState/RenderbufferObject.h
```
Note the `TextureState/../SamplerState/SamplerObject.h` spelling — **`-H` prints the path as written, un-normalized**; prefix matching without `os.path.normpath` would miss a `../`-spelled violation. This is a real trap present in today's output.

The 666 total confirms (and slightly updates) the "661 headers" figure in `WireLog.h:12`.

### 5.4 A five-flag command reproduces the closure exactly — no build tree required

```bash
cd <repo root>
clang++ -std=gnu++23 \
  -Iinclude -IMobileGL -IMobileGL/MG_Pipe \
  -I3rdparty/xxHash -I3rdparty/Vulkan-Headers/include \
  -H -E -o /dev/null probe_pipe.cpp
```
Result: `exit=0`, **666 header lines, 9 `MG_State/` — byte-identical to the full-flag run.** Adding `-DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_INFO` changes nothing (666 either way).

Why so few flags: `include/glslang/` and `include/spirv_cross/` are **vendored into the repo** (not submodules) — `ls include/` gives `EGL GL GLES3 KHR android_debug.h glslang ska spirv_cross`, and the `-H` output shows `include/glslang/Include/Types.h` and `include/spirv_cross/spirv_cross_c.h` winning over the `3rdparty/` copies because `-Iinclude` precedes them in `MOBILEGL_INCLUDE_DIR`. The only submodule-provided headers in the closure are:

| header | provider | `.gitmodules` |
|---|---|---|
| `ska/flat_hash_map.hpp` (`Includes.h:53`) | `include/ska` | submodule `include/ska` |
| `xxhash.h` (`Includes.h:56`) | `3rdparty/xxHash` | submodule |
| `vulkan/vulkan.h` (`Includes.h:131`) | `3rdparty/Vulkan-Headers/include` | submodule |

So the CI job needs exactly `git submodule update --init include/ska 3rdparty/xxHash 3rdparty/Vulkan-Headers` — the `flatc-check` pattern (`test.yml:312-315`). No glslang submodule, no `update_glslang_sources.py`, no SPIRV-Cross, no flatbuffers, no CMake configure, no `${CMAKE_BINARY_DIR}/generated/MGGitHash.h`.

### 5.5 Speed, and `-E` vs `-fsyntax-only`, and `g++`

| command | wall time | header lines |
|---|---|---|
| `clang++ … -H -E -o /dev/null probe_pipe.cpp` | **0.137 s** | 666 |
| `clang++ … -H -fsyntax-only probe_pipe.cpp` | 1.889 s | 666 |

`-E` is ~14× faster and produces the identical closure, because the include graph is a preprocessor fact. **Use `-E -o /dev/null` for the closure assertion.** Keep one `-fsyntax-only` run per probe as a separate, second assertion ("the header is self-contained and compiles standing alone") — that is a genuinely different property and still under 2 s.

`g++` works too:
```
$ g++ -std=gnu++23 -Iinclude -IMobileGL -IMobileGL/MG_Pipe -I3rdparty/xxHash \
      -I3rdparty/Vulkan-Headers/include -H -E -o /dev/null probe_pipe.cpp
exit=0 ;  651 header lines ; MG_State: 9
. MobileGL/MG_Pipe/MGPipeTypes.h
.. MobileGL/Includes.h
... MobileGL/Defines.h
.... /usr/include/signal.h
```
Same dot format, same 9 `MG_State` hits (the 651 vs 666 difference is entirely in libstdc++/glibc internals). **GCC trap**: `g++ -H` appends a trailer block

```
Multiple include guards may be useful for:
/usr/include/asm/errno.h
…
3rdparty/xxHash/xxhash.h
include/glslang/Include/../Include/visibility.h
```
— 54 non-dot lines in this run. The parser **must** accept only lines matching `^\.+ ` and discard everything else, or it will count a guard-advice line as a closure member. Clang emits no such trailer.

Since `g++` works, the gate does **not** have to `apt-get install clang-20` (~20 s): `g++` is preinstalled on every `ubuntu-latest` image. Recommendation is still to prefer clang-20 for parity with the real build (`test.yml:59-60`) and fall back to `c++`/`g++` — the script should take `--compiler` and print which one it used.

### 5.6 The closure of `ProgramObject.h` today, and why "no glslang" is not an assertable property

Same probe method, `#include <MG_State/GLState/ProgramState/ProgramObject.h>`:

```
total: 682 header lines ; in-tree: 33
. MobileGL/MG_State/GLState/ProgramState/ProgramObject.h
.. MobileGL/Includes.h
... MobileGL/Defines.h
... MobileGL/MG_Util/PlatformStubs.h
... MobileGL/MG_Util/Debug/Log.h
... MobileGL/MG_Util/Types.h
.... MobileGL/MG_Util/GLExtensions.h
.. MobileGL/MG_State/GLState/ProgramState/ShaderObject.h          ← forbidden after P0.5
... MobileGL/MG_State/GLState/ProgramState/ShaderStage.h
... MobileGL/MG_State/GLState/ProgramState/ShaderCompileTask.h
.... MobileGL/MG_Util/Async/JobNode.h
..... MobileGL/MG_State/GLState/ErrorState/ErrorCode.h
..... MobileGL/MG_State/GLState/ErrorState/ErrorInfo.h
.... MobileGL/MG_Util/ShaderTranspiler/CompileEnv.h
..... MobileGL/Config.h
...... MobileGL/MG_Backend/BackendObjects.h
....... MobileGL/MG_Backend/BackendObject.h
........ MobileGL/MG_State/GLState/TextureState/TextureEnum.h
....... MobileGL/MG_Backend/DirectGLES/BackendObject_DirectGLES.h
........ MobileGL/MG_Util/BackendLoaders/OpenGL/Loader.h
....... MobileGL/MG_Backend/DirectVulkan/BackendObject_DirectVulkan.h
........ MobileGL/MG_Util/BackendLoaders/Vulkan/Loader.h
.... MobileGL/MG_Util/ShaderTranspiler/Types.h
.... MobileGL/MG_State/GLState/BufferState/BufferState.h
..... MobileGL/MG_Util/Miscellany/IndexGenerator.h
..... MobileGL/MG_State/GLState/BufferState/BufferObject.h
...... MobileGL/MG_Util/Math/VectorTypes.h
...... MobileGL/MG_State/GLState/BufferState/PipeResource.h
.... MobileGL/MG_State/GLState/ProgramState/ShaderPreprocessCache.h
..... MobileGL/MG_State/GLState/ProgramState/ShaderSourceKey.h
... MobileGL/MG_State/GLState/ProgramState/ShaderCompileAdoptionMap.h
.. MobileGL/MG_Util/Metrics/BufferMetrics.h
.. MobileGL/MG_Util/ShaderTranspiler/SpvcSession.h                ← forbidden after P0.5
```
The whole middle block (`ShaderStage.h` … `ShaderSourceKey.h`) hangs off `ShaderObject.h` and disappears when that include goes. `Config.h` → `MG_Backend/BackendObjects.h` → **both backends' object headers** is the surprising edge: `ProgramObject.h` currently drags in `BackendObject_DirectGLES.h` *and* `BackendObject_DirectVulkan.h` *and* both loaders. Cutting `ShaderObject.h` cuts all of them.

**Critical design constraint.** `MobileGL/Includes.h` is the umbrella and it pulls, unconditionally:
- `:53` `<ska/flat_hash_map.hpp>`
- `:56` `<xxhash.h>`
- `:59` `<spirv_cross/spirv_cross_c.h>`
- `:79-83` `<glslang/Include/Types.h>`, `<glslang/Public/ShaderLang.h>`, `<glslang/SPIRV/GlslangToSpv.h>`, `<glslang/Include/intermediate.h>`, `<glslang/MachineIndependent/localintermediate.h>`
- `:131` `<vulkan/vulkan.h>`
- `:147-148` `"MG_Util/Debug/Log.h"`, `"MG_Util/Types.h"`

`ARCHITECTURE.md:260` explicitly permits `ProgramArtifacts.h` to include `<Includes.h>`. **Therefore an assertion of the form "the closure contains no glslang / spirv-cross / SPIRV-Reflect header" is unsatisfiable by construction and must not be written.** The task brief's proposed forbidden set has to be narrowed to in-tree paths:

- Probe A (`MG_Pipe/MGPipeValueTypes.h`): forbid `MobileGL/MG_State/`, `MobileGL/MG_Impl/`, `MobileGL/MG_Backend/`, `MobileGL/MG_Remote/`. (The doc only demands `MG_State/GLState`; the other three are free and match §16's layering.)
- Probe B (`MG_State/GLState/ProgramState/ProgramArtifacts.h`): forbid the exact files `ProgramState/ShaderObject.h` and `MG_Util/ShaderTranspiler/SpvcSession.h`, plus — because they are only reachable through those two — `MG_Util/ShaderTranspiler/`, `MobileGL/Config.h`, `MobileGL/MG_Backend/`, `MG_State/GLState/BufferState/`, `MG_State/GLState/ProgramState/Shader*.h`. Start with the doc's two names as the hard gate and add the rest as a second, tighter list once the header lands green.

If the project later wants the glslang-free assertion for real, the route is the `MG_Remote/Transport` route: **do not include `Includes.h`** (probe C, section 6.3), not a bigger forbidden list.

### 5.7 No macro-driven includes anywhere in `MobileGL/`

`grep -rn "^\s*#\s*include\s\+[A-Za-z_]" --include=*.h --include=*.cpp MobileGL` → **zero matches** (3377 `#include` lines total). Every include is a literal `"…"` or `<…>`. The only non-header includes are `MG_Pipe/MGPipe.h:65` `"PipeCalls.def"` and `:68,77,80,83,86,89,92` `"generated/Pipe*.inc"`.

This means a **pure-text transitive include walker in Python needs no compiler and cannot be fooled by macro spelling** — the basis for the fast layer-1 gate in section 6.2. It can still be fooled by `#if` (an include inside a false branch would be reported as present); the compiler probe is the arbiter.

---

## 6. Proposed design

Name it `scripts/check_include_closure.py`, and the CI job `include-graph-check` (the name `ARCHITECTURE.md:568` already reserves).

### 6.1 The probe manifest — data, not code

Put the probe definitions in the script as a module-level table so a reviewer can read the whole contract in one screen (the `gen_pipe.py` house style — see its `GENERATED_BANNER` and generator tables):

```python
PROBES = [
    Probe(
        Name        = "value-header",
        Header      = "MG_Pipe/MGPipeValueTypes.h",         # spelled as the TU will include it
        Forbidden   = ["MobileGL/MG_State/", "MobileGL/MG_Impl/",
                       "MobileGL/MG_Backend/", "MobileGL/MG_Remote/"],
        Allow       = [],                                    # deliberately empty; see policy note
        Why         = "ROADMAP P0.5 / ARCHITECTURE.md:260 - it must not include MG_State/GLState",
    ),
    Probe(
        Name        = "artifacts-header",
        Header      = "MG_State/GLState/ProgramState/ProgramArtifacts.h",
        Forbidden   = ["MobileGL/MG_State/GLState/ProgramState/ShaderObject.h",
                       "MobileGL/MG_Util/ShaderTranspiler/",
                       "MobileGL/Config.h",
                       "MobileGL/MG_Backend/"],
        Allow       = [],
        Why         = "ARCHITECTURE.md:260 - no ShaderObject.h (-> glslang), no SpvcSession.h",
    ),
    Probe(
        Name        = "wire-header",                          # free bonus, green today
        Header      = "MG_Remote/Transport/ITransport.h",
        Forbidden   = ["MobileGL/Includes.h"],
        Allow       = [],
        Why         = "WireLog.h:9-24 / ITransport.h:25 - no GL umbrella in the byte-pipe layer",
    ),
]
```

Policy for `Allow`: copy the stdio gate's wording (`test.yml:831-835`) — **start empty; an addition needs a reason in the pull request, not a quiet whitelist entry.** If an entry is ever added, require a `Reason` string on it and print it in the summary so it stays visible.

`Forbidden` entries are matched as **path prefixes on the repo-relative, `os.path.normpath`-ed path** (so `TextureState/../SamplerState/SamplerObject.h` normalizes to `SamplerState/SamplerObject.h` and matches `MobileGL/MG_State/`). Any path outside the repo (system, `/usr/...`) is ignored.

### 6.2 Two modes, one script

**`--mode text` (default; no compiler, no submodules, ~50 ms).** Transitive walk of `#include` directives starting at the probe header. Resolution order per the CMake include dirs (`CMakeLists.txt:530-544`): (1) for `"quoted"` includes, the directory of the including file; (2) `include/`; (3) `MobileGL/`; (4) `MobileGL/MG_Pipe/`. Anything that does not resolve to a file inside the repo is a leaf (that is every `<vector>`, `<vulkan/vulkan.h>`, `<xxhash.h>`). Report a violation as `probe → chain → forbidden header`, using the recorded parent chain. This mode is safe in `pipe-gates` as-is.

**`--mode clang --compile-commands <path>` (authoritative).** Writes each probe TU into a temp dir, derives flags either from a `compile_commands.json` entry (take any entry whose `file` is under `MobileGL/`, strip `-o`, `-c`, `-MD/-MF/-MT`, and the input file; keep every `-D`, `-I`, `-isystem`, `-std`) or, when `--compile-commands` is absent, from the verified minimal set of 5.4. Runs

```
<cxx> <flags> -H -E -o /dev/null <probe.cpp>
```
capturing **stderr**, parses `^(\.+) (.*)$` only, normalizes each path against the compiler's cwd, and applies the same forbidden/allow logic. Then, as a second assertion, runs `-fsyntax-only` once per probe and fails if the header is not self-contained.

The two modes should agree; make a disagreement a hard failure with a message that says which mode saw what — that is a free consistency check on the text walker.

Both modes print, unconditionally, one machine-greppable summary line per probe in the house style:

```
include-closure: value-header      OK   187 headers in closure, 0 forbidden (mode=clang, cxx=clang++-20)
include-closure: artifacts-header  OK   642 headers in closure, 0 forbidden
include-closure: wire-header       OK     8 headers in closure, 0 forbidden
include-closure: 3 probes, 0 problem(s)
```
and on failure, a chain, followed by `::error::` + `exit 1`:

```
include-closure: value-header FORBIDDEN MobileGL/MG_State/GLState/RenderState/RenderState.h
include-closure:   chain: MG_Pipe/MGPipeValueTypes.h
include-closure:       -> MobileGL/MG_State/GLState/RenderState/RenderState.h
include-closure:            -> MobileGL/MG_State/GLState/FramebufferState/FramebufferObject.h
include-closure:                 -> MobileGL/MG_State/GLState/TextureState/TextureObject.h
```

Flags to provide: `--mode {text,clang,both}`, `--compile-commands PATH`, `--compiler PATH` (default: `$CXX`, then `clang++-20`, then `clang++`, then `c++`), `--probe NAME` (run one), `--self-test`, `--json PATH` (dump the closure for the informational report), and — per the `check_doc_citations.py` precedent — a `--strict` if the gate is to land warning-only first (recommendation: it should NOT; it lands with the headers, green from commit one).

`REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))` — the `gen_pipe.py:37` / `check_doc_citations.py:33` idiom.

### 6.3 The third probe is free — take it

`MG_Remote/Transport/ITransport.h` includes only `"../Protocol/mg_protocol_base.h"` and `<cstdint>` (`:33,35`). Asserting "closure contains no `MobileGL/Includes.h`" is green today, costs ~30 ms, and pins the rule that `WireLog.h:9-24` says `Framing.h` has already violated once. It also gives the gate a probe that is green *before* P0.5 lands, so the script can be reviewed and merged independently of the header extraction.

### 6.4 The negative control as a real test, not a manual demo

`ROADMAP.md:7` — "每个门必须能因它存在的理由变红" — and `ROADMAP.md:16` — "人为加回一个 `MG_State` include 能变红" — require it. Make it a `--self-test` subcommand that **always runs as part of the gate**, so it cannot rot:

1. Synthesize a fourth, throwaway probe TU in the temp dir:
   ```cpp
   #include <MG_Pipe/MGPipeValueTypes.h>
   #include <MG_State/GLState/RenderState/RenderState.h>   // deliberate violation
   ```
   Run the *same* analysis over it with the *same* forbidden list.
2. Assert it reports **at least one** violation, and that the reported chain names `RenderState.h` at depth 1.
3. If it reports zero, the gate itself is broken → `::error::` + exit 1 with the message "negative control did not trip: the closure gate is not actually checking anything".

Do this **without touching any tracked file** — a synthesized TU in `tempfile.mkdtemp()`, never a temporary edit of a real header. (An earlier `git checkout`-based negative control would leave the tree dirty on a crash and race the `git diff --exit-code` step of the same job.)

Optionally strengthen it: also self-test the *text* walker and the *clang* mode separately, so a broken parser in one mode is caught even if the other mode still works.

For the "两条闭包断言" wording: the gate should also assert `len(green_probes) >= 2` for the two named headers, so deleting a probe from the manifest is itself red.

### 6.5 Wiring as a ctest

Add `MobileGL/MG_Test/Purity/CMakeLists.txt` and `add_subdirectory(Purity)` to `MobileGL/MG_Test/CMakeLists.txt` next to line 90 (`add_subdirectory(Pipe)`), with a comment in the same voice as 88-89:

```cmake
cmake_minimum_required(VERSION 3.14)

# The P0.5 include-closure assertions (ROADMAP P0.5, ARCHITECTURE.md:501 gate A). No
# compilation of MobileGL, no GL context: it preprocesses two probe TUs and reads
# clang's -H output. The build tree's own compile_commands.json supplies the real flags.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(NAME MobileGLPurity.IncludeClosure
         COMMAND ${Python3_EXECUTABLE}
                 ${MGL_ROOT}/scripts/check_include_closure.py
                 --mode both
                 --compile-commands ${CMAKE_BINARY_DIR}/compile_commands.json
                 --compiler ${CMAKE_CXX_COMPILER}
                 --self-test)
set_tests_properties(MobileGLPurity.IncludeClosure PROPERTIES LABELS unit)
```

Notes and traps:
- `LABELS unit` is required or `ctest -L unit` (`test.yml:200,202`) will never run it. `--no-tests=error` protects only against an *empty* selection, not against a mislabelled test.
- `${CMAKE_BINARY_DIR}/compile_commands.json` exists because of `CMakeLists.txt:160`, but **it is written at the end of configure**; using it from a test (run time) is fine, from `execute_process` at configure time is not.
- Pass `${CMAKE_CXX_COMPILER}` so the test uses the same compiler that configured the build.
- Do **not** use `set_tests_properties(... ENVIRONMENT ...)` unless you must: `ARCHITECTURE.md:567` and `.github/workflows/test.yml:222-227` both record that a ctest `ENVIRONMENT` property *replaces* and *overrides* the job environment silently.
- This test travels in the `mobilegl-linux-runtime.tgz` only if the packaging list (`test.yml:126-137`) covers it: it packs `${BUILD_DIR}/MobileGL/MG_Test` and the root `CTestTestfile.cmake`, so a `MG_Test/Purity/CTestTestfile.cmake` **is** included — but the test then runs on the `test` job's runner, where `scripts/check_include_closure.py` exists (the job does `actions/checkout@v6` at 155-156) yet `3rdparty/` and `include/ska` do **not** (no submodules). In that environment `--mode clang` will fail on a missing `<ska/flat_hash_map.hpp>`. **Handle this explicitly**: either make the ctest `--mode text` only (always safe), or have the script detect the missing include roots and fail with a clear "run this from a full checkout" message rather than a compiler error. Recommendation: the ctest runs `--mode text --self-test` unconditionally and upgrades to `--mode both` only when the probe's include roots all exist.

### 6.6 Wiring as CI

**Preferred: a new standalone job `include-graph-check`**, modelled on `flatc-check`, inserted after the `flatc-check` job (i.e. after `test.yml:323`) so the file's ordering still reads gates-then-consumers:

```yaml
  # The P0.5 interface-purity gate A (ARCHITECTURE.md:501): the two extracted headers'
  # include closure, asserted on `-H` output rather than on symbols, because
  # `nm --undefined-only` is blind to "included but not called". It needs a preprocessor
  # and three submodules and nothing else - no CMake configure, no glslang sources - so
  # it does not depend on build-linux, for the same reason pipe-gates does not.
  include-graph-check:
    name: Include-closure purity gate
    runs-on: ubuntu-latest

    steps:
      - name: Checkout repo
        uses: actions/checkout@v6

      - name: Check out the three header submodules the closure needs
        # ska/flat_hash_map.hpp, xxhash.h and vulkan/vulkan.h are the only submodule
        # headers Includes.h reaches; glslang and spirv_cross are vendored under include/.
        run: git submodule update --init include/ska 3rdparty/xxHash 3rdparty/Vulkan-Headers

      - name: Install clang
        run: sudo apt-get update && sudo apt-get install -y clang-20

      - name: Include-closure assertions and negative control
        run: python3 scripts/check_include_closure.py --mode both --compiler clang++-20 --self-test
```

Drop the `Install clang` step and pass `--compiler g++` if the ~20 s apt cost is unwanted — 5.5 proves `g++ -H` gives the same in-tree answer, at the cost of not matching the shipping compiler.

**Alternative, if a separate job is unwanted:** add the two steps to `pipe-gates` after line 848 (before the citation lint). `pipe-gates` currently has no submodules and no compiler, so this adds a `git submodule update --init` and an apt install to a job whose comment (812-813) advertises "they take seconds". `--mode text` alone would preserve that property with zero new dependencies; `--mode both` would not. If the implementer wants exactly one job, run `--mode text --self-test` in `pipe-gates` and `--mode clang` in the new job.

**Do not forget** `test.yml:3-8`: add `feat/disaggregated` to the `push: branches:` list, or the gate will not run until the branch merges.

---

## 7. Summary of design decisions and their evidence

| decision | evidence |
|---|---|
| Gate name `include-graph-check` | `ARCHITECTURE.md:568` reserves it |
| `-H`, not `-MM` | `-H` carries depth → printable chain (5.1 vs 5.2); `WireLog.h:17-19` says the gate "asserts on `-H` output" |
| `-E -o /dev/null`, not `-fsyntax-only`, for the closure | 0.137 s vs 1.889 s, identical 666 lines (5.5) |
| Parse only `^\.+ ` lines | `g++ -H` appends 54 guard-advice lines (5.5) |
| `os.path.normpath` before prefix match | today's output contains `TextureState/../SamplerState/SamplerObject.h` (5.3) |
| Forbidden set = in-tree prefixes only, **not** glslang/spirv-cross | `Includes.h:59,79-83` pulls them unconditionally and `ARCHITECTURE.md:260` lets `ProgramArtifacts.h` keep `<Includes.h>` (5.6) |
| Selective submodules `include/ska 3rdparty/xxHash 3rdparty/Vulkan-Headers` | the only submodule headers in the closure; glslang/spirv_cross are vendored under `include/` (5.4); pattern from `test.yml:312-315` |
| No CMake configure needed in CI | five-flag command reproduces the closure exactly (5.4) |
| `compile_commands.json` still used, for the ctest | `CMakeLists.txt:160` makes it unconditional; 816 entries verified |
| Negative control as an always-on `--self-test` on a synthesized TU | `ROADMAP.md:7` "每个门必须能因它存在的理由变红"; `ROADMAP.md:16` "人为加回一个 `MG_State` include 能变红" |
| Empty allow-list, additions need a PR reason | the stdio gate's policy, `test.yml:831-835` |
| Third probe on `ITransport.h` | free, green today, locks the rule `Framing.h` already broke (`WireLog.h:17-20`) |
| `LABELS unit` on the ctest | `test.yml:198-203` runs `ctest -L unit --no-tests=error` |

---

## 8. The nm / `.text` per-symbol attribution report

Requirement, `ROADMAP.md:16`: "`nm`/`.text` 变化可逐符号归因". Home for the output, `ARCHITECTURE.md:507`: "符号与 `.text` 漂移每阶段作为信息性指标发布". Name reserved by `ARCHITECTURE.md:568`: `monolith-symbol-report`.

There is **no existing nm/size tooling in the tree** — `grep -rn "nm --defined\|nm -D\|size --format\|llvm-nm\|readelf"` over `*.yml *.py *.sh *.md` outside `docs/Disaggregated/` returns nothing. This is greenfield.

### 8.1 Verified commands (run against `~/w7/pipe/build-linux/libMobileGL.so`, 19,101,056 bytes)

```bash
$ nm --defined-only libMobileGL.so | wc -l
30511

$ nm --defined-only -S --size-sort libMobileGL.so | tail -3
0000000000d8abdc 0000000000026fe4 r __GNU_EH_FRAME_HDR
00000000007ffd80 000000000003e859 T _Z7yyparsePN7glslang13TParseContextE
0000000000f302e0 00000000000fa000 b _ZZN8MobileGL7MG_Util5Debug3LogEPKciS3_zE6buffer

$ nm --defined-only -S -C libMobileGL.so | grep -E " t | T " | head -3
0000000000c13240 0000000000000013 t atexit
00000000001d1c90 000000000000000b t __clang_call_terminate
0000000000ae9d20 0000000000004453 t CreateShaderModule

$ size --format=sysv libMobileGL.so | grep -E "^\.text|^Total"
.text                10792579    1868752
Total                17209819
```

So: `nm --defined-only -S` yields `addr size type name` per symbol (the per-symbol `.text` attribution); `size --format=sysv` yields the section roll-up (`.text` = 10,792,579 bytes here). Both are the right tools; note `size --format=sysv` is **per-section, never per-symbol** — the per-symbol half must come from `nm -S`.

### 8.2 Proposed `scripts/symbol_report.py`

```
python3 scripts/symbol_report.py --before <dir-or-so> --after <dir-or-so> \
        [--nm nm] [--size size] [--markdown out.md] [--json out.json] [--threshold 0]
```

For each of `libMobileGL.so` (and, when present, `libMobileGL_s.a` / `MobileGLServer`):

1. `nm --defined-only -S <so>` → `{mangled_name: (type, size)}`. Keep the mangled name as the key (stable), carry a demangled form from `nm -C` for display only. Symbols with no size (e.g. `U`, absolute) are dropped.
2. `size --format=sysv <so>` → per-section sizes, for the roll-up header.
3. Diff into four buckets, each sorted by `|Δsize|` descending:
   - **removed** (in before, absent in after)
   - **added** (absent in before, in after)
   - **resized** (same name, different size)
   - **unchanged** (count only)
4. Print a summary line and a Markdown table:
   ```
   symbol-report: .text 10792579 -> 10794112 (+1533, +0.014%)
   symbol-report: 30511 -> 30514 defined symbols: 6 added, 3 removed, 11 resized
   ```
5. **Attribution.** For P0.5 specifically, the interesting bucket is *renames caused by de-nesting*: `ProgramObject::LinkArtifacts` → a namespace-scope `LinkArtifacts`. Give the script a `--rename-map old=new` (repeatable) or a `--demangle-normalize` pass that strips `ProgramObject::` before matching, so a pure move shows up as **0 added / 0 removed / N renamed, byte-identical sizes** rather than as N+N churn. That is precisely what "逐符号归因" means for this phase, and without it the report will be an unreadable wall of mangled churn.
6. Exit 0 always in the informational form; provide `--fail-on-added-symbol-bytes N` for a future hard gate. `ARCHITECTURE.md:507` classes this as informational for now.

Guard rails to encode in the script and its docstring:
- Compare only builds with the same `CMAKE_BUILD_TYPE` — the visibility presets differ between Debug and non-Debug (`CMakeLists.txt:572-583`, `622-635`), which changes `nm --defined-only` output wholesale.
- `MOBILEGL_ENABLE_LTO` (default OFF, `CMakeLists.txt:108`) must be OFF on both sides; `-ffunction-sections`/`--gc-sections` only appear with it (137-146).
- Note in the output which compiler and build type produced each side; a clang-20 vs clang-22 comparison is meaningless.

### 8.3 Where the output goes

A `monolith-symbol-report` job, `needs: [build-linux]` plus a second build of the *base* commit — or, cheaper for P0.5: a step inside `build-linux` that runs the script against the artifact of the previous run is not available without extra plumbing, so the practical first version is **local**: the implementer builds `HEAD~1` and `HEAD` in two WSL build dirs and runs the script, pasting the summary into the commit message / PR body, with the full Markdown attached as a CI artifact when the job exists.

For the CI form, follow the existing artifact idiom (`test.yml:140-146`, `290-296`):
```yaml
      - name: Upload symbol report
        uses: actions/upload-artifact@v7
        with:
          name: monolith-symbol-report
          path: ci-artifacts/symbol-report.md
          if-no-files-found: error
```
and additionally echo the summary into `$GITHUB_STEP_SUMMARY` so the numbers are visible without downloading (no existing job does this yet — it would be a new, welcome idiom for an informational metric).

---

## 9. Open items for the implementer

1. **`feat/disaggregated` is not in `test.yml:3-8`'s trigger list.** Any gate added now is dormant until that changes or someone runs `workflow_dispatch`. Fix this in the same commit or the gate's "green" is meaningless.
2. **Decide the two forbidden lists precisely** before writing the headers — `ARCHITECTURE.md:260` gives the minimum (`MG_State/GLState` for probe A; `ShaderObject.h` + `SpvcSession.h` for probe B); section 6.1 proposes tighter supersets. A too-tight list that the extracted header cannot satisfy will stall P0.5.
3. **`MAX_DRAW_BUFFERS`, `SamplerParameters`, `BorderColorForm`, `VertexAttribute`, `VertexBufferBindingPoint`** are named in `ARCHITECTURE.md:260` but are not in `RenderState.h`; locate them and check whether *their* current homes pull `MG_State/GLState` transitively, because they move into the same value header.
4. **The five reflection types are nested in `class ProgramObject`** (`ProgramObject.h:44,76,1146,1210,1409`). De-nesting them renames every mangled symbol that mentions them — plan the `--rename-map` of 8.2 before running the first report, or the "逐符号归因" gate will look catastrophic.
5. `ARCHITECTURE.md:563` cites `MobileGL/MG_Test/CMakeLists.txt:93-95` for the `MOBILEGL_BUILD_DISAGGREGATED` guard; in this worktree it is at **100-103**. Re-pin that citation when the doc lint goes `--strict` (`test.yml:849-861`).
6. Consider whether the gate should also run in the `MOBILEGL_BUILD_DISAGGREGATED=1` configuration. The closure of `MGPipeTypes.h` was measured with `-DMOBILEGL_BUILD_DISAGGREGATED=1` present and absent and was 666 lines either way (5.4), so today it makes no difference — but `Init.cpp`'s branch and `MG_Config::Transport` do change, so a probe that ever reaches `Config.h` would. Run both, it costs 0.3 s.
