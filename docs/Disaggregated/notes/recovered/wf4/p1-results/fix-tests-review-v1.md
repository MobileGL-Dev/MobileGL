# Adversarial review — P1 package `fix-tests` (F3), branch `p1/fix-tests` @ `c67c47f7`

Reviewer ran independently in WSL `~/w7/p1-fix-tests` (the tree; the task text's `~/w7/fix-tests`
does not exist — the implementer declared this, deviation 1, and it is correct).
Tree state at start and at end: `git status --porcelain` shows only the three untracked build
directories; `git log -1` is `c67c47f7`. Nothing was left changed (two temporary mutations were
made for negative controls and reverted with `git checkout --`; verified clean afterwards).

**Verdict: approved. 0 majors, 6 minors.**

I set out to refute "correct and complete" and could not. Every claim in
`fix-tests-v1.md` reproduced, and two of them reproduce *stronger* than claimed.

---

## 1. What was re-run independently (command + observed output)

### 1.1 The diff is what it says it is

```
$ git -C ~/w7/p1-fix-tests diff --name-only 97b997d5..HEAD
MobileGL/MG_Test/Framebuffer/FramebufferTest.cpp
MobileGL/MG_Test/SanityTest.cpp
MobileGL/MG_Test/ScopedPipeVerb.h
MobileGL/MG_Test/Texture/TextureTest.cpp
```

128 insertions / 5 deletions, all inside `MobileGL/MG_Test/**`. No production source, no
`scripts/`, no CMake, no `.def`/generated file, no fixture, no `MG_Test/Pipe/**` (the other
package's area), no `MG_IntegrationTest/**`. The full diff was read line by line.

Commit message shape checked against the lane rule (subject + blank line + `- ` bullets, no
`Co-Authored-By`): conforms.

### 1.2 Build configurations are the reference ones (deviation 2/3 verified, not taken on trust)

Diffing every `MOBILEGL_*` / `CMAKE_BUILD_TYPE` / `CMAKE_CXX_COMPILER` cache entry against
`~/w7/pipe/<dir>/CMakeCache.txt`:

```
--- build-linux ---  IDENTICAL to pipe/build-linux
--- build-push ---   only  CMAKE_CXX_COMPILER:STRING vs :UNINITIALIZED  (same value)
--- build-verify --- only  CMAKE_CXX_COMPILER:STRING vs :UNINITIALIZED  (same value)
```

`MOBILEGL_BUILD_BENCHMARK:BOOL=OFF` is present in `~/w7/pipe`'s three caches too, so
deviation 3 reproduces the reference trees rather than changing them. Compile definitions
confirmed from `build.ninja`: `build-linux` has neither, `build-push` has
`-DMOBILEGL_PIPE_PUSH=1`, `build-verify` has `-DMOBILEGL_PIPE_PUSH=1 -DMOBILEGL_PIPE_VERIFY=1`.

`cmake --build {build-linux,build-push,build-verify} -j 12` → all three `ninja: no work to do`,
i.e. the committed tree is exactly what was tested.

### 1.3 Pull-build symbol report, `--threshold 0`

```
$ python3 scripts/symbol_report.py --before ~/w7/p1-before-libMobileGL.so \
                                   --after build-linux/libMobileGL.so --threshold 0
symbol-report: .text 10792579 -> 10792579 (+0, +0.000%)
symbol-report: .data 76824 -> 76824 (+0)   .bss 1296872 -> 1296872 (+0)   .rodata 1534938 -> 1534938 (+0)
symbol-report: 27799 -> 27799 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
```

G1 holds. (Trivially, by construction — the helper is header-only test code — but re-run.)

### 1.4 `gen_pipe.py --check` / `--self-test`

```
gen_pipe: 71 calls (11 screen, 60 context), 69 verify payloads, 63 PipeInputs fields (7 sticky), 69 verbs, 9 classes
gen_pipe: generated files are up to date                       rc=0
gen_pipe: self-test: 6 negative-control trip(s), positive control OK   rc=0
```

`scripts/check_include_closure.py` also clean (3 probes, 0 forbidden) — the new header does not
break the layering gate.

### 1.5 `ctest -L unit` in all three build directories

```
build-linux  rc=0  100% tests passed, 0 tests failed out of 1482
build-push   rc=0  100% tests passed, 0 tests failed out of 1482
build-verify rc=0  100% tests passed, 0 tests failed out of 1482
```

### 1.6 `ctest -N` name diff

```
build-linux 2360 names, build-push 2360, build-verify 3178
removed vs ~/w7/p1-before-ctest-names.txt: 0     added: 26
vs the integrated tree ~/w7/pipe/build-linux (2360): 0 removed, 0 added   <-- exact match
```

The 26 additions are the other P1 packages' (already at the branch point); **this package adds
none, removes none, renames none.** The `build-verify` surplus is confirmed by label census:
`integration-verify` 818 + `integration-gpu` 1696; `~/w7/pipe/build-verify` also lists 3178.
So the surplus is the configuration, not a name change.

---

## 2. The four things I tried to catch it on

### 2.1 "A test helper that makes tests pass unconditionally" — refuted, twice

**Control A (mine, stronger than the implementer's): neutralise the helper entirely.**
`sed -i 's/^#if MOBILEGL_PIPE_PUSH$/#if defined(MGL_REVIEW_NEUTRALISE) \&\& MOBILEGL_PIPE_PUSH/'`
on `MobileGL/MG_Test/ScopedPipeVerb.h` (all four guards, so ctor, `Renew()` and dtor become
no-ops in every build), rebuild `build-verify`, `ctest -L unit -j 8`:

```
99% tests passed, 11 tests failed out of 1482
  5 DirectGLESSanity.BindsAMultisampleTextureDespiteTheDefaultMipmapFilter (Subprocess aborted)
  6 DirectGLESSanity.BindingZeroClearsPreviousNativeTextureBinding
 82 DirectGLESTextureSync.UnitMemoRefusesToDriveATwinFromAnotherTexture
310 FramebufferTest.DrawIntoAWidenedDrawBufferReachesTheDriverWithAlphaWritesMaskedOff
311 FramebufferTest.ClearIntoAWidenedDrawBufferKeepsAlphaWritableAndSubstitutesOne
312 FramebufferTest.ApplicationAlphaMaskOffIsStillHonouredOnANativeDrawBuffer
313 FramebufferTest.DualSourceBlendFactorsReachTheDriverWhenTheExtensionIsThere
314 FramebufferTest.DualSourceBlendIsDeclinedRatherThanThrownWhenTheExtensionIsMissing
315 FramebufferTest.DualSourceFactorsAreDeclinedEvenWithBlendingDisabled
316 FramebufferTest.DualSourceFactorsWithBlendingDisabledStillReachACapableDriver
471 TextureTest.StorePackedWordsToClientCopiesWordsVerbatimUnderPackParams
```

Exactly eleven, and exactly F3's eleven — every declaration in the diff is load-bearing (none is
decorative), and no *other* test in the 1482 depends on the helper. So the helper cannot be
making anything pass that was not already failing for the F3 reason, and none of these eleven is
a test that cannot fail.

**Control B (mine): substitute the WIDEST verb class for a narrow one.** In
`MobileGL/MG_Test/Texture/TextureTest.cpp:4481` change `MGPipeVerb::ReadPixels` →
`MGPipeVerb::DrawArrays` (kDraw, 47 fields, the largest mask there is) and run that one test in
`build-verify` with `MOBILEGL_LOG_FILE_PATH` set:

```
exit=134 (SIGABRT)
[Linux TextureTest/FATAL]: MGPipe: Fatal{UnmigratedPipeInput, "GetPixelStoreParameters@DrawArrays"}
```

This is the decisive anti-rubber-stamp result: the ctor runs the *production*
`MGPipeFillForVerb` (`MobileGL/MG_Impl/Pipe/PipeFill.cpp:528-575`), which stamps only
`kMGPipeClassFieldMask[class of verb]` — so a read outside the declared verb's may-read set is
still `Fatal{UnmigratedPipeInput}`, naming the field *and* the declared verb, even when the
declared verb is the broadest one. `FillPoints.def` remains the only thing that says what a verb
may read. It also proves the test binary and the backend share one `gPipeInputs`: the abort came
out of the backend's accessor, on the block the test's ctor stamped.

Both were reverted with `git checkout --` and the affected targets rebuilt; the restored tests
pass and `git status --porcelain -- MobileGL` is empty.

### 2.2 "Fields / call sites the enumeration missed" — refuted

Enumerated every direct backend-helper call in the unit tree myself
(`awk` over the three files, plus a repo-wide grep):

* `MobileGL/MG_Test/Framebuffer/FramebufferTest.cpp` — 11 `SyncRenderState(` calls at
  `:1238,1271,1283,1323,1356,1377,1395,1422,1440,1453,1471`; **every one** sits inside a
  `ScopedPipeVerb` scope or after a `Renew()`.
* `MobileGL/MG_Test/SanityTest.cpp` — 4 `BindCurrentTextures()` (`:329,372,380,391`) and
  2 `SyncNeccessaryTextures()` (`:3025,3046`); all covered.
* `MobileGL/MG_Test/Texture/TextureTest.cpp` — 2 `StorePackedWordsToClient(` (`:4482,4508`);
  both covered.
* `grep -rlE 'MG_Backend::(DirectGLES|DirectVulkan)::[A-Za-z_]+::[A-Za-z_]+\('
  MobileGL/MG_Test --include=*.cpp` returns exactly those three files and no others.
* The same grep over `MobileGL/MG_IntegrationTest` returns **nothing** — integration scenarios
  drive GL entry points only, so `MGP_FILL` always fires there. The implementer's stated reason
  for not touching `MG_IntegrationTest/**` is factually right, not an excuse.

### 2.3 "A mechanism that silences the comparator" — refuted, and the package under-claims

The unit lane in `build-verify` runs with `Features.PipeVerify` **off** unless
`MOBILEGL_PIPE_VERIFY=1` is in the environment, so the implementer's green `ctest -L unit` only
exercised the poison, not the compare-at-read hook. That was the one place a wrongly *placed*
declaration (fill before a mutation the helper must see) could hide: the push value would go
stale against the live context and nobody would look. So I armed it:

```
$ MOBILEGL_PIPE_VERIFY=1 ctest --test-dir build-verify -L unit --no-tests=error -j 8
rc=0   100% tests passed, 0 tests failed out of 1482
```

and proved the arming was real rather than a silent no-op:

```
$ MOBILEGL_PIPE_VERIFY=1 MOBILEGL_LOG_FILE_PATH=/tmp/mgl-verify.log \
    ./build-verify/.../FramebufferTest --gtest_filter='*DrawIntoAWidened*'
[Linux FramebufferTest/INFO]: MGPipe: verify armed - 63 fields, 69 verbs, fatal=1
# same run without the env var: 0 occurrences of "MGPipe: verify armed"
```

With the comparator genuinely armed, every accessor read in all eleven tests returned exactly
what a live pull would have returned. That is a stronger statement than the result file makes,
and it closes the placement question completely: no `ScopedPipeVerb` in this diff is placed
before a mutation its helper is meant to see.

### 2.4 "The pull build moves" / "a global escape hatch" / "the poison is weakened" — refuted

* Symbol report 0/0/0/0, `.text` +0 (§1.3). The class body is `#if MOBILEGL_PIPE_PUSH`-gated to
  nothing (`MobileGL/MG_Test/ScopedPipeVerb.h:46,62,67`), the ctor signature is identical in
  both arms, so call sites cannot rot.
* No global escape hatch: no `#define`, no env knob, no weakening of `MGP_INPUT_CHECK`, no
  change to `PipeInputs.h`/`PipeFill.cpp`/`FillPoints.def`. The whole mechanism is "call the
  production filler for a named verb".
* Whole-binary single-process runs reproduce (the destructor re-arm's stated purpose):
  `SanityTest` exit=0 82 tests, `FramebufferTest` exit=0 61 tests, `TextureTest` exit=0
  192 tests, 0 `[  FAILED  ]` lines each. With the helper neutralised the same three binaries
  are 134/1/1 with 11 and 3 failures — i.e. the re-arm is not covering anything up.

---

## 3. Minors

1. **Nested scopes silently invalidate the enclosing declaration, and the header does not say
   so.** `MobileGL/MG_Test/ScopedPipeVerb.h:68` — the destructor bumps `CurrentVerbSerial`, so
   closing an inner scope makes the *outer* scope's stamps stale. The two current sites survive
   only because a `draw.Renew()` happens to follow
   (`FramebufferTest.cpp:1453` after the `Clear` scope at `:1439-1441`). The header's guidance
   at `:38-42` documents `Renew()` as "a second verb of the same kind begins" and nested scopes
   as the way to handle another class, but never states the interaction. A future author who
   nests a scope and then calls the outer helper again gets an abort. One sentence in the
   `Renew()` comment would fix it.

2. **The exit fill makes the next poison Fatal name a misleading verb.**
   `ScopedPipeVerb.h:74` picks `MGPipeVerb::GetGpuTimestampNs` as the leave-verb, so a test that
   forgets a declaration *after* another scope closed aborts with
   `Fatal{UnmigratedPipeInput, "<Field>@GetGpuTimestampNs"}` — reading as "a timestamp query
   tried to read this", which is not what happened. The implementer's own demo A produced the
   clearer `@<none>` only because that test declared nothing at all. A dedicated
   "left the verb" sentinel (or documenting the string) would keep the diagnostic honest.

3. **Only one of the verb classes that reach each helper is exercised.**
   `SanityTest.cpp:3025,3046` declare `DrawArrays` for `SyncNeccessaryTextures()`, but that
   helper is reached in production from `kReadback` too — `DirectGLES.cpp:9020` (`ReadPixels`)
   and `:9191` (`GetTexImage`), among 14 call sites. Choosing the class the helper "belongs to"
   is exactly what F3 asks for, and the retrace / integration lanes are the oracle for the other
   classes (that is F1's job), so this is not a defect — but the result file's table reads as if
   the declared verb were the only class involved, and it is not.

4. **The known `GetFramebufferBindingSlotFast` poison gap is what produced the pre-fix push
   SEGFAULT, and it is worth recording.** `DirectGLES.cpp:147-157`: when the block is unfilled,
   `MGB_CTX_IDENTITY` is `nullptr` and `g_fbSlotCacheContext` is also `nullptr`, so the cache is
   treated as warm and `*g_fbSlotCache[target]` dereferences null. This is the documented D4
   gap (brief line 151, closed in P2), not this package's, but it means a push-build test that
   reaches that path can crash or read a stale slot without the poison ever firing — the one
   place where "green" is not proof. No such test exists today (§2.2).

5. **A brief-vs-tree fact, recorded per the standing instruction.** The brief (G3) calls
   `integration-verify` "the 742-entry lane" and `P1-LANE-FINDINGS.md` says 878 integration
   entries; the tree says **818** `integration-verify` and 1696 `integration-gpu`
   (`ctest --test-dir build-verify -N -L <label>`), identical in `~/w7/pipe/build-verify`. The
   result file's 818 is right; the brief's number is stale. Tree wins.

6. **The result file under-reports its own strongest evidence.** It does not mention that the
   verify build's unit lane is also green with the comparator actually armed
   (`MOBILEGL_PIPE_VERIFY=1`, §2.3). That is the result that rules out mis-placed declarations,
   and it belongs in the record.

## 4. Declared deviations — all six check out

1. tree `~/w7/p1-fix-tests` (confirmed: `~/w7/fix-tests` does not exist; branch, branch point
   `97b997d5` and ownership are as briefed);
2. the three build dirs did not exist and were configured by the implementer (confirmed
   equivalent to `~/w7/pipe`'s, §1.2);
3. `-DMOBILEGL_BUILD_BENCHMARK=OFF` (confirmed present in all three reference caches);
4. `~/w7/p1-before-ctest-names.txt` as the `-N` baseline (I additionally diffed against the
   integrated tree: 0/0);
5. the extra wrong-verb demonstration (I reproduced an independent, stronger variant);
6. the destructor's re-arm as an unbriefed design choice (reproduced; see minors 1 and 2).

No undeclared deviation found. F1, F2 and F4 are untouched, as stated; the
`integration-verify` lane and the 79-case retrace were not run here, which is correct — they are
red for F1/F2 reasons this package does not own, and nothing in this diff can reach them
(no `MG_IntegrationTest/**`, no production source, no fill table).
