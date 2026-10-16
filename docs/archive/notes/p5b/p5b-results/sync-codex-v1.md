# P5b sync migration — 2026-09-16

Commit: `08fd9e9ca1888d795654fe133968526411be334c`, parent `9606466a`.
Worktree: `/home/swung/w7/p5b-d1-codex`. Integrator-approved contract is CONTRACT-P5B §9.

## Result

Five existing opcodes now serve FenceSync, ClientWaitSync, GetSyncStatus, WaitSync and
DeleteSync. The client allocates Fence-kind handles and retains local opaque proxies; the
wire carries only `{slot, gen}`. The apply thread owns native fences, validates generations,
preserves timeout/failed wait results, and deletes native orphan fences before backend detach.
Frontend orphan cleanup after session shutdown releases only proxies. New slots legitimately
start at generation zero; recycled generations must increase, and stale/destroyed handles
are protocol corruption. No host pointer crosses the wire.

MGPFenceWait is 24 bytes (was 16), with Flags/Pad0 appended; ClientWaitSync retains its flush
flag. FenceStatus and FenceWait use their existing reply slots with a 4-byte answer. All five
opcodes have matching verb stamps. Field declarations and generated stamp tables agree.
The class counts after this commit are A=2/B=53/C=16 (before integrator's named-blit migration).

The frontend FenceSync guard previously returned nullptr for every split session through
MGL_BACKEND_SLOT_PTR_LOCAL. This explains why the dynamic d1 census had no FenceSync first
blocker despite many trace calls. The guard now reads the class-B emitter pointer; in a pull
build this is the exact expression the macro previously produced. A backend with no native
fence slot, or a fence creation that returns nullptr, retains GL_Sync.cpp's old monolith
always-signaled fallback **on the server**. Real native wait answers are never replaced.

## Validation

- Split build successful, clang Release, maximum -j8.
- Related unit selection: 40/40 (RemoteEmitTable/FieldOwnership/FenceWireRoundTrip at that
  point). Final FenceWireRoundTrip selection after adding the decline control: 4/4.
- Real GL `DirectGLES.Split.SyncWireScenario` plus private-log gate: 2/2.
- The GL scenario asserts all five sync records crossed, waits for GPU completion, validates
  rendered pixels and leaves one orphan to exercise session teardown.
- Wire controls preserve timeout (rather than silently returning signaled), flush flags and
  64-bit timeout; reject stale reused handles; verify native deletion and null-native fallback;
  missing consumers produce DECLINED with no manufactured answer.
- Representative Minecraft retraces: 4/4. Sodium DirectGLES/DirectVulkan SSIM 0.999986;
  vanilla in-world both 0.999976. Same values as standalone d1.
- gen_pipe.py --check and gen_pipe_field_ownership.py --check pass; git diff --check passes.
- No full-stage gate or review repeated. G1 and remaining quick lanes belong to the integrator.

Logs: `/home/swung/w7/p5b-sync-{unit,unit-final,integration}.log`;
trace summary `/home/swung/w7/retrace-out/p5b-sync-targets/summary.json`.

## Integration notes

The four A/B trace targets' immediate missing verb was named framebuffer blit, owned by the
integrator; sync completes real fence semantics rather than removing that particular blocker.
Cherry-picking over the integrator's blit change may need B/C count arithmetic combined and
both new ServerVerbSink declarations retained. r1 was deliberately not merged here.
No backend implementation changes, no adb, no push.
