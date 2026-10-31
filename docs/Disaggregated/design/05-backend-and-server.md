# Backend Changes and the Server Side (formerly ARCHITECTURE §9–§10)

> Design details. The design highlights and an index of all sections are in [`../ARCHITECTURE.md`](../ARCHITECTURE.md). Section numbers follow the original `ARCHITECTURE.md`; `ARCHITECTURE.md §N` in code comments resolves by number here.

## 9. Backend state machine changes

### 9.1 Things left untouched

Espryt's persistent ring, buffer pool, fallback-repack, scratch FBO, driver binding shadow, Adreno / Mali workarounds, SPIRV-Cross session, restart rewriting and multi-draw tiering; Magma's `VulkanRenderer` memo, `PipelineFactory`, `ProgramFactory`, `UniformManager`, the five `Vk*Manager`s, `SwapchainObject`, and D18's node-style container discipline. The "inputs changed, algorithm must not" category is guarded by the **G5 byte-identity gate** (`scripts/p3a_untouched_regions.sh` eleven functions, `scripts/p4a_untouched_regions.sh` 17 regions), and a re-pin must change both together.

### 9.2 strangler scaffolding: `PipeInputs` + per-verb fill + poison generation (P1)

```cpp
// MG_Backend/MGPipe/PipeInputs.h — organized by memo key, with accessor types exactly matching what the backend read before
#if MOBILEGL_PIPE_PUSH

#### 9.2.1 Server-side verb stamps and read checks

The client's residual fill stamps every field at its own verb boundary, and the server must swap in its own stamp before reading `PipeInputs`: when it shares one block with the client, without zeroing every read is "fresh" and `rsp` is always 0, making the gate a no-op; when the blocks are separate, without a stamp the first read aborts.

- Stamp points: `PipeApplier::StampVerbBoundary` calls `MGPipeServerStampVerbBoundary` only on ops that `MGPipeVerbForWireOp` (`MGP_VERB_OP_LIST` in `FieldOwnership.def`) maps to a verb: it increments `CurrentVerbSerial`, records the verb, stamps `RECORD_SUPPLIED` / `APPLIER_DERIVED` fields within that verb class's readable mask (`FillPoints.def`) with the new stamp, and zeroes the stamps of the rest ("never filled" precedes the sticky test, so zeroing also cancels the sticky exemption).
- Non-boundary records (such as `set_dynamic_state` between two draws) are not stamped and carry over the previous boundary's serial, mask and verb name, but the stamped flag has been cleared after the previous record, so the read check treats them as outside a stamp; all 20 draw entry points are stamped as `DrawArrays`, the mask is correct, and the name in the Fatal is generic.
- The read check splits by ownership class (§3.1's G8) only inside a server-stamped verb: reading a stale value in `RECORD_SUPPLIED` / `APPLIER_DERIVED` is `Fatal{RoleViolation}`, `FATAL` aborts outright, and `BARRIER_PULLED` (for generator cross-check only, no production rows) counts into `rsp` within that verb class, with `CountBarrierPull` deciding when to abort instead (record not barriered, two roles using different blocks, not admitted under strict). Outside a stamp everything is the ordinary poison `Fatal{UnmigratedPipeInput}`.
- The stamped flag is cleared by `PipeApplier::LeaveApplier` after each record; the server process has no client half to clear it, and a missed clear makes every later read judged by the previous verb's mask.

#### 9.2.2 Client writes to `PipeInputs` under run-ahead

Under run-ahead, while unbarriered records are being applied, `PipeInputs` is server-role memory: for these verbs the client does not fill, does not stamp, and does not withdraw the server's stamp; it only runs the tracker and the emitter (`MGPipeValidateForVerb`'s `fillOwed`). The only legal write by the GL thread is the fill of barriered verbs, and it must hold at the moment of writing:

- The validate order is fill → emit → park; during fill the apply thread may still be executing previously published unbarriered records, so before every write `QuiesceApplierBeforeFill` first waits for the applier to catch up with the last published seq (`ClientSession::WaitForApplyToCatchUp`, the same kind of forced wait as `glFinish`).
- The block's write is split into two parts with the record publication in between: before emission write the verb serial and withdraw the server stamp and the verb name; after emission do the residual field walk; the emitted record reads this block when it is applied, and one wait cannot cover both parts.
- `ClientSession::RefusePipeInputsTouchWhileApplierOwnsIt` checks the fact (the apply thread is not inside the applier) and does not trust the caller's declaration; a missing wait is `Fatal{RoleViolation, "gPipeInputs"}`.
- Under lockstep `appliedSeq` is already on the last published seq, so the wait returns immediately. When client and server use different blocks (`MGPipeBlocksAreDistinct`: role drills or spawn), the client writes only its own block and need not wait, and the server block's stamp is cleared only by `PipeApplier::LeaveApplier`.
#  define MGB_CTX (&::MobileGL::MG_Pipe::gPipeInputs)
#else
#  define MGB_CTX (::MG_State::pGLContext)
#endif
```

Three steps: A alias (P1, mechanical replacement + per-verb-class fill points, `nm` unchanged) → B push (P2, the tracker fills `gPipeInputs`, verify builds compare against the snapshot version draw by draw) → C handle conversion (P3a–P4a, P7, `SharedPtr<frontend object>` → handle + descriptor, writeback becomes a callback). Poison is **per-verb generation**: reading a field not filled for the current verb is `Fatal{UnmigratedPipeInput, "<Getter>@<Verb>"}`.

**P13 wrap-up**: the pull branch is deleted together with the macro (W3b, `MGB_CTX` no longer exists, and the backend takes state only through `gPipeInputs` and applier records); the monolith also goes through the record arm (W4–W5), and the backend's frontend-object arm and frontend-keyed twin entries are deleted (W6) — Espryt's twin table only accepts client-minted handles, object death notifications carry a handle, and release is by handle (the same release as the server's `object_death`).

### 9.3 Track V / Track H

Track V (value types: render state, pixel store, capability bits, scalars) is a mechanical migration; Track H (167 `SharedPtr<MG_State…>` read sites) is the real work. 74% of the read sites are translation inputs, so "bump a version and let the server pull" does not work; the values themselves must go over.

### 9.4 Residual value block

During migration, `SetResidualValueState` (`ResidualValueBlock`) is called temporarily: its size only shrinks, never grows (1248 → 8, only `CapabilityBits` left), and it is deleted in P13 W3c — op 46 retires as a rejecting line, and the capability cross-check goes to the verify comparator; `set_context_values` carries the remaining value-class fields (also sent under monolith, W4d / W8).

### 9.5 Re-keying identity memos

`{slot, gen}` + explicit destroy means that of the 21 identity memos, **11 are deleted outright**, 2 move their debounce to the client (§5.4), 7 are re-keyed to a cheaper comparison, and 1 (D18) is untouched. The unified fact: a version counter that enters a memo key either wraps or is simply not bumped by the mutation it fears; identity comparison is a patch over the hole.

### 9.6 A/B and narrowing the criteria

`MOBILEGL_PIPE_PUSH`'s subsystem bitmap is a real A/B in phase B; after phase C, when the bits are cleared, the backend still runs the re-keyed code, so the **compile-time** `MOBILEGL_PIPE_LEGACY_MEMOS` once preserved the pre-handle arm. P13 ended this: `LEGACY_MEMOS` and the pre-handle arm are deleted, bits 0–13 are permanently on, and clearing a bit is a named refusal at startup (the original Off lane becomes the negative control for this refusal), leaving only bit 63 settable. Dependencies between bits are refused on both sides (the client family gate **does not emit at all**, otherwise "the client has cleared dirty while the server takes the old arm" would drop uploads); a backend with no consumers sends nothing.

## 10. Server side

### 10.1 Object tables and the applier

`MG_Remote/Server/PipeApplier`: decode → update the object tables and `PipeInputs` → call the backend. Monolith (since P13) is the same record arm minus the codec and the ring: state records go straight into the applier through `PipeRoute`'s monolith table, and verb records are handed by the emitters in `MG_Impl/Pipe/Verb` to this process's `MG_Backend/Record/RecordVerbSink`. The server does not hold a full copy of buffers (only the staged shadow) and does not hold the frontend object graph; under any transport, no `SharedPtr` or raw frontend pointer may cross the applier boundary (P5c role guard, P5e rule F). `InProcessTransport` and spawn go through exactly the same codec path.

### 10.2 Net gain on the monolith side

Even if IPC never ships: a whole class of address-reuse ABA becomes inexpressible; the FBO → program ordering hazard disappears; the layering inversion of `SwapchainObject` writing into `MG_Impl` disappears; `inproc` = the render thread. The monolith's net line count increases; the argument looks only at per-thread CPU.

### 10.3 Index host mirror (`Server/IndexHostMirror`, P8, pending a new ruling)

Design: under `kCapNeedsHostIndexBytes`, the server incrementally maintains a host mirror of element-array buffers from the `ResourceCreate/Respecify/SubData` stream it has to receive anyway, with zero extra traffic; the budget is `MOBILEGL_PIPE_INDEX_MIRROR_MB` (default 64), and beyond the budget it degrades to transporting via `MGHostSpan` per draw. **P6's a6 audit measured that host index spans have no producer and client indices are already owned buffers**, so P8 must be ruled again against real consumers rather than copying this section (see the git history).
