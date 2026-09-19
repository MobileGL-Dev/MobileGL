# P5 scout — GPU-written tracking and the persistent-map push

Tree read: `/home/swung/w7/pipe`, branch `feat/disaggregated` @ `a29807cc` (P4a landed). READ ONLY —
nothing was edited, built or run. Every line number below was opened in this tree at that commit.
Repo-relative paths are under `MobileGL/` unless they start with `docs/` or `tools/`.

P5's row is `docs/Disaggregated/ROADMAP.md:21`. The design of record for this subject is
`docs/Disaggregated/ARCHITECTURE.md` sections 8.1 (lines 275-290), 8.2 (292-294) and 12 (479-502).

---

# PART 1 — MarkGpuWritten / SyncGpuWrites

## 1.1 What they mean today

`BufferObject::MarkGpuWritten()` — `MG_State/GLState/BufferState/BufferObject.cpp:364-367`. Three lines,
and it sets **two** flags:

```
m_hasDefinedContent = true;
m_gpuWritePending   = true;
```

Declared `BufferState/BufferObject.h:185`; the flag's own comment is at `BufferObject.h:267`
("Set by MarkGpuWritten, cleared by SyncGpuWrites once the shadow is refreshed"). The contract at
`BufferObject.h:180-184`: "the flag only says a GPU write is outstanding" — whether the answer is a pull-back
or a wait is the backend's business.

`BufferObject::SyncGpuWrites()` — `BufferObject.cpp:369-395`:

* `:370` early-out when `!m_gpuWritePending`;
* `:372-374` clears the flag **unconditionally**, with the reason stated: "without a readback op the shadow
  can never catch up, and retrying on every subsequent read would only repeat the same no-op";
* `:375-387` under `MOBILEGL_PIPE_PUSH` + `MGPipeResourceSubsystemEnabled()`: `MGPipeEmitResourceReadback(*this)`
  (`:384`) and return. The comment at `:376-381` is the one P5 has to honour verbatim: the answer comes back
  through `OnBufferWriteback`, and "in monolith the whole sequence is synchronous inside the applier, so the
  caller sees the reconciled shadow **on return**".
* `:389-394` legacy arm: `g_bufferBackendOps->ReadbackFromGpu(*this)`.

## 1.2 The six producer sites (all backend-side today)

`ARCHITECTURE.md:280` says "6 处 `MarkGpuWritten`". They are:

Espryt (3), all in `MG_Backend/DirectGLES/DirectGLES.cpp`:

| line | function | what it means |
|---|---|---|
| `DirectGLES.cpp:556-572` | `MarkShaderStorageBuffersGpuWritten` | every SSBO binding point, once the points are bound and the draw/dispatch is about to go out. Push arm `MarkBufferGpuWritten(obj)` `:566`; pull arm `obj->MarkGpuWritten()` `:570` |
| `DirectGLES.cpp:573-620` | `SyncAtomicCounterBuffers` | every bound atomic counter (`:616` push / `:618` pull). Reason at `:611-614`: every conformance case reads the increment back with `glMapBufferRange`/`glGetBufferSubData` |
| `DirectGLES.cpp:2588-2606` | `MarkWritableImageBufferTexturesGpuWritten` | buffer textures on image units, **only when `Access != GL_READ_ONLY`** (`:2578-2581`), and deliberately from the draw/dispatch preparation and not from `glBindImageTexture`'s eager sync (`:2583-2586`) |

Magma (3):

| line | function | what it means |
|---|---|---|
| `MG_Backend/DirectVulkan/Renderer/UniformManager.cpp:1073-1076` | `ResolveStorageTexelBufferDescriptor` | `EnsureGpuResidentStorage()` then `MarkGpuWritten()`, again only when `Access != GL_READ_ONLY` |
| `UniformManager.cpp:1229-1231` | `ResolveStorageBufferDescriptor` | SSBO block: `EnsureGpuResidentStorage()` then unconditional `MarkGpuWritten()` |
| `Renderer/VulkanRenderer.cpp:11617-11618` | `BeginXfbCaptureForDraw` | the XFB capture targets — "the capture is a GPU write like any shader's" |

**The push-arm funnel is one function**: `BufferImpl::MarkBufferGpuWritten` —
`MG_Backend/DirectGLES/Managers.cpp:2509-2540`. Three things there are contract:

* `:2511-2514` with the subsystem off it falls straight through to `bufferObject->MarkGpuWritten()`;
* `:2516-2528` with the subsystem on and either the handle or `gMGPipeCallbacks.OnGpuWritten` missing it
  **deliberately does not fall back** — it logs `MGLOG_E_ONCE` and drops the announcement, so the A/B can see it;
* `:2530-2539` the announcement is **one** range of `{0, kMGPipeWholeBuffer}`, never zero ranges, and the
  comment at `:2533-2536` states the other half of the contract: the client's `OnGpuWritten` must set **both**
  `m_hasDefinedContent` and `m_gpuWritePending`, or the next `resource_respecify` carries
  `HasDefinedContent = 0`, `Ops_H_Respecify` orphans instead of uploading, and the shader-written contents
  are dropped with nothing saying so.

Handle lookup is `HandleOfBuffer` — `Managers.cpp:2491-2502`, which goes through the twin table's single-entry
front memo (its comment names `MarkBufferGpuWritten` as one of the three hot callers).

## 1.3 The client half that already exists

`MG_Impl/Pipe/ResourceTracker.h:583-604` — `MGPipeClientOnGpuWritten`:

* `:584-592` **asserts** `rangeCount == 1 && ranges != nullptr`, and says why in as many words:
  "zero will mean *a fully narrowed set — nothing is dirty* at P8/P9", so a zero-range announcement must not
  be read as "mark the whole buffer".
* `:593-599` an unresolvable handle is a dropped `MarkGpuWritten`, i.e. "a stale shadow read back as if it
  were current" — `MGLOG_E_ONCE`.
* `:600` `buffer->MarkGpuWritten()`.

Installed by `MGPipeInstallClientResourceCallbacks` — `ResourceTracker.h:606-614`, and only over a slot the
backend did not claim.

`ResourceTracker.h:577-581` states the direction: `OnGpuWritten` is a **NARROWING** channel — the client
builds a conservative pending set at its own emission points and the callback only ever *removes* from it —
but "P3a's implementation marks exactly what the three Espryt `MarkGpuWritten` sites mark today ... The
narrowing itself is P8/P9's."

## 1.4 The consumers — where `SyncGpuWrites` is called

`docs/Disaggregated/MEASUREMENTS.md:111` records the census as "`SyncPersistentMappedRange` / `SyncGpuWrites`
= 20 / 6, 与计划一致". The six **backend** consumers:

* `MG_Backend/DirectGLES/Managers.cpp:4922` — `SyncFloat64AttributeAsFloat32` (legacy-memos arm only)
* `MG_Backend/DirectGLES/DirectGLES.cpp:6186` — the primitive-restart rewrite's CPU read of the source EBO
* `MG_Backend/DirectGLES/MultiDraw.cpp:512` — same shape, multi-draw rebase
* `MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:3541`, `:4012`, `:4346`

Frontend consumers (not in that 6 — they are inside the object / `MG_Impl`):

* `BufferObject.cpp:451` and `:466` — the two "no resident op" fallbacks inside `LandBytesIntoResidentStore`
* `BufferObject.cpp:515` — `FillSubData` (a clear is ordered after earlier GPU writes)
* `BufferObject.cpp:547` — `CopyDataFrom`, on the **source**
* `BufferObject.cpp:561` — `AcquireMemory` (`glMapBuffer`)
* `BufferObject.cpp:639` — `AcquireMemoryRange`, skipped for the one map shape that looks at nothing
  (non-persistent write map with an invalidate bit over an adopted store — `:626-638`)
* `MG_Impl/GLImpl/Buffer/GL_Buffer.cpp:957` and `:995` — `GetBufferSubData_State` / `GetNamedBufferSubData_State`

## 1.5 What "client-side conservative MarkGpuWritten" has to become

The whole of today's machinery runs **backend-side**: all six producers read backend-visible state
(`MGB_CTX->GetBufferBindingPoint(...)` for Espryt, descriptor resolution for Magma). In a split build the
backend is the server, so every one of those six is on the wrong side of the line for the decision the
**client** has to make synchronously at `SyncGpuWrites` (`BufferObject.cpp:369`) when the app calls
`glMapBuffer` or `glGetBufferSubData`. Asking the server "did you write this?" is a round trip the design
forbids in steady state.

So P5 owes a client-side pending set. Five concrete consequences:

1. **The set is built from state the client already owns.** Each of the six sites has a client-side twin:
   SSBO binding points, atomic-counter binding points, image-unit bindings whose `Access != GL_READ_ONLY`,
   XFB targets. `ARCHITECTURE.md:575` already names the component:
   `MG_Remote/Client/... GpuWritePending [P5+]`.
2. **Two producers are NEW at P5 and are not among the six.** `ARCHITECTURE.md:508` lists them under the
   zero-round-trip column: `glReadPixels` into a pack PBO becomes *fire-and-forget + client-side
   `MarkGpuWritten`* ("严格优于 monolith 的无条件停等"), and `glEndTransformFeedback` drops its unbounded
   fence wait and instead sets `MarkGpuWritten` on the capture targets. Both are client edits with no server
   participation.
3. **The `rangeCount == 1` assert at `ResourceTracker.h:587-592` has to be revisited on the same day.**
   It is the exact statement "a zero-range announcement is illegal"; the narrowing channel needs zero to
   become legal and to mean "nothing dirty". Two readings: (a) P5 keeps the assert because P5 only *adds*
   the client set and leaves the server announcement at whole-buffer, deferring the narrowing to P8/P9 as
   `ResourceTracker.h:579-581` says; (b) P5 has to relax it because a client that already marked the buffer
   conservatively will get a server announcement it must be allowed to answer with zero. **Reading (a) is
   what the docs say**, and the ROADMAP row does not mention narrowing — the brief should say so explicitly,
   because a package that reads `ResourceTracker.h:577-581` alone will assume it owns the narrowing too.
4. **`SyncGpuWrites` clearing the flag unconditionally (`BufferObject.cpp:372-374`) does not survive a
   transport unchanged.** Today there are two states (pending / not pending) and the clear is safe because
   the readback is synchronous inside the applier. Under split there is a third state — *the readback was
   emitted and the answer has not arrived* — which this function cannot express: if it clears and the reply
   is deferred, the shadow is silently stale for the object's life. `ARCHITECTURE.md:509` lists
   "GPU-write pending 的 buffer 首次 CPU 读" among the **unavoidable** blocking points
   ("monolith 本来就 `glFinish()`"), so the intended shape is *block until `OnBufferWriteback` lands*,
   matching P5's "阻塞 `read_pixels`" row. The reverse-channel drain points the client must service while
   blocking are enumerated at `ARCHITECTURE.md:472`: `glGetError`, `glGetQueryObject*`, `glClientWaitSync`,
   `glGetSynciv`, `eglSwapBuffers`, `glMapBuffer*`/`glGetBufferSubData`/`glCopyBufferSubData`, **and every
   iteration of every wait loop**.
5. `ARCHITECTURE.md:292-294` (section 8.2) is the ordering rule that makes this correctness and not
   performance: every `WritebackFromBackend` is followed by `BumpBufferMutationEpoch()`, and under split the
   epoch bump must be applied by the server *after* the writeback and *before* any later command that reads
   that handle — "反向通道需要与正向通道相同的有序保证". `PipeApply.cpp:1815-1846`
   (`MGPipeApplyResourceReadback`) already states its half: no serial moves, and the hook is called
   synchronously so the writeback and the bump have both happened before the caller reads.

---

# PART 2 — The persistent-map path, and what "block-granularity push" means

## 2.1 What P3a landed

**Three adoption entry points**, all of which call the same emitter under push:

| site | trigger | line |
|---|---|---|
| `BufferObject::TryAdoptLargeStorage()` | every `Respecify` / `AllocateImmutableStorage` of a store `>= 16 MiB` | `BufferObject.cpp:228-244`; threshold `kLargeBufferAdoptBytes = 16u*1024u*1024u` at `:229`; emitter at `:238` |
| `BufferObject::EnsureGpuResidentStorage()` | backend-initiated (Magma calls it on the SSBO / texel-buffer / XFB paths just before `MarkGpuWritten`) | `BufferObject.cpp:592-614`; emitter at `:603` |
| `BufferObject::AcquireMemoryRange()` | the app's own `glMapBufferRange(PERSISTENT\|WRITE)` without `FLUSH_EXPLICIT` | `BufferObject.cpp:645-664`; emitter at `:657` |

All three are guarded against a live mapping (`:233`, `:600-602`) — adoption releases the CPU shadow and a
live mapping may *be* that shadow, so freeing it under the application is a use-after-free
(`BufferObject.cpp:594-599`). `TryAdoptLargeStorage` is additionally gated by
`MG_Config::Features.DisableLargeBufferAdoption` (`BufferObject.cpp:232`, declared `Config.h:208`, env
`MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION` at `ConfigLoader.cpp:229`).

**`SyncPersistentMappedRange()`** — `BufferObject.cpp:341-353`. It is the per-draw push for the **emulated**
(non-adopted) persistent map only. Early-outs in order: not mapped `:342`; **GPU-resident `:346`** ("this is
the whole point of the persistent-map path — the per-draw whole-buffer re-upload that used to run here is
gone"); not `Persistent` `:347`; not `Write` `:348`; `FlushExplicit` `:349`; empty range `:350`. Otherwise it
calls `NotifySubData(mappedRange)` `:352`, which under push emits `resource_subdata` (`BufferObject.cpp:82-94`).

**`FlushMemoryRange` → `NotifyFlushMappedRange`** — `BufferObject.cpp:97-115`, emitting
`MGPipeEmitResourceFlushRange` at `:107` and carrying the application's **real, unnormalised**
`Flags<BufferMappingAccessBit>` (`:103-106`; emitter `MG_Impl/Pipe/PipeFill.cpp:736-748`; applier
`MG_Pipe/PipeApply.cpp:1787-1813`, with `:1808-1812` repeating that merging the flags would take the upload
shape decision away from the side that pays for it).

**Wire + ops**:

* `MG_Pipe/PipeCalls.def:84` — `X(MapPersistent, MGPHandleOnly, kScreen, kReplySlot|kOptional)`
* `PipeCalls.def:85` — `X(UnmapPersistent, MGPHandleOnly, kScreen, kOptional)`
* `PipeCalls.def:137` — `X(ResourceFlushRange, MGPFlushRange, kCtxObject, kNone)`
* `PipeApply.h:95-96` — `void* (*MapPersistent)(MGPipeHandle res, Uint64 size, const void* seedBytes);`
  and `void (*UnmapPersistent)(MGPipeHandle res);`
* applier `PipeApply.cpp:1893-1927` / `:1929-1953`; client emitter `PipeFill.cpp:772-786`.
* **`UnmapPersistent` has no producer and that is deliberate** — `PipeFill.cpp:764-771`: there is no backend
  unmap hook to convert, `PipeResource::ReleasePersistentMap()` (from `RedefineStorage`, `BufferObject.cpp:~155`)
  tells the backend nothing, and emitting one would be new behaviour under D-J. `MGPipeApplyUnmapPersistent`
  is the only one of the four buffer-only calls with a kind discriminator, and it Fatals on a mistyped kind
  (`PipeApply.cpp:1938-1946`).

## 2.2 The three Espryt persistent-mapped rings

`Managers.cpp:871-882` declares `struct PersistentRing` (store + `retired` + `frameMarks` + the immutable
knobs). The three instances:

* `g_uboRing` — `Managers.cpp:883`, "Global-UBO ring", alignment taken from
  `GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT` at store-creation time (`fixedAlignment == 0`, see `:877-879`).
* `g_unpackRing` — `Managers.cpp:884-890`, "Texture unpack ring".
* `g_uploadRing` — `Managers.cpp:899-906`, "Buffer upload ring", 4 MiB initial / 64 MiB max / 64-byte
  alignment (`:830-832`). Its rationale at `:890-898` is the WAR story in one paragraph: Mali's
  `glBufferSubData` resolves the hazard by **blocking in the call** (`osup_sync_object_wait`) until every
  referencing job retires, which under 26.3's per-frame UBO and chunk-mesh SubData streams serialized whole
  frames (~1 fps while chunks stream); staging into this ring and issuing `glCopyBufferSubData` keeps the
  hazard on the GPU timeline where it is just job ordering.

Allocation discipline: monotone `head`/`tail` linear cursors (`Managers.cpp:838-844`), `RingAllocate`
`:3320`, slow path `:3343`, `RingOnPresent` `:3427`, and the three per-present drains at
`Managers.cpp:3482/3500/3502` called from `DirectGLES.cpp:12460-12462`.

## 2.3 The >=16 MiB adoption story, and why P5 breaks it

`BufferObject.cpp:206-227` is the primary source for the motivation and is worth quoting to the brief in
full: Minecraft 26.3 streams chunk meshes into 128 MB vertex arenas with plain `glNamedBufferSubData` — the
one write API with no synchronization hint — and on Mali every route that hands the driver a write into a
busy **mutable** store either parks the calling thread (`glBufferSubData`, and `glMapBufferRange` **even with
`GL_MAP_UNSYNCHRONIZED_BIT`**) or ghost-copies the whole destination on a driver worker (~167 ms per touched
arena, the recurring in-world hiccup). Backing the arenas with immutable stores also kills the ghost but
eagerly commits every arena's full extent (+hundreds of MB), which LMK'd the device.

Measured: `MEASUREMENTS.md:87` — "≥16 MiB 可变 store 定义时采纳为 coherent persistent map 后 **p99 163→21 ms、
稳态 40→115 fps、省 ~400 MB**". Restated at `ARCHITECTURE.md:481` with "**整个 monolith 改造期一动不动**
（D-B4），只有 IPC 那一步会打破它", and `MEASUREMENTS.md:471` calls the number "没动过的东西是否真的没动"
的最直接读数. The P3a and P4a A/Bs both re-read it and both stayed in the 21-26 ms band:
`MEASUREMENTS.md:465` (3) (25.457 → 26.322, +3.4%) and `MEASUREMENTS.md:598` (25.297 → 26.841, +6.1%).

**Why a split build cannot keep it.** `MGPipeApplyMapPersistent` returns a raw `void*` that the client stores
as the store's base (`m_resource.AdoptPersistentMap(base)`, `BufferObject.cpp:238` / `:605` / `:658`). Across a
process boundary that pointer is meaningless. `ARCHITECTURE.md:483-490` is the tier table; **P5 runs at tier
T2** (`ARCHITECTURE.md:490`: "`AcquirePersistentMap` 返回 `nullptr`，前端已在三处容忍" — those three places
are exactly `BufferObject.cpp:238`, `:603-606`, `:657-660`) **and at T2 the client-side push is mandatory**
(`ARCHITECTURE.md:496`: "client 侧 persistent map 推送三件套（T2 档强制，P5）"). T0/T1 are P11's
(`ROADMAP.md:28`, `MEASUREMENTS.md:23-34` for the spike-B evidence).

**One genuine ambiguity for the brief.** Under `inproc` the server *is* the same process, so `MapPersistent`
could legitimately return a usable pointer and adoption would still work. Two readings:

* (a) P5's inproc keeps adoption alive and only exercises the push through a forced-T2 negative control;
* (b) P5 forces T2 even under inproc, so inproc and spawn are the same code path.

**(b) is the reading the gates force**, and this is the single most decision-shaped thing in this report: the
P5 exit gate says "`persistent-map-push` 出数" (`ROADMAP.md:21`), and that counter is **structurally zero if
adoption survives** — `PipeStats.cpp:68-70` says so ("today a persistent map is a permanent address space
donation ... so there is no push to count"). `InProcessTransport.h:11-16` argues the same way from the other
end ("It is not a test double ... `inproc` is a delivery mode of its own"). The knob that would express (a)
as a negative control, `MOBILEGL_IPC_ADOPT_TIER` (`ARCHITECTURE.md:494`, `:615`), **does not exist anywhere
in the tree** — a full-tree grep finds it only in those two doc lines. Evidence that would settle it beyond
doubt: whether the brief wants `LargeArenaAdoptionScenario`
(`MG_IntegrationTest/Scenarios/LargeArenaAdoptionScenario.cpp:450`) to stay green under inproc — it asserts
an adoption happens, and it cannot be green at T2 and produce `persistent-map-push` bytes at the same time.

## 2.4 What has to cross, at what granularity

`ARCHITECTURE.md:496-500` is the spec, three items. Restated concretely with the tree's own line numbers.

### (1) A `hasLiveHostWrites` bit — zero new record kinds

`ARCHITECTURE.md:498`: the server only needs to know "does this resource have a live host writer right now",
which is what `IsBufferDrawClean` is trying to say, so the bit rides on the `ResourceRespecify` /
`ResourceSubData` payload.

The **applier-side field already exists**: `MGPipeResourceRecord::HasLiveHostWrites`, `PipeApply.h:225`, with
the comment "ALWAYS FALSE IN P3a, AND WRITTEN BY NOBODY ... a verify build pins that it is false, so that
phase cannot land a silent semantic change under it."

The **wire field does not**. `MGPResourceDesc` (`MG_Pipe/MGPipeTypes.h:283-317`, `MGP_ASSERT_POD(..., 88)` at
`:318`) carries `HasDefinedContent` at `:301` and `ImageBindableHint` at `:302` but no live-host-writes bit;
`MGPSubData` (`MGPipeTypes.h:997-1010`, `MGP_ASSERT_POD(..., 72)` at `:1011`) likewise. **Both structs have
free padding** — `MGPResourceDesc`'s `Uint16 Pad0` at `:303` and `Uint32 Pad1` at `:313`; `MGPSubData`'s
`Uint8 Pad0[3]` at `:1004` and `Uint32 Pad1` at `:1008` — so P5 can take a byte out of a pad and the size
asserts do not move. **But**: `MGPipeTypes.h:304-310` defines the storage-defining-vs-metadata rule, and a
new bit is a **metadata** field (a respecify that moves only it must not force a reallocation ack or clear
`PendingUploads`); it has to be added to the field list stated beside `MGPipeResourceRespecifyNeedsAck`
(P4a ID-18 M4), or the first push of the bit costs a spurious reallocation.

**The pins P5 must lift, all four of them:**

* `PinNoLiveHostWrites` — `PipeApply.cpp:839-850` (verify body) / `:852` (no-op body), called at
  `:883` (`ApplyBufferWrite`), `:1630` (`resource_respecify`), `:1804` (`resource_flush_range`),
  `:1832` (`resource_readback`) and `:1908` (`map_persistent`). `PipeApply.cpp:1904-1907` names
  `map_persistent` as **"THE ONE CALL A LATER PHASE ATTACHES THE PRODUCER TO"**.
* `Managers.cpp:2669-2674` — `MOBILEGL_ASSERT(!record->HasLiveHostWrites, "...P3a has no producer for it")`
  inside `IsBufferDrawCleanByHandle`, plus the live `if (record->HasLiveHostWrites) return false;` at `:2674`.
* `Managers.cpp:2675-2681` — the **last frontend read in that function**, `frontend->IsMapped()`. Its comment
  at `:2643-2657` is written directly to P5: "The frontend read retires the moment P5 gives
  `HasLiveHostWrites` a producer, and it is the last frontend read in this function."
* The regression that comment records is the one P5 will re-commit if it gets this wrong: the first cut of
  the handle arm answered `HasLiveHostWrites` alone, an emulated persistent map read **draw-clean forever**,
  `SyncPersistentMappedRange` was never reached again, and the frame drew the last uploaded bytes with no
  diagnostic (`Managers.cpp:2650-2655`; unit gate `MG_Test/SanityTest.cpp:4583`, narrative `:4566-4582`).

### (2) The dirty bytes, in blocks

`ARCHITECTURE.md:499`, restated concretely:

* **The set**: the tracker keeps `m_livePersistentMaps` = buffers that are `Persistent` + `Write` +
  **not** `FlushExplicit` + **not** `GpuResident`. That predicate is literally
  `SyncPersistentMappedRange`'s early-out chain (`BufferObject.cpp:346-349`) read as a membership test.
  `m_livePersistentMaps` **does not exist in the tree** — the only hit is that doc line.
* **The trigger**: at every validate point, for every member reachable by *this* operation — VAO / index /
  indirect / UBO / SSBO / atomic / XFB target, i.e. "the union of the backend's 20
  `SyncPersistentMappedRange` sites".
* **The block size**: `MOBILEGL_IPC_PERSISTENT_BLOCK_KB`, **default 64** (`ARCHITECTURE.md:499` and the env
  inventory at `:615`), i.e. **64 KiB blocks**. The env var does not exist in the tree either
  (`ConfigLoader.cpp` has no such parse; the only `MOBILEGL_IPC_*` mention anywhere is in the docs).
* **Keyed on**: the buffer's `MGPipeHandle` + the block index within the mapped span (offset =
  blockIndex × blockBytes). The natural delivery vehicle is the existing `resource_subdata` record — no new
  record kind — which the client already splits by a destination-box cap through
  `MGPipeForEachSubDataRecordRange` (`PipeFill.cpp:694-713`; a record's offset caps at 2^31-1, `:707-712`).
* **Phase 1** (P5): conservative — treat the whole mapped span as dirty and split it into blocks.
  **Phase 2**: precise — shadow-in-shm 64 KiB dirty bits with `memcmp` first
  (`MOBILEGL_IPC_SHADOW_SHM`, default 1 from Phase 2, `ARCHITECTURE.md:615`).
  `ARCHITECTURE.md:499` grants Phase 2 the right to be pulled forward if Phase 1 is unacceptable on the
  Create/Flywheel fixtures, and calls it "计划里唯一允许因测量改变阶段顺序的地方".

**The 20 sites, enumerated** (I count **21 call lines** in non-test code, not 20 — caveat below):

Espryt (9): `DirectGLES.cpp:361` (`ResolveIndirectCommandBytes`), `:6185` (primitive-restart source),
`:6439`, `:6440`, `:6541`, `:6542` (indirect-count draw + parameter buffer, two pairs);
`Managers.cpp:2817`, `:2994`; `MultiDraw.cpp:511`.

Magma (12): `DirectVulkan.cpp:281`, `:472`, `:796`; `UniformManager.cpp:2024`;
`VulkanRenderer.cpp:3542`, `:3621`, `:4013`, `:7428`, `:12423`, `:12424`; `VkBufferManager.cpp:628`, `:679`.

Caveat, flagged rather than resolved: `MEASUREMENTS.md:111` records 20/6 as "与计划一致", and
`Managers.cpp:5047-5048` speaks of "the eleven Espryt `SyncPersistentMappedRange` / `SyncGpuWrites` sites"
where my Espryt count is 9 + 3 = 12. Both published counts are one below mine, so one specific line is
excluded by a convention I could not find — most likely `DirectGLES.cpp:361`, which is a shared helper
rather than a draw-path site (**guess**). The evidence that would settle it is the per-site attribution table
§5.7 that `ARCHITECTURE.md:290` refers to ("20 处 `SyncPersistentMappedRange` + 6 处 `SyncGpuWrites` 按 §5.7
逐站点归属") — I did not find such a table in the tree, and the brief writer should not assume it exists.

### (3) The ordering property the push must preserve

In monolith, `SyncPersistentMappedRange` → `NotifySubData` → `MGPipeEmitResourceSubData` is a memcpy on the
same thread as the draw that immediately follows, so the bytes the app wrote before the draw call are the
bytes the draw sees. Under split, the block push and the draw both travel on `SEG_CMD`, in order, so the ring
preserves it — **provided the push is emitted at the validate point and not lazily**. A push that is deferred
past the draw record is exactly the C-1 regression above, re-committed at the transport layer.

`MOBILEGL_COHERENT_AS_FLUSH` keeps working in split mode unchanged (`ARCHITECTURE.md:502`): the two
`coherent_as_flush: true` Create fixtures take the same buffer path in split and monolith, which is the only
reason a by-name comparison between them means anything.

---

# PART 3 — The `persistent-map-push` counter

**It already exists and does not need to be added. It needs to be WIRED.**

* Enum member: `MG_Util/Metrics/PipeStats.h:72` — `PersistentMapPush`, comment at `:71`
  ("Bytes pushed because a persistently mapped range was published to the backend").
* Long name: `PipeStats.cpp:171` — `"persistent-map-push"`, in `kByteClassNames`.
* **Short name: `pmap`** — `PipeStats.cpp:~203`, `kByteClassShort` index 7 (the array reads
  `"buf","tex","ubog","ubon","vtxc","idxc","icmd","pmap","resid"`). That is the token that appears inside the
  summary line's `bytes/f[...]` bracket.
* Test pin: `MG_Test/Util/PipeStatsTest.cpp:289` —
  `EXPECT_STREQ(PS::NameOf(PS::ByteClass::PersistentMapPush), "persistent-map-push")`.
* **Not wired, and the non-wiring is itself a contract**: `PipeStats.cpp:68-70` —
  "`persistent-map-push`  Not wired in P0: today a persistent map is a permanent address space donation
  (D4/D-B4) that survives the whole monolith track, so there is no push to count until the IPC track breaks
  it." Repeated at `ARCHITECTURE.md:619`, which adds that the site inventory at `PipeStats.cpp:16-100`
  "那份清单是契约" — **so P5 must edit `PipeStats.cpp:68-70` in the same commit that adds the `AddBytes`
  call, or the inventory is lying.** Related: `PipeStats.cpp:24-27` already says `stage-buffer` explicitly
  does **not** cover "bytes an app writes THROUGH a persistent map ... see `persistent-map-push`", so the two
  classes must not double-count.

**Good news for G1**: `PersistentMapPush` sits at `PipeStats.h:72`, i.e. **before** the
`#if MOBILEGL_PIPE_PUSH` block that opens at `:75` (guarding `CsoBlobBytes` at `:84`). It is already in both
the pull and the push build, so wiring it resizes no array and moves no pull symbol.

## `mpr` today, and how it differs

`mpr` is a **different axis entirely** — a CallClass, not a ByteClass:

* `CallClass::MapPersistentRoundtrips` — **`PipeStats.h:141`**, inside the `#if MOBILEGL_PIPE_PUSH` block
  that opens at `:117`; the rationale comment runs `PipeStats.h:130-140`.
  **Doc correction for the brief**: `ARCHITECTURE.md:492` cites `PipeStats.h:126` for this — that line
  number is stale at `a29807cc`; it is `:141`.
* Long name `"map-persistent-roundtrips"` (`PipeStats.cpp:~178`), printed as `mpr=` at `PipeStats.cpp:443`.
* **Counted at the client emitter**, `PipeFill.cpp:781-784`, behind `PipeStats::Enabled()`; the applier
  bumps its own copy first and unconditionally at `PipeApply.cpp:1900`.
* **Definition: every `MapPersistent` EMISSION — mint OR decline.** The reasoning is spelled out three times
  (`PipeApply.cpp:1893-1901`, `PipeFill.cpp:776-780`, `ARCHITECTURE.md:492`): counted as "round trips
  actually taken" it is 0 by construction in monolith and could never go red for the reason it exists;
  counted as attempts it is the same number in both modes, equals exactly "one per storage definition", and a
  regression that acquires per-draw shows up on the first window.
* Gates: `StorageBufferRegrowScenario.cpp:255`
  (`NStorageDefinitionsCostNMapPersistentRoundtripsNotOnePerDraw`) and `LargeArenaAdoptionScenario.cpp:450`
  (`AnAdoptionCostsExactlyOneMapPersistentRoundtrip`). Both skip outside their own ctest lane
  (`StorageBufferRegrowScenario.cpp:172`, `LargeArenaAdoptionScenario.cpp:226`).
* Device readings on file (`MEASUREMENTS.md:465` (4)): 26.3 = 8 on both devices (first window 4, then 2+2 —
  all ≥16 MiB store definitions); sodium 1; rd12 and create-instancing 0; the P2 arm always 0.

**The difference P5 must publish, in one sentence**: `mpr` counts *acquisition attempts* (calls, one per
storage definition, unchanged by the split), while `persistent-map-push` counts *bytes the client had to ship
because the acquisition was declined*. They are **anti-correlated**: at tier T2 every `MapPersistent`
declines, `mpr` stays exactly where it is today, and `pmap` goes from 0 to whatever the Phase-1 conservative
push costs. A P5 run where `pmap` is still 0 has not reached T2 — see §2.3's ambiguity.

---

# PART 4 — `PersistentCoherentMapScenario`

**IT DOES NOT EXIST.** A full-tree grep for `PersistentCoherentMap` returns exactly two hits, both docs:
`docs/Disaggregated/ARCHITECTURE.md:500` and `docs/Disaggregated/ROADMAP.md:21`. There is no such file in
`MG_IntegrationTest/Scenarios/` (85 scenario files; none matches) and no such name in
`MG_IntegrationTest/CMakeLists.txt`. **P5 must write it.** The gate cannot "pass unchanged under inproc"
because there is nothing to pass.

**The spec, all of it, is `ARCHITECTURE.md:500`**: map `PERSISTENT|WRITE|COHERENT`, write through the
pointer, **make no other GL call**, draw, read back and check. "门从第一天就有" — it is meant to exist before
the push does.

**Shapes to copy from:**

* `MG_Test/SanityTest.cpp:4583` —
  `TEST(DirectGLESBufferDrawProbe, ALiveHostMapKeepsTheHandleArmProbeDirtyBetweenTwoDraws)`, the unit-level
  version of exactly this hazard, with the narrative at `:4566-4582` and an `EsprytSlotTablesEnabled()` skip
  at `:4590`. It builds the applier record by hand "because this case is about the PROBE".
* `Scenarios/LargeArenaAdoptionScenario.cpp` (adoption + `mpr`), `Scenarios/StorageBufferRegrowScenario.cpp`
  (per-definition roundtrips), `Scenarios/ResidentIndexScenario.cpp:40` (names `SyncPersistentMappedRange`
  and the acquire/memo interaction), `Scenarios/CrossFrameBufferScenario.cpp` plus `StreamedArenaScenario`
  (the recycle cases `MEASUREMENTS.md:523` names as the hedge for the two-arm flush ladder).

**A trap to write into the scenario on day one.** A scenario-sized buffer is far under the 16 MiB
`kLargeBufferAdoptBytes` (`BufferObject.cpp:229`), so `TryAdoptLargeStorage` never fires — but
`AcquireMemoryRange`'s own adoption **does** fire for `PERSISTENT|WRITE` without `FLUSH_EXPLICIT`
(`BufferObject.cpp:655-661`), subject only to whether the backend mints. So the same test lands in the
**adopted** arm or the **emulated** arm depending on the driver (`glBufferStorageEXT` presence) and on the
build, and those two arms exercise completely different code (`SyncPersistentMappedRange` returns at `:346`
in one and pushes at `:352` in the other). `MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION` does **not** separate
them — it is read only at `BufferObject.cpp:232`, inside `TryAdoptLargeStorage`. The scenario must assert
which arm it is in, or it will silently test the wrong one on half the devices.

**What it would newly catch under inproc**: the client pushes blocks at validate points and the server
applies them on `mgl-srv-apply`; a push that loses the race with its draw record turns the scenario red.
That is exactly why the ROADMAP puts this gate in P5 and not in P11.

---

# PART 5 — WAR / flush hazards that get harder when write and use are on different threads

All three named hazards are instances of the same shape: **a shadow of driver/GPU state, kept on the thread
that will use it.** Split moves the *use* to `mgl-srv-apply` and leaves the *write* on the app thread. Two of
the three get harder; the third is a role-isolation item, not a threading one.

## 5.1 The Mali SubData WAR stall and the three-tier flush ladder — **gets harder**

The ladder is documented at `Managers.cpp:1047-1076` and implemented twice:
`FlushPendingRangesFrom(twin, hostBase, size)` at `Managers.cpp:1051` (the handle arm) and
`FlushPendingRangesNow` at `Managers.cpp:1316` (the pull arm, byte-frozen against `5cb826b0` by G1 —
`MEASUREMENTS.md:523` records why there are two, and ID-15 supersedes ID-13 on it; the in-tree note is at
`Managers.cpp:1162+`).

Tiers, with their live conditions:

1. `glMapBufferRange(WRITE | INVALIDATE_BUFFER-or-RANGE)` + memcpy, taken when `wholeBuffer` **or**
   `size >= kInvalidateRangeMinBytes` = **128 KiB** (`Managers.cpp:1045`, applied `:1131-1143`).
2. staging ring + `glCopyBufferSubData` (`:1144-1152`) — `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH` forces
   this tier as the map path's negative control (`:1069-1070`).
3. direct `glBufferSubData` (`:1153`), potentially stalling.

Stakes on file: 9 × 128 MB of ghosting per frame ≈ 380 ms, the measured 2-4 fps (`Managers.cpp:1064-1070`).

**Why split makes it harder, three distinct edges:**

* **The bytes are the client's.** Both arms read `hostBase`, which on the handle arm is the lambda
  `liveHostBase` re-evaluated **at every use** (`Managers.cpp:2744-2748`), precisely because two ordinary
  events move or free what an earlier base pointed at — a shadow resize (`PipeResource::ResizeShadow` is
  reserve+resize, so a grow past the reserve reallocates) and persistent-map adoption (the shadow is cleared
  and shrunk) — `Managers.cpp:2739-2743`. **Under split the server cannot hold a pointer into the client's
  shadow at all**, so "re-read at every use" necessarily becomes "snapshot into `SEG_STAGE` at emission", and
  the window between snapshot and apply is new.
* **Tier 1's `INVALIDATE_RANGE` is a claim that the old bytes are dead**, and it is only true if the bytes
  being copied are the newest. `Managers.cpp:1126-1129` records that this edge has already drawn blood once:
  widening the map to page bounds "looked free and was not — the widened bytes clobbered GPU-written data
  (an SSBO counter beside the app's SubData) with the stale shadow". A late or widened snapshot taken on a
  different thread does the same thing, and unlike tier 3 it fails **silently** rather than by stalling.
* **The flush granularity is deliberately *not* the union.** `Managers.cpp:1072-1076`: ranges are flushed as
  queued because "bytes, not flush calls, are the cost axis here, and collapsing a scattered flush into its
  union re-copied nearly whole chunk-mesh arenas every frame". The 64 KiB block push of §2.4 is a *different*
  granularity policy landing on the same buffers, and the brief should say which one wins when both are live
  on one arena.

## 5.2 The `g_uploadRing` reset asymmetry — **real, but probably not a live bug**

`OnBackendContextDestroyed` (`Managers.cpp:2583-2596`) calls `ResetRingForNewContext` for **`g_uboRing`
(`:2594`)** and **`g_unpackRing` (`:2595`)** and **not for `g_uploadRing`** (declared `Managers.cpp:899-906`).

Two readings:

* **(i) a latent bug** — the upload ring keeps a dead context's `id` and `mappedPtr`.
* **(ii) harmless, and the eager pair is merely redundant** — `RingAvailable` (`Managers.cpp:3288-3299`)
  lazily resets any ring whose `store.contextGeneration != g_bufferContextGeneration` (`:3293-3295`).

**(ii) is right, and I checked it rather than guessing**: every `RingAllocate(g_uploadRing, ...)` is guarded
by a `ringUsable` computed from `UploadRingUsableNow()` → `RingAvailable(g_uploadRing)` in the same function
— `:1103` guards `:1144`, `:1211` guards `:1221`, `:1379` guards `:1420`, `:1454` guards `:1464`;
`UploadRingUsableNow` itself at `:1035` and `:1322` (one per arm). `ResetRingForNewContext`
(`Managers.cpp:912-925`) also deliberately **keeps** `store.generation` across the wipe, because frame
serials survive context recreation and a restarted counter could revalidate a stale per-program slot cache.

**Why it still matters for P5.** `g_bufferContextGeneration` is **server state**, and the re-open mechanism
(`BumpBufferMutationEpoch()` at `Managers.cpp:2573` and `:2587`) is **server-local**. A client that memoises
"I already pushed blocks 3..7 of this arena" has no way to learn that the server's ES context died and its
rings were dropped. Whatever epoch the client keys its push memo on must be re-opened by a **server event on
the reverse channel**, not by a client-local counter. There is no callback for this in `MGPipeCallbacks`
today — the ten are fixed and `static_assert`-pinned at `MGPipeCallbacks.h:56-58` — so P5 either folds it
into `OnCapsInvalidated`/`OnSurfaceChanged` or pays the eleventh-callback price that assert exists to make
visible.

## 5.3 `ScopedDefaultUnpackState::s_synced` — **a role-isolation item, not a threading one**

`Managers.cpp:5430-5491`. The flag is a **process-global `static inline Bool`** at `:5490`, alongside five
process-global value shadows (`:5491-5496`). `EnsureShadowSynced` (`:5452-5470`) pins the driver to the GL
defaults (4/0/0/0/0/0) exactly **once per process**, and thereafter `Apply` (`:5472-5480`) is pure
compare-and-set. The class comment (`:5424-5429`) states the invariant it rests on: "The backend unpack state
is set ONLY by MobileGL's own save/restore helpers ... all of which restore to the resting default, so the
shadow stays accurate."

Two consequences:

* **The role-isolation audit undercounts.** `ARCHITECTURE.md:578` says MGPipe brings the process globals a
  role split must duplicate down from four (`pGLContext`, `gBackendFunctionsTable`, `pActiveBackendObject`,
  `pDefaultFramebufferInfo`) to **two**, and that this is "`inproc` 从'成本可疑的实验'变成'可交付形态'的直接
  原因". `s_synced` plus its five value shadows is a **sixth** process global of exactly that kind, unnamed
  in that audit. In P5's reduced path the client role should never touch GL, so this is latent rather than
  live (**guess** — the P5 purity gates do not exist yet, so I could not check); it belongs on the brief's
  list regardless, because it is the shape those gates are meant to catch.
* **It is never reset on context death.** `OnBackendContextDestroyed` (`Managers.cpp:2583-2596`) resets the
  rings and the binding caches and not this. Benign today, because a lost context returns the driver to the
  GL defaults the shadow already claims; it stops being benign the day a server re-attaches to a context
  whose unpack state something else moved.

## 5.4 One more that the brief did not name, and that belongs on the list

`IsBufferDrawClean` / `IsBufferDrawCleanByHandle` do **unlocked emptiness probes** of two containers the
apply side mutates: `resource->pendingResidentWrites` (`Managers.cpp:2668`, `:2707`) and
`resource->pendingRanges` (`:2676`, `:2712-2713`), both flagged in-comment as "the same unlocked emptiness
probe `EnsureBufferResource`'s replay branch uses" (`:2711-2712`). Today the probe and the mutation are both
on the context thread. With `mgl-srv-io` and `mgl-srv-apply` as two threads (`ROADMAP.md:21`) that is no
longer automatic, and `syncedChangeSerial` is already an `atomic` read with acquire ordering (`:2684`) while
these two are not.

---

# Appendix — one-line answers

1. `MarkGpuWritten` = `BufferObject.cpp:364`; `SyncGpuWrites` = `:369`. Six backend producers
   (`DirectGLES.cpp:570` / `:618` / `:2603`, `UniformManager.cpp:1075` / `:1231`, `VulkanRenderer.cpp:11618`),
   funnelled under push through `Managers.cpp:2509`, answered client-side at `ResourceTracker.h:583`.
   P5 must build the set **client-side** at its own draw/dispatch emission points, plus two new producers
   (`glReadPixels`→PBO and `glEndTransformFeedback`, `ARCHITECTURE.md:508`), and must give `SyncGpuWrites`
   either a third state or a block.
2. `MapPersistent` = `PipeCalls.def:84` / `PipeApply.cpp:1893`; `UnmapPersistent` = `:85` / `:1929`
   (**no producer**, `PipeFill.cpp:764-771`); `ResourceFlushRange` = `:137` / `PipeApply.cpp:1787`.
   Three rings `g_uboRing` / `g_unpackRing` / `g_uploadRing` at `Managers.cpp:883` / `:884` / `:899`.
   Adoption at 16 MiB (`BufferObject.cpp:229`), p99 163→21 ms (`MEASUREMENTS.md:87`). Split runs at T2
   (`ARCHITECTURE.md:490`), push mandatory, **64 KiB blocks** keyed on `{MGPipeHandle, block index}`,
   shipped as existing `resource_subdata` records, plus a `hasLiveHostWrites` metadata bit that exists in
   `PipeApply.h:225` but **not** on the wire (`MGPipeTypes.h:283` / `:997` — use the pads at `:303` / `:1004`).
3. The counter **exists and is unwired**: `ByteClass::PersistentMapPush`, `PipeStats.h:72`, short name
   `pmap`, inventory disclaimer at `PipeStats.cpp:68-70` that P5 must edit. `mpr` is a *call* class
   (`PipeStats.h:141`, **not** `:126` as `ARCHITECTURE.md:492` says) counting acquisition attempts; it is
   unchanged by P5 and anti-correlated with `pmap`.
4. `PersistentCoherentMapScenario` **does not exist** — docs only. Spec at `ARCHITECTURE.md:500`; nearest
   relative `SanityTest.cpp:4583`; the trap is that a small buffer still adopts via `AcquireMemoryRange`
   (`BufferObject.cpp:655`), so the scenario must pin which arm it is testing.
5. Hardest across threads: the flush ladder's `hostBase` snapshot (`Managers.cpp:1047-1076`, `:2739-2748`,
   `:1126-1129`) and the client's push memo versus the server-local `g_bufferContextGeneration` /
   `BumpBufferMutationEpoch` (`Managers.cpp:2573` / `:2587` / `:3293`). `s_synced` (`Managers.cpp:5490`) is a
   role-isolation item. Bonus: the unlocked `pendingRanges` / `pendingResidentWrites` probes
   (`Managers.cpp:2668` / `:2676` / `:2707` / `:2712`).
