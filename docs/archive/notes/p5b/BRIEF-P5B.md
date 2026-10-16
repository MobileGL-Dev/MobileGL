# BRIEF-P5B — verb migration under inproc, Minecraft-first

**Goal, one sentence:** fully separate-thread rendering of REAL workloads as fast as possible —
every Minecraft trace, and the four A/B trace cases, rendered by the apply thread under
`MOBILEGL_TRANSPORT=inproc` — by migrating the class-C slots the census measured, in the order
it measured them, with the inproc lane's abort count and the traces' first-blocker list as the
only gates (ID-66, ID-68).

Read `PACKAGE-PREAMBLE.md` first, then `MobileGL/MG_Remote/CONTRACT-P5B.md` (the wire contract,
every row of it), then this file.

## 1. What c0b landed (head `p5b/c0b`, see `p5b-results/c0b-v1.md`)

Every package compiles and links on day one against real headers:

- the wire rows: `draw_vbo` grew `kDrawIsIndirect` + the `MGPDrawIndirect` second tail; five
  rows appended at opcodes 72..76 (`bind_shader_image`, `patch_parameter`, `bind_stream_output`,
  `set_storage_block_binding`, `copy_framebuffer_to_texture`); `MGPCopyRegion` grew its two GL
  names; the clear discriminants have one spelling in `MGPipeTypes.h`;
- the codec: encode/decode arms for every row, tail cross-checks, the name blob;
- `WireVerbSink` has one method per row (13 new + the widened `OnDrawVbo`), and
  `ServerVerbSink` overrides every one with a `Fatal{UnmigratedVerb, "<GL slot>"}` stub;
- the client emit table's class-C list is PARTITIONED per package
  (`MGR_UNMIGRATED_{D1,I1,T2,F1,TAIL}_SLOTS`, `kEmittedSlots{D1,I1,T2,F1}`, ownership
  `static_assert`s 19 / 7 / 7 / 11 / 20) so four packages edit four disjoint regions;
- the stamp rows (`FieldOwnership.def`), the generated tables, the pinned counts, and a
  round-trip unit case per row in `PipeWireCodecTest.cpp`.

## 2. The four packages

Each package owns its slots, its list and count in `EmitTables.cpp`, its stub bodies in
`PipeApplier.cpp`, and its cases in `PipeWireCodecTest.cpp` + the integration lane. CONTRACT-P5B
§2 gives every slot's record fields, sink dispatch and refusal names; §8 the two pre-granted
edits to c0b's files.

| package | measured slots (census entries) | companions (same rows, owned too) | rows | model |
|---|---|---|---|---|
| **d1** — indexed / instanced / multi-draw / indirect | `DrawElements` 56 (+ every Minecraft trace), `MultiDrawElementsBaseVertex` 18, `DrawArraysInstancedBaseInstance` 4, `DrawElementsBaseVertex` 4, `MultiDrawArrays` 2, `MultiDrawElements` 2, `DrawArraysInstanced` 2, `MultiDrawElementsIndirect` 2, `MultiDrawArraysIndirectCount` 2, `DrawElementsInstancedBaseVertex` (trace) | `DrawElementsInstanced`, `DrawElementsInstancedBaseInstance`, `DrawElementsInstancedBaseVertexBaseInstance`, `DrawRangeElements`, `DrawRangeElementsBaseVertex`, `MultiDrawArraysIndirect`, `MultiDrawElementsIndirectCount`, `DrawArraysIndirect`, `DrawElementsIndirect` | `draw_vbo` (59) | Claude, premium |
| **i1** — image / compute / barrier / copy-image / storage block | `BindImageTexture` 138, `DispatchCompute` 49, `CopyImageSubData` 30, `MemoryBarrier` 18, `ShaderStorageBlockBinding` 4 | `DispatchComputeIndirect`, `MemoryBarrierByRegion` | `bind_shader_image` (72), `launch_grid` (60), `memory_barrier` (61), `resource_copy_region` (53), `set_storage_block_binding` (75) | Claude |
| **t2** — XFB spans, XFB object bind, patch parameter | `BeginTransformFeedback` 95, `PatchParameteri` 43, `BindTransformFeedback` 2 | `EndTransformFeedback`, `PauseTransformFeedback`, `ResumeTransformFeedback`, (`DeleteTransformFeedback`: no row, see contract) | `begin/end/pause/resume_stream_output` (62..65), `bind_stream_output` (74), `patch_parameter` (73) | Claude |
| **f1** — clears, framebuffer-sourced copies, mips | `ClearBufferiv` 12, `ClearBufferuiv` 8, `ClearBufferfv` 6, `ClearBufferfi` 1, `ClearNamedFramebufferfv` 2, `CopyTexImage2D` 3, `GenerateMipmap` 2 | `ClearNamedFramebufferfi/iv/uiv`, `CopyTexSubImage2D` | `clear` (57), `copy_framebuffer_to_texture` (76), `generate_mipmap` (54) | codex astra — additive and mechanical |

Per slot the work is the same three edits: the emitter (class C → B in `EmitTables.cpp`, the
pre-verb hook order kept), the `ServerVerbSink` body (the backend call the contract names), and
a lane case that proves the record crossed. Nothing in the backends, unless the contract names
the site (i1's mirror skip, f1's optional mip-storage re-derivation) - and then only behind
`#if MOBILEGL_BUILD_DISAGGREGATED`.

## 3. The per-package gate

- **the inproc lane's abort count for your slots → 0**, and each affected scenario reaches its
  NEXT first blocker, reported by name (`Fatal{UnmigratedVerb, "…"}`, `Fatal{UnmigratedEmulation,
  "…"}`, or a wrong-answer failure - whichever comes first). Run
  `~/w7/p5b-c0b-census.sh <your tree> <outdir>` and diff `results.json` against c0b's baseline
  (`~/w7/p5b-c0b-census-logs/results.json`: 432 passed / 185 skipped / 505 aborted / 27 failed,
  all 505 aborts `UnmigratedVerb`, the per-slot table identical to `census-classC.md`'s).
- **d1 additionally: every Minecraft trace reaches its next first blocker under inproc**,
  reported by name per fixture (the census's trace table), via the same per-entry private-log
  method as `p6_census_traces.{sh,py}`; a trace that RENDERS is reported with its SSIM.
- the integrator's quick gate: unit lanes green (c0b's baseline + yours), `integration-split`
  inproc 22/22, push `integration-gpu` 1128/1128 name-for-name, G1 0/0/0/0 `.text +0`.
- no reviews between rounds; one codex review at P5b's close.

## 4. File ownership

| file | owner in P5b | others |
|---|---|---|
| `MG_Pipe/MGPipeTypes.h`, `PipeCalls.def`, `PipeFields.def`, `FieldOwnership.def`, `Coverage.def`, `FillPoints.def`, `scripts/gen_pipe*.py`, `MG_Remote/CONTRACT-P5B.md` | c0b / integrator | t2: `kCapBackendOwnsXfbCapture` (contract §6.5); i1: the `GetProgramForDispatch` row (§6.9). Anything else: ask. |
| `MG_Remote/Wire/PipeWireCodec.{h,cpp}` | w1's file; c0b landed the arms | a package edits the layout arm of ITS row only, with the integrator |
| `MG_Remote/Server/PipeApplier.{h,cpp}` | v1's file | each package replaces ITS stub bodies (the `// ---- d1/i1/t2/f1 ----` blocks) and may add ITS tallies |
| `MG_Remote/Client/EmitTables.{h,cpp}` | c1's file | each package edits ITS `MGR_UNMIGRATED_*_SLOTS`, ITS `kEmittedSlots*`, ITS emitters |
| `MG_Remote/Client/SlotCaps.h` | c1's file | t2: the `EndTransformFeedback` probe → `kCapBackendOwnsXfbCapture` |
| `MG_Test/Wire/PipeWireCodecTest.cpp`, `MG_Test/Wire/RemoteClientTest.cpp` | shared | append your cases below c0b's; the partition pins (`UnmigratedSlotCount()` etc.) move with your counts |
| `MG_IntegrationTest/**` | t1's file | each package adds ITS lane cases / scenario arms |
| `MG_Backend/DirectGLES/**`, `MG_Backend/DirectVulkan/**` | untouched by c0b | only the sites the contract names, only behind `#if MOBILEGL_BUILD_DISAGGREGATED` |
| `~/w7/notes/p5b/p5b-results/<slug>-v1.md` | you | |

## 5. The loop that follows

1. The four packages land, each on its own gate. The integrator merges in landing order,
   regenerates (`gen_pipe.py`, `gen_pipe_field_ownership.py` - never hand-merge `generated/`),
   runs the quick gate.
2. **Re-census** on the merged head: the lane (`p5b-c0b-census.sh`) and the traces. The new
   first-blocker table is the next round's package list, same shape, same gates.
3. Migrate the new first blockers (wave 3's tail is P9/P10-shaped: readbacks, queries, syncs)
   and the residual-input debts `rsp` points at where a verb's pull is what blocks it.
4. Repeat until the four A/B trace cases render under inproc on the Redmi. That is P5b's exit;
   P6's spawn transport follows (ID-68).
