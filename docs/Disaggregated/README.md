# MGPipe: MobileGL frontend/backend split

**What it does**: MGPipe puts an explicit interface between MobileGL's frontend (which handles the application's GL calls) and backend (which calls the GLES / Vulkan driver) — the frontend **pushes** state changes to the backend, and the backend no longer reads the frontend's memory directly. With this interface, the frontend and backend can run on two threads, two processes, or even two machines.

**How far along**: see [`CURRENT_STAGE_PROGRESS.md`](CURRENT_STAGE_PROGRESS.md). The current phase and next steps are maintained only on that page and are not repeated here.

## One diagram

```
The application's GL calls
  → frontend MG_Impl: handles GL semantics; before every command that does work (draw, clear…), turns state changes into records
  → MGPipe interface (two function pointer tables, generated from the call catalog PipeCalls.def)
  → one of three: direct call (single-threaded) │ queue → another thread │ queue → another process / another machine
  → backend MG_Backend: Espryt (GLES) / Magma (Vulkan), updates its own state from the records and calls the driver
  ← reverse messages (errors, GPU writeback, window size) come back to the frontend in order
```

## Documentation map

| To learn | See |
|---|---|
| Where things stand, next steps | [`CURRENT_STAGE_PROGRESS.md`](CURRENT_STAGE_PROGRESS.md) |
| Key design points and the design of each part | [`ARCHITECTURE.md`](ARCHITECTURE.md) → [`design/`](design/) |
| How many phases there are, what each phase did | [`ROADMAP.md`](ROADMAP.md) |
| Key numbers for performance and verification | [`MEASUREMENTS.md`](MEASUREMENTS.md) |
| How to build, run, and test | [`guide/build-and-run.md`](guide/build-and-run.md) |
| Where the code is / terminology / engineering discipline | [`guide/code-map.md`](guide/code-map.md), [`guide/glossary.md`](guide/glossary.md), [`guide/discipline.md`](guide/discipline.md) |

Organization history: two cleanups on 2026-09-24 — the top level keeps only indexes, the design body moved into `design/`, getting-started and reference material live in `guide/`, and phase details live in `notes/<phase>/` (those notes have been deleted from the tree; see git history); the pre-cleanup version is commit `1fb18d9`. Earlier design contests and review drafts are in git history (`8b31de2f`, `1794ac94`, `8349babe`, `87ee17c6`).
