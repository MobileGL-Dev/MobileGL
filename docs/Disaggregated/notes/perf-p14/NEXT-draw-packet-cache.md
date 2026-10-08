# Next phase: a per-(VAO, program) draw-packet cache (design sketch)

This is a starting point for the next phase, not an implementation. The numbers are from the P14
close-out (`BENCH.md`, rd12 monolith, Y700, pinned, render thread, ms/frame).

## 1. The problem

On the record arm, a draw crosses five layers, and each layer keeps its own per-object state:

| layer | Espryt feat | Espryt dev | delta | what it touches per draw |
|---|---|---|---|---|
| client half | 1.78 | 0.39 | +1.39 | tracker shutters (VAO, program, context gens), VaoMemo, emitter latches, slot allocator, residual block, verb port |
| applier | 0.07 | 0 | +0.07 | vertex-elements record, applier unit windows, global-constants block |
| backend | 1.07 | 0.62 | +0.45 | VAO twin, texture/sampler records + twins (clean checks), program twin, UBO ring |
| driver | 1.69 | 1.36 | +0.33 | the same GL calls as dev; slower, consistent with cache pressure (+56 % L1D refills, +16.6 % instructions, whole run) |

Minecraft rd12 issues 1471 draws a frame, about 3.3 GL calls per draw. Between two consecutive
draws, usually only the bound VAO and the program's default-uniform block change. Each layer still
re-derives "what moved" from its own structures, and each of those structures is a different cold
cache line per VAO: the frontend VAO, the VaoMemo entry, the emitter latch, the applier record, and
the backend twin. Pull touches one of them, the frontend VAO.

None of the individual items is above 2 % of the frame. The sum is the gap: on Espryt monolith,
91 vs 115 fps.

## 2. What one draw packet holds

There is one packet per (VAO lifetime id, program lifetime id) pair that has drawn. It lives in a
client-side table, and in the server's record space on the split arms. Its fields:

- **Identity:** VAO lifetime id + config version, program lifetime id + link version, and the
  context serial.
- **Client side:**
  - the vertex-elements handle and its published config version;
  - the buffer-set and index handles and their hash, which the VaoMemo holds today;
  - the shader CSO handle;
  - the residual-field values this pair contributed (the bound-VAO and program pins);
  - the context-global epoch it was validated at (see section 3).
- **Applier side:** a pointer, or slot index plus generation, to the vertex-elements record and the
  stored buffer/index window, so that bind_vertex_elements applies by reference instead of copying.
- **Backend side (Espryt):**
  - the VAO twin;
  - the resolved draw buffers;
  - the program twin;
  - the sampled-texture list with each entry's synced serials;
  - the synced attachment list;
  - the UBO block size.
- **Backend side (Magma):** the wire vertex-input state, the pipeline handle, the descriptor-set
  layout, and the pass-compatibility id.
- **A "packet valid at" stamp per layer:** the serials and epochs each layer checks to trust its
  slice of the packet.

The goal is that a draw whose packet is valid at every layer touches the packet and the per-draw
inputs, and nothing else. The per-draw inputs are the uniform span, the draw arguments, and the UBO
ring slot.

## 3. Keys and invalidation sources

**Keys.** Lifetime ids only, never GL names or pointers. This is the ABA rule, already broken and
fixed three times: sampler, texture, program. The packet table key is (VAO lifetime id, program
lifetime id, context serial).

Invalidation sources, by layer:

- **Frontend context-global state.** This is everything the tracker reads that is neither VAO- nor
  program-derived: render/pipeline state versions, texture content/params/bind generations,
  framebuffer attachment and bind versions, buffer bind-point generations, TFB, pixel pack, patch,
  vertex attribute defaults, and the program pipeline.
  - This needs a context mutation epoch, bumped in every GL/EGL entry except the
    whitelist: draws, glBindVertexArray, and float glUniform*.
  - The bump fits in the `DECLARE_GL_FUNCTION_END` macros, with the whitelist as a constexpr name
    test, plus the manual entries (glFinish/glFlush and the hand-written extension wrappers) and
    the EGL entries.
  - **Audit item:** every MOBILEGL_GL_API definition outside the macros.
- **VAO.** The config version (attribute pointers and enables), the index slot version, and buffer
  respecification through `AnyVaoAttributeGeneration`. These are already lifetime- and
  version-keyed.
- **Program.** Link version, backend state version (sampler units through glUniform1i), block
  binding version, and image unit version. Float uniforms move only the UBO content version,
  which is a per-draw input and not a packet invalidation.
- **Applier.** The record `ContentSerial`, `ContextSerial`, `MGPipeSlots().ResetEpoch()`, and the
  vertex-buffers and index-buffer serials. These are already monotonic.
- **Backend.**
  - Texture: record `Serial`, `ParamsSerial`, and `PendingUploads`; twin force-resync flags;
    sampler CSO serial; shared-image state.
  - Context generation.
  - Magma only: texture image epoch, resource erase epoch, the image layout leaving GENERAL, and the
    store destroy epoch. The P14 Magma memos already check these.
  - The backend needs one "texture state moved" epoch, so that the per-bound-texture clean check
    can be skipped when nothing in the texture family moved. This is the item the content-version
    lesson warns about: glGenerateMipmap grew mips without a version bump. Every backend-side mutation
    of a twin's synced state must bump the epoch too.

## 4. Split arms and the verify comparator

- **The packet is per role.** The client half of the packet (handles, latches, residual pins)
  lives in the client. The applier and backend halves live wherever the applier runs: the same
  thread on monolith, the apply thread on inproc, the server process on spawn and tcp.
- **Nothing in the packet crosses the wire.** The wire stays the record stream. A server-side
  packet is keyed on the vertex-elements handle and the shader-CSO handle (slot + generation)
  plus the applier context serial, because the server never sees frontend lifetime ids.
- **Run-ahead.** Client-side validity must not depend on server state. The client's half is
  checked against client epochs only, exactly like today's latches.
- **The verify comparator (`MOBILEGL_PIPE_VERIFY`) compares the residual block against a live
  snapshot at the verb.**
  - A packet hit that skips residual copies would make the comparator diverge, unless the packet's
    pins are what it compares.
  - Proposal: under verify, take the slow path always and assert that the packet would have hit
    with the same values. That gives a free cross-check of the epoch's coverage on every verify
    lane.
- **POISON stamps (trace build only).** The stamp loop stays, or the packet records the stamp set
  and applies it with one bulk write.

## 5. Risks

- **A missed epoch bump renders stale state.** This is the dominant risk. Mitigations:
  - the verify-lane cross-check above;
  - a negative control that removes the bump from one entry and must go red;
  - SSIM 16/16 on every change;
  - the existing retrace CI.
- **ABA on packet keys.** Lifetime ids only, plus context serial and slot reset epoch.
- **Memory.** One packet per drawn (VAO, program) pair. Minecraft has thousands of chunk VAOs ×
  a few programs, so a bounded table with LRU is about 100-200 B × 10k ≈ 2 MB. The table must
  not itself become the next cache-miss source: entries go in a flat array with an index stored
  on the frontend VAO.
- **Backend-side invalidation completeness** (section 3, backend): the same class as the reverted
  e8e1521.
- **Code size and review cost.** It touches every layer. It should land behind a runtime knob,
  default off until the CI and SSIM gates and an FCL soak are green.

## 6. Rough gain estimate (rd12, ms/frame, Espryt monolith)

| layer | removable on a packet hit | why not all of the delta |
|---|---|---|
| client half (+1.39) | 0.6-0.9 | the uniform span, bind record and draw record still have to be produced |
| applier (+0.07) | 0.03 | bind by reference |
| backend (+0.45) | 0.25-0.35 | the UBO ring slot and the GL calls stay |
| driver (+0.33) | 0.1-0.3, if the cache-pressure reading is right | unproven; measure L1D refills before and after |

Total: about 1.0-1.6 ms of the 2.5 ms gap. That is roughly 91 -> 100-106 fps on Espryt monolith.
Magma's share is similar on the client half (+1.21). Its backend is already below dev, and its
driver term is +0.24.

Inproc gains the client-half part on the client thread: that is the bound thread on Espryt rd12,
so 112 -> above dev's 115. Magma inproc is bound on the apply thread, which gains only the backend
and applier part.

Prototype first, before the full design: the client-half packet (tracker + emitter + residual pins,
with the context epoch) on monolith Espryt only. Measure it against the 0.6-0.9 ms estimate, then
decide on the backend half.
