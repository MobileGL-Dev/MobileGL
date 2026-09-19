# Device verification brief (Kimi): build FCL with the P5e MobileGL and prove it still runs

You are verifying a rendering-library change on a real Android phone. The change under test is
`MobileGL` at commit `44f91c74` on branch `feat/disaggregated`: a large refactor (about 9,800 lines)
that stops the rendering backend from reading the GL frontend's live objects and makes it read the
pushed record stream instead. The threading is UNCHANGED by it — the split still runs lockstep —
so the question this run answers is "did the refactor break rendering?", not "is it faster". Build the launcher, install it,
run the game, and report what actually happened. Do not change any source file — if something does
not build or does not run, report it precisely; fixing it is not your job.

## The one hard rule about hardware

**The only device you may touch is the Redmi with serial `2f7cbe2e`.** Every adb command must carry
`-s 2f7cbe2e`. Never run a bare `adb` command that could pick another device, never `adb
disconnect`/`kill-server`, never reboot, never factory-reset anything. The phone is rooted: `adb -s
2f7cbe2e shell su -c '<cmd>'` works when you need root (you need it only for the fan and the
thermal/clock reads below).

## 1. Build

Windows, Git Bash. The launcher repo is `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher`;
its `settings.gradle.kts` already points the `:MobileGL` subproject at the worktree
`MobileGL-disagg`, which holds the change under test. Do not modify either.

```bash
cd /c/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher
export JAVA_HOME="C:/Program Files/Java/jdk-21"
export JAVA_TOOL_OPTIONS='-Djdk.net.unixdomain.tmpdir=C:\Users\geekerwan\.gradle\uds'
./gradlew.bat :FCL:assembleFordebug -Pmobilegl.buildDisaggregated=ON -Pmobilegl.buildDisaggregatedInproc=ON -Pmobilegl.pipePush=ON --console=plain
```
- `JAVA_TOOL_OPTIONS` is not optional: without it every gradle invocation dies with
  `Unable to establish loopback connection`.
- The three `-P` flags are what select the split (disaggregated) build of the native library. A build
  without them is not the thing under test.
- Expect ~1-3 minutes and `BUILD SUCCESSFUL`. If it fails with
  `Could not move temporary workspace ... transforms`, that is a known flaky gradle cache error —
  run the same command again once. Any other failure: capture the first 40 lines of the error
  (especially any `error:` line naming a `.cpp`/`.h` file) and STOP; report it.
- Output: `FCL/build/outputs/apk/fordebug/FCL-fordebug-1.3.3.2-all.apk` (~717 MB). Record its
  timestamp and confirm it is newer than when you started.

## 2. Install

```bash
adb -s 2f7cbe2e install -r /c/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/FCL/build/outputs/apk/fordebug/FCL-fordebug-1.3.3.2-all.apk
```
Expect `Success`. Confirm with
`adb -s 2f7cbe2e shell "dumpsys package com.tungsten.fcl.mgdebug.debug | grep lastUpdateTime"`.

## 3. Prepare the run

```bash
# split (the thing under test):
adb -s 2f7cbe2e shell "echo inproc > /sdcard/FCL/mg_transport.txt"
adb -s 2f7cbe2e shell "rm -f /sdcard/FCL/mg_env.txt"          # no extra env for the first run
# fan on so nothing thermal-throttles, and keep the screen awake:
adb -s 2f7cbe2e shell "su -c 'echo 2 > /sys/class/xm_power/hw_monitor/pwm_fan/target_level'"
adb -s 2f7cbe2e shell input keyevent KEYCODE_WAKEUP
adb -s 2f7cbe2e shell input keyevent 82
adb -s 2f7cbe2e shell svc power stayon true
adb -s 2f7cbe2e logcat -c
```
`/sdcard/FCL/mg_transport.txt` selects the transport at launch: `inproc` = the split build under
test, `monolith` = the single-threaded control.

## 4. Run and watch

Start a background log capture, then launch. The app counts down ~5 s and auto-launches Minecraft
26.3-rc-3 into the world `test`; the world takes 1-3 minutes to load.

```bash
adb -s 2f7cbe2e logcat -v time -s MobileGL:I > /c/.../kimi-run-inproc.log 2>&1 &
adb -s 2f7cbe2e shell am start -n com.tungsten.fcl.mgdebug.debug/com.tungsten.fcl.activity.SplashActivity
```
**The in-world signal** is the renderer's own periodic line in that log:
```
MGPipe stats: frames=<N> window=120 draws=<M> draws/f=<D> ...
```
The game is genuinely rendering the world once three consecutive lines show `draws/f` above ~400
(a menu draws a handful; the world draws ~850). `frames=` must keep climbing.

Once in-world, let it run 60 seconds, then collect:
- **fps**: from two `MGPipe stats:` lines ~30 s apart — `(frames2 - frames1) / (seconds between their
  timestamps)`. Report the number and the two lines you computed it from.
- **a screenshot**: `adb -s 2f7cbe2e exec-out screencap -p > /c/.../kimi-inproc.png`, then look at it
  and say in one sentence what is on screen (a Minecraft world? a black screen? a menu?).
- **liveness**: `adb -s 2f7cbe2e shell pidof com.tungsten.fcl.mgdebug.debug` must still print a pid.
- **the failure scan**: search the whole captured log for `Fatal{`, `FATAL`, `abort`, `SIGSEGV`,
  `SIGABRT`, `RoleViolation`, `UnmigratedPipeInput`, `UnmigratedVerb`, `ProtocolCorruption`. Also
  `adb -s 2f7cbe2e logcat -d -b crash -v time | tail -40`. **Quote verbatim anything you find** —
  one of these lines is the single most important thing you can bring back. Note: two lines are
  EXPECTED and harmless, ignore them: `Failed to load EGL function: eglSwapBuffersWithDamageEXT`
  and `... eglUnlockSurfaceKHR`.

Then stop it: `adb -s 2f7cbe2e shell am force-stop com.tungsten.fcl.mgdebug.debug`.

## 5. The control run

Repeat section 3-4 with `echo monolith > /sdcard/FCL/mg_transport.txt` and a separate log file
(`kimi-run-monolith.log`, `kimi-monolith.png`). This is the same library running single-threaded: it
tells us whether anything you saw in the split run is specific to the change or true of both.

## 6. Restore the device

```bash
adb -s 2f7cbe2e shell am force-stop com.tungsten.fcl.mgdebug.debug
adb -s 2f7cbe2e shell "su -c 'echo 0 > /sys/class/xm_power/hw_monitor/pwm_fan/target_level'"
adb -s 2f7cbe2e shell svc power stayon false
adb -s 2f7cbe2e shell "echo monolith > /sdcard/FCL/mg_transport.txt"
```

## 7. The report

Write it to the path given at the end of this brief BEFORE your final message, then answer with two
lines: the report path and a one-word verdict (`PASS` / `FAIL` / `BLOCKED`).

The report must contain, in this order:
1. **Verdict**, one line: does the split build run the game normally? PASS only if it reached the
   world, kept rendering for 60 s, showed no Fatal/crash, and the screenshot shows the world.
2. Build: the command you ran, BUILD SUCCESSFUL or the error, the APK timestamp, whether you had to
   retry.
3. Split run: time to in-world, the fps you computed and the two lines behind it, the screenshot
   description, pid alive yes/no, and the failure scan (verbatim quotes or "none found").
4. Monolith run: the same four items.
5. A short comparison: split fps vs monolith fps, and anything that differed between the two runs.
6. Anything unexpected you saw and did not understand — describe it rather than explaining it away.

Do not edit source files, do not commit anything, do not push, do not touch any other device or any
other repository. If a step cannot be completed, say which step and why, and continue with the steps
that do not depend on it.
