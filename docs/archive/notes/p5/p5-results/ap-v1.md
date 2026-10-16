# ap — P5 trace APK build and plumbing evidence

## Scope and source

**Complete:** final recipe run exited 0 with `P5_APK_DONE`,
2026-09-16 10:01:22–10:05:01 UTC−04:00. All three APKs are debug-signed,
verified and ready for the separate device step. Only the two cited failed-build
logs remain; no `~/w7/p5-ap-*` temporary files remain.

Build tree: `/home/swung/w7/p5-joint`, branch `p5/joint`, HEAD
`e61d00123c41d74f98cd06b63768be12d9b6348a` (ID-63's scratch head, not the final merged head).
No commits, checkout/merge/reset, device access, LFS fetch, or changes to any `build-*`
directory or another P5 worktree. All commands run in Arch WSL. `~/w7/pipe` is only
read as the source of hydrated dependency links and history.

## Plumbing truth

The premise that no Gradle file references `pipePush` is false: it is absent from
the Kotlin scripts but present in the **repository-root Groovy `build.gradle`**.
`android-plugin/settings.gradle.kts:18` includes `:MobileGL`, and line 19 maps it
to `..`; `android-plugin/app/build.gradle.kts:223` declares the library dependency.
`build.gradle:23-25` reads `rootProject.findProperty('mobilegl.pipePush')`, falls
back to `MOBILEGL_PIPE_PUSH_APK`, then `OFF`. `build.gradle:84` passes
`-DMOBILEGL_PIPE_PUSH=...` in the library's trace flavor. Its CMake path is the
root `CMakeLists.txt` (`build.gradle:93`), not the replay runner's CMake project.
History: `7a2e2561 [Build] (Android): let the trace APK be built in the push shape for the paired A/B`.

The surviving P4a files were inspected with
`unzip -p <apk> lib/arm64-v8a/libMobileGL.so | strings`:

| P4a APK | Lines containing `MGPipe` | Push-only function-name diagnostic |
|---|---:|---|
| `~/w7/notes/p4a/apk/trace-pull.apk` | 3 | `MGPipeApplySetVertexAttribDefaults` absent (0 lines) |
| `~/w7/notes/p4a/apk/trace-push.apk` | 109 | `MGPipeApplySetVertexAttribDefaults` present |

`MGPipeEmit` itself is not visible in the stripped P4a push APK. Absence of that
private symbol's spelling is not evidence of a pull build. The push APK contains
the function-name diagnostic `MGPipe: MGPipeApplySetVertexAttribDefaults does not
reproduce the carried value on this build ...`, along with push-only protocol
and handle diagnostics. P4a therefore used a real compile-time push arm, not
just the runtime mask. `ConfigLoader.cpp:260-270` separately parses the runtime
`MOBILEGL_PIPE_PUSH` mask; in compiled push it defaults to the migrated subsystem
mask, and the runtime setting cannot compile absent push code into a pull binary.

## Minimal uncommitted mapping

The existing push mapping is unchanged. Five lines in
`android-plugin/build.gradle.kts` expose the two split options in the existing
`com.android.library` configuration, which reaches `:MobileGL`. Both default OFF.
No change to `android-plugin/app/build.gradle.kts` is needed, because its native
project builds `libtrace_replay_runner.so`, not `libMobileGL.so`.

```diff
diff --git a/android-plugin/build.gradle.kts b/android-plugin/build.gradle.kts
index c351d75e..f118094d 100644
--- a/android-plugin/build.gradle.kts
+++ b/android-plugin/build.gradle.kts
@@ -29,6 +29,11 @@ subprojects {
                 }
                 externalNativeBuild {
                     cmake {
+                        // P5 trace A/B: :MobileGL owns libMobileGL.so, not :app's replay runner.
+                        arguments += listOf(
+                            "-DMOBILEGL_BUILD_DISAGGREGATED=${project.findProperty("mobilegl.buildDisaggregated") ?: "OFF"}",
+                            "-DMOBILEGL_BUILD_DISAGGREGATED_INPROC=${project.findProperty("mobilegl.buildDisaggregatedInproc") ?: "OFF"}",
+                        )
                         mobileGlCmakeCompilerLauncher().takeIf(String::isNotEmpty)?.let { compilerLauncher ->
                             arguments += listOf(
                                 "-DCMAKE_C_COMPILER_LAUNCHER=$compilerLauncher",
```

Suggested integrator commit subject (not committed here):
`[Build] (Android): expose disaggregated trace APK options`.

| Arm | `mobilegl.pipePush` | `mobilegl.buildDisaggregated` | `mobilegl.buildDisaggregatedInproc` |
|---|---|---|---|
| pull | OFF | OFF | OFF |
| push | ON | OFF | OFF |
| split | ON | ON | ON |

Root CMake defaults these options OFF (`CMakeLists.txt:23,30,35`), makes INPROC
imply DISAGGREGATED (`:460-464`) and DISAGGREGATED imply PUSH (`:496-501`).

## Recipe and validation

Reusable command, after applying/merging the mapping into the chosen final tree:

```sh
bash ~/w7/notes/tools/wsl_build_p5_apks.sh /path/to/final-tree p5
```

The second argument is optional and defaults to `p5`; output is
`~/w7/notes/<phase>/apk/trace-{pull,push,split}.apk`. The recipe rejects the
read-only integration tree and any other `~/w7/p5-*` worktree. It never runs adb.
Use a separate permitted final-tree checkout if the final merged head lives in
the read-only integration tree.

Each arm uses Gradle 8.10.2, JDK 21, SDK `~/android-sdk`, NDK 27.3.13750724,
`:app:assembleTraceRelease`, arm64-v8a only, debuggable release, INFO logging,
and `-Pmobilegl.apkSuffix=p5<arm>`. AGP selects optimized native
`RelWithDebInfo` (`-O2 -g -DNDEBUG`) for the debuggable release variant, matching
the P4a recipe; it does not compile an unoptimized Debug library.
The final recipe uses Gradle `--offline` with the existing populated dependency
cache, avoiding repository revalidation delays and downloads. A fresh machine
must have these Gradle/SDK dependencies provisioned before running it.
The recipe explicitly passes all three CMake switches for every arm, so Gradle's
argument-specific `.cxx` configurations cannot silently retain split flags in pull.
It uses only Android `.cxx`/`build` output directories, never desktop `build-*`.

`apkSuffix` changes the output filename only. For actual side-by-side installation,
the recipe also passes `-Pmobilegl.applicationIdSuffix=.p5<arm>`. The final
manifest package IDs are recorded by `aapt` in each arm's `.badging.txt`.
Each APK is signed with `~/.android/debug.keystore`, alias `androiddebugkey`,
and checked by `apksigner verify --verbose --print-certs`. V4 sidecars are disabled.

Evidence files beside the APKs: `build-provenance.txt`, `build-plumbing.diff`,
`trace-<arm>.proof.json`, `.cmake.txt`, `.badging.txt`, `.signing.txt`, and
`.symbols.txt`. APK extraction is to a `~/w7/p5-ap-*` temporary file, removed
automatically. GNU `strings` counts matching lines, not occurrences within lines.
Private symbols are stripped from release APKs; the report distinguishes retained
function-name diagnostic strings from true ELF symbol definitions. The recipe also
uses NDK `llvm-nm --defined-only --demangle` on the unstripped native output and
requires `llvm-strip --strip-unneeded` to reproduce the packaged `.so` **byte for
byte**, tying those definitions to the delivered binary without inflating APKs.

Initial configure failed because all thirteen submodule gitlink paths in the joint
tree were empty directories. Failure evidence is retained at
`~/w7/p5-apk-pull-missing-dependencies.log`. The recipe links only missing/empty
dependency paths to existing hydrated local copies under `~/w7/pipe`, refuses
broken links/missing source dependencies, and never initializes/fetches submodules.
The links are left in place. Successful build logs are removed individually.
During the first push attempt the links were externally replaced by empty
directories again (10:00 timestamps; HEAD unchanged), causing another CMake
failure. Evidence: `~/w7/p5-apk-push-dependency-links-removed.log`. The full
recipe was restarted after restoring the links. Other owners must not toggle
joint-tree dependency links while an APK build is running.

## Three-APK proof table

All sizes are exact bytes. Each `.so` is `lib/arm64-v8a/libMobileGL.so` extracted
from the named APK, not a desktop library.

| APK under `~/w7/notes/p5/apk/` | APK bytes | `.so` bytes | `MGPipe` string lines | Push-only name in APK (`MGPipeApplySetVertexAttribDefaults`) | Remote names in APK | `MOBILEGL_TRANSPORT` string lines |
|---|---:|---:|---:|---|---|---:|
| `trace-pull.apk` | 8,504,077 | 15,461,752 | 3 | absent | absent | 0 |
| `trace-push.apk` | 8,626,957 | 15,699,448 | 116 | present | absent | 0 |
| `trace-split.apk` | 8,737,549 | 15,941,584 | 190 | present | present | 5 |

Push-only function-name strings are diagnostic literals, not exported ELF symbols.
Split additionally retains genuine RTTI type-name strings such as
`N8MobileGL9MG_Remote9Transport18InProcessTransportE` and
`N8MobileGL9MG_Remote6Client20BackendObject_RemoteE`, plus the diagnostic naming
`ServerLoop::Start before ServerSession::Accept`. None occurs in pull or push.

The exact-strip-equal native binaries provide the stronger ELF-symbol check:

| Arm | `llvm-nm` defined `MGPipeEmitResourceSubData` | `llvm-nm` defined lines containing `MG_Remote` | Stripped bytes equal APK `.so` |
|---|---|---:|---|
| pull | absent | 0 | PASS |
| push | present (function plus its once-log latch) | 0 | PASS |
| split | present (function plus its once-log latch) | 459 | PASS |

The five split transport string lines, absent in both other APKs, are:

```text
MOBILEGL_TRANSPORT
Config: MOBILEGL_TRANSPORT=inproc - the MGPipe record stream crosses a real ring to an apply thread
Config: MOBILEGL_TRANSPORT='%s' names a transport P6 implements and P5 does not; staying on monolith. This run is NOT a split run.
Config: Ignoring invalid env variable MOBILEGL_TRANSPORT='%s'; expected monolith|inproc|spawn|unix:<path>|pipe:<name>, using monolith
MG_Remote client: MOBILEGL_TRANSPORT=%s%s is refused by name - P5 implements `inproc` only, and falling back to monolith would make this lane green for the wrong reason. spawn / unix: / pipe: are P6's
```

| Arm | Verified manifest package | APK SHA-256 |
|---|---|---|
| pull | `top.mobilegl.plugin.p5pull.trace` | `8dd7154f6e25c1cb998669cb3ea0e62a16c6595aa44dd4322b0118c6b628ed47` |
| push | `top.mobilegl.plugin.p5push.trace` | `998c83b5782194555756a49826407f2efaf4c499b9f330f4fcab2dcd2e0e3965` |
| split | `top.mobilegl.plugin.p5split.trace` | `bce8c6f751179ffe68124198819ee15c6871b8642d21ff03c17ea99f12a3cbda` |

All three manifests contain `application-debuggable`, minSdk 26, targetSdk 34,
and only `native-code: 'arm64-v8a'`. All three pass APK signature schemes v2 and
v3 with the same RSA-2048 Android Debug certificate, SHA-256
`97818b7025d0da0df79d1398306a699e18e06c1f15ea9626ece996526574148d`.
Their `.cmake.txt` records confirm INFO, optimized RelWithDebInfo, PUSH
OFF/ON/ON, DISAGGREGATED OFF/OFF/ON and INPROC OFF/OFF/ON; VERIFY is OFF throughout.

The final source delta remains the five-line Kotlin mapping plus the thirteen
local dependency symlinks (git reports those gitlink-to-symlink type changes).
**Do not commit the dependency symlinks.** The mapping is the only source edit to
integrate. HEAD remained `e61d00123c41d74f98cd06b63768be12d9b6348a` for every arm.

## Runtime handoff — no device work performed

The **split APK defaults to monolith**. The third A/B arm must use it with
`--env MOBILEGL_TRANSPORT=inproc --env MOBILEGL_PIPE_STATS=1` (and the same stats
period and all other benchmark settings as pull/push). A fourth control arm uses
the same split APK with `MOBILEGL_TRANSPORT` absent and stats still enabled; it
must equal push within measurement noise. This is a requirement for the separate
device run, not a performance result measured by this package.

This is an **environment variable on Android, not a config-file setting**:
`MobileGL/ConfigLoader.cpp:43` accepts the `MOBILEGL_` prefix; `:53-59` selects
`::environ` on POSIX/Android; `:63-72` captures key/value pairs; `:78-84` queries
that map; `:321` reads `MOBILEGL_TRANSPORT` with default `monolith`, and `:331-335`
selects inproc. `android-plugin/app/src/trace/cpp/trace_replay_core.cpp:143-149`
applies runner overrides with `setenv`, and `:237` invokes it.
`~/w7/notes/tools/p4a_ab_redmi.sh:88` already passes generic `--env` settings.

The existing wrapper `tools/trace_replay/run_android_retrace_local.py:26,31`
hardcodes `top.mobilegl.plugin.trace` and passes that at `:147-148`; the separate
A/B owner must target the actual per-arm package IDs and APK paths recorded here,
including for force-stop/activity/run-as operations. `android-plugin/trace-replay-ci.sh`
already accepts `--package`. No runner/device script is changed by this package.

Canonical sizes from ID-47 / `MG_Remote/Transport/SessionRings.h:83-97` are
command ring 8 MiB, stage 32 MiB, reply pool 16 MiB, event ring 256 KiB; command
and event mappings additionally include a 4096-byte control page. Reply pool has
eight 2 MiB slots, each with 16-byte header: max payload 2,097,136 bytes. The
640x480 RGBA8 readback (1,228,800 bytes) fits. A full 2400x1080 RGBA8 readback
does not fit and is a named refusal/P6 readback debt (ID-47), so the A/B owner
must preserve the reduced-path surface sizes.

No desktop gate rerun is claimed: the user supplied the owner's completed gate
and forbade touching its build directories. This package validates Android build
configuration, artifacts, signatures and identity; runtime equivalence belongs to
the separately guarded device A/B.
