# Frontend: state tracker, texture upload, shaders (formerly ARCHITECTURE §5–§7)

> Design details. The design highlights and an index of all sections are in [`../ARCHITECTURE.md`](../ARCHITECTURE.md). Section numbers follow the original `ARCHITECTURE.md`; `ARCHITECTURE.md §N` in code comments resolves by number here.

## 5. Frontend state tracker (`MG_Impl/Pipe/Tracker`)

### 5.1 Pushing happens at validate time before a verb, not in GL setters

Per-setter pushing would turn Blaze3D's redundant `glEnable/glDisable` in every batch into interface calls, strictly slower than today; the correct form is gallium's `st_validate_state`. It lands as the single `MGPipeValidateForVerb(MGPipeVerb)` (nine validate classes, `FillPoints.def`), exactly before every call through the tables and after every early return. Only the resource ops that are already dispatched at GL call time today (the seven hooks of `BufferBackendOps`) push at call time.

### 5.2 Dirty bits: zero new bookkeeping for value classes, five aggregate generations for object classes

- Value-class groups (render state, pixel pack, patch, attrib defaults, shader bindings …) reuse the existing version counters directly as the shutter.
- Object-class groups (vertex buffers, framebuffer, sampler views / samplers / images, const / shader buffers, SO targets) reduce the shutter to a single `Uint64` comparison through five **aggregate generations** — without aggregation there is no way to answer "did any bound texture move".
- Completeness is guaranteed by `scripts/gen_pipe_dirty_surface.py` (every mutator → the generation that must be bumped, `--check` + `--self-test` in CI). **A shutter that cannot see its own subject** is a trap we have hit three times: a new field must come with a "record field → setter → shutter" line, and mixing it into an existing generation is preferred (a new counter would bloat the pull object, which G1 does not allow).

#### 5.2.1 Answers of the dirty-surface mapping (`DirtySurface.def`)

`DirtySurface.def` states, for every frontend mutator observable by the backend, "who publishes this change". A missing publication does not error, it only silently renders stale state, so the list is mechanically enumerated by `scripts/gen_pipe_dirty_surface.py`: it scans `MG_Impl/GLImpl` and `MG_State/GLState` for `pGLContext->` calls matching `MUTATOR_PREFIXES` and for `MGP_NOTE_MUTATION` sites (the latter carry a field name, and the line is written on the enclosing function); `--check` cross-checks the line set in both directions.

The answers list only publishers that fire on **every path that makes a modification** (an early return with an unchanged value does not count as a modification); a bit that fires only on some paths, if listed separately, would make the table-constructed shutter under-trigger, and under-triggering is the dangerous direction.
- `NEW_*`: a dirty bit in `Tracker.h`, published at the next verb; `kImmediate`: straight to the backend from the same function body; `kReverseChannel`: the backend writes back to the frontend; `kNoBackendRead`: there is no backend read site.
- `kExplicitDestroy`: when the last reference is released, the client's `MGPipeEmit*DestroyAndFree` in turn does the wire delete (when the class has a delete line), the backend notification, and frees the slot; `kUnpublishedDestroy`: program pipelines and shaders have no wire handle, so there is nothing to delete.
- `kPulledEveryVerb`: no bit movement; the field is in the may-read table of its verb class, and the residual fill copies it at every verb; `kPulledPartialShutter|NEW_*`: likewise backed by the pull, with the attached bit firing only on some modification paths.

Bit answers are derived by the script: the RenderState family is compared both ways with `RenderState.cpp` (`BumpVersions()` moves two counters, a bare `++m_version` moves only `NEW_RENDER_STATE`); for the remaining bits, the members / fields the shutter reads are intersected with those the mutator transitively writes, which can only detect the one wrong answer "this bit's shutter never moves on any path", and cannot prove that every path moves. Functions whose write root cannot be located are tainted, and answers that depend on them are UNDECIDED and must be listed in `MGP_DIRTY_SURFACE_UNDECIDED_LIST`; listing one that is already decidable also turns red. Prose answers are not derived (except the bits attached to `kPulledPartialShutter`); the scanner matches function bodies by text, mutators inside lambdas are attributed to the enclosing function, and a change published through a helper is read as a deferred publication; the semantic proof is still `MOBILEGL_PIPE_VERIFY`.

#### 5.2.2 Record field → setter → shutter table

Every field of a record triggered by a dirty bit in `Tracker.h` may only change through a setter that moves that bit's shutter input; otherwise it must be emitted unconditionally at the setter (the `resource_*` family, `set_texture_params`). `DirtySurface.def` only maps mutators to bits and cannot see that a derived field depends on a mutator of another family, so the table below is maintained by hand; when a record gains a field, add a row here.

| Bit | Record and fields | Setters that change the fields | Shutter inputs |
|---|---|---|---|
| 6 | `create/bind_shader_state`, `set_draw/dispatch_program`: Cso, StageMask, GlobalUboSize, artifact blob ref, the two bound handles | `glUseProgram`, `glLinkProgram`, `glBindProgramPipeline`, `glUseProgramStages` | lifetime id × link version; under a pipeline, `stageLinks` (pipeline name × each stage's lifetime id and link version) |
| 7 | program bindings: image units, block bindings, uniform write set | `glUniform1i` on opaque uniforms, `glUniformBlockBinding`, `glShaderStorageBlockBinding`, any `glUniform*` | the four per-program counters; under a pipeline, `stageLinks` is mixed in as well |
| 8 | `set_global_constants`: ShaderCso, Version, default uniform block mirror | `glUniform*` on the default block, `glUseProgram` | lifetime id × UBO content version; under a pipeline, `stageState` |
| 11 | `set_framebuffer_state`: Fbo, Color[8]/Depth/Stencil/ReadSurface (Res, Kind, InternalFormat, TextureTarget, Layered, Level, Layer, UploadTarget), DrawBuffers[8], Width/Height/Layers/Samples/FixedSampleLocations, IsDefault, Complete, Target | `glFramebufferTexture*`, `glFramebufferRenderbuffer`, `glDrawBuffer(s)`, `glReadBuffer`, `glFramebufferParameteri`, `glBindFramebuffer`; storage redefinition of an attached texture or renderbuffer (format, texture target, size, sample count, completeness are inlined at emission, so `PipePublishDescriptor` bumps the attachment aggregate) | attachment aggregate × draw / read binding slot version |
| 12 | `set_sampler_views`: per unit {View, Texture} | `glBindTexture`, `glActiveTexture`, texture or sampler parameters, uploads defining a level, appearance of the default texture; the program in use (`glUseProgram`, relink, `glUniform1i` of a sampler uniform) | content × params × bind generation × `opaqueUnits` |
| 13 | `bind_sampler_states`: per unit, the sampler CSO handle | `glBindSampler`, `glDeleteSamplers`, `glSamplerParameter*`, `glTexParameter*` | params × sampling resolution × bind generation |
| 14 | `set_shader_images`: per unit within the program's image-unit window, {Res, InternalFormat, Layer, Level, Layered, Access} | `glBindImageTexture`, texture content / params, `glUniform1i` of an image uniform; the program in use (`glUseProgram`, relink) | content × params × bind generation × `programImages` |

Bits 11–14 also fire when the share group's twin aggregate clock changes (the setter is invoked through a sibling context); when the maximum touched texture unit changes, 12 and 13 also fire.

### 5.3 Render state: one whole blob crosses the wire, identity takes only the pipeline subset (D-B1)

```
create_render_state(cso, pipelineSubsetChunks)   // pipeline subset only
bind_render_state(cso, version, pipelineVersion) // 12 B in steady state
set_dynamic_state(dynamicChunks, version)        // only the changed dynamic chunks
```

- `RenderStateParameters` (1168 B) crosses as one whole block: Espryt does a three-segment memcmp, and field order is load-bearing; splitting it into several CSOs would require hand-maintaining a ~150-field partition with no tripwire.
- Identity takes only the pipeline subset, otherwise `glViewport` / `glScissor` would mint a new CSO every time and flush the server's pipeline memo. The partition is written in one place only: the chunk table in `MGPipeRenderStateSpans` (7 pipeline chunks 396 B + 8 dynamic chunks 772 B, `static_assert` completeness), and G7 asserts "the subset hash changes ⟺ `m_pipelineStateVersion` changes".
- Espryt's `SyncRenderState` is untouched; Magma's pipeline memo key is `cso.slot`.

### 5.4 Validation invariants, merging and suppressors

The rule (D-B3): **all `set_*` / `bind_*` of a verb complete before that verb; the server lazily specializes shaders and pipelines at the verb from the pushed state.** Apart from "a resource is created before it is bound", there is no ordering requirement among `set_*`. Merging: whole blocks beat per-field; the high-water mark is directly the `count` argument; only the sets the program resolved are sent; the **set hash suppressor** debounces on the client side (previously ~175 lines in the backend).

### 5.5 Sampler views are resolved on the client

Which texture a shader sees depends on the sampler uniform type, mipmap completeness and the undefined default texture; resolution stays on the client (with a memo), and gallium's "one view per slot" is the resolved form. Two backend-specific post-processing steps stay on the server: Espryt's raw-depth-fetch sampler substitution and Magma's feedback-loop detection.

### 5.6 Lifetime, share groups, composite programs

- `resource_create` is sent when the frontend object is constructed, storage is lazily defined by `resource_respecify`, and `resource_destroy` is sent at destruction; the death is forwarded to every emitter (freeing only the slot previously caused a UAF). A view is destroyed before its storage owner, an FBO attachment pins its texture, and a buffer texture pins its buffer.
- The program pipeline composite (`CompositeResolver`) is fully composed on the frontend and pushes a single handle; it is released exactly once (the resolver swaps it out, or `~ProgramObject`, D-H7); the memo is keyed by `(ContextId, pipeline GL name)`.

#### 5.6.1 Handle, release and memo key of the program pipeline composite

- The composite is composed on the frontend by `GLContext::GetProgramForDraw()` and cached in its own `ProgramPipelineObject`; the program emitter reserves the high segment of `ShaderCso` by the composite's own lifetime id (§2.1) and gives it a handle, sending `create_shader_state` like an ordinary program, and the server does not know it is a composite. `MGPipeSlotAllocator::Allocate` rejects that segment and `AllocateComposite` is the only entry; the reserved segment is used instead of a handle flag because a flag would have to be carried and masked by every `ShaderCso` consumer on both sides, whereas the reserved segment makes "an ordinary program cannot get a composite slot" guaranteed by the allocator.
- A slot has two release paths: `MGPipeCompositeResolver::Observe` releases the old composite when the signature changes, and the composite's own `~ProgramObject`. Both go through `MGPipeEmitShaderCsoDestroyAndFree`, and the later one is a no-op (`MGPipeSlotAllocator::Free` rejects a slot not live in that generation and does not advance the generation either). Relying on the destructor alone is not enough: once the frontend holds an extra reference, the destructor will not come first when the signature changes.
- The signature is `ComputeDrawProgramSignature()` (each graphics stage's lifetime id and link version), excluding `GetBackendStateVersion()`: the latter also changes on `glUniform1i` of a sampler uniform, which would make the composite recompile on every draw. Bit 6's shutter in `Tracker.h` reads the same set of values.
- The memo key is `(GLContext::GetTextureContextId(), pipeline GL name)`: the resolver is a process singleton, while pipeline names are allocated per context; keying by name alone would hit the same-named entry of another context after make-current and release a composite that is still alive, rebuilding the program on every switch; the context address is also wrong, because a destroyed and recreated context can land at the old address, whereas this id is never reused.
- When a context is destroyed, its composite is released through `~ProgramObject`, and leftover entries can never be found again; `Reset()` at make-current discards them without releasing, so the table length is bounded only by the entries whose composite slots are still live.

### 5.7 Ownership of emulation

Rule: **transformations the driver cannot express are lowered in the tracker; transformations forced by the hardware / driver are lowered in the driver.**

| Falls to the client (pure CPU transformations that read frontend bytes) | Falls to the server |
|---|---|
| client vertex / index arrays (bytes cross the wire, never pointers), maximum index scan, `*IndirectCount` count resolution, CopyImage shadow mirror, XFB CPU primitive count, compressed texture / unpack normalization | primitive-restart rewriting, multi-draw tiering and flattening (read from the index host mirror, §10.3), viewport-array replay, fp64 vertex narrowing, image-bindable storage widening, mipmap generation (the client allocates level storage) |

**Stale index discipline is a per-site table**: the reconcile before a client-side scan must reproduce monolith's set verbatim (maximum index scan = `SyncPersistentMappedRange()` + `SyncGpuWrites()`; `*IndirectCount` only the former).

## 6. Texture subdata and dirty ownership

- Texture uploads are done by Espryt at sync time over the **accumulated** region (Mali prices by job count; ~100 rects against one union box measured +6 ms/frame), so sending a record per `glTexSubImage` would reproduce this cliff.
- So the client accumulates in its own rect model and sends **one** `ResourceSubData` at the next validate / flush point, carrying both the union box and the region list, and **the server picks the upload shape** (the decision stays on the side that pays the GPU cost). The stride is carried explicitly, no longer relying on pointer comparison.
- **Dirty ownership is reversed**: the client maintains the emission cursor and clears its own flags after emission; clearing dirty waits for the applier's acceptance (a rejected level stays in the dirty table); the server's own re-dirtying is the server's business.
- `TextureUploadShapeScenario` records the upload shape as golden (a gate since P3b/P4b D2; Mali frame-time deltas have no device here, recorded on Adreno).

### 6.1 Server-side pending upload set

- The client clears its dirty flag for a level as soon as the applier accepts that level's `ResourceSubData`; the backend's upload loop has early-return arms (Espryt: incomplete texture, multisample target only refreshes), so accepted shapes are accumulated by the applier per `(uploadTarget, level)` in `MGPipeResourceRecord::PendingUploads`, consumed and cleared only where the backend actually uploads, and no number of bails loses texels.
- Accumulation reuses the client's rect model: the union box is taken as a union, rect lists are concatenated; if either side has only a box, or the list would exceed `kMGPipeMaxPendingUploadRegions`, that entry degrades to a box-only entry (still covering every texel, just transferring extra gaps).
- respecify discards only the pending uploads on the storage it replaces (the boxes are in that storage's coordinate system; preserving them across a shrink would upload out of bounds):
  - an empty level = a whole-resource redefinition (`glBufferData` / `glBufferStorage`, `glTexStorage*`, texture views), clearing everything;
  - a named level = one `glTexImage*D`, dropping only that `(uploadTarget, level)`, regardless of whether the descriptor changed. Clearing everything would lose texels of other levels that were already accepted but not yet uploaded: `glTexImage2D(0)` → draw (texture incomplete, backend bails) → `glTexImage2D(1)`;
  - a metadata-only change (`RespecifyRedefinesNoStorage`: the descriptor re-sent when the sticky `BindMask` / `ImageBindableHint` changes) drops nothing.
- A buffer has no pending uploads and never counts as metadata-only either.

#### 6.1.1 The region list is the exact dirty footprint

The region list of a texture `resource_subdata` is the **exact dirty footprint** of that level since it last became clean: disjoint boxes that cover every written texel and not one more (`MGPipeTextureEmitter::EmitOneLevel`). Reason: once a server-side image may contain GPU writes, an upload beyond the application's write range would overwrite them. Magma always treats the list as the texels to upload; Espryt does the same when the driver texture may contain GPU writes, and otherwise still chooses the upload shape itself between the box and the rects (the Mali job count is still decided by the server).
- `RegionCount == 0` only means a whole-level write; every other write carries boxes (even a single one), so the applier's accumulation never relaxes an exact contribution into a union box because one side is "box only".
- Exception: when the footprint exceeds the `kMGPipeMaxPendingUploadRegions` boxes one record can carry, it falls back to the storage's rect list (coverage only, logged once); when that list is in turn withheld (the rects fill 3/4 of the union box), 0 is an **inexact** partial box. Readers that read "box only" as "the client owns every texel in the box" (the coverage-box rule of `AdoptDriverLevelIntoShadow`) are inexact only in this case.

#### 6.1.2 Texture view uploads are recorded on the storage owner

`glTextureView` shares texels with the source texture, so the client records every upload made through a view or through the owner's name on the **storage owner**: `TextureObjectView` converts the dirty mark into owner coordinates (`ToOwnerLevel` / `ToOwnerUploadTarget` / `ToOwnerRegionOffset`) and forwards it to the owner, the drain list in `TextureEmit.h` is keyed by (storage owner, upload target, level), and every texture `resource_subdata` points at the owner handle; the applier therefore advances only the owner record's `Serial` and `PendingUploads`.
- Espryt's per-texture clean gate `BackendTextureObject::IsDrawSyncCleanByRecord` reads the owner record pointed to by `Desc.ViewOf` (`PipeTextureStorageRecordForRecord`) for the two storage clauses, while the three parameter clauses still read the view's own record: a view has its own texture parameters and built-in sampler. This is the same rule as the frontend's `TextureObjectView::GetContentVersion()` forwarding to the owner.
- If the storage clauses read the view's own record, uploads through the owner's name would never become visible on the view after it is first sampled. The gate is `TextureViewAliasScenario.TheOwnerUploadDrainRunsAgainAfterADrawBoundary`, run under monolith and all three transport arms.

## 7. Shader state = SPIR-V + reflection archive

- `CreateShaderState` sends per-stage SPIR-V + a reflection archive across the wire, **not source**: glslang entirely on the client, SPIRV-Cross entirely on the server, a file-level split; no compile pool on the server. The archive serializer is `ProgramArtifactsCodec`, and monolith calls it only in verify builds (D-H3).
- The server **lazily specializes** at the verb from the pushed state (D-B2: draw FBO clamp mask, fragColor broadcast count, storage-block binding signature, image formats, patch parameters, etc.) — exactly what the two backends did before.
- link / compile failure needs no synchronous return: `GL_LINK_STATUS` is answered by the client from the `ProgramObject`, and backend failures surface as ≥ERROR lines in the server log (control-plane `LogLine`, lossless, `MG_Remote/Transport/LogForward.h`).
- Internal shaders (color blit, depth mip, multisample resolve) are baked into SPIR-V signed into the tree (`Wire*Spirv.h`), and the freshness gate `MOBILEGL_BAKED_INTERNAL_SHADERS` (`MG_Test/SelfTest/BakedInternalShadersTest.cpp`) reruns in-tree glslang and compares byte for byte (P7).
