# p05-value-types — adversarial review v1

Reviewed: `~/w7/p05-value-types`, branch `p05/value-types`, HEAD `92e925f9` (one commit on `6672778b`).
Reference: `~/w7/pipe` @ `6672778b` (read-only; untouched — `git status` clean apart from its build dirs).
Review scripts: `scratchpad/wf3/p05-review-vt/s1..s6.sh`; logs under `~/w7/logs/review-vt/`.
Tree state at exit: `git status --short` prints only `?? build-linux/` (identical to the implementer's state); the temporarily copied gate script was removed; the red-for-its-reason experiments were done on out-of-tree copies (`~/w7/logs/review-vt/red*/`), never in the tree.

**Verdict: APPROVED — 0 majors, 3 minors.** The claim "correct, complete, pure move meeting the brief" survived every attempt to refute it.

---

## 1. What I tried to refute, and what I observed

### 1.1 "Cut, not copy" — verbatim-move proof (s2)

Reconstructed the moved regions from the base commit and diffed them against the new header:

```
git show 6672778b:.../RenderState.h    | sed -n '15,370p'   -> old_rs.txt
git show 6672778b:.../SamplerObject.h  | sed -n '14,97p'    -> old_so.txt
git show 6672778b:.../VertexArrayObject.h | sed -n '17,70p' -> old_vao.txt
```
- Non-vertex part (`old_rs + old_so`, with the two `MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS` → `kMGMaxDrawBuffers` respellings applied) vs `MGPipeValueTypes.h:32-471`: `diff -B` **exit 0** — byte-exact including indentation and comments.
- Vertex part (`old_vao` de-indented by 4) vs `MGPipeValueTypes.h:474-529`: `diff -B` **exit 0**.
- Whole region `diff -w -B`: the only extra lines are the three the brief prescribes (`namespace MG_State::GLState {`, `class BufferObject;`, closing brace).
- Banner `MGPipeValueTypes.h:1-7` == `MGPipeTypes.h:1-7` with the path substituted (`banner-ok`).
- Deleted "extra" lines (`RenderState.h:371`, `SamplerObject.h:97`, `VertexArrayObject.h:71`) are all blank (`cat -A` shows `$`), so deviation 1 of the result file is whitespace-only as declared.

Member order, defaults, enum underlying types (`BorderColorForm : Uint8` at `MGPipeValueTypes.h:441`), `constexpr`-ness (`MAX_VIEWPORTS` at `:244`), namespaces (`MG_State::GLState` reopened at `:473`, D3) — all unchanged by construction of the diff above.

### 1.2 ODR — no second definition anywhere

- Brief grep (8 structs + 3 enums) over `MobileGL/`: **11 hits, all in `MobileGL/MG_Pipe/MGPipeValueTypes.h`** (lines 32, 168, 208, 219, 229, 239, 441, 447, 476, 517, 525).
- Extended grep for **all 17 moved enums** over `MobileGL tools android-plugin` (`*.h *.hpp *.cpp *.inc`, build dirs excluded): 17 hits, all in the new header.
- `MAX_DRAW_BUFFERS\s*=`: exactly one, `FramebufferObject.h:107` `= MobileGL::kMGMaxDrawBuffers` (D4). `kMGMaxDrawBuffers` appears 8 times (header, tests, FramebufferObject.h); none in `MG_Backend`.

### 1.3 Purity / closure (s3, s4)

- Text: `grep -n "MG_State/\|MG_Backend/\|MG_Impl/\|MG_Remote/" MGPipeValueTypes.h` → **empty** (exit 1).
- Includes of the four forwarding headers: `RenderState.h:10-11` = `<Includes.h>` + `<MG_Pipe/MGPipeValueTypes.h>` only; `SamplerObject.h:10-11` same; `VertexArrayObject.h:10-13` keeps `BufferObject.h`/`Types.h` + value header; `FramebufferObject.h:14` adds the value header.
- Compiler closure, brief's exact flags, `-H -E` on `#include <MG_Pipe/MGPipeValueTypes.h>`: **654 lines, 0 under `MobileGL/MG_State/ | MG_Backend/ | MG_Impl/ | MG_Remote/`**. `MobileGL/` headers in the closure: `Defines.h, Includes.h, MG_Pipe/MGPipeValueTypes.h, MG_Util/{Debug/Log.h, GLExtensions.h, Math/VectorTypes.h, PlatformStubs.h, Types.h}`. `-fsyntax-only` exit 0 (self-contained).
- **Real gate run** (copied `~/w7/p05-include-gate/scripts/check_include_closure.py` into `scripts/` for the run, removed afterwards; `REPO_ROOT` is derived from the script location so it had to be in-tree):
  - `--mode both --self-test`: `value-header OK 66 headers (text)`, `OK 654 headers (clang)`, `SELF-CONTAINED OK`; `artifacts-header SKIP` (expected, other package); `wire-header OK`; `self-test: 4 negative-control trip(s), parser checks OK`; `3 probes, 1 skipped, 0 problem(s)`, exit 0. The 4 trips = self-test steps 1 and 2 (step 2 is the roadmap's literal "add an `MG_State` include back and it goes red", which only runs once `MGPipeValueTypes.h` exists — it does now, and it trips) in both modes.
  - `--require-all`: exit 1, names only the missing `ProgramArtifacts.h` (D9 semantics correct; the value header itself is not the reason).
  - `--probe value-header`: `1 probes, 0 skipped, 0 problem(s)`.
- **Red-for-its-reason, by hand** (out-of-tree shadow copy at `~/w7/logs/review-vt/red/MG_Pipe/MGPipeValueTypes.h` with `#include <MG_State/GLState/RenderState/RenderState.h>` inserted after `:15`, put first on the `-I` path): the same `-H` probe reports **1 forbidden line**. Restored = nothing to restore; the tree was never edited.
- **Tripwire fires**: shadow copy with `sizeof(RenderStateParameters) == 1` → `error: static assertion failed ... note: expression evaluates to '1168 == 1'`.

### 1.4 Behaviour unchanged (s3)

- `cmake --build build-linux -j 16`: `ninja: no work to do` (the implementer's build is current).
- `ctest -L unit --no-tests=error -j 8`: **100% passed, 0 failed of 1460**, 11.0 s. The two new tests ran inside the `-L unit` selection (`#1053`, `#1054` Passed) — they are labelled, not orphaned.
- `ctest -N` names, `comm -23 before after`: **lost: none**; added exactly `PipeCatalogue.ValueTypeLayoutsArePinned`, `PipeCatalogue.ResidualBlockIsExactlyItsTwoValueStructsPlusPatchTail` (1327 → 1329).
- `python3 scripts/gen_pipe.py --check`: `generated files are up to date`, exit 0; tree still clean afterwards.
- `nm --defined-only -S` keyed on mangled name (type+size): **30511 → 30511, diff empty**; with addresses included: diff empty. `size --format=sysv`: `.text 10792579` and `Total 17209819` on both sides. `objcopy --only-section=.text` + `cmp`: **`.text` byte-identical**. The baseline `.so` carries a `GIT@a4baa9d` configure stamp (implementer already attributed it); since `.text` is byte-identical to a build known to be at `6672778b`+header-move, the baseline is a valid `6672778b` build.
- `grep -rc "pGLContext" MobileGL/MG_Backend | sort` identical to pipe; `git diff --stat 6672778b -- MG_Backend MG_Impl MG_Util` empty.

### 1.5 Lost transitive includes on platforms that were not built

- 27 files include the touched headers; every `.cpp` among them (8) is in `build-linux/compile_commands.json`.
- 31 `.cpp` under `MobileGL/` are not built on Linux (Android JNI, WGL/CGL/NSOpenGL, MG_Remote split-only, benchmarks, a dead `VkTextureSamplerManager.cpp` referenced by no CMake file). Inspected their includes: none includes `RenderState.h`/`SamplerObject.h`/`VertexArrayObject.h` directly; the JNI ones go through `MG_Impl/GLImpl/*` and `Includes.h` (same path the Linux build exercises). The 18 split-only TUs (`~/w7/pipe/build-linux-split`) are all `MG_Remote/Transport/*` and `MG_Test/Wire/*`, which by the wire-header rule do not include `Includes.h` at all. No plausible Android/Windows/split fallout found.

### 1.6 Wiring, formatting, docs

- No CI format gate exists (`grep clang-format .github/workflows/*.yml` empty); `clang-format --dry-run` on the new header nevertheless reports **0 warnings** (clang-format 22.1.6, repo `.clang-format`). No trailing whitespace in the diff (`grep -P '^\+.*[ \t]+$'` count 0); LF endings, newline at EOF, mode 100644.
- `scripts/check_doc_citations.py` (base tree, the `test.yml:853-861` lint, which is `|| true` anyway) on the three `docs/Disaggregated/*.md`: `19 citations ... 0 problem(s)`. `MGPipeTypes.h` line numbers are unchanged (the comment rewrite is line-for-line), so `ARCHITECTURE.md:359`'s `MGPipeTypes.h:535` still points at `MGL_RESIDUAL_BLOCK_SIZE`.
- Commit message: prescribed subject verbatim + `- ` bullets explaining why; no attribution lines; not pushed; no fixture binaries touched.
- Deviations 1-7 in the result file were each checked and are as declared (whitespace, comment wording, additional-only test expectations, tooling not present in this tree).

## 2. Majors

None.

## 3. Minors

1. **`MGPipeTypes.h:16-18` overstates the repaid debt.** The rewritten comment says "this header no longer reaches into MG_State". The `-H` closure of `#include <MG_Pipe/MGPipeTypes.h>` still contains `.. MobileGL/MG_Backend/BackendObject.h` → `... MobileGL/MG_State/GLState/TextureState/TextureEnum.h` (`BackendObject.h:11`), exactly the D1 reason the brief gives for not moving `DynamicBackendParameters`. The following sentence ("that one include is what still keeps purity gate A off this header") is correct, so nothing functional depends on it, but the first sentence should read "no longer includes MG_State directly; the remaining MG_State reach is `TextureEnum.h` via `BackendObject.h`". Comment-only; fix in the integrator's docs/comment pass or a follow-up, not a reason to hold the package.
2. **`PipeCatalogue.ValueTypeLayoutsArePinned` is tautological on the CI toolchain** (`PipeCatalogueTest.cpp:117-135`): every `EXPECT_*` compares the same compile-time constants the header's `static_assert`s already pin, so on Linux/libstdc++ it cannot fail without the build already failing. This is exactly what the brief asked for ("runtime twins ... so the numbers show up in ctest output on every platform"), and it does earn its keep on MSVC/NDK where a static assertion might be `#if`'d out later; recorded so nobody counts it as an independent gate.
3. **Result-file line range `SamplerObject.h:14-96` vs brief `14-97`**: line 97 at `6672778b` is blank, so the cut is the brief's cut; only the description differs. No action.

## 4. Notes for the integrator (not findings against this package)

- The package's `build-linux/CMakeCache.txt` has `MOBILEGL_ITEST_VK_ICD=` empty (implementer's Program203 diagnosis checks out against `MG_IntegrationTest/CMakeLists.txt:284-298`); reconfigure the three p05 build dirs with the lavapipe ICD before relying on a plain `ctest -L integration-gpu`.
- With this package merged after include-gate, `check_include_closure.py --mode both --self-test` already reports `value-header OK` in both modes plus the step-2 negative control tripping; only `artifacts-header` remains `SKIP` until program-artifacts lands.
