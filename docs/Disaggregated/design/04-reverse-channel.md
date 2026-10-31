# Reverse Channel (formerly ARCHITECTURE §8)

> Design details. The design highlights and an index of all sections are in [`../ARCHITECTURE.md`](../ARCHITECTURE.md). Section numbers follow the original `ARCHITECTURE.md`; `ARCHITECTURE.md §N` in code comments resolves by number here.

## 8. Reverse channel

### 8.1 `MGPipeCallbacks` (`MG_Pipe/MGPipeCallbacks.h`)

Seven named callbacks replace the 95 call sites where the backend directly pokes frontend objects (the naming is a deliberate deviation D8). Under monolith they are direct calls; under split they are records on `SEG_EVENT`.

| Callback | Role |
|---|---|
| `OnGlError` | deferred GL errors; **must be ordered with respect to the command stream** |
| `OnGpuWritten` | buffer ranges the GPU has written (narrowing the client's conservatively built pending set) |
| `OnBufferWriteback` | PBO readback, XFB capture results; must be ordered with the epoch bump |
| `OnTextureWriteback` | texels of CPU-fallback generated mips |
| `OnMipLevelsGenerated` | shape only |
| `OnSurfaceChanged` | format and size of the default framebuffer (when the server owns the display, this is how the client learns the window size) |
| `OnCapsInvalidated` | replaces `InvalidateCompileEnv` |

`OnLog` has been removed (P9 W3; together with `OnTexturePullRequest` removed in W2, callbacks 9 → 7): zero producers, zero consumers, no `EventKind`. Server logs go through the control-plane `LogLine` (with `level`), forwarded by the queue + sending thread in `MG_Remote/Transport/LogForward.h`: ≤WARN is lossy (dropped when the queue is full, counted, the gap marked in-stream by `LogForward{Dropped}`), ≥ERROR is lossless + rate-limited (over-rate lines queue with delay; when the error byte cap is exceeded they are merged into one `LogForward{Coalesced}`), FATAL waits with a bound to be written out; the logging thread never waits on the socket.

Frontend objects invented out of thin air by the backend (Magma placeholder textures, the swapchain default FB placeholder) become server-native; truly deleting the `m_backend`-style members in pull builds would change `sizeof` (G1), so they retire with P13 (D-K).

### 8.2 Ordering is a correctness requirement

The epoch bump after each writeback must be applied by the server before any later command that reads that handle. **The reverse channel needs the same ordering guarantee as the forward channel.**

### 8.3 Errors, acks and logs

- **The only entry allowed to ack synchronously is `glBufferStorage`**: `ResourceRespecify`'s `kNeedsAck` is narrowed by a per-record predicate to the "immutable **buffer**" (D-A2); texture OOM was already deferred to sync time under monolith. respecify carries per-level scope and a "metadata-only" form, and does not mistakenly drop accepted pending uploads.
- All other errors are late, going through the ordered `OnGlError`.

#### 8.3.1 The server log forwarding queue

`LogForwardChannel` in `MG_Remote/Transport/LogForward.h`: `Offer` only copies one line into the queue under the channel mutex, and the single sending thread writes frames in enqueue order, holding no lock while writing the socket, so for lines below FATAL, forwarding costs only one lock plus one copy (`LogForwardCounters::maxOfferNs`), regardless of whether the peer is reading. The server's own log file is written before forwarding (`Log.cpp`), so it always has all lines.
- DEBUG / INFO / WARN are lossy: a line that would push the lossy bytes over `lossyQueueBytes` is dropped and counted; when the gap closes (the next ≤WARN line that fits, a `LogFlush` ack, or `Close`), the channel itself inserts a WARN `LogForward{Dropped}` stating the count, bytes and start time.
- ERROR is lossless and rate-limited: a token bucket (`errorBurstLines` to start, replenished by `errorLinesPerSecond`); over-rate lines queue in order with delay and are not dropped; when queued error bytes exceed `errorQueueBytes` (8 times the lossy cap), further lines are not enqueued, replaced by one ERROR `LogForward{Coalesced}` placed before the next entry that can be enqueued.
- FATAL = ERROR + waiting: wait at most `fatalFlushMs`, until the sending thread has written it and everything before it; after one wait times out, later FATALs do not wait until the sending thread writes anything (`MGLOG_F` also marks survivable cases, which may fire in succession). `Log.cpp` hands off FATAL lines outside the log lock.
- The `LogFlush` ack (`OfferControlFrame`) is ordered after every previously submitted line, so `MGPipeSyncPeerLog`'s "when the ack arrives, all preceding lines are already in the file" holds; while it is queued, the sending thread is not rate-limited.
- `Close()` accepts no more lines and gives the queue `closeDrainMs` to drain without rate limiting; if the sending thread is still stuck in a write, it calls the owner-supplied `unblock` (`ServerMain`: `shutdown(2)` on the control socket) so the thread can be joined rather than leaked, and remaining entries count as abandoned.
- Only the TCP server form forwards (`ServerMain.cpp`, `MOBILEGL_IPC_LOG_FORWARD`); the unix spawn server and the inproc apply thread write `<base>.server.log` themselves.

### 8.4 Texture re-mint: all bytes are in the server's hands, no pull (P9 W2 withdraws the original "only new stall class" D-B6)

The original design assumed the server kept no texels and re-minting had to pull levels back from the client. After P5c the server has `StagedTextureStore`, and GPU-written layers are already in the GPU image — **neither backend's re-mint needs any bytes from the client**:

| | Source of re-mint bytes under split | monolith |
|---|---|---|
| Espryt | `RequireImageBindableStorageByHandle`: layers that really exist on the driver are read back from the old GPU texture one by one, with pending boxes merged using the store; layers the driver does not have (defined only after the last sync) are not read, store coverage replays the store, otherwise they are skipped; when the driver refuses the read while the store covers it, fall back to the store; layers already immutable and needing no widening are kept as they are | the frontend arm replays the client shadow |
| Magma | rebuild the VkImage with `STORAGE`, `PreserveTextureContentsOnRecreate` GPU→GPU copy | same as left |

- Changing only what the store reads is not enough: GPU-written layers have old bytes or no bytes in the store; in the corpus both Espryt re-mints (`iris-photon`, `iris-derivative`) are render targets with no bytes in the store.
- "Whole-format regeneration" on both arms uploads only the pending layers, with no readback and no pull; "view source re-mint" leaves only the source needing widening and goes through the same ByHandle path.
- Removed: `OnTexturePullRequest` (together with `OnLog` removed in W3, callbacks 9 → 7), the `texture-remint-pull` flag. op 50 `ResourceSubDataComplete` is append-only and kept as a retired line (decoding rejects it). `MOBILEGL_PIPE_TEXEL_RETAIN_MB` has no consumers left, but in pull builds it is deleted with P13 (G1).
- `ImageBindableHint` is still a precaution: if the hint arrives before the first sync, storage is allocated image-bindable directly, saving one server readback + retransfer.
- Known dev defect (not P9): monolith Espryt's frontend arm replays the client shadow, overwriting GPU-written texels.
- Census, coverage matrix, gates and red-once: see the git history.

### 8.5 XFB scatter stays on the server

The patch loop needs "the bytes before capture", and after the split the authoritative shadow belongs to the server (`StagedShadow`), so scatter runs to completion in place on the server; the return trip is a single `OnBufferWriteback` carrying the patched whole range, sliced to a quarter of `SEG_EVENT` capacity (`MGPipeBufferWritebackSliceBytes`), and landing the last slice implies every preceding one. Orphan targets (`HasDefinedContent == 0`) are zero-filled and scattered as usual. `OnXfbScatterReady` has been removed (callbacks 10 → 9). For the landing argument see the git history (espryt D1).
