# P1 package `fix-tests` — F3 only (`p1/fix-tests`, WSL `~/w7/p1-fix-tests`)

Branch `p1/fix-tests`, branched from the integrated `feat/disaggregated @ 97b997d5`.
Owns finding **F3** and nothing else. F1, F2 and F4 belong to other packages and were not touched.

## Commits

| sha | subject |
|---|---|
| `c67c47f7` | `[Test] (Pipe): let a test that drives a backend helper directly declare the verb it stands in` |

Diff against the branch point (`git diff --stat 97b997d5..HEAD`):

```
 MobileGL/MG_Test/Framebuffer/FramebufferTest.cpp | 30 ++++++++-
 MobileGL/MG_Test/SanityTest.cpp                  | 18 +++++-
 MobileGL/MG_Test/ScopedPipeVerb.h                | 78 ++++++++++++++++++++++++
 MobileGL/MG_Test/Texture/TextureTest.cpp         |  7 +++
 4 files changed, 128 insertions(+), 5 deletions(-)
```

Nothing outside `MobileGL/MG_Test/**` was touched; `MG_Test/Pipe/**` (the other package's) was
not touched either; no production source, no `scripts/`, no CMake, no fixture.
`MG_IntegrationTest/**` was not touched: the helper is a header-only class used only by unit
targets, and every one of them already has `${MGL_ROOT}/MobileGL` on its include path, so no
shared helper needed to move there.

## What the fix is

`MobileGL/MG_Test/ScopedPipeVerb.h` — `MobileGL::MG_Test::ScopedPipeVerb`, an RAII
"this test is standing inside verb X" object:

* **ctor** calls the real `MG_Pipe::MGPipeFillForVerb(verb)` — the exact call `MG_Impl` makes
  before that verb reaches a backend. Nothing is bypassed and nothing is faked: the block is
  filled out of the live `GLContext` through the production filler.
* **`Renew()`** re-fills the same verb, for a test that drives the helper again after moving
  frontend state — that is what a second GL entry point's `MGP_FILL` would have done.
* **dtor** re-arms the poison with a `kQuery` fill (`GetGpuTimestampNs`, whose class mask is a
  single field): the serial bumps and every field the scope stamped goes stale. This matters
  when the suite runs as one process — a developer running `./FramebufferTest` directly rather
  than one ctest entry per case — where otherwise one case's declaration would silently cover a
  later case that forgot to make one.
* **pull build**: the whole class body is `#if MOBILEGL_PIPE_PUSH`-gated to nothing; the
  constructor still takes an `MGPipeVerb` (the enum is unconditional in `MGPipe.h`), so call
  sites are identical in both arms and cannot rot.

Placement rule, copied from the brief's D7 for `MGP_FILL`: the declaration goes **immediately
before** the backend call, after every frontend mutation that call is meant to see. A call of a
different verb *class* in the same test gets its own nested scope (`{ ScopedPipeVerb clear(Clear); ... }`).

It is not a global escape hatch, the poison was not weakened, and no test name changed.

### Where it is used (exactly the eleven failing entries, no others)

| file | test | verb declared | why |
|---|---|---|---|
| `MG_Test/SanityTest.cpp` | `DirectGLESSanity.BindsAMultisampleTextureDespiteTheDefaultMipmapFilter` | `DrawArrays` | `DirectGLES::BindCurrentTextures()` is the per-draw unit walk — it reads `GetProgramForDraw`, a `kDraw`-only field |
| `MG_Test/SanityTest.cpp` | `DirectGLESSanity.BindingZeroClearsPreviousNativeTextureBinding` | `DrawArrays` + 2×`Renew()` | three walks = three draws; the third stands after `glBindTexture(GL_TEXTURE_2D, 0)` |
| `MG_Test/SanityTest.cpp` | `DirectGLESTextureSync.UnitMemoRefusesToDriveATwinFromAnotherTexture` | `DrawArrays` + `Renew()` | `SyncNeccessaryTextures()` is the per-draw unit walk; the second fill re-reads the memo keys, which is the premise (the silent slot swap moved none of them) |
| `MG_Test/Framebuffer/FramebufferTest.cpp` | `DrawIntoAWidenedDrawBufferReachesTheDriverWithAlphaWritesMaskedOff` | `DrawArrays` | `SyncRenderState(forColorClear=false)` is the draw arm |
| `…` | `ClearIntoAWidenedDrawBufferKeepsAlphaWritableAndSubstitutesOne` | `DrawArrays`, then `Clear` (nested scopes) | the test issues a draw then a clear; the clear half must go through on `kClear`'s fill set alone |
| `…` | `ApplicationAlphaMaskOffIsStillHonouredOnANativeDrawBuffer` | `DrawArrays` | |
| `…` | `DualSourceBlendFactorsReachTheDriverWhenTheExtensionIsThere` | `DrawArrays` | |
| `…` | `DualSourceBlendIsDeclinedRatherThanThrownWhenTheExtensionIsMissing` | `DrawArrays` + `Renew()` | two draws, `glBlendFunc` between them |
| `…` | `DualSourceFactorsAreDeclinedEvenWithBlendingDisabled` | `DrawArrays`, `Clear` (nested), `Renew()` | draw / clear / draw |
| `…` | `DualSourceFactorsWithBlendingDisabledStillReachACapableDriver` | `DrawArrays` | |
| `MG_Test/Texture/TextureTest.cpp` | `TextureTest.StorePackedWordsToClientCopiesWordsVerbatimUnderPackParams` | `ReadPixels` + `Renew()` | `ReadbackImpl::StorePackedWordsToClient` reads the PACK block; two stores with `glPixelStorei` between |

## Verification — commands and actual output

Environment: WSL Arch, `CCACHE_BASEDIR=/home/swung/w7`, `cmake --build <dir> -j 12`.

### Baseline before the fix (the failure being fixed, reproduced)

```
ctest --test-dir build-linux -L unit --no-tests=error -j 8   # rc=0  100% passed, 0 failed out of 1482
ctest --test-dir build-push  -L unit --no-tests=error -j 8   # rc=8   99% passed, 11 failed out of 1482
ctest --test-dir build-verify -L unit --no-tests=error -j 8  # rc=8   99% passed, 11 failed out of 1482
```

The 11 in both are exactly the F3 list (push reports `Failed` / one `SEGFAULT` — that build has
`MOBILEGL_PIPE_POISON == 0`, so it reads the *zeroed* block rather than aborting; verify reports
`Subprocess aborted`, the poison Fatal).

### (a) `ctest -L unit` green in all three build dirs

```
cmake --build build-linux  -j 12   # 0
cmake --build build-push   -j 12   # 0
cmake --build build-verify -j 12   # 0
ctest --test-dir build-linux  -L unit --no-tests=error -j 8   # rc=0  100% tests passed, 0 tests failed out of 1482
ctest --test-dir build-push   -L unit --no-tests=error -j 8   # rc=0  100% tests passed, 0 tests failed out of 1482
ctest --test-dir build-verify -L unit --no-tests=error -j 8   # rc=0  100% tests passed, 0 tests failed out of 1482
```

Additionally, each affected binary run **whole, in one process** (verify build), which is the
case the destructor's re-arm protects:

```
build-verify/MobileGL/MG_Test/SanityTest                 exit=0   82 tests, PASSED
build-verify/MobileGL/MG_Test/Framebuffer/FramebufferTest exit=0   61 tests, PASSED
build-verify/MobileGL/MG_Test/Texture/TextureTest         exit=0  192 tests, PASSED
```

### (b) pull build symbol-identical to `~/w7/p1-before-libMobileGL.so`

```
python3 scripts/symbol_report.py --before ~/w7/p1-before-libMobileGL.so \
                                 --after build-linux/libMobileGL.so --threshold 0
```

```
symbol-report: before: /home/swung/w7/p1-before-libMobileGL.so (19100448 bytes on disk)
symbol-report: after : build-linux/libMobileGL.so             (19100448 bytes on disk)
symbol-report: .text 10792579 -> 10792579 (+0, +0.000%)
symbol-report: .data 76824 -> 76824 (+0)   .bss 1296872 -> 1296872 (+0)   .rodata 1534938 -> 1534938 (+0)
symbol-report: 27799 -> 27799 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
```

**0 / 0 / 0 / 0, `.text` delta 0.** (Expected by construction: the library contains no test code,
and the helper is a no-op header in the pull build anyway.)

### (c) `ctest -N` name diff

```
ctest --test-dir build-linux -N > ft-N-raw.txt
sed -n 's/^ *Test *#[0-9]*: //p' ft-N-raw.txt | sort > ft-names-linux.txt   # 2360 names
comm -23 ~/w7/p1-before-ctest-names.txt ft-names-linux.txt | wc -l          # 0 removed
comm -13 ~/w7/p1-before-ctest-names.txt ft-names-linux.txt                  # 26 added
```

**Zero removed.** The 26 additions are all from the earlier P1 packages, already merged at the
branch point — **this package adds none**:
`DirectGLES|DirectVulkan.PipeVerifyArmingScenario.{Armed,CorruptedFieldIsReported}`,
`DirectGLES|DirectVulkan.PoisonOmissionScenario.{OmittedFieldAbortsOnThatVerb,TheSequenceThePoisonControlsRun,WithoutOmissionCompletes}`,
`PipeCatalogue.{FloatVectorsCompareBitwise,SixValueStructsHaveFieldLists,StickyFieldsAreExactlyTheSeven,VerbTableIsTheFunctionTable}`,
and the 13 `PipeInputsTest.*`.

`build-push` registers the same 2360 names (0 added / 0 removed vs `build-linux`). `build-verify`
registers 3178 — the extra 818 are the `integration-verify` lane that `MOBILEGL_ITEST_REQUIRE_GPU=ON`
turns on in that configuration, not a name change from this package.

### (d) the helper is not a rubber stamp

Two mutations of `FramebufferTest.DrawIntoAWidenedDrawBufferReachesTheDriverWithAlphaWritesMaskedOff`
in `build-verify`, run directly with `MOBILEGL_LOG_FILE_PATH` set so the Fatal line is readable:

**A — the declaration deleted from that one test:**

```
exit=134 (SIGABRT)
Fatal{UnmigratedPipeInput, "GetRenderStateParametersVersion@<none>"}
```

**B — the declaration kept but naming the WRONG verb class** (`GenerateMipmap`, `kTextureOp`,
whose mask does not carry the render-state fields):

```
exit=134 (SIGABRT)
Fatal{UnmigratedPipeInput, "GetRenderStateParametersVersion@GenerateMipmap"}
```

B is the stronger half: the helper does not make the poison tolerant, it fills exactly the named
verb's class mask, and a read outside that mask still aborts naming the field **and the declared
verb**. The fill table remains the only thing that says what a verb may read.

**C — restored:** `exit=0`, `100% tests passed, 0 tests failed out of 1`, and
`git status --porcelain -- MobileGL/ scripts/ CMakeLists.txt` is empty. The tree ends clean at
`c67c47f7`.

## F2 enumeration

Not owned by this package. F1, F2 and F4 were not touched.

## Deviations

1. **Tree path.** The tree is `~/w7/p1-fix-tests` (the task text said `~/w7/fix-tests`). Branch,
   branch point (`97b997d5`) and ownership are as briefed. Tree wins.
2. **The three build directories did not exist.** The task said the tree has `build-linux`,
   `build-push` and `build-verify` configured; it had none. I configured them myself, copying
   `~/w7/pipe`'s cache flags exactly (Release, `/usr/sbin/clang++`, ccache, INFO, `BUILD_TEST=ON`,
   `BUILD_INTEGRATION_TEST=ON`, mesa EGL vendor + lavapipe ICD, `CMAKE_POLICY_VERSION_MINIMUM=3.5`;
   `MOBILEGL_PIPE_PUSH=ON` for push, `MOBILEGL_PIPE_VERIFY=ON` + `ITEST_REQUIRE_GPU=ON` for verify,
   matching `~/w7/pipe/build-verify`).
3. **`-DMOBILEGL_BUILD_BENCHMARK=OFF` added to all three configures.** With benchmark left at its
   default the configure dies in the fetched Google Benchmark sub-project
   (`CMake Error: CMAKE_CXX_COMPILER not set, after EnableLanguage` from its
   `check_cxx_compiler_flag`). `~/w7/pipe`'s three caches all carry
   `MOBILEGL_BUILD_BENCHMARK:BOOL=OFF`, so this reproduces the reference trees rather than
   changing anything. A transient `googletest` clone failure on the first `build-verify` configure
   was cleared by deleting `build-verify/_deps/googletest-{src,subbuild}` and re-configuring.
4. **`ctest -N` baseline.** Acceptance (c) does not name a baseline file; I used
   `~/w7/p1-before-ctest-names.txt` (the pre-P1 list), which is the strictest available and shows
   both that nothing was removed and that the only 26 additions in the whole lane came from the
   other packages.
5. **Extra demonstration.** Acceptance (d) asks only for the removal case (A). I also ran the
   wrong-verb case (B), because "removing it makes the test fail" alone would also hold for a
   rubber stamp that filled everything; B is what shows it does not.
6. **Destructor behaviour is a design choice not spelled out in the brief.** Leaving the scope
   re-arms the poison (a one-field `kQuery` fill) rather than doing nothing. Rationale in the
   header and above; without it a single-process run of a test binary would let one case's
   declaration cover a later case that made none. It uses only the public filler — no production
   source, and no new API, was needed.

## Unfinished

Nothing in this package's scope.

Out of scope, and still open where the findings file left them: **F1** (missing `FillPoints.def`
rows), **F2** (`GetSamplingResolutionGeneration` going stale mid-verb) and **F4** (the
integrator's command corrections). Consequently the `integration-verify` lane and the 79-case
verify retrace were **not** run here — they are still red for F1/F2 reasons this package does not
own, and running them would prove nothing about F3. The only lane this package's change can
affect is `ctest -L unit`, which is green in all three build directories.
