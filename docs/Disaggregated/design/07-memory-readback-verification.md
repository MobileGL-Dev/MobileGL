# Persistent map, readback and verification (formerly ARCHITECTURE §12–§13)

> Design details. The design points and the index of all chapters are in [`../ARCHITECTURE.md`](../ARCHITECTURE.md). Section numbers follow the original `ARCHITECTURE.md`; an `ARCHITECTURE.md §N` in a code comment is found by its number here.

## 12. Persistent map and ≥16 MiB adoption

`AcquirePersistentMap` is a permanent address-space donation (a mutable store of ≥16 MiB is adopted automatically, MC 26.3 p99 163 → 21 ms), unchanged through the whole monolith rework (D-B4). Under the split, the POST probe chooses among three tiers (measured in spike B):

| Tier | Shape | Conclusion |
|---|---|---|
| T0 | the client allocates an `AHardwareBuffer` BLOB, the server imports it (Vulkan / GLES) | the only tier that reads and writes fully on both devices and both backends (the P11 main thrust) |
| T1 | the server exports an opaque fd | only the Adreno Vulkan path |
| T2 | refuse, push on the client side | the permanently correct fallback; **the tier today's split uses** (`MOBILEGL_IPC_ADOPT_TIER=2`), forced on the stream data plane (P11 A1: at handshake `Transport::SettleAdoptTierAtHandshake` prints one `Refuse{AdoptTierOnStream}` line for 0/1 and goes T2; on a shared segment the client gets `Fatal{UnimplementedAdoptTier}` during the handshake, `CONTRACT-P11.md` §1) |

Under T2 the client-side push has three parts: no map/unmap command pair (the payload carries `hasLiveHostWrites`); at every validate point, push the reachable mapped buffers in 64 KiB blocks, with dirty-page tracking + hash suppression sending only the changed blocks (P5d); the gate is `PersistentCoherentMapScenario`. `mpr` counts every `MapPersistent` emission (a mint or a refusal both count), so it can be asserted under the monolith too.

### 12.1 Settling the adoption tier at handshake

The tier is settled during the handshake, before the first `map_persistent`, by the side that knows the data plane (`MG_Remote/Transport/AdoptTier.h`). T0 being unusable is never a `Fatal`; it is T2 plus one line: a named line when `MOBILEGL_IPC_ADOPT_TIER=0`, and only an MGLOG_D when unset, counted either way.

| Data plane | Side | knob 0 / unset | knob 1 |
|---|---|---|---|
| stream | both ends | only T2 (no shared memory); knob 0 prints `Refuse{AdoptTierOnStream, "T0"}` | `Refuse{AdoptTierOnStream, "T1"}`, T2 |
| shared segment | client | requests T0 in `Hello`'s `LinkTerms.adoptTier` (0; `kAdoptAskT0Default` when unset); whether T0 really runs is read from `kCapAdoptT0` at the first `map_persistent` (`ClientSession::AdoptT0`) | `Refuse{AdoptTierClosed, "T1"}`, requests T2 |
| shared segment | server | does not look at its own knob: it follows the client's request, approving it at the first native bind (`ServerSession::SettleAdoptT0AtBind`, with the later `CapsSnapshot` carrying `kCapAdoptT0`) or stating the reason by name | same as left |

The use-time check on the T2 path, `Client::AdoptTierIsEmulate`, reads back the settlement result: a settled session takes T2 as usual; under knob 0 / 1, a `map_persistent` that no handshake settled (a caller with no session) aborts by name as `Fatal{UnimplementedAdoptTier}`, rather than silently counting as T2.

## 13. Readback, the roundtrip list and verification

### 13.1 Steady-state zero roundtrips and the unavoidable blocking points

- Zero roundtrips: all draw / clear / blit / copy / dispatch / XFB / bind / CSO / `set_*` / upload / `present`; all caps sites; `glGetError` / `glFinish` / `glFlush`; fence and query creation and non-blocking polling; `eglSwapBuffers`; restart / multi-draw.
- Unavoidable (rare): the handshake; the surface lifetime; `glReadPixels` into client memory; the first CPU read of a buffer with GPU writes pending; sync / query results with a timeout; the `glBufferStorage` ack; texture pulls; ring / stage exhaustion and present credit.
- Across machines every `kWaitReply` (`ResourceCreate`, `SetTextureParams`, a texture `ResourceSubData`) is one RTT: the per-frame count is a mandatory measurement for P6.5, and reducing it is P9 / P10 work.

#### 13.1.1 Per-record exemptions from the kWaitReply row

`kWaitReply` is a row-level property; whether a record actually blocks is decided by a per-record predicate, and the route and the wire emit table read the same function, so one side does not wait for a reply the other never asked for. That is why the texture `ResourceSubData` listed in the third point above no longer pays an RTT per record on the wire arm.
- `resource_subdata` (`MGPipeSubDataWantsItsReply`): nobody reads the Bool in the buffer half, so it does not wait. On the wire arm the texture half does not wait either: a texture record is sent only when the server consumes the texture family (the consumer bit in the caps mirror, reading the same fact as the applier's acceptance check), so the applier's only legitimate refusal, "no consumer", cannot fall on this record; the other refusals (such as the target not being a texture, no texels or no bytes, an unknown handle) are bugs, and the server side trips a wire report or counts them by name. A record the client itself cancels (device lost, stage exhaustion) still answers "not accepted", and the level stays dirty for a retry. The wait thus moves from each record to the `SEG_STAGE` window reclamation, proportional to the window instead of to the number of calls (measured under "the wait granularity of texture uploads" in `MEASUREMENTS.md`).
- `resource_respecify` (`MGPipeResourceRespecifyWantsItsReply`): on the wire arm the buffer half does not wait; the texture half waits until the object's create has been accepted (`MGPipeHandleIsPublished`), after which acceptance is already a fact the client knows, so first use pays one blocking create; if the create is refused the latch is not set, and later calls wait for the real answer as usual, at the cost of throughput rather than silence.
- Under the monolith the answer returns directly on the calling thread at no cost, so the predicate keeps its original answer (the texture half still takes it).

#### 13.1.2 The client-side conservative GPU write set

At `glMapBuffer` / `glGetBufferSubData` on the application thread, `BufferObject::SyncGpuWrites` answers "may the GPU have written this buffer". The backend's mark points are on the server side, and asking each time is a roundtrip forbidden in the steady state, so the client builds this set from the bindings it holds before each draw / dispatch record is sent (`MG_Impl/Pipe/Verb/GpuWriteSet.h`).

- Better too many than too few: marking once too often costs one readback, while a missing mark silently reads a stale shadow. Each producer has a counter (`ProducerMarkCount`), and a unit test asserts that it really fires.
- The first four producers correspond to backend mark points: SSBO binding points, atomic counter binding points (marking every touched point, wider than the backend), image buffer textures with `Access != GL_READ_ONLY`, and XFB capture targets. Two places where the backend is deliberately narrow are copied as they are: a read-only image binding is not marked; the image walk happens at draw preparation, not at `glBindImageTexture`.
- Two producers the backend does not have: a `glReadPixels` reading into a pack PBO returns as soon as it is issued, only marking, and the copy is deferred to the first time a read really happens; under transport `glEndTransformFeedback` no longer waits for a fence without bound, only marking the capture targets (the monolith still waits).
- Narrowing is not implemented yet: `MGPipeClientOnGpuWritten` accepts only one whole-buffer range; zero ranges is reserved for a future "fully narrowed" and must not be taken as the whole buffer written.
- It does not cover the backend's own scratch buffers (Espryt's conversion vertex stream and primitive-restart replacement buffer, Magma's UBO ring): the backend side has no `MarkGpuWritten` for them either, so it is a hole the two sides share.

#### 13.1.3 glReadPixels: only the rectangle goes on the wire, the pack state stays on the client

The `read_pixels` record carries the rectangle, not the layout: `MGPReadbackInfo::DstSize` is the tight `w*h*bytesPerPixel`, and the server reads into these tight rows with a neutral pack state (that is the reply payload), after which the client—the only side that holds the application pack state—scatters the rows to the application pointer according to the application's `GL_PACK_*`, not writing the gaps between rows. If `DstSize` were the packed size, the server would allocate by it while the backend writes by the live pack state, and the write would overflow (4x3 RGBA8, `PACK_ROW_LENGTH=8`, `SKIP_ROWS=1`, `SKIP_PIXELS=2`: 80 B allocated, the driver writes up to byte 120). The rule is about layout only, and the same applies to depth and depth-stencil reads.
- When the tight answer is larger than one reply slot (`LinkTerms.maxReplyBytes`), what is split is the read, not the reply: if one row fits, split into full-width row bands of `floor(maxReplyBytes / rowBytes)` rows; otherwise split each row into column runs of `floor(maxReplyBytes / bpp)` pixels. Each band is an ordinary `read_pixels` record with its own box and tight `DstSize`, each scattered according to the pack state; the bands cover the rectangle exactly once in row-major order and are contiguous in the tight layout, so the neutral-pack fast path can still read straight into the application pointer (`PlanReadbackBands` / `ForEachReadbackBand`).
- Only when not even one pixel fits in a reply (including a link with no reply pool) does the client refuse by name as `Fatal{ReplyTooLarge}` before emitting: a pixel cannot be split smaller than itself.

### 13.2 The five-part verification gate (replacing the monolith's byte-identity gate)

1. **Interface purity**: gate A checks the include graph (`scripts/check_include_closure.py`), gate B that the server image references neither `MG_State::GLState` nor glslang (P6's a6 measured that the server still links `MG_Impl`, tracked by the downward ratchet in `scripts/link_ratchet.py`; P13 brings 172 down to 27 named debts, zeroes the `# P13` annotations, and a script refuses regressions, while W7's `MobileGL_server_linkcheck` turns it into a real link), and gate C zero `pGLContext` in `MG_Backend`; plus "every backend memo key is `{slot, gen}`" (`HandleRecycleScenario`).
2. **Semantic shadow comparison** `MOBILEGL_PIPE_VERIFY=1`: the tracker fills a second `PipeInputs` with `SnapshotFromGLContext()`, and G4 compares field by field at every draw, printing the first divergence; verify builds never ship and survive P13.
3. **Behavior A/B**: the trace corpus has SSIM ≥ 0.99 under monolith / split (before P13 there were also the monolith-pull / monolith-push arms); test names only grow, never shrink (G14), and the skip census guards "skips" and the name set; CTS is within 0.5 pp per backend; the upload-shape goldens and the recorder goldens (§13.3).
4. **Performance**: Redmi `2f7cbe2e`, reboot-clean, paired A/B in the same thermal window, with **per-thread CPU time** as the metric; **recorded only, no gate** (user, 2026-09-08). Performance rounds after P13 use the `dev` build as the baseline (user, 2026-10-07).
5. **Coverage + poison + handle discipline**: G6 0 UNMAPPED, dirty-surface 0 unmapped, G8 ownership complete, per-verb poison, G7, the residual-block ratchet.

The byte-level identity that survives: with `MOBILEGL_BUILD_DISAGGREGATED=OFF` the library has zero `MG_Remote` symbols (since P13 it is the shipping library). **G1** (the pull build's symbol set and `.text` identical to the baseline) retires with the pull build in P13, and zero `MG_Remote` symbols, the skip census and the recorder goldens take over. **Every gate must be able to go red for the reason it exists** (R-16): it has a negative control and has really run red once; a change the public GL cannot see gets a white-box assertion; the refusal census reads the private log per case (`ctest -V` is a false zero).

### 13.3 The long-term semantic gate: the MGPipe recorder (P13)

`MG_Test/Pipe/RecorderGoldenTest.cpp` (P13 W8): the recorder sits at every gate through which a record leaves the client—the two MGPipe interface tables and the route escapes (all state / object records), and the verb session (all verb records). A fixed GL call sequence (a buffer + a VAO + a sampled texture + a program + four kinds of draw; a framebuffer with two textures + two kinds of clear + blit + mipmap) is recorded record by record, and normalization does exactly one thing: the addresses of `MGPBlobRef` / `MGHostSpan` pointing at caller memory are zeroed, the lengths kept, and the bytes they point at hashed. A golden is one line per record (ordinal, call name, byte count, hash), under `MG_Test/Pipe/goldens/recorder/`; it is rewritten only with `MOBILEGL_RECORDER_GOLDEN_WRITE=1`, and the rewriting run must go red. Negative controls: changing one field of a verb record must go red on that record; changing one GL input must go red at the resource record. Both library shapes (with and without `MG_Remote`) compare against the same golden—on its first run it found that the monolith library without a transport did not send `set_context_values` (fixed), and that `RenderStateParameters`'s padding bytes were not zeroed (which would stop a content-addressed CSO from being reused between two contexts with the same state; fixed). It covers only the pushed content, not the backend's interpretation of it (open question 12).
