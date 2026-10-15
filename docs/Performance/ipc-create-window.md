# The client's create window (MOBILEGL_IPC_CREATE_WINDOW)

Measured 2026-09-26 on the split stack (client and server in separate processes). These are the numbers
that used to live in the comment above `IpcTable::CreateWindow` in `MobileGL/Config.h`. The longer record,
with the logs and the attempts that failed, is
[`notes/p12/CREATE-WINDOW-MEASURED.md`](../Disaggregated/notes/p12/CREATE-WINDOW-MEASURED.md) (in Chinese).

## What the knob does

Every new buffer, texture or renderbuffer sends a `resource_create` record. When these numbers were taken,
the client waited for the server's answer to every one of them, because a create the server refused must be
un-published so that later records for that object take the blocking path.

With a window of N, up to N creates go out without waiting. Each caller reads a provisional accept, the
answers are collected in order, and the oldest is waited for once the window is full. A create that comes
back declined is un-published when its answer is collected. 1 turns the window off.

The window runs only on a link that keeps a declared answer until its reader takes it
(`ILink`'s `RetainsReplies`, the stream link's declared-answer store). On the shared-memory link it is
always 1.

## Setup

- Server: an Android phone (a generic system image on a MediaTek mt6893 with a Mali GPU, Android 14).
- Client: Minecraft on a separate machine, connected over TCP with the stream data plane.
- Workload: the single long frame in which the game loads its world. Every arm sends the same records:
  4,536 creates, 103,097 records in all and about 101 MB of staged bytes, with identical per-op counts.
- "Blocking waits" counts the times the client blocked on a create's answer, on the blocking path or while
  collecting the oldest answer from a full window (`by_op 2` in the client's link metrics). "Wall clock" is
  the duration of that frame.

## Why the window did nothing at first

The first version changed nothing: window 4 and window 1 both took 4,536 blocking waits and 28.64 s. It
took three findings, in this order, to make it work:

1. The client kept answers in a ring of the last 8. Between two creates the client emits on the order of
   a thousand records, so the answer was always gone by the time the client came back for it (at the first
   miss the head wanted seq 310 while the ring held seqs 1,175 to 1,504, and it wanted 310 on every miss
   after that).
2. A residency budget that tried to collect answers early could not help: what displaces an answer is
   what the server sends, not what the client emits, and the server is behind.
3. So the fix was retention: a link keeps a declared answer until its reader takes it.

The answer is kept because the wire declared that it will be read, not because the caller did not wait.
The window's own creates are both fire-and-forget and read, so a flag keyed on "the caller does not wait"
would switch off the very mechanism it exists to enable.

## Results

### With retention, both ends rebuilt

| window | blocking waits | wall clock | round trip per blocking wait |
|---:|---:|---:|---:|
| 1 (the original blocking shape) | 4,536 | 28.66 s | 5,672 us |
| 2 | 2,500 | 17.27 s | 5,285 us |
| 4 | 1,644 | 11.02 s | 4,082 us |

The round trip does not grow as the window deepens. The roughly 5 ms is a wakeup cost that the server
batches away, not its apply throughput, so collecting four creates per wakeup is a real 2.6x.

A first sweep with three interleaved runs per arm showed the curve still falling at the old ceiling of 4
(1 to 2 saved 10.29 s, 2 to 3 saved 3.74 s, 3 to 4 saved 1.83 s), so the ceiling was raised.

### Sweep past 4 (one binary, three interleaved runs per arm)

| window | blocking waits | wall clock, median | spread |
|---:|---:|---:|---:|
| 4 | 1,503 | 13.23 s | 11.37 - 16.29 s |
| 5 | 1,277 | 11.31 s | 10.61 - 12.12 s |
| 6 | 1,155 | 10.79 s | 9.69 - 10.81 s |
| 8 | 859 | 10.96 s | 8.92 - 11.03 s |

## Reading

- 4 to 5 saves 1.92 s, 5 to 6 saves 0.53 s, and 6 to 8 saves nothing (-0.18 s, inside the session noise
  of about 1.5 to 2 s). The curve is flat from about 6.
- Past that point the frame is not limited by the window: window 8 has 296 fewer blocking waits than
  window 6 and the wall clock does not move.
- The answer store is not the limit either. Its peak, logged by the stream link, read 6 to 11 entries
  against a capacity of 24 at every window from 5 up.
- What remains is a roughly constant 350 to 400 creates that block for reasons the window depth does not
  reach, and the 101 MB and 103k records the server has to apply. A later server-side timing at window 6
  put the server's apply time at 1.95 s of a 9.64 s frame, while the client's 1,161 blocking waits at
  about 4.9 ms each came to 5.7 s. Sending the same bytes over USB instead of Wi-Fi did not change the
  wall clock.

## What shipped

| commit | default | ceiling |
|---|---:|---:|
| `3a255580` (retention lands) | 2 | 4 |
| `102f2ca7` (sweep above) | 6 | 8 |
| `3a6c5c7f` (current) | 10 | 12 |

No sweep beyond 8 is recorded. 10 sits on the flat part of the curve, and a deeper window costs only
entries in the stream link's answer store. The ceiling, `kCreateWindowMax` in
`MobileGL/MG_Remote/Client/WireTables.cpp`, is held by a `static_assert` to at most half of that store's
24 entries; the other half is for the one blocking create in flight and for answers a retired window has
left behind. The store's overflow is a named fatal error, never an eviction. The environment variable
accepts 1 to 12; a value outside that range is ignored with a warning and the default is used.
