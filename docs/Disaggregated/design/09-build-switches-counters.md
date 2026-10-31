# Build layout, switches and counters (formerly ARCHITECTURE §16, appendices A and B)

> Design details. The design points and the index of all chapters are in [`../ARCHITECTURE.md`](../ARCHITECTURE.md). Section numbers follow the original `ARCHITECTURE.md`; an `ARCHITECTURE.md §N` in a code comment is found by its number here.

## 16. Build layout

```
MobileGL/MG_Pipe/            always in the build: the *.def catalog and tables, MGPipe*.h payloads / handles / callbacks, PipeApply, PipeRoute, generated/*.inc
MobileGL/MG_Impl/Pipe/       Tracker, PipeFill, SlotAllocator, CsoCache, CompositeResolver, ResourceTracker, SetHashSuppressor, *Emit.h
MobileGL/MG_Backend/MGPipe/  PipeInputs.{h,cpp}
MobileGL/MG_Remote/          only with MOBILEGL_BUILD_DISAGGREGATED: CONTRACT-*.md, CapsCodec, FatalFamilies.def / FatalFunnel, Handshake.h
  Protocol/   protocol.fbs, generated/, SurfaceOpCodec, mg_protocol_base.h
  Transport/  ITransport, SocketTransport, InProcessTransport, ILink, ShmLink, StreamLink, Ring, SessionRings, ReplySlot, EventRing,
              Doorbell, ShmSegment(+Posix/Win32), FdPassing, Framing, AuthToken, ControlInbox, LinkMetrics, WireLog
  Wire/       PipeWireCodec (record codec, WireVerbSink)
  Client/     BackendObject_Remote, ClientSession, EmitTables, WireTables, CapsMirror, PersistentMapTracker, GpuWritePending
  Server/     PipeApplier, ServerLoop, ServerSession, ServerMain / ServerEntry / ServerSpawn, PreAuthGate, StagedShadow, StagedTextureStore,
              SurfaceControlFrame, ServerDisplay, InProcessServer, DisplayServerJni
```

- `MOBILEGL_BUILD_DISAGGREGATED` (default ON) adds `MG_Remote/**`; when OFF, `MG_Config::Transport` is a `constexpr Monolith`. `MOBILEGL_BUILD_DISAGGREGATED_INPROC` implies the former and adds role isolation: the two roles are separated by the apply thread and the control mailbox, rather than by adding TLS to the 1494 `pGLContext->` read points.
- `MOBILEGL_TRANSPORT = monolith | inproc | spawn` is the **topology** (G1-relevant); the control plane and data plane are selected by the two variables in §11.9. **A split build with `MOBILEGL_TRANSPORT` unset is the monolith control arm**.
- Three build flavors (since P13): default (the split shape, `MG_Remote` compiled, the in-process shims), verify, and monolith (the shipping shape: configure with `-DMOBILEGL_BUILD_DISAGGREGATED=OFF`, `MG_Remote` not compiled); Android has two APK flavors (monolith / split). The pull build and the `MOBILEGL_PIPE_PUSH` CMake option are deleted in P13.
- Modules (P13 W7): `SOURCE_FILES` is split into seven OBJECT libraries according to the link ratchet's PARTITION, and `MobileGL` / `MobileGL_s` are assembled from them; in a split build `MobileGL_server_linkcheck` links only the SERVER and SHARED modules with `--no-undefined`, so a frontend reference outside the baseline fails at build time (`scripts/ci/server_link_check.py`). The record arm's server-side code is in `MG_Backend/Record/` (`RecordVerbSink`, `StagedShadow`, `StagedTextureStore`), and client verb emission is in `MG_Impl/Pipe/Verb/`, no longer depending on `MG_Remote`.
- Test wiring: the ctest `ENVIRONMENT` is built with `mgl_itest_join_environment(...)`; every split / spawn / tcp entry has its own log path, and logs are split per role (`<base>.client.log` / `<base>.server.log`); the spawn and tcp lanes match the split name set (`scripts/ci/spawn_lane_parity.py`), and each entry records its arm proof.
- CI (`.github/workflows/test.yml`, `apk.yml`): `pipe-gates` (G1–G8, the generator self-test, the symbol report, dirty-surface, field ownership, G5, the documentation-reference lint), `flatc-check`, the include closure, the build / integration / retrace lanes of each flavor, APK + AVD. The `feat/disaggregated` triggers in both workflows are temporary and are removed before merging into `dev`.

### 16.1 Wiring rules for the integration test lanes

Every lane in `MG_IntegrationTest/CMakeLists.txt` must be able to go red for the reason it exists (§13.2); the wiring rules:
- A case that reads the library log must own the log exclusively: the library opens the log with `fopen(path, "w")`, and every process starting on the same path truncates it, so under `ctest -j` a reader sees a neighbor's truncated file, with the same symptom as "the counter was never emitted". An entry therefore carries a private `MOBILEGL_LOG_FILE_PATH`, and the filter selects only that case; when a lane must run a whole scenario (re-filtering would change the test names), it adds a `RESOURCE_LOCK` named after the log, and ctest will not run entries holding the same lock at the same time.
- Every variable that selects an arm is written as the entry's own ctest `ENVIRONMENT` property: the property overrides only the variables it states, and the rest leak in from the job environment (a job-level `MOBILEGL_TRANSPORT=inproc` quietly turns a monolith entry that does not pin `monolith` into another split).
- ctest does not count SKIP as a failure, so a lane that skips everything is as green as a lane that passes everything: a lane that must really run pins `MOBILEGL_ITEST_REQUIRE_GPU=1`, so an unavailable harness becomes a failure; a feature opt-in (such as `MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1`) must reach the process that reads it in each arm, and the tcp arm's server is started ahead of time by the `TcpServer.Start` fixture and does not inherit the client entry's environment, so it must be set on the fixture as well.
- `ctest -L` matches by regular-expression substring: a new label must not contain any existing label that is selected without anchors, so the qualifier goes in the middle (`integration-magma-all-spawn` does not contain `integration-magma-spawn`, and `integration-magma-spawn-buffers` does not contain `integration-magma-buffers`); selecting a single label is written `'^label$'`.
- A gated lane carries a private second label and runs with `--no-tests=error`: when the build lacks the option the match is empty and fails, rather than a zero-entry green.
- Three transport arms: `Split.` is inproc, and the server is the apply thread in the test process; `Spawn.`'s server child inherits `::environ` (`ServerSpawn.cpp` removes only `MOBILEGL_TRANSPORT` and the `MOBILEGL_IPC_*` that are not server-owned); `Tcp.`'s server is the `TcpServer.Start` fixture shared by the whole lane, started once by `scripts/ci/tcp_server_fixture.py` with its own environment, forking one session child per Hello.
- A knob read by the server in an entry's ctest `ENVIRONMENT` reaches only the inproc and spawn servers; a tcp copy would run the default arm under a name claiming the knob and would never go red. Such entries register split + spawn only (for Magma they are listed in `MAGMA_SERVER_ENV_KNOB_NO_TCP` in `scripts/ci/spawn_lane_parity.py`), and the twin entry with the knob at its default keeps all three arms; a tcp arm requires the fixture to carry the knob: append it to `TcpServer.Start`'s `ENVIRONMENT` when it is harmless to the whole lane, otherwise start a separate supervisor that carries it (`P8Coverage.cmake`). Probes that can only be read in this process (the descriptor-pool census, the server loop hooks, the server cache peeking, validation-layer output) register for inproc only (`MAGMA_INPROC_ONLY`).
- Magma has three layers: the gated `integration-magma-{split,spawn,tcp}` lanes are hand-arranged, each with its flavor (strict + dual block, adoption tier, run-ahead credit, cache pinning), and enter the CI gate only after passing; `integration-magma-all-*` replays every `mgl_itest_register_split_arms` call; `integration-magma-full-*` runs the whole integration binary minus `MGL_ITEST_MONOLITH_MECHANISM_FILTER` and `DualBlockScenario`. The last two layers enter no CI gate, and a case is promoted to the gated layer by the commit that fixes its blocking point.
- `MGITEST_SPLIT_LANE=1` arms the `ScenarioFixture` destructor assertion: the case must advance the client encoder's record sequence number, and the tcp arm must also prove there is no local server child process. It suits only curated lanes where every case draws; the whole-binary sets (`integration-magma-full-*`, `integration-verify-split`) do not set it, because pure query cases are answered by `CapsMirror` and produce no records, at the cost that these sets do not prove records crossed the wire.

## Appendix A: switches

CMake: `MOBILEGL_BUILD_DISAGGREGATED` (ON), `MOBILEGL_BUILD_DISAGGREGATED_INPROC` (ON, implies the former), `MOBILEGL_PIPE_VERIFY` (OFF, never ships), `MOBILEGL_BUILD_SERVER_SPIKE` (Android spike only), `MOBILEGL_FLATC_EXECUTABLE` (serves `flatc-check` only). `MOBILEGL_PIPE_PUSH` and `MOBILEGL_PIPE_LEGACY_MEMOS` are deleted in P13 (an OFF in an old cache only warns and is ignored).

At runtime, MGPipe (`MobileGL/Config.h`, `ConfigLoader.cpp`):

| Variable | Default | Description |
|---|---|---|
| `MOBILEGL_PIPE_PUSH` | `0x3fff` | subsystem bitmap (`MG_Pipe/MGPipe.h`, bits are never reused); since P13 bits 0–13 are fixed on, and clearing any of them is refused by name at startup; only bit 63 ("the behavior control for turning off CSO content addressing") can still be set |
| `MOBILEGL_PIPE_VERIFY` / `_VERIFY_FATAL` / `_VERIFY_CORRUPT` | 0 / 1 / empty | the shadow comparison and its negative controls. The comparator is compiled only into the verify build; in other builds `MOBILEGL_PIPE_VERIFY=1` only warns; `_VERIFY_CORRUPT` is not in `Config.h`, and only the verify build reads it when the comparator is armed (`ArmVerify` in `PipeFill.cpp`, likewise through the environment and the system configuration file). The poison negative control has no runtime switch: the integration test's harness flag `MGITEST_POISON_OMIT` is armed by a re-exec'd child process through `MGPipeSetPoisonOmission`, which only the verify build has (`Harness/PoisonOmissionArm.h`) |
| `MOBILEGL_PIPE_STATS` / `_STATS_PERIOD` / `_STATS_FILE` | 0 / 120 / empty | boundary counters (Appendix B); the dump is written per role as `<base>.client.json` / `<base>.server.json` |
| `MOBILEGL_PIPE_INDEX_MIRROR_MB` | 64 | the index mirror budget (P8); `_LEGACY_MEMOS`, `_TEXEL_RETAIN_MB` and `_HANDLE_ABA_CONTROL` are deleted with their arms in P13 |

At runtime, transport and IPC:

| Variable | Default | Description |
|---|---|---|
| `MOBILEGL_TRANSPORT` | `monolith` | topology: `monolith` / `inproc` / `spawn` |
| `MOBILEGL_IPC_CONTROL` / `MOBILEGL_IPC_DATA` | `fork` / `auto` | the control-plane endpoint and the data plane (§11.9); `MOBILEGL_IPC_SERVER_PATH` locates the forked server |
| `MOBILEGL_IPC_TOKEN` / `MOBILEGL_IPC_REQUIRE_SAME_BUILD` | empty / 0 | the token (≥16 bytes; without a token only loopback is allowed); require the same build on both ends (lane discipline) |
| `MOBILEGL_IPC_PREAUTH_*` / `MOBILEGL_IPC_AUTH_BACKOFF_*` | — | the server pre-authentication limits and the failure backoff (Ph) |
| `MOBILEGL_IPC_CONTROL_TIMEOUT_MS` / `MOBILEGL_IPC_COLD_START_MS` | 5000 / 20000 | the steady-state cap on control replies and the cold-start budget |
| `MOBILEGL_IPC_LOG_FORWARD` | on when remote | server logs are forwarded over the control connection, and the client writes them into its own `<base>.server.log` |
| `MOBILEGL_IPC_SURFACE` | `offscreen` | `server` = use the server's own window, with the client headless (P12) |
| `MOBILEGL_IPC_RING_MB` / `MOBILEGL_IPC_STAGE_MB` | 8 / 32 | `SEG_CMD` / `SEG_STAGE`; a quarter of stage is the content chunking budget |
| `MOBILEGL_IPC_RUN_AHEAD` | 1 | P5e run-ahead; only one half of the conjunction (the server must publish `kCapRunAheadApply`); `0` is the A/B control |
| `MOBILEGL_FRAMES_IN_FLIGHT` | 3 | 1..64, shared by both backends (the old name `MOBILEGL_MAGMA_FRAMESINFLIGHT` is a deprecated alias): the number of Magma frame contexts; Espryt waits for frame S+1-N to complete after presenting S; the split derives the present credit from it as max(1, N-2) (P15) |
| `MOBILEGL_IPC_PRESENT_CREDIT` | derived from `MOBILEGL_FRAMES_IN_FLIGHT` (default 1) | 1..8, §14; an explicit setting overrides the derived value, for experiments only |
| `MOBILEGL_IPC_POLL_ESCALATE` | 64 | a zero-timeout poll for an unreported fence answers "not complete" locally, and only the Nth consecutive poll of the same fence makes a roundtrip (P10, `CONTRACT-P10.md` §1); `0` = a roundtrip on every poll, the A/B control |
| `MOBILEGL_IPC_BATCH_WAITS` | 1 | under lockstep, value-class records return as soon as they publish; in the verify build `MOBILEGL_PIPE_VERIFY=1` forces 0 (`RUN_AHEAD` likewise forces 0, `ROLE_SPLIT_STATE` forces it off; other builds do not change them) |
| `MOBILEGL_IPC_EVENT_WAIT_MS` | 2000 | the whole budget for the server to wait for the client to drain when a reverse event meets a full ring (§11.7) |
| `MOBILEGL_IPC_WIRE_DEFERRED_MB` | 64 | the watermark for deferred reclamation on the Magma wire arm; `0` is the M2 negative control |
| `MOBILEGL_IPC_PERSISTENT_HASH_SUPPRESS` | 1 | persistent-map pushes send only the changed blocks (fixed 64 KiB granularity, `PersistentMapTracker::BlockBytes()`); `0` is the A/B control that pushes the whole range |
| `MOBILEGL_IPC_ADOPT_TIER` | unset = T0 | unset (the user made this the default on 2026-09-29) = T0 when it can be used, with a quiet fallback to T2 when it cannot (stream, no AHB, POST failure, server does not allow it): only MGLOG_D, counted as usual; `0` = ask for T0 by name, and the fallback has a named line (A1's `Refuse{AdoptTierOnStream}` and so on); `1` = T1 is off, a named refusal and then T2; `2` = T2 (the pre-P11 default). `CONTRACT-P11.md` B2 |
| `MOBILEGL_IPC_ALLOW_ADOPT_T0` | 1 | server-owned: `0` = never approve T0 (the session goes T2), preserved through `ServerSpawn` |
| `MOBILEGL_IPC_STRICT_ERRORS` / `MOBILEGL_IPC_AUDIT` / `MOBILEGL_IPC_ROLE_SPLIT_STATE` | 0 / 0 / 1 | residual input reads escalate to Fatal / retired staging is filled with `0xDD` / dual block (P5f; on by default since P15, `0` turns it off, and the negative control sets 0 explicitly; under dual block the barriered fill writes the client block and no longer quiesces the applier) |
| `MOBILEGL_IPC_SPIN_US` / `MOBILEGL_IPC_SERVER_AFFINITY` | 50 / `auto` | pre-park spin budget / apply-thread affinity (ignored by the Redmi kernel) |
| `MOBILEGL_IPC_RECOVER` | 1 | after a session ends from device loss, the application's next `eglCreateContext` / `eglCreate*Surface` / `eglInitialize` dials again for a new session (old contexts stay lost, surfaces are rebuilt in the new session); `0` = the loss is permanent for the whole process |
| `MOBILEGL_IPC_RESPAWN` | — | refused by name: no phase implements a full re-push after a server restart |

Explicitly not introduced: `MOBILEGL_IPC_PROGRAM` (there is no relink tier), `MOBILEGL_IPC_VALIDATE_SERVER`. Planned but not wired: `MOBILEGL_IPC_IDLE_EXIT_S` (unresolved).

## Appendix B: boundary counters (`MobileGL/MG_Util/Metrics/PipeStats.h`)

When off, each site costs one global load + one branch that never hits. The byte kinds (stage buffer / texture / UBO / client arrays, `pmap`, `resid`, `csob-blob`, `seg` = bytes written into `SEG_STAGE`), the call kinds (`draws`, `accessor-calls`, texture upload emit / box / rect / jobs, `csom` / `csob`, `mpr`, `trp`, `rsp`, `wrec`, server-applied control records `ar` (applier_reset) / `od` (object_death), `maxrec` / `ringwraps` / `ringpads` / `ringwaits`, the doorbells `srv` / `srvpark` / `cli` / `clipark`), and the hit/miss of the six memo gates. Every `MOBILEGL_PIPE_STATS_PERIOD` frames there is one `MGPipe stats:` summary line, written to the log per role. Reading traps: `accessor-calls` is a static lower bound, and judging performance looks only at CPU time; under spawn the client does not advance the frame window (`frames=0`), so windowed fields are invalid on a spawn client while the run total stays valid (see git history §12.1). The site list (which paths are **not** wired) is at the top of `PipeStats.cpp`, and that list is a contract.
