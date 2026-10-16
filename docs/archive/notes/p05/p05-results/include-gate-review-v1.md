# Adversarial review — package `p05-include-gate` (v1)

Tree `~/w7/p05-include-gate`, branch `p05/include-gate` = `fe3dc1dd` + `2318f6ae` on `6672778b`.
Reviewer stance: refute "correct, complete, pure, meets the brief (A, B.0, B.3, C, D)".
Verdict: **approved — 0 majors, 9 minors.** Tree left exactly as found (`git status --porcelain --untracked-files=all | grep -v build-linux/` empty before and after every experiment).

All scripts used are under `scratchpad/wf3/p05-include-gate-review/s01..s07.sh`; long outputs under `~/w7/review-p05g/`.

## 1. Scope and purity of the diff

`git diff --stat 6672778b..HEAD` = exactly the five B.4-assigned files, 1089 insertions, 0 deletions:
`.github/workflows/test.yml` (+26), `MobileGL/MG_Test/CMakeLists.txt` (+3), `MobileGL/MG_Test/Purity/CMakeLists.txt` (+15), `scripts/check_include_closure.py` (+659), `scripts/symbol_report.py` (+386).
`git diff --stat 6672778b -- MobileGL/MG_Backend MobileGL/MG_State MobileGL/MG_Pipe` empty. No fixture binaries. No CRLF (`xargs grep -lU $'\r'` empty). No attribution trailers in either commit message (`git log --format=%B | grep -i "co-authored\|signed-off"` empty); subjects are `[Type] (Scope): …` with `- ` bullets.
No C++ was added, so "copied vs moved", member order, alignment, constexpr-ness and static_assert coverage are not applicable to this package; the symbol report on the rebuilt library is 0/0/0/0 (section 3).

## 2. Spec conformance (B.3) — line-by-line against `scripts/check_include_closure.py`

| B.3 item | where | verdict |
|---|---|---|
| `REPO_ROOT` idiom, `description=__doc__`, `include-closure: ` prefix, `::error::` + exit 1 | `:62,:64,:161-166,:544,:651-653` | as specified |
| `PROBES` table with Name/Header/Tu/Forbidden/Allow/TextLimits/Why, values identical to B.0 | `:72-118` | byte-compared against the B.0 table: identical (six forbidden prefixes for artifacts incl. the `ProgramState/Shader` prefix, `glslang::` ≤ 2) |
| flags `--mode/--compiler/--compile-commands/--probe/--self-test/--require-all/--json` | `:546-558` | all present; default mode `text` |
| compiler order `$CXX, clang++-20, clang++, c++, g++`, printed | `:141,:286-303,:591` | as specified |
| default flags = the measured 6, cwd = REPO_ROOT | `:125-132,:576` | identical to the brief's line |
| prereq check that refuses rather than downgrades | `:135-139,:340-345` | verified: without the submodules, `--mode clang` exits 1 with the exact `git submodule update --init …` message (s06) |
| `-H` parse: stderr only, `^(\.+) (.*)$`, normpath, outside-repo ignored | `:156,:169-194,:348-352` | as specified; g++ trailer verified live and by canned transcript |
| text mode: literal walk, `"…"` includer-dir first then the 5 search dirs, unresolved = leaf | `:145-151,:197-241` | as specified |
| `--mode both` disagreement is a failure printing both sets | `:624-631` | verified (experiment B3) |
| `-fsyntax-only` second assertion, own line | `:410-424,:632-634` | verified |
| SKIP semantics + `--require-all` | `:603-615` | verified (section 4) |
| manifest assertion for both required probes | `:121,:561-566` | present |
| self-test 1–5, tempdir only, never touches a tracked file | `:431-540,:572` | verified incl. controls 2/3 and the third-`glslang::` control, which only arm when the headers exist (experiments B1, C1) |
| CMake stanza | `MG_Test/Purity/CMakeLists.txt` | verbatim to the brief; inserted after `add_subdirectory(Pipe)` at `MG_Test/CMakeLists.txt:91-93` |
| CI job after `flatc-check`, before `benchmark`, steps verbatim | `test.yml:325-349` | YAML parses (Windows PyYAML): 13 jobs, neighbours `['flatc-check','include-graph-check','benchmark']`, no `needs`, triggers untouched (D10) |

`scripts/symbol_report.py`: every flag in the spec list exists (`:269-289`); `nm --defined-only -S` / `c++filt` / `size --format=sysv` (`:131-138`); buckets sorted by |Δ| (`:189-191`); the two summary lines + Markdown (`:324-363`); always exit 0 (`:382`); guard rails in the docstring (`:24-31`); `--self-test` with two canned transcripts (`:209-263`).

Declared deviations (result file §3, items 1–6) were each checked and are accepted: `--strip-scope` de-nesting is the only semantics that produces the fold the brief asks for (a literal prefix delete would put the two spellings in removed/added); one SKIP line per probe; disagreement check on probes only; the extra normalised-names line; local clang 22/g++ 16 vs CI clang-20; cosmetic YAML spacing.

## 3. Independent re-run of the verification list (all in `~/w7/p05-include-gate`, s04)

```
cmake --build build-linux -j 16                        -> [47/47] … (incremental, clean)
ctest --test-dir build-linux -L unit --no-tests=error -j 8
                                                        -> 1459 tests, 0 failed (2 skipped LogLevel.* as on base)
ctest -N name diff vs ~/w7/pipe/build-linux            -> lost: (none); added: MobileGLPurity.IncludeClosure ; 1327 -> 1328
ctest --test-dir build-linux -L unit -N | grep -c MobileGLPurity   -> 1
python3 scripts/gen_pipe.py --check                    -> generated files are up to date, exit 0
python3 scripts/check_include_closure.py --mode both --self-test              -> exit 0, "3 probes, 2 skipped, 0 problem(s)", 2 trips
python3 scripts/check_include_closure.py --mode text --self-test              -> exit 0, 1 trip
python3 scripts/check_include_closure.py --mode both --self-test --require-all -> exit 1, two "SKIP is a failure under --require-all" lines
python3 scripts/check_include_closure.py --mode both --compiler g++ --self-test -> exit 0, 2 trips
python3 scripts/symbol_report.py --self-test           -> OK (2 canned transcripts, 5 buckets)
python3 scripts/symbol_report.py --before ~/w7/pipe/build-linux/libMobileGL.so --after build-linux/libMobileGL.so
   -> .text 10792579 -> 10792579 (+0); 27799 -> 27799 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed; 27060 unchanged
ODR grep (8 value-type structs)                        -> 8 hits, all still in MG_State (nothing moved by this package, as expected)
git status                                             -> clean
```

Generated CTestTestfile (`build-linux/MobileGL/MG_Test/Purity/CTestTestfile.cmake`): `add_test(MobileGLPurity.IncludeClosure "<python3>" ".../MobileGL/MG_Test/../../scripts/check_include_closure.py" "--mode" "text" "--self-test")`, `LABELS "unit"`, no ENVIRONMENT. `build-linux/MobileGL/MG_Test/CTestTestfile.cmake:20 subdirs("Purity")`, and the runtime tarball packs `${BUILD_DIR}/MobileGL/MG_Test` whole (`test.yml:126`), so the `test` job's `ctest -L unit` reaches it the same way it reaches `Pipe`.

## 4. Gate semantics and red-for-its-reason (s05, every edit restored by an EXIT trap)

**A. Manual negative control on the live probe.** `sed` inserted `#include <MG_State/GLState/RenderState/RenderState.h>` after `ITransport.h:35`; `--mode both --probe wire-header` → exit 1, both modes `wire-header FORBIDDEN … 1 forbidden` with the identical 3-hop chain `ITransport.h → RenderState.h → Includes.h` (76 / 672 headers in closure vs 2 / 42 clean). `git checkout --` restored; 0 dirty lines.

**B. Probe A exercised with a scratch `MobileGL/MG_Pipe/MGPipeValueTypes.h`** (never run before; deleted afterwards):
- pure (`<Includes.h>`, `<MG_Util/Math/VectorTypes.h>`, `<cstddef>`, `<type_traits>`): `value-header OK 66 / 654 headers, 0 forbidden`, SELF-CONTAINED OK, **control-2 arms and trips in both modes** (9 violations, RenderState.h at depth 1), `3 probes, 1 skipped, 0 problem(s)`, exit 0. So the B.0 forbidden set is satisfiable with `Includes.h` in the closure, and the brief's layout will go green.
- `RenderState.h` added back: both modes `FORBIDDEN … 9 forbidden`, chain printed, exit 1 — the roadmap's literal red.
- `<MG_Backend/BackendObject.h>` (the D1 route): `FORBIDDEN … 2 forbidden` both modes — the gate would catch `DynamicBackendParameters` being moved after all.

**C. Probe B exercised with a scratch `ProgramArtifacts.h`** (`<Includes.h>`, `<set>`, exactly two `glslang::`): `artifacts-header OK 65 / 653, 0 forbidden`, SELF-CONTAINED OK, control-3 trips in both modes (16 violations, ShaderObject.h at depth 1), text-limit control trips (`glslang:: occurs 3 times, budget is 2`), exit 0. Then `#include "ShaderObject.h"` (quoted spelling) → `FORBIDDEN … 16 forbidden` with chain; a third `glslang::` in a comment → `FORBIDDEN`, `text limit: glslang:: occurs 3 times`. `MobileGL/Config.h` is not reached via `Includes.h` (0 forbidden), so the Config.h rule is satisfiable too.

**D. CI file-set simulation** (s06): tracked files of `MobileGL include scripts` only + the three submodules linked in (exactly what `include-graph-check` sees) → `--mode both --self-test` exit 0 with the same 42-header wire closure and 2 trips. Every header the closure reaches outside the submodules (`include/EGL/egl.h`, `include/GL/*`, `include/GLES3/gl32.h`, `include/glslang/*`, `include/spirv_cross/spirv_cross_c.h`) is `git ls-files`-tracked, so the job does not depend on `update_glslang_sources.py` or system GL headers. Same set without the submodules (the `test` job's checkout) → `--mode text --self-test` exit 0, and `--mode clang` refuses with the submodule message. `apt-get install -y clang-20` is the same mechanism `build-linux` already uses (`test.yml:61`), so the install step is no less reliable than the existing lane.

## 5. Majors

None.

## 6. Minors (none blocks; ordered by usefulness to the integrator)

1. **`flags_from_compile_commands` corrupts a two-token `-isystem DIR`** (`check_include_closure.py:327-330`): the path token is dropped and the bare `-isystem` then swallows the next flag. Repro: canned entry `-DFOO=1 -isystem /opt/sys/include -I…/MobileGL -std=gnu++23` → `['-DFOO=1', '-isystem', '-I/…/MobileGL', '-std=gnu++23']`. Latent here: this tree's `compile_commands.json` uses no `-isystem` (Init.cpp entry checked), and neither CI nor the ctest passes `--compile-commands`. Fix: treat `-isystem` like `-o` (keep both tokens).
2. **`--require-all` only inspects selected probes**: `--mode text --probe wire-header --require-all --self-test` → exit 0 (`1 probes, 0 skipped, 0 problem(s)`) with both P0.5 headers absent (`main:593-615`). Not reachable from the CI/ctest command lines, which never pass `--probe`, but the ratchet has a hole for a hand-run. `--probe value-header --require-all` correctly exits 1.
3. **Text-only ctest lane has no arbiter for `#if`-guarded includes**: an `#if 0`-guarded `RenderState.h` include in the value header gives `FORBIDDEN 9 forbidden` in text mode and `OK` in clang; `--mode both` reports MODE DISAGREEMENT (2 problems). This is the brief's design, but it means the unit ctest can go red for something the compiler never includes. Rule for the two header authors: no conditionally compiled includes of forbidden paths, ever.
4. **`count_text_limits` counts comments** (`:272-283`): the brief's own suggested comment line for `ProgramArtifacts.h` ("`glslang::` appears exactly twice below (D5)") is itself a third token and turns probe B red (experiment C2). The program-artifacts author must spell it without the `::`, or the integrator sees a spurious FORBIDDEN on the first real run.
5. **`tempfile.mkdtemp` is never removed** (`:572`); two `/tmp/mgl-include-closure-*` directories were left behind by the review runs. Harmless on CI runners; a `shutil.rmtree` in a `finally` would do.
6. **ERROR + disagreement interplay**: when clang-mode preprocessing fails for a probe, `run_probe` returns `set()` (`:387`) and the disagreement check (`:624-631`) can print a spurious MODE DISAGREEMENT on top of the ERROR. Cosmetic; the run is red either way.
7. **`symbol_report.py` folding hides a count change**: with `--strip-scope`, the copied-not-moved case (both `ProgramObject::LinkArtifacts::Reset()` and `LinkArtifacts::Reset()` present after) surfaces as `resized +16`, not `added`, and the per-name `count` (1→2) is not in any table (`:150-157,:161-192`). Visible, but mislabelled; the ODR grep stays the primary defence, as D says.
8. **`flags_from_compile_commands` takes the first entry whose absolute path contains `MobileGL/`** (`:311-313`): on the Windows main worktree (`…/FoldCraftLauncher/MobileGL/`) every entry matches, including 3rdparty TUs. WSL trees are unaffected; only matters for a developer using the flag on Windows.
9. **Manifest assertion is two literals in one file** (`:121` vs `:72-118`): it guards against accidental deletion of a probe, not against a deliberate edit that removes both. Acceptable for its stated purpose ("deleting a probe is red").

## 7. Notes for the integrator (not findings)

- Probes A and B have now been exercised end-to-end with scratch headers built from the brief's own include lists (section 4 B/C); both go OK, their controls arm, and each forbidden rule was tripped at least once. Expect `1 skipped` after value-types lands and `0 skipped` after program-artifacts.
- The `--require-all` flip (C.3) is one appended token in each of `MG_Test/Purity/CMakeLists.txt:10` and `test.yml:349`.
- `clang++-20` is absent in WSL; local runs use clang 22 / g++ 16 and produce the same 42-header wire closure as the brief's measured defaults.
