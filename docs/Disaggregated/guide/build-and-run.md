# Build, run and test

> Getting started. For the documentation index see [`../README.md`](../README.md); for every switch see [`../design/09-build-switches-counters.md`](../design/09-build-switches-counters.md) appendix A.

## Three builds

Since P13 there is only one single-process shape: the backend takes its state solely from the records the frontend pushes (the record arm); the old "backend reads the frontend directly" path went away together with the pull / push builds (`MOBILEGL_PIPE_PUSH` is no longer a CMake option).

| Build directory | CMake | Purpose |
|---|---|---|
| `build-linux` | default | The shipped shape (the FCL embedded library is this one): single process, record arm, `MG_Remote` not built |
| `build-verify` | `-DMOBILEGL_PIPE_VERIFY=ON` | Every draw compares the pushed state field by field with the frontend's actual state; slow, never shipped |
| `build-split` | `-DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON` | Can split into two threads / two processes / two machines; with `MOBILEGL_TRANSPORT` unset it is the single-process control on the same record arm |

The library is assembled from seven OBJECT modules (`mg_util` / `mg_pipe` / `mg_backend` / `mg_frontend` / `mg_remote_transport` / `mg_remote_client` / `mg_remote_server`, partitioned by `scripts/link_ratchet.py`); the split build also has `MobileGL_server_linkcheck`, which proves the server half links without the frontend modules (the remaining frontend references are named debts in the link-ratchet baseline).

## Choosing a topology

The split build selects the topology at run time with `MOBILEGL_TRANSPORT`; **unset means the single-process (monolith) control**:

| `MOBILEGL_TRANSPORT` | Shape |
|---|---|
| unset / `monolith` | Single thread, direct calls |
| `inproc` | Two threads in one process (the main CI acceptance shape) |
| `spawn` | Two processes; `MOBILEGL_IPC_CONTROL` (`fork` / `unix:<path>` / `tcp://host:port`) and `MOBILEGL_IPC_DATA` (`auto` / `shm` / `stream`) then choose the control and data planes; across machines also set `MOBILEGL_IPC_TOKEN` (at least 16 bytes) |

```text
MOBILEGL_TRANSPORT=inproc MOBILEGL_ITEST_REQUIRE_GPU=1 ctest --test-dir build-split -L integration-split --output-on-failure
ctest --test-dir build-split -L 'integration-(spawn|tcp)' --output-on-failure   # the entries carry their own spawn / tcp environment and private logs
```

Other common knobs: `MOBILEGL_IPC_RUN_AHEAD` (default 1, the client sends and moves on), `MOBILEGL_IPC_STAGE_MB` (default 32; a quarter of it is the chunk budget for large uploads), `MOBILEGL_IPC_SURFACE=server` (use the Android server's own window), `MOBILEGL_PIPE_STATS=1` (boundary counters, the `MGPipe stats:` lines in the log).

## Android

- Two APK flavors: single-process / split (Gradle properties `mobilegl.buildDisaggregated`, `mobilegl.buildDisaggregatedInproc`; a standalone plugin build defaults to split, the FCL embedding defaults to single-process). The split APK also needs `MOBILEGL_TRANSPORT`; unset means the single-process control.
- The on-device server has two shapes: an off-screen foreground Service (`MobileGLServerService`) and the on-screen `MobileGLDisplayActivity`; only one at a time. Both the plugin and trace flavor APKs carry them (standalone builds enable `mobilegl.buildDisaggregated` by default); the entry point is the "Render Server" page in the APK's bottom navigation. In the plugin flavor these three components are not exported; only the trace build allows adb to start them directly by component name.
- For the cross-machine setup with the computer as client and the phone as server (starting the server, setting the token, the `adb forward` fallback), see "configuring the two sides" in the phase notes (in git history).
- Performance comparisons use only the Redmi `2f7cbe2e`, pinned as described in the pin-verification record (in git history): reboot-clean, same thermal window, paired A/B. Performance work after P13 uses the `dev` build as its "before" baseline (user, 2026-10-07).
