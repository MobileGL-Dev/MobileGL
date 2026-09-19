# P5b Codex APK / Redmi runner handoff

2026-09-16. Dedicated scripts; original campaign scripts and evidence remain intact.

## Build state

Final source head: `82683d4a83b5c8f4f7c375a7a92ebee9a1854646`, including both closing-review fixes. The first real build started at 2026-09-16T14:12:42-04:00 in phase `p5bcodex1` and was interrupted by an entire WSL restart at approximately 14:14 (retained exec session exited 1; uptime reset to 0 minutes; all native/Gradle processes disappeared). Its output directory, build log and native cache are preserved. Pull reached `mergeTraceReleaseJniLibFolders`; no signed APK/proof was produced.

State: waiting for the root task's memory/concurrency coordination. The next authorized build will use **p5bcodex2**, the same exact source head and dedicated APK tree, and a fresh output directory. The commands below name that next phase; they must not be run until its three APK proofs exist.

Build tree: `/home/swung/w7/p5b-apk-codex`; output: `/home/swung/w7/notes/p5b/apk/p5bcodex2`.

The wrapper `notes/tools/p5b_codex_build_apks.sh` requires `--head` with the full SHA and a clean dedicated checkout. It shares the old `wsl_build_p5_apks.lock`, refuses an existing output directory, retains each arm's build log and proofs, and builds pull/push/split sequentially. `P5B_APK_JOBS=4` is the default. A generated Gradle init script also sets CMake compile/link job pools, because AGP invokes Ninja directly. No tracked source build file is modified.

In WSL Arch, after the root task has moved the clean APK tree to the final source commit:

```bash
HEAD_SHA=82683d4a83b5c8f4f7c375a7a92ebee9a1854646
test "$(git -C /home/swung/w7/p5b-apk-codex rev-parse HEAD)" = "$HEAD_SHA"
P5B_APK_JOBS=4 bash /home/swung/w7/notes/tools/p5b_codex_build_apks.sh   --head "$HEAD_SHA" --phase p5bcodex2 /home/swung/w7/p5b-apk-codex
```

Prerequisites: local hydrated `~/w7/pipe` dependencies, JDK 21 at `/usr/lib/jvm/java-21-openjdk`, SDK at `~/android-sdk`, Gradle 8.10.2 and its offline dependencies. The native build must be optimized `RelWithDebInfo`, arm64 only. Each proof checks source SHA, embedded Git stamp, package/signing/ABI, push/remote symbol identities, and byte-for-byte equality between the packaged library and stripped build library. The three outputs are `trace-pull.apk`, `trace-push.apk`, and `trace-split.apk`; splitctl reuses the split APK with monolith runtime.

## Windows Git Bash setup and prerequisites

Run in Windows **Git Bash**, not WSL, since the existing Android runner uses Windows Python/adb. The root task owns all device operations and synchronization of the clean Windows source tree.

```bash
TREE=/c/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg
TOOLS=//wsl.localhost/Arch/home/swung/w7/notes/tools
APKS=//wsl.localhost/Arch/home/swung/w7/notes/p5b/apk/p5bcodex2
FIXTURES=//wsl.localhost/Arch/home/swung/w7/pipe/tools/trace_replay/fixtures
OUT_ROOT=/c/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL/.trace-work/p5b-redmi
PHASE=p5bcodex2
cd "$TREE"
command -v bash git python adb
python --version
HEAD_SHA=$(python -c 'import json,sys; print(json.load(open(sys.argv[1]))["source_head"])' "$APKS/trace-pull.proof.json")
test "$(git rev-parse HEAD)" = "$HEAD_SHA"
git diff --quiet --ignore-submodules=all HEAD --
for arm in pull push split; do
  test -s "$APKS/trace-$arm.apk" && test -s "$APKS/trace-$arm.proof.json" || exit 1
done
python "$TOOLS/p5b_codex_runner_selftest.py"
```

All four fixture archives and their manifest goldens must already be hydrated. The adapter validates their existence/content and refuses Git LFS pointer files; it never fetches LFS. Case names:

- `improved-transparency-minecraft-26.3`
- `minecraft-1.21.4-in-world`
- `minecraft-1.21.4-fabric-sodium-in-world`
- `minecraft-1.21.4-fabric-iris-bsl-in-world`

Dry runs perform no device operations, locking, builds, or file writes:

```bash
bash "$TOOLS/p5b_codex_redmi.sh" --head "$HEAD_SHA" --phase "$PHASE"   --mode correctness --tree "$TREE" --out-root "$OUT_ROOT" --dry-run
bash "$TOOLS/p5b_codex_redmi.sh" --head "$HEAD_SHA" --phase "$PHASE"   --mode benchmark --tree "$TREE" --out-root "$OUT_ROOT" --dry-run
```

Expect `DRY_GROUPS=8` and `DRY_GROUPS=32`. The target is fixed at Redmi serial `2f7cbe2e`, which must support adb and `su` after reboot. The existing pin script must be readable at `//wsl.localhost/Arch/home/swung/w7/notes/p2/devices/pin_device.sh`. The session uses the existing campaign lock at `/c/Users/geekerwan/AppData/Local/Temp/claude/C--Users-geekerwan-AndroidStudioProjects-FoldCraftLauncher-MobileGL/8ab6b3cb-c783-4826-9fd7-39ff3498af5b/scratchpad/w7/locks/2f7cbe2e`; it waits at most 900 seconds and never breaks another owner's lock. No other campaign should operate that device concurrently.

## Device commands (root task only)

```bash
bash "$TOOLS/p5b_codex_redmi.sh" --head "$HEAD_SHA" --phase "$PHASE"   --mode correctness --tree "$TREE" --out-root "$OUT_ROOT"
```

Correctness must finish with 8/8 groups and `session-exit-code.txt` equal to 0. Each case/backend requires an actual fresh PNG, the manifest target call and SSIM threshold (including improved-transparency 0.995), successful replay, and private-log proof of inproc, apply thread, and positive wire records. A skip is failure. Then:

```bash
python "$TOOLS/p5b_codex_matrix.py" require-correctness   "$OUT_ROOT/$PHASE/2f7cbe2e/correctness" --head "$HEAD_SHA" --phase "$PHASE"
bash "$TOOLS/p5b_codex_redmi.sh" --head "$HEAD_SHA" --phase "$PHASE"   --mode benchmark --tree "$TREE" --out-root "$OUT_ROOT"
```

Benchmark is four arms × two backends × four cases = 32 groups, with three repeats each and the existing nofinish/last-200-frame protocol. The reducer retains the previous lowest-mean-repeat selection and reports p50/p99, CPU, mapped views and available memory evidence. No timing delta is a pass/fail threshold. Failed replay, missing evidence, short samples or pin drift invalidate a group and exclude it from paired metrics.

All arms explicitly use `MOBILEGL_IPC_STAGE_MB=256`, the authorized P5b exit profile for the 128 MiB improved-transparency upload. This leaves the production 32 MiB default unchanged. Stage mapping is 256 MiB per role view / 512 MiB summed inproc views; this is not an inferred RSS increase. Other controls are `MOBILEGL_IPC_VERB_BARRIER=1`, stats enabled, and transport inproc for split / monolith for pull, push and splitctl. rd12 is excluded from this matrix.

The session reboots, fixes fan level 2, checks temperature below 40000 mC, pins/checks before and after each group, and cleans up unpin/fan0/stayon-off before releasing its own lock. Each retry uses a new attempt directory; rerunning a completed mode requires a new `--out-root` or phase. Do not remove old evidence to rerun.

Outputs are under `$OUT_ROOT/$PHASE/2f7cbe2e/{correctness,benchmark}`: `matrix.json`, `matrix.md`, cleanup exit, per-group attempt/runner exit and pin evidence; benchmark also produces `ab-tables.md`. Per-repeat originals include result JSON, private `mobilegl.log`, screenshots and benchmark JSON. Missing/failed evidence cannot produce a green matrix.

## Runner checks already performed

Both shell scripts pass `bash -n`; Python modules compile. Real Windows Git Bash dry runs produced exactly 8/32 groups, and the Windows adapter imports the existing Windows runner correctly. Nine artifact-input selftests passed on Windows and WSL (skip, SSIM, missing/truncated PNG, split proof, wrong splitctl runtime, stale directories, short benchmark). Logs: `~/w7/p5b-codex-redmi-dry-{correctness,benchmark}.log` and `~/w7/p5b-codex-runner-selftest.log`.
