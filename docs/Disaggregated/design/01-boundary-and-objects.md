# Boundary and Object Model (formerly ARCHITECTURE §1–§2)

> Design details. The design highlights and an index of all sections are in [`../ARCHITECTURE.md`](../ARCHITECTURE.md). Section numbers follow the original `ARCHITECTURE.md`; `ARCHITECTURE.md §N` in code comments resolves by number here.

## 1. Boundary

### 1.1 In one sentence

`MG_Backend` was already a state machine glued to the target API (Espryt = DirectGLES, Magma = DirectVulkan); what it lacked was an explicit declaration of "what I was told". MGPipe is that declaration: the frontend **pushes** changes across before every verb, and the backend no longer pulls `MG_State::pGLContext`. The interface is derived backward from the key structures the two backends maintain themselves (`SetupDrawSnapshot`, `DrawTextureSyncKeys`, `ResolvedVertexBindings`, `g_syncedRenderStateParameters`, `BufferBackendOps` …); gallium is where the vocabulary is headed, not a premise of the derivation.

### 1.2 Two function pointer tables

`MGPipeScreen` (share-group scope: caps, resource, persistent map, fence) and `MGPipeContext` (everything else), generated from `PipeCalls.def`.

- A struct of function pointers instead of a virtual base class: a null entry already means "not implemented, the frontend falls back", and `MG_Test` can mock the whole thing by replacing the table.
- The two tables are separate from day one; splitting them later would renumber the records; v1 has only one screen, one context and one flow.
- EGL lifecycle and the caps side stay on the virtual functions of `pActiveBackendObject` (a rare path, on the control plane).

### 1.3 Three forms, one backend

| Form | What the tables hold | Purpose |
|---|---|---|
| `monolith` (default) | the backend's own functions; callbacks call `MG_State` directly | ship |
| `inproc` | emitter → applier on a second thread in the same process | CI main acceptance; that is, the monolith **render thread** |
| `spawn` | emitter → data plane → applier in another process → the same set of backend functions | two-process form; the control plane can be `fork` / `unix:` / `tcp://` (§11.9), and the server can be on another machine |

The only hook point is one `#if MOBILEGL_BUILD_DISAGGREGATED` branch inside `MG_Backend::Init()`: when `MG_Config::Transport != Monolith` it installs `MG_Remote::BackendObject_Remote`. Zero `#ifdef`s at `MG_Impl`'s boundary call sites.

## 2. Object model

### 2.1 Handle = `{slot, gen}` (`MG_Pipe/MGPipeHandles.h`)

- An 8-byte POD; **the client mints it, the server never returns a handle** → the whole catalog has zero creation round trips (deviation D1 from gallium).
- Slots are allocated densely per kind (free list + high-water mark), the server's object table is an array; `gen` is incremented only when a slot is reused.
- kind: `Buffer, Texture, Renderbuffer, Framebuffer, Xfb, RenderStateCso, VertexElementsCso, SamplerCso, SamplerViewCso, ShaderCso, Fence, Query, Context`. Reserved: `{0,0}` = null; `Framebuffer {0,1}` = the default framebuffer; the high segment of `ShaderCso` is for program pipeline composites.
- GL names are for diagnostics only (`GlNameForDiag`), never for identity, memo keys or content hashes.

### 2.2 Two kinds of generation, strictly separated

| | Owner | Answers | Crosses the wire |
|---|---|---|---|
| `MGPipeHandle::Gen` | client | "Is it still the same GL object?" | yes |
| `MGGen` (backend epoch, e.g. `g_bufferMutationEpoch`) | server | "Did I re-mint the driver object myself?" | **never** (it appears only in the form of a texture pull request, §8.4) |

The client's wrapping `Uint16` version counter is never the sole proof of freshness: when it crosses the wire, either widen it or send it alongside `{slot, gen}`.

### 2.3 CSO and mutable objects

- CSO (render state, vertex elements, sampler, sampler view, shader) is **content-addressed on the client side** (Mesa `cso_cache` precedent), and sends `delete_*` on LRU eviction; render-state capacity 64 (retuned in P13), sampler state 256.
- Buffer / Texture / Renderbuffer go through create / respecify / subdata / destroy; Framebuffer / Xfb are per-context identity + `set_*` payload.
- Two deviations: **D-G1** Espryt's vertex elements are addressed by VAO identity (the twin holds the driver VAO name and cannot be shared); **D-F2** Espryt's sampler view is addressed by texture identity, sampler state is content-addressed and reference-counted (an entry pointed to by a `BuiltinSampler` must not be evicted by the LRU).

#### 2.3.1 sampler state cache: reference counting and value lifetime

sampler state is value-addressed on the client side (`MGPipeSamplerCsoCache`, capacity 256): hash, probe, then confirm with memcmp; trusting a 64-bit hash match alone would merge two states into one CSO, causing incorrect filtering that no gate can see.
- Live handles avoid the LRU by reference count rather than by capacity: a published `MGPTextureParams` keeps pointing at its `BuiltinSampler` while the texture parameters do not change, the applier does not resolve this handle, and eviction does not count as a parameter change, so the handle is neither re-sent nor rejected. Therefore every `Acquire` takes a reference and `Release` returns one; the LRU considers only entries with zero references; when everything is pinned and capacity is exceeded, mint beyond capacity and count it in `OverCapacityMints` (an extra slot costs memory, a stolen handle costs correctness).
- The holders are the texture's built-in sampler (released when the texture dies or when parameters change and it re-acquires) and every CSO currently bound by `bind_sampler_states`; any caller that will still name the handle after returning must hold one. Capacity greater than `kMGPipeMaxTextureUnits` is only a size bound: a cache that cannot hold one full round of emission will mint beyond capacity every round.
- Slots carry no lifetime id: a CSO belongs to a value, not to a frontend object (another `SamplerObject` may hold the same value), `~SamplerObject` frees no slot, and the only death path is client-side LRU eviction. So a `NotifyStateObjectDestroyed(SamplerCso, lifetimeId)` coming from `~SamplerObject` never has a handle behind it; the backend must not key sampler twins by a `SamplerObject`'s lifetime id; a twin's life is only from `create_sampler_state` to the `delete_sampler_state` issued by the LRU.
