# Per-context pipe dirty mask: plan (2026-10-09, not started)

Replaces `MGPipeTracker::Update`'s per-verb polling of ~25 frontend counters with a per-context
mark word. Mutations OR in the subsystem bits they affect; the validate point visits only the
marked bits and keeps today's leaf compare. A sharper form of PLAN-P15 stage 1; it is client-side
only and the record stream stays byte-identical on every transport.

## Expected savings (revised by the planning pass)

The pool this change can touch on FCL is ~0.04-0.06 ms/frame:
- `Tracker::Update` 0.018-0.029;
- `CopyField` 0.018-0.021;
- part of `MGPipeValidateForVerb` self.

| workload | gain (ms/frame) |
|---|---|
| FCL | ~0.012-0.03 |
| rd12 | ~0.10-0.15 |

The 0.3-0.5 of PLAN-P15 stage 1 also skipped `ReadDrawBindings`, `MarkGpuWritesForDraw` and the
backend walks, which are out of scope here.

## Design

- `GLContext` gets:
  - `Uint32 m_pipeDirty = ~0u`;
  - an owner serial (`TakePipeDirty(trackerSerial)` returns all bits to a different tracker, so
    the verify shadow walk or a test tracker cannot steal marks);
  - `m_seenShareClock`.
- Root check = mask 0. No separate counter.
- Shutter groups are the visit unit, computed with today's formulas word for word:

  | group | bits | covers |
  |---|---|---|
  | GR | 0, 1, 3 | render state, pipeline state, patch |
  | GK | 2 | pixel pack |
  | GA | 4 | attrib defaults |
  | GV | 5, 9, 10 | VAO, vertex buffers + base instance, index |
  | GP | 6, 7, 8 | program walk |
  | GT | 12, 13, 14 | texture aggregates, bind generation, max unit |
  | GF | 11 | framebuffer |
  | GB | 15, 16, 17 | bind points |

  Closure: GT and GB also compute GP, because they mix `shader`, `opaqueUnits` and `programImages`.
- **Share-group clock:** in `ShareGroupState`, an atomic gen plus a context count.
  - Bumped by:
    - the unclassified-entry prologue;
    - classified bodies that write shared objects (`Uniform*`);
    - texture aggregates;
    - `BumpSamplingResolutionGeneration`;
    - link publish.
  - The bump is skipped while the group has one context; a later context starts with all bits.
  - Ordering: release `fetch_add` after the object writes, acquire load in the tracker.
  - Fan-out `{6, 7, 8, 12, 14, 15}`.
- **Pseudo-bit CV** (bit 18) for `EmitContextValues`. Its own accumulator is consumed only when a
  verb class that reads context values runs.

## Mark sites (chokepoints first; no per-call-site edits where a funnel exists)

| site | marks |
|---|---|
| M1 RenderState: sink from the `GLContext` constructor | `BumpVersions` → GR; the bare `++m_version` sites go through one `BumpRenderVersion()`; `SetPixelStoreParam` → bit 2 |
| M2 `BindingSlot` change sink (`MG_Util/Types.h`), marks only on a real change | FBO draw/read slots → 11; each VAO's index slot → 10 |
| M3 TextureState | `NoteUnitTouched`, bind generation, content/params notes → GT, plus CV when the high-water mark grows; sampling-resolution bump → GT + clock; `SetActiveTextureUnit` → CV |
| M4 BufferState | bind-point change → GB by target; `TouchBindPoint` → CV |
| M5 VertexArrayState | bind, delete, attribute change → GV |
| M6 FramebufferState | attachment change → GF |
| M7 GLContext | `UseProgram` / pipeline bind / pipeline delete → GP\|GT\|GB; XFB state → 17 + CV; attrib defaults → 4 |
| M8 `MGPipeNoteAggregate` | via M3-M6; clock bump for TextureContent/TextureParams/FramebufferAttachment |
| M9 `BumpLinkObservableVersions` → `MGPipeNoteProgramObjectChanged()` | covers link, both joins and the binary failure path; the hot `ProgramObject` setters get no marks (a TLS read per uniform write) |
| M10 tracker carry | pending base instance → GV |

**Export prologue.** `DECLARE_GL_FUNCTION_END*` (`Definitions.cpp`) runs `MGP_ENTRY_PROLOGUE(name)`
with a compile-time lookup in a new `MG_Pipe/PipeDirtyEntries.def`.
- Default: mark all bits and bump the clock.
- A `kBodyMarks` row means the body and funnels mark.
- Hand-written exports that do not forward (`glFinish`, `glFlush`, debug stubs, ...) call the
  prologue themselves.
- EGL/GLX/WGL/CGL entries mark all bits.

**Events outside GL entries:**

| event | coverage |
|---|---|
| MakeCurrent | tracker `Reset` + mark all on the incoming context; after the TLS rebase, move this into `CurrentGLContextSlot::operator=` |
| surface resize | not a tracker input (frontend default FB is a fixed placeholder); EGL mark-all covers it anyway |
| deletes that unbind | mark-all, plus the M2/M5 sinks on the inner unbinds |
| buffer realloc | no shutter reads buffer size; no change |
| GenerateMipmap mip growth | entry mark-all; the inner path also goes through the M8/M3 funnels |
| context reset / new session | `MGPipeForgetEndedSession` → `Reset` → all bits |
| backend in-verb writes | the M3/M8 funnels |
| async link completion | M9 |
| other contexts in the group | the share clock |

## Validate path

- **`Update` visit set:** the context marks, the tracker carry, the share fan-out when the clock
  moved, and all bits until primed; mapped to groups and closed.
  - If the set is empty, return 0.
  - Otherwise compute, compare and latch the visited groups only.
- **Oracle** (verify builds, `PipeVerify` armed): a shadow full walk F against the gated walk G.
  - It requires F == G, else `Fatal{PipeDirtyUnderMark, "<bits>@<verb>"}` with a ring of the last
    8 entry names.
  - It honours `VERIFY_FATAL=0`.
- **Emission:** wrapped in `if (liveDirty)`. Fired bits, and therefore records, are unchanged.
- **CV accumulator:** the existing generation and suppressor fast path stays as a second line of
  defence. `VerbMaskReadsContextValues` is precomputed per class.
- **Optional residual gating (C5)** is limited to rows that depend only on the address or the
  context identity.
  - It is keyed on context id, the bound VAO lifetime id, a fill-plan rebuild epoch and an
    applier-reset / pin-release epoch.
  - `GetProgramForDraw` / `GetProgramForDispatch` are never skipped: `CopyField` joins pending
    links and refreshes the uniform mirror there.
  - The accumulator is cleared only inside `fillOwed`; under run-ahead, unbarriered verbs keep it.
  - Poison stamps still stamp every walk field.
- **Left alone:** the run-ahead/barriered predicate, `QuiesceApplierBeforeFill` and the dual-block
  (`ROLE_SPLIT_STATE`) semantics.

## Found during planning: a pre-existing cross-context gap

Shutters 11-14 compare per-context aggregate counters that a texture or renderbuffer change bumps
only on the live context.
- Effect: a change made through context B, with no verb on A in between, does not fire A's
  framebuffer or texture bits.
- The mask keeps today's bytes exactly (visit only).
- Closing the gap means mixing the share clock into shutters 11-14 (C8). That changes the wire for
  multi-context traces, so it is a separate decision.
- FCL is single-context.

## Tests

- **Existing, unedited:** `TrackerTest` (`TrackerWalk`, `TrackerShippedEmitter`,
  `TrackerAggregates`) stays green; `RecorderGoldenTest` goldens are unchanged.
- **New `TrackerDirtyMarks`** table test, one row per tracker input. Each row:
  - mutates through the real mutator, not the export, so mark-all cannot hide a gap;
  - asserts the fired bits match the full walk;
  - asserts the expected bits were marked.
- **New share-group cases:**
  - deterministic: A walks, B writes a uniform on a shared program, A walks again and bit 8 must
    fire;
  - threaded: a hand-off between threads, checked by readback in `ConcurrentContextsTest`.
- **Red-once:** drop each funnel's mark in turn and see its row fail.
- **CI:**
  - `integration-verify` runs the oracle fatally.
  - Verify-only negative control `MOBILEGL_PIPE_DIRTY_DROP=<site>`, one site per class.
  - With `FATAL=0`, `entry-prologue` lists every entry still relying on mark-all.
  - `gen_pipe_dirty_surface.py --check` gains mark coverage.
  - A scanner CTest checks that every export has a `.def` row or the default.
- **Wire identity:** spawn/inproc/TCP arms, `spawn_lane_parity.py`, the retrace legs, and PipeStats
  fire counts identical to the baseline.

## Measurement

- **Host:** PipeStats per-group visits, the TrackerRoot hit rate, fires equal to the baseline.
- **Device:**
  - cpuhunt, FCL monolith on both backends + rd12, 3 interleaved reps.
  - Read from profiles: `Tracker::Update`, `ValidateForVerb` inclusive/self, `CopyField`, the
    prologue, and the TLS resolver for extra reads.
  - fps only as a no-regression check.

## Commits

| # | commit | size | risk | gain |
|---|---|---|---|---|
| C1 | regroup `Tracker.h` into shutter groups; still visits all | ~+250/-150 | low | 0 |
| C2 | mark plumbing M1-M10, prologues mark-all, `.def`, DROP knob, oracle in audit mode | ~+500, 14 files | medium | 0 (prologue +~0.004) |
| C3 | gate on, fatal oracle, share-group tests | ~+150 | medium | most of the `Update` cut |
| C4 | CV accumulator | ~+40 | low | ~0.002 FCL |
| C5 | residual gating (optional, measure first) | ~+120 | medium | 0.005-0.01 FCL / 0.03-0.05 rd12 |
| C6 | classify the FCL between-draw entries, table test, scanner, CI controls | ~+400 | medium-high | the actual win |
| C7 | docs, SCOREBOARD | small | - | - |
| C8 | optional, separate approval: close the cross-context aggregate gap | ~+60 | wire change in multi-context traces | correctness |

**C6 entry list:**
- draws (free);
- `BindVertexArray`, `BindBuffer`, `BindVertexBuffer`;
- float `Uniform*` / `UniformMatrix*` → {7, 8} + clock; `Uniform1i` → {7, 8, 12, 14};
- `UseProgram`, `BindFramebuffer`;
- `Viewport`, `Scissor`, `PolygonMode`, `BlendFunc*`;
- `Enable`/`Disable`, once every capability path is confirmed to use `SetCapability`;
- the depth, colour and stencil setters;
- `BindTexture`, `ActiveTexture`, `BindSampler`.

**Overlap with the TLS work (`tlshoist`):**
- `Core.h` / `Core.cpp`: separate hunks.
- `PipeFill.cpp`: medium conflict around `gPipeInputs` and `MGPipeNoteAggregate`.
- `TrackerTest` fixtures: use `.Take()`.
- `Definitions.cpp` macros and the GLImpl entry bodies: conflict if TLS hoists there.

Order:
1. C1 can run now.
2. C2-C5 after the TLS merge.
3. C6 after the TLS per-entry hoist.
