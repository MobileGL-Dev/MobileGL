# v1-codex-verify — the five cross-family findings ID-52 assigned to a verifier

Verifier: Claude (Opus 5). Worktree `~/w7/p5-verify`, detached at `p5/v1@571cbabb` (base `5e5bf7b9`),
`build-split` reconfigured by nobody — the directory was already configured and
`build-split/CTestTestfile.cmake` was confirmed present before any ctest result below was believed.
Every perturbation was applied, built, run, then reverted with `git checkout --`; `git status` was
verified empty between findings and is empty now, at `571cbabb`, detached.
`~/w7/p5-verify-submodlinks.sh off` before every git command, `on` before every build.
Nothing was fixed and nothing was committed. `~/w7/p5-v1`, `~/w7/p5-v1-joint`, `~/w7/pipe/build-*`
were never written to.

**Verdicts: 5 CONFIRMED, 0 REFUTED, 0 PARTIAL.** Two of them (C2, C6) are confirmed *behaviourally*,
not only at source level, and each has a red-once control that passes on a fixed tree.

Temp files kept because they are cited: `~/w7/p5-verify2-redcheck.py` (the perturbed copy),
`~/w7/p5-verify2-redcheck-pristine.py`, `~/w7/p5-verify2-redcheck-baseline.log`,
`~/w7/p5-verify2-redcheck-perturbed.log`.

## The unperturbed run, first, so a red below is not a build error

```
$ cd ~/w7/p5-verify && cmake --build build-split -j 14
BUILD_RC=0
$ cd build-split && ctest -R 'ServerLoopTest|StagedShadowTest'
100% tests passed, 0 tests failed out of 11
```

Re-run after the last revert, with the tree back at `571cbabb`: `BUILD_RC=0`,
`100% tests passed, 0 tests failed out of 11`.

One environment fact that bounds two of the findings: **the split integration lane cannot run on
`p5/v1` standalone.** `MG_Backend/Init.cpp:163` calls `Client::CreateRemoteBackendObject()`, and the
only definition linked on this branch is v1's own weak placeholder (`ServerLoop.cpp:753-764`), which
`MGLOG_F`s `Fatal{UnimplementedRemoteBackendObject}` and aborts. The harness notices:

```
$ ctest -R 'DirectGLES.Split.ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels$' --verbose
3114:   SKIPPING every scenario: the EGL bring-up ABORTS on this platform: a forked pre-flight child
        died on signal 6 (Aborted). MobileGL asserts rather than returning an error here, so the
        scenarios would have taken the whole test binary down with them
3114: [  SKIPPED ] ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels
```

(Note for whoever reads a bare summary line: plain `ctest` prints `100% tests passed, 0 tests failed
out of 1` for that entry. It is a SKIP.)

---

## C2 — blocker — a control request posted after the apply thread's final cancellation pass waits forever

**Verdict: CONFIRMED (behaviourally, deterministically).**

**The window, at `571cbabb`.** `RunOnApplyThread` reads `m_running` at `ServerLoop.cpp:428` and does
not hold `m_controlMutex` while doing so:

```cpp
416    MobileGLResult ServerLoop::RunOnApplyThread(ControlWork work, void* user) {
...
428        if (!m_running.load(std::memory_order_acquire)) return work(user);
429
430        const std::lock_guard<std::mutex> callerLock(m_callerMutex);
431        {
432            std::unique_lock<std::mutex> lock(m_controlMutex);
...
435            m_controlPending = true;
436            m_controlFinished = false;
437        }
...
445        std::unique_lock<std::mutex> lock(m_controlMutex);
446        m_controlDone.wait(lock, [this] { return m_controlFinished; });
```

`ApplyThreadMain`'s final cancellation pass takes `m_controlMutex`, finds `m_controlPending == false`,
releases it, and only *then* clears `m_running` — outside the lock:

```cpp
310        {
311            const std::lock_guard<std::mutex> lock(m_controlMutex);
312            if (m_controlPending) { ... m_controlFinished = true; ... }
319        }
320        m_controlDone.notify_all();
321
322        m_running.store(false, std::memory_order_release);
323        SignalExited();
```

`m_controlFinished` is `false` at construction (`ServerLoop.h:154`) and the caller re-sets it to
`false` at `:436`, so no stale `true` can rescue it. Nothing can ever set it after `:319`.

**Perturbation (throwaway, exactly as the review proposed).** A deterministic latch immediately after
the successful `m_running` check, plus one case that holds a caller there while another thread stops
and joins the loop:

- `ServerLoop.cpp`, three `std::atomic<bool>` in `namespace MobileGL::MG_Remote::Server`
  (`g_mglVerifyC2Arm/Arrived/Release`), and after `:428`:
  ```cpp
          if (g_mglVerifyC2Arm.load(std::memory_order_acquire)) {
              g_mglVerifyC2Arrived.store(true, std::memory_order_release);
              while (!g_mglVerifyC2Release.load(std::memory_order_acquire)) {
                  std::this_thread::sleep_for(std::chrono::milliseconds(1));
              }
          }
  ```
- `ServerLoopTest.cpp`, one case `ServerLoopVerifyC2.AControlRequestPostedAfterTheFinalCancellation
  PassWaitsForever`: real `ServerFixture` handshake + `StartLoop`, the caller runs on its own
  `std::thread` behind a `std::promise`, the case waits for `Arrived`, then
  `clientTransport->Shutdown()` + `ServerLoopInstance().Stop()`, then releases the latch and does
  `future.wait_for(30s)`; on timeout it prints and `std::_Exit(42)`.

**Commands and verbatim outcome.**

```
$ cmake --build build-split -j 14            # BUILD_RC=0
$ ctest -R 'ServerLoopTest|StagedShadowTest' # latch compiled in, UNARMED
100% tests passed, 0 tests failed out of 11

$ timeout 90 ./build-split/MobileGL/MG_Test/Wire/ServerLoopTest \
      --gtest_filter=ServerLoopVerifyC2.*
[ RUN      ] ServerLoopVerifyC2.AControlRequestPostedAfterTheFinalCancellationPassWaitsForever
C2_ARM: the caller is past the m_running check and held
C2_STOP: Stop() returned in 0 ms; Running()=0
C2_RESULT: CALLER_HUNG - no answer 30000 ms after the loop was joined
```

**`Stop`'s bounded join completes while the caller is hung — answered, and it is the sharp half.**
`C2_STOP: Stop() returned in 0 ms; Running()=0`. The thread had already run `SignalExited()` at
`:323`, so `m_exitCv.wait_for(kJoinTimeoutMs)` at `:482` was satisfied immediately and `m_thread.join()`
at `:499` returned. There is no `Fatal{ApplyThreadJoinTimeout}` and no diagnostic of any kind: teardown
reports success while an EGL caller is blocked forever on `m_controlDone`. This matches the
same-family review's M-7 "same function, narrower race" paragraph, which named the window but did not
execute it; this is the execution.

**Minimal fix shape (verified to work).** Move the `m_running` clear INSIDE the same
`m_controlMutex` critical section as the cancellation pass, and re-check it under that mutex in
`RunOnApplyThread` before publishing:

```cpp
// ApplyThreadMain, inside the lock_guard block at :310-319, replacing the store at :322
            m_running.store(false, std::memory_order_release);
// RunOnApplyThread, first statement inside the m_controlMutex block at :432
            if (!m_running.load(std::memory_order_acquire)) return MOBILEGL_ERR_NOT_INITIALIZED;
```

Returning `NOT_INITIALIZED` rather than running `work(user)` inline is deliberate: by `:319` the apply
thread has already destroyed the backend at `:305` and given up the context, so the inline arm would
reach the driver with no context. (It also avoids widening M-7's silent fallback.)

**Red-once control.** The case above IS the control, and it is not trivially always-red: with the fix
shape applied it goes green, and the eleven shipped cases stay green.

```
$ timeout 90 ./build-split/MobileGL/MG_Test/Wire/ServerLoopTest --gtest_filter=ServerLoopVerifyC2.*
C2_ARM: the caller is past the m_running check and held
C2_STOP: Stop() returned in 0 ms; Running()=0
C2_RESULT: CALLER_RETURNED rc=1 (NOT_INITIALIZED=1)
[       OK ] ServerLoopVerifyC2.AControlRequestPostedAfterTheFinalCancellationPassWaitsForever (2 ms)
$ ctest -R 'ServerLoopTest|StagedShadowTest'
100% tests passed, 0 tests failed out of 11
```

v1's round 2 should ship this case (or its shape) — no case in the suite covers the window today.

---

## C6 — major — the seven backend-internal capability reads still use the client's object

**Verdict: CONFIRMED (source-level AND behaviourally).**

**The seven are unconverted, verbatim.**

```
$ cd ~/w7/p5-verify && rg -n pActiveBackendObject \
      MobileGL/MG_Backend/DirectGLES/Utils.cpp \
      MobileGL/MG_Backend/DirectGLES/BackendObject_DirectGLES.cpp
MobileGL/MG_Backend/DirectGLES/BackendObject_DirectGLES.cpp:815:        if (pActiveBackendObject && targetIndex < kFormatCapabilityTargetCount &&
MobileGL/MG_Backend/DirectGLES/BackendObject_DirectGLES.cpp:819:                pActiveBackendObject->GetFormatCapabilities().SampleCounts[targetIndex][formatIndex];
MobileGL/MG_Backend/DirectGLES/Utils.cpp:74:            if (!pActiveBackendObject || targetIndex >= kFormatCapabilityTargetCount) {
MobileGL/MG_Backend/DirectGLES/Utils.cpp:82:            const FormatCapabilityCache& cache = pActiveBackendObject->GetFormatCapabilities();
MobileGL/MG_Backend/DirectGLES/Utils.cpp:126:            if (!pActiveBackendObject || ShouldUseCaveatFormat(internalFormat, targetIndex)) {
MobileGL/MG_Backend/DirectGLES/Utils.cpp:220:            if (pActiveBackendObject == nullptr) {
MobileGL/MG_Backend/DirectGLES/Utils.cpp:260:                if (pActiveBackendObject && !ShouldUseCaveatFormat(internalFormat, targetIndex)) {
```

Seven sites, at the seven line numbers CONTRACT-P5 §4 table 3's `pActiveBackendObject` row names and
that `ServerLoop.h:70-76` repeats as the work to be done. Not one takes a format cache as a parameter.
`ServerLoop.h:70-76` still says they do:

> the seven backend-internal reads of pActiveBackendObject — ClampSamplesToBackendSupport
> (BackendObject_DirectGLES.cpp:815, :819) and five in Utils.cpp (:74, :82, :126, :220, :260), all of
> them format-capability lookups — take the format cache as a parameter instead.

**Site-by-site: function, role, thread.** All seven are format-capability lookups inside
`MG_Backend/DirectGLES`, which under split is the SERVER's backend implementation, and every one of
them is reached only from paths that touch GL — i.e. from the apply thread, which under R-1 owns the
context for life.

| site | function | reached from | role / thread under split |
|---|---|---|---|
| `Utils.cpp:74`, `:82` | `HasCachedFormatCapability` | `ShouldUseCaveatFormat` → `GenerateFormatInfo`, `ShouldUseCaveatTextureFormat`, `ShouldUseCaveatRenderbufferFormat` | server, `mgl-srv-apply` |
| `Utils.cpp:126` | `GenerateFormatInfo` | `GenerateTextureFormatInfo` (`Managers.cpp:6396`, `:6797`, `:6897`, `:7155`, `:7707`), `GenerateRenderbufferFormatInfo` (`Managers.cpp:12976`) | server, `mgl-srv-apply` |
| `Utils.cpp:220` | `UsesWidenedPacked16NormStorage` | `GenerateFormatInfo` (`Utils.cpp:136`) | server, `mgl-srv-apply` |
| `Utils.cpp:260` | `BackendFormatAddsAlpha` | `BackendTextureFormatAddsAlpha` (`Managers.cpp:6074`, `:8342`, `:8810`, `:8933`, `DirectGLES.cpp:11173`) | server, `mgl-srv-apply` |
| `BackendObject_DirectGLES.cpp:815`, `:819` | `ClampSamplesToBackendSupport` | `Managers.cpp:6910` (immediately before the multisample texture allocation) and `Managers.cpp:12990` (immediately before `glRenderbufferStorageMultisample`) | server, `mgl-srv-apply` |

The role half is not an inference. `ServerLoop::CreateBackend` builds the private
`BackendObject_DirectGLES` (`ServerLoop.cpp:153`) and its `Initialize()` installs the process-wide
resource op table — `BackendObject_DirectGLES.cpp:849` → `Managers.cpp:2712-2721`
`MGPipeSetResourceOps(&g_glesResourceOps)` — and table 3 rules `g_resourceOps` **server-exclusive,
and the client must NEVER read it**. So `Managers.cpp`'s op bodies (e.g.
`RearmPipeTextureLevelUpload`, `Managers.cpp:4073`, which contains the `:6897`/`:6910` pair) are the
server's, running inside `PipeApplier` on the apply thread.

**What they dereference.** `MG_Backend/Init.cpp`'s `InitSplitRoles` step 4:

```cpp
160            // 4. and only now the CLIENT's backend object in the one global that holds it.
161            //    Table 3: pActiveBackendObject holds BackendObject_Remote and the server's
162            //    BackendObject_DirectGLES stays private to ServerLoop.
163            pActiveBackendObject = MG_Remote::Client::CreateRemoteBackendObject();
```

There is one such global and no thread-keyed shim, so the server's apply thread reads the client's
`BackendObject_Remote` — whose `GetFormatCapabilities()` is the non-virtual `BackendObject` accessor
(`BackendObject.h:594`, `BackendObject.cpp:478`) over c1's mirror, not the private object's probed
cache.

**Behavioural confirmation (it was cheap, so it was done).** Throwaway case in `ServerLoopTest.cpp`:
two `BackendObject_DirectGLES` subclasses that expose the protected `MutableFormatCapabilities()`,
given deliberately different masks — `SampleCounts[0][RGBA8] = {2}` for the object installed in
`pActiveBackendObject` (the client's role) and `{8}` for the object standing in for
`ServerLoop::Backend()` — then one call to the server helper
`MG_Backend::DirectGLES::ClampSamplesToBackendSupport(0, RGBA8, 0, 8)`.

```
$ timeout 120 ./build-split/MobileGL/MG_Test/Wire/ServerLoopTest --gtest_filter=ServerLoopVerifyC6.*
C6_RESULT: ClampSamplesToBackendSupport(target=0, RGBA8, samples=8) = 2 [global/client head = 2, server-private head = 8]
../MobileGL/MG_Test/Wire/ServerLoopTest.cpp:295: Failure
Expected equality of these values:
  got
    Which is: 2
  8
the helper answered 2, which is the mask of the object in pActiveBackendObject - under split that is
the CLIENT's BackendObject_Remote, not the server's private cache
[  FAILED  ] ServerLoopVerifyC6.AServerCapabilityHelperReadsTheServersOwnCacheNotTheGlobal
```

The helper follows the global's mask, exactly as the review predicted. That *is* the red-once control:
it asserts the answer `ServerLoop.h:70-76` promises and fails for its own reason with the two masks
named in the message.

**Scope note the integrator should hold.** On `p5/v1` standalone the wrong *value* is not yet
observable end-to-end, because `CreateRemoteBackendObject` is the weak placeholder that aborts, so
nothing ever installs a real `BackendObject_Remote`. The defect becomes a live wrong answer the moment
c1's object is linked (the joint tree). What is fully confirmed today, and what v1 owns, is that the
conversion table 3 and v1's own header commit to has not been done, in any of the seven places.

**Minimal fix shape.** As table 3 already rules, and as `ServerLoop.h:75` already scopes it at "six
functions across two files": give the five `Utils.cpp` helpers and `ClampSamplesToBackendSupport` a
`const FormatCapabilityCache&` parameter, threaded from the caller that knows its role — under split
`ServerLoop::Backend()->GetFormatCapabilities()`, under monolith `pActiveBackendObject`'s. The
red-once control is the case above with `EXPECT_EQ(got, 8)` retained.

---

## C7 — major — make-current still rebinds and release-current still drops the native context

**Verdict: CONFIRMED (source-level AND behaviourally).**

**The claim in the code.** `ServerLoop.cpp:608-611`:

```cpp
608                // THE ONE eglMakeCurrent OF THE PROCESS'S LIFE, ON THIS THREAD. Every later
609                // client-side eglMakeCurrent onto the same surface finds the context already
610                // current here and costs nothing; the owner slot is written once.
611                ok = backend->MakeEGLCurrent(dpy, draw, read, ctx);
```

The forwarder is a straight pass-through; it adds no already-current test of any kind.

**There is no shortcut anywhere below it, either.** `BackendObject_DirectGLES::MakeEGLCurrent`
(`:935-969`) validates display/surface/context and then calls `DirectGLES::MakeCurrent()` at `:961`
unconditionally, and routes a release request (`IsReleaseCurrentRequest`, `:937`) to
`DirectGLES::ReleaseCurrent()` at `:938`. `DirectGLES::MakeCurrent` (`:11919-11955`) calls native
`eglMakeCurrent` at `:11925` unconditionally, then on success **always** writes the owner at `:11931`,
re-registers the resource op table at `:11934`, and invalidates seven caches at `:11937-11951`.
`DirectGLES::ReleaseCurrent` (`:11958-11975`) calls native `eglMakeCurrent(dpy, NO_SURFACE,
NO_SURFACE, NO_CONTEXT)` at `:11964` and clears the owner at `:11973`. A client release-current
therefore unbinds the apply thread's context and empties `g_backendContextOwnerThread` — which is the
predicate the phase exists to make true.

**Perturbation (instrumentation, throwaway).** A counter plus one `std::fprintf(stderr, ...)` at each
of the four points — immediately before the native call at `DirectGLES.cpp:11925` and `:11964`, and
immediately before the owner write at `:11931` and the owner clear at `:11973`.

**Drive and count.** The split lane cannot run here (weak stub, above), and I may not modify
`~/w7/p5-v1-joint`, so the sequence was driven on the monolith lane, where `MakeEGLCurrent` reaches
exactly the same chain — the only difference being which thread calls it, which the forwarder at
`:611` does not change.

```
$ ctest -R 'DirectGLES.ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels$' --verbose
2085: C7_NATIVE_BIND #1 (DirectGLES::MakeCurrent -> native eglMakeCurrent)
2085: C7_OWNER_WRITE #1 (MakeCurrent stores this thread as owner)
2085: C7_NATIVE_BIND #2 (DirectGLES::MakeCurrent -> native eglMakeCurrent)
2085: C7_OWNER_WRITE #2 (MakeCurrent stores this thread as owner)
2085: C7_NATIVE_RELEASE #1 (DirectGLES::ReleaseCurrent -> native eglMakeCurrent NO_CONTEXT)
2085: C7_OWNER_CLEAR #3 (ReleaseCurrent clears the owner slot)
1/1 Test #2085: ... ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels ... Passed 0.21 sec
```

**Count per request: one native `eglMakeCurrent` and one owner write per bind, one native
`eglMakeCurrent` and one owner clear per release.** A single scenario in a single process performed
**two** binds — the second cost a full native call, a second owner write, a resource-op-table
re-registration and seven cache invalidations — and one release, which really did hand the context
back to the driver. "Costs nothing" and "the owner slot is written once" are both false as written.

**Minimal fix shape.** Either (a) make the claim true — an already-current shortcut at the
`DirectGLES::MakeCurrent` layer, keyed on `g_backendContextOwnerThread == this_thread` together with
the surface/context triple, returning early before the native call and before the invalidations; and
`ServerMakeEGLCurrent`/the release forwarder must refuse to forward a client release-current at all
under split, since the apply thread owns the context for life and the client has no business
unbinding it — or (b) delete the three-line comment and say what actually happens. (a) is what
charter §2 promised; (b) is the honest minimum. The two are not equivalent: (b) leaves the false
context-ownership predicates the package claims to eliminate.

**Red-once control.** The instrumentation above, turned into an assertion: drive bind, bind, release
through the forwarders and assert `native_binds == 1` and `owner_writes == 1` across the pair. It
fails today with `2` and `2`, naming the counter, and would pass on (a). This needs the joint lane
(or a v1 harness with a real EGL context) to be armed against the forwarders rather than the backend
object directly; on `p5/v1` alone only the backend-object half is reachable.

---

## C9 — major — the red-check runner accepts any non-zero ctest exit

**Verdict: CONFIRMED.**

**The rule, verbatim** (`~/w7/p5-v1-redcheck.py:147-155`):

```python
147                rc, out = ctest(regex)
148                if rc == 0:
149                    findings.append("%s: the check stayed GREEN with the implementation broken" % name)
150                    print("   *** STILL GREEN - THE CHECK CANNOT FAIL FOR ITS OWN REASON ***")
151                else:
152                    print("   RED as required (ctest rc=%d)" % rc)
153                    for line in out.splitlines():
154                        if "Failure" in line or "which means" in line or "Timeout" in line:
155                            print("      | " + line.strip()[:160])
```

Non-zero is the whole test. Nothing asserts the expected assertion, Fatal or timeout for the
individual perturbation; lines 153-155 print whatever happens to match three substrings and print
nothing at all when none do.

**Work was done on a copy**, `~/w7/p5-verify2-redcheck.py`, differing from `~/w7/p5-v1-redcheck.py`
in exactly `ROOT` and `-j`:

```
$ diff <(sed -e 's#p5-verify#p5-v1#' -e 's#-j 14#-j 24#' ~/w7/p5-verify2-redcheck.py) ~/w7/p5-v1-redcheck.py \
    && echo COPY_IS_OTHERWISE_IDENTICAL
COPY_IS_OTHERWISE_IDENTICAL
```

**Unperturbed run of the copy against `~/w7/p5-verify`** (`~/w7/p5-verify2-redcheck-baseline.log`):

```
$ python3 ~/w7/p5-verify2-redcheck.py
...
=== restoring and re-checking the baseline
baseline green again

P5_V1_REDCHECK_ALL_WENT_RED
SCRIPT_RC=0
```

All ten controls really do go red today, and **every one of them exits ctest with `rc=8`** — e.g.
`RED as required (ctest rc=8)` for `resolved-mask-not-logged`, `audit-poison-disarmed`,
`r11-copy-removed` and `coverage-widened`. That is the sharp edge of the finding: `8` is also what an
unrelated failure returns, so the runner's only discriminator carries no information.

**Perturbation, exactly as proposed.** The `ctest` runner function is stubbed for exactly ONE
perturbed invocation — the first control that names
`ServerLoopTest.AControlRequestRunsOnTheApplyThreadAndUnparksIt`, i.e. `control-runs-on-caller` — to
return `(8, "unrelated fixture failure")` without running. The two baseline invocations and the other
nine controls are untouched and ran for real.

**Verbatim outcome** (`~/w7/p5-verify2-redcheck-perturbed.log`):

```
=== control-runs-on-caller (the EGL lifecycle calls would reach the driver from the app thread)
   [C9 STUB] returning (8, 'unrelated fixture failure') without running ctest
   RED as required (ctest rc=8)
...
=== restoring and re-checking the baseline
baseline green again

P5_V1_REDCHECK_ALL_WENT_RED
SCRIPT_RC=0
```

Yes to both halves of the question: the script prints `RED as required` for a control that did not run
at all and whose payload contains no assertion, no Fatal and no timeout, prints no evidence line under
it, records no finding, and still ends with `P5_V1_REDCHECK_ALL_WENT_RED` and exit 0. This is R-16's
second clause verbatim — 一条负面对照必须断言它自己的失败原因，不能只断言进程失败了 — and the runner
does only the latter.

**Minimal fix shape.** Give each `CASES` entry an expected-reason field (a regex over ctest output:
the gtest failure location and message, the `Fatal{...}` wording, or `Timeout`) and require it:
replace `if rc == 0 / else` with `if rc == 0 or not re.search(expected, out): findings.append(...)`.
For the two entries the same-family review called tautological (`resolved-mask-not-logged`,
`park-predicate-loses-control`) the expected reason is what makes them controls at all — the second
must require `Timeout`, not merely non-zero.

**Red-once control for the fix.** The stub above: with the expected-reason gate in place the same
`(8, "unrelated fixture failure")` must produce
`control-runs-on-caller: red, but not for its own reason` in FINDINGS and exit 2 instead of printing
`P5_V1_REDCHECK_ALL_WENT_RED`. (Not executed — the gate is v1's to write.)

---

## C10 — major — the parking tests arm on a counter incremented before the wait

**Verdict: CONFIRMED, and wider than claimed: all eleven cases stay green, not just the two.**

**The counter is incremented before the wait** (`ServerLoop.cpp:268-269`):

```cpp
268            m_parks.fetch_add(1, std::memory_order_acq_rel);
269            const bool woke = bell.Wait(control.consumerParked, ready, spinUs, Transport::kWaitForever);
```

`Doorbell::Wait` then spins for `spinUs` (`Doorbell.h:131-137`) and re-tests the predicate after
announcing (`:142-147`) before it ever calls `Park` at `:159`. `ParkCount() > 0` therefore observes
entry *toward* a park. `ServerLoopTest.cpp:230-234` and `:299-303` arm on exactly that.

**Perturbation, exactly as proposed** — only the wait expression at `:269`:

```cpp
            const bool woke = (static_cast<void>(spinUs), true); // C10 PERTURBATION: never parks
```

(the `static_cast<void>` keeps `spinUs` used; nothing else on the line or around it changed). The
thread now busy-spins and never blocks.

**Verbatim outcome.**

```
$ cmake --build build-split -j 14     # BUILD_RC=0
$ ctest -R 'ServerLoopTest.AControlRequestRunsOnTheApplyThreadAndUnparksIt|ServerLoopTest.StopKillsTheDoorbellJoinsAndTheThreadReallyExits'
1/2 Test #1975: ServerLoopTest.AControlRequestRunsOnTheApplyThreadAndUnparksIt ....   Passed    0.00 sec
2/2 Test #1977: ServerLoopTest.StopKillsTheDoorbellJoinsAndTheThreadReallyExits ...   Passed    0.00 sec

100% tests passed, 0 tests failed out of 2

$ ctest -R 'ServerLoopTest|StagedShadowTest'
100% tests passed, 0 tests failed out of 11
```

Both named cases stay green with parking deleted, as claimed — and so does the whole suite. Their
prose ("the case asserts the thread REALLY PARKED first, because a loop that spun instead would pass
this without the predicate ever mattering", `ServerLoopTest.cpp:221-222`) asserts precisely the
property the perturbation removes, and neither notices. Note this is not the same hole as the
redcheck's `park-predicate-loses-control` entry, which perturbs the *predicate* (`:258`) and does
correctly time out; nothing perturbs the *wait*.

**Minimal fix shape.** Arm on the flag the Doorbell sets immediately before it blocks —
`RingControl::consumerParked` (`Ring.h:144`), stored at `Doorbell.h:142` inside the blocking section
and cleared at `:145`/`:150`/`:165` — instead of on `ParkCount()`. The fixture already holds it:
`fixture.clientSegments.CmdControl()->consumerParked`. (Alternatively move `m_parks.fetch_add` into a
callback the Doorbell invokes on a real `Park`, which makes `ParkCount()` mean what its name says and
fixes both cases at once.)

**Red-once control (executed, both directions).** One case that polls `consumerParked` for 5 s and
requires it to have been set:

```
# with the :269 perturbation applied
[ RUN      ] ServerLoopVerifyC10.TheThreadReallyBlocksInTheDoorbellNotJustCountsTowardIt
../MobileGL/MG_Test/Wire/ServerLoopTest.cpp:280: Failure
Value of: sawParked
  Actual: false
Expected: true
consumerParked was never set, so the apply thread never entered the blocking park - ParkCount()
counted an intention, not a park
[  FAILED  ] ServerLoopVerifyC10.TheThreadReallyBlocksInTheDoorbellNotJustCountsTowardIt (5000 ms)

# ServerLoop.cpp reverted, same case
[       OK ] ServerLoopVerifyC10.TheThreadReallyBlocksInTheDoorbellNotJustCountsTowardIt (1 ms)
```

---

## The five

| # | claim | verdict | evidence |
|---|---|---|---|
| C2 | a control request posted after the apply thread's final cancellation pass waits forever (`ServerLoop.cpp:310/:322/:428-446`) | **CONFIRMED** (blocker) | latch case: `C2_RESULT: CALLER_HUNG - no answer 30000 ms after the loop was joined`, while `C2_STOP: Stop() returned in 0 ms; Running()=0` — the bounded join succeeds and reports nothing. Fix shape returns `rc=1 (NOT_INITIALIZED)` and keeps 11/11 green |
| C6 | the seven backend capability reads still go through `pActiveBackendObject` (`ServerLoop.h:71-77`, `Utils.cpp:82`, `BackendObject_DirectGLES.cpp:819`) | **CONFIRMED** (major) | all seven present and unconverted at the exact table-3 line numbers; two-object harness: `ClampSamplesToBackendSupport(...) = 2` — the global's/client's mask, not the server-private `8`. Wrong *value* becomes live only once c1's `BackendObject_Remote` is linked |
| C7 | make-current is not once-only and release-current drops the native context (`ServerLoop.cpp:608-611`, `DirectGLES.cpp:11919-11973`) | **CONFIRMED** (major) | no already-current shortcut at any layer; instrumented run: two binds in one process → `C7_NATIVE_BIND #1`, `#2` with `C7_OWNER_WRITE #1`, `#2`, plus `C7_NATIVE_RELEASE #1` / `C7_OWNER_CLEAR #3`. 1 native call + 1 owner write per request |
| C9 | the red-check runner accepts any non-zero ctest exit (`p5-v1-redcheck.py:146-154`) | **CONFIRMED** (major) | stubbed control returning `(8, "unrelated fixture failure")` prints `RED as required (ctest rc=8)`, logs no evidence line, records no finding, and the script still ends `P5_V1_REDCHECK_ALL_WENT_RED` / exit 0. Every genuine red also exits `8` |
| C10 | the park tests arm on `ParkCount`, incremented before the wait (`ServerLoop.cpp:268-269`, `ServerLoopTest.cpp:230-234`, `:299-303`) | **CONFIRMED** (major) | with the `:269` wait replaced by `true`, both named cases Passed and **all 11 Passed**. A `consumerParked`-based arming goes red for its own reason and green again on revert |

## Closing state

`~/w7/p5-verify`: clean, detached at `571cbabb`, submodule links `on`, `build-split` rebuilt from the
unperturbed sources — `BUILD_RC=0`, `100% tests passed, 0 tests failed out of 11`.

## Two things that need an integrator ruling

1. **C7's fix is a design choice, not a repair.** Making "the context is held for life" true means a
   server that REFUSES to forward a client release-current, not just an already-current shortcut.
   That changes what the twelve forwarders are allowed to do and touches c1's nine EGL virtuals, so
   it is not obviously inside v1's round 2. If only the comment is deleted, charter §2's claim should
   be struck from v1's report too, and the false-ownership-predicate risk booked for P6.
2. **C6's live wrong answer is a joint defect, v1's undone work is not.** v1 can and should do the
   "pass the format cache down" conversion (six functions, two files, table 3 already rules it). But
   the observable divergence needs c1's `BackendObject_Remote` and its caps mirror, so whichever
   package fills that mirror owns the end-to-end control. Worth pairing with ID-52's ruling inside
   item 3 (under an active transport the server's own state is the only base) so both land in the
   same round.
