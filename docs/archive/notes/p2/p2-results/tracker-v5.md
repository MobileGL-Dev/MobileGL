# P2 package B — `p2/tracker` — result, rework round 5

Tree `~/w7/p2-tracker`, branch `p2/tracker`, branched from the contract commit `9c6a8a25`
(the `p2/contract` tag). Not pushed. Supersedes `tracker-v4.md`; everything that file says
still holds except its §2.3 cross-product numbers and its §7.4 "remaining limits", both
replaced by §2 and §6 below.

Round-5 scope: `tracker-review-v4.md`'s two majors, implemented as the FIXED DESIGN the task
handed down (two-level tokens on both sides, alias tracking with taint, field-resolved
readers, the four-way match rule, `--check` counting only field-resolved answers, and the
listed self-test controls). One commit sits on top of `9ff6061c`. The review's nine minors
are **not** closed this round except where the fix happened to close one (§4).

---

## 1. Commits

Rounds 1–4 (`9c6a8a25..9ff6061c`, unchanged): `f52dd262`, `9c9df515`, `8d000f7e`,
`a0c44c4f`, `e54e7399`, `e49f0ea7`, `b3daa404`, `7993d711`, `57b85f73`, `80b861b8`,
`d5d4e757`, `dc452c02`, `4e8f7179`, `06f9adfd`, `9ff6061c`.

Round 5 (`9ff6061c..HEAD`, new):

| sha | subject | closes |
|---|---|---|
| `dddb60b0` | `[Fix] (Pipe, DirtySurface): resolve both sides of the dirty-surface derivation to member+field, follow reference and pointer aliases, and turn every write the analysis cannot place into an UNDECIDED answer instead of a verdict` | **MAJOR 1, MAJOR 2** |

Round-5 diffstat: 2 files, +990 / −271.

```
MobileGL/MG_Pipe/DirtySurface.def |   60 +-
scripts/gen_pipe_dirty_surface.py | 1201 +++++++++++++++++++++++++++++--------
```

Both are B's rows in C.5. Whole package: 26 files, `git diff --summary p2/contract..HEAD`
shows **0** `mode change` lines; every subject matches `^\[Type\] (Scope): `; `git log
--format=%B p2/contract..HEAD` contains no `Co-Authored-By` (grep count 0). `git status
--porcelain` empty after the commit. **Nothing compiles**: neither file is included by any
translation unit (the only mention of `DirtySurface.def` outside itself is still the comment
at `MG_Impl/Pipe/PipeFill.cpp:926`), and all three build directories print `ninja: no work
to do.` — so every binary measured in §3 is the one round 4's review measured.

---

## 2. The two majors — one root cause, one fix

### 2.1 What was wrong (confirmed, both parts)

The two halves of the derivation did not resolve to the same thing. The writer side
recorded `m_x.f = v` as `MEM:m_x` + `FIELD:f`, but a write bound to a **reference**
(`for (auto& blendState : m_parameters.BlendStates) { blendState.ColorEquation = color; }`)
as `FIELD:ColorEquation` with **no** `MEM:` at all — so seven `RenderState.cpp` setters read
as writing nothing, and the gate printed a false UNDER-FIRING about them (MAJOR 1). The
reader side resolved `render.PatchVertices` through the walk local `render` to the whole of
`MEM:m_parameters`, so once round 4 taught the writer side to see `m_parameters`, every setter
that wrote any byte of it "supported" `NEW_PATCH_STATE`: 5 legal carriers became 35, and
`X(SetClearColor, NEW_RENDER_STATE|NEW_PATCH_STATE)` was green (MAJOR 2). Reproduced at
`9ff6061c` with the reviewer's own shapes (§2.4's "old" column is that script, loaded as a
module).

### 2.2 What landed (`dddb60b0`) — the fixed design, item by item

1. **Two-level tokens on both sides.** Every write and every read is `MEM:<member>` plus
   `FIELD:<member>.<leaf>`, where `<leaf>` is the first field below the member (a deeper path
   collapses to it — that is the granularity the shutter reads at) or `*` for a whole-member
   write or read (`m_x = y;`, `memcpy(&m_x, …)`, `m_x.reset()`, `return m_x;`).
2. **Alias tracking.** `extract_writes()` parses each body's declarations: a local with a
   `&`/`*` mark, or whose initialiser takes an address, is an alias; `auto& r = m_x.y`,
   `for (auto& e : m_x.arr)`, `T* p = &m_x.y`, a pointer rebound by `p = &m_x.g` (every
   binding counts — `RenderState::SetHint`'s `Hint* slot = nullptr; … slot = &m_parameters.X;
   *slot = mode;` is that shape), and an alias of an alias all resolve to the root member and
   the path they were bound to; a write through them records `MEM:m_x` `FIELD:m_x.y`. A
   value local written through `->` or `*` is a pointer or an iterator and is resolved the
   same way. A root the analysis **cannot place** — a reference/pointer parameter, a local
   bound to a call result or a ternary, a name the body never declares (a member without the
   `m_` prefix such as `ImageUnitBinding::Access`, a global), an assignment operator no
   pattern attributes, a `##` left after expansion — **taints the function**: a
   `TAINT:<site>` token that rides the call-graph fixed point, so a mutator that reaches a
   tainted body is UNDECIDED for every bit, never a verdict. Nothing is trusted by name
   except the standard library's `size()/begin()/find()` family: a non-read-only method
   call on an unplaceable root taints too (its callee's writes are still inherited by name,
   but the object they land in is unattributed, and for `push_back`/`reset` that is the
   whole write).
3. **The reader resolves to the same token, through one-line getters.** `render.PatchVertices`
   is `CTXFIELD:GetRenderStateParameters.PatchVertices` in `shutter_readers()`, which
   `shutter_movers()` resolves — `GetRenderStateParameters` → `m_renderState.GetAllParameters()`
   → `return m_parameters;` = `FIELD:m_parameters.*` — and narrows to
   `FIELD:m_parameters.PatchVertices`. A bare use of a walk local still expands whole. The
   walk's shutters now resolve to:
   ```
   NEW_PATCH_STATE      m_parameters{PatchDefaultInnerLevel,PatchDefaultOuterLevel,PatchVertices}
   NEW_PIXEL_PACK       m_pixelStorePackParameters{*}, m_pixelStoreUnpackParameters{*}
   NEW_SAMPLER_VIEWS    m_anyTextureContentGeneration{*}, m_textureBindGeneration{*}
   NEW_SO_TARGETS       m_anyBufferChangeGeneration{*}, m_transformFeedbackGeneration{*}
   NEW_VERTEX_ATTRIB_DEFAULTS  m_anyVertexAttribDefaultGeneration{*}
   ```
   (all 18 resolved; the pack/unpack ternary still yields both halves, as declared).
4. **Match rule** (`match_writes()`): a writer supports a bit iff there is a member in
   common AND, both sides field-resolved for it, the field sets intersect (`*` on either
   side is every field); a member in common with no field information on one side is
   **COARSE**; UNDER-FIRING only when every member the shutter reads is unwritten or written
   in disjoint fields with both sides resolved.
5. **`--check`** counts only SUPPORTED pairs as derived, prints the COARSE and UNDECIDED
   tallies, and **fails** on an UNDECIDED pair unless `DirtySurface.def`'s new
   `MGP_DIRTY_SURFACE_UNDECIDED_LIST(X)` marks it — and fails on a mark whose pair the
   derivation now decides, so a mark cannot outlive its reason. The list is **empty**.
6. **Self-test**: 10 → **21** negative controls, all against synthetic dictionaries or
   synthetic bodies through the real extractor (§2.5).

### 2.3 The gate's own output at HEAD

```
$ python3 scripts/gen_pipe_dirty_surface.py --check        # rc 0, 3.0 s (was 1.5 s)
dirty-surface: 73 mutators, all mapped, no stale rows; 45 render-state answers derived from RenderState.cpp and matching
dirty-surface: 8 other (mutator, bit) answers derived from their shutter in Tracker.h - supported at field level, under-firing only; 0 COARSE (a member in common, no field information, not counted); 0 UNDECIDED (listed in MGP_DIRTY_SURFACE_UNDECIDED_LIST, not counted); 24 rows carry a prose answer (kExplicitDestroy, kImmediate, kNoBackendRead, kPulledEveryVerb, kReverseChannel, kUnpublishedDestroy) that no derivation checks
dirty-surface: the under-firing half read 1262 function bodies across 82 files under MobileGL/MG_State/GLState + MobileGL/MG_Impl/Pipe, macros expanded first, both sides resolved to MEM:<member> + FIELD:<member>.<leaf>; 157 of those bodies carry a write it could not attribute and taint whatever reaches them, 10 of the 73 mutators reach one; 347 members are written outside those roots and any absence claim over one is undecided; a mutating call on a member-rooted lvalue and a call resolved by name both only WIDEN what a mutator is credited with, and a FIELD token is not scoped to a type

$ python3 scripts/gen_pipe_dirty_surface.py --self-test    # rc 0
dirty-surface self-test: 21 negative controls, all tripped; positive controls OK (SetPixelStoreParam's sixteen token-pasted writes are read at field level, and the seven reference-alias setters of RenderState.cpp resolve to m_parameters' fields)
```

**Final tallies, honestly**: derived (supported at field level) **8 of 8** non-render
(mutator, bit) answers in the file; COARSE **0**; UNDECIDED **0**. The eight are
`BumpTextureBindGeneration←NEW_SAMPLER_VIEWS`, the three `SetCurrentVertexAttribute*←
NEW_VERTEX_ATTRIB_DEFAULTS`, the three patch rows ←`NEW_PATCH_STATE`, and
`SetPixelStoreParam←NEW_PIXEL_PACK` (the `kPulledPartialShutter` bit). The number is the
same 8 round 4 reported, but three of those were vacuous then (any byte of `m_parameters`)
and none is now: `SetPatchVertices` supports the bit through `FIELD:m_parameters.PatchVertices`
and `SetClearColor` cannot. **The count did not go down; what changed is that the gate can
now refuse the other 30.**

The ten mutators that reach a tainted body are `MarkBufferObjectForDeletion`,
`MarkFramebufferObjectForDeletion`, `MarkProgramForDeletion`, `MarkRenderbufferObjectForDeletion`,
`MarkSamplerObjectForDeletion`, `MarkShaderForDeletion`, `MarkTextureObjectForDeletion`,
`MarkTransformFeedbackObjectForDeletion`, `RecordError`, `SetNamedTransformFeedbackBinding` —
every one a prose row, so no answer in the file depends on them. The taint they reach is
real, not a parser artefact, and it is the by-name call resolution that spreads it: each
calls something named `Bind(`, and one body of that name (`ImageUnitBinding::Bind`,
`TextureState.h:27-34`) writes `Level`, `Layer`, `Access`, `Format` — members without the
`m_` prefix, which the analysis cannot tell from globals.

### 2.4 The verdicts flipping, old script vs new, over the whole cross product

`~/w7/p2r5-logs/flips.out` (the script at `9ff6061c` loaded as a module against the same
tree; 73 rows × 16 non-render bits):

```
old tallies: {'REJ': 1104, 'ACC': 64}
new tallies: {'UNDECIDED': 160, 'UNDER-FIRING': 996, 'SUPPORTED': 12}
flips (old -> new): {('REJ', 'UNDECIDED'): 138, ('ACC', 'UNDECIDED'): 22, ('ACC', 'UNDER-FIRING'): 30}
-- REJ -> SUPPORTED (must be empty or explained):            (empty)
-- legal carriers per bit (old ACC count -> new SUPPORTED count):
   NEW_PATCH_STATE              33 ->  3   ['SetPatchDefaultInnerLevel', 'SetPatchDefaultOuterLevel', 'SetPatchVertices']
   NEW_PIXEL_PACK                1 ->  1   ['SetPixelStoreParam']
   NEW_VERTEX_ATTRIB_DEFAULTS    3 ->  3   ['SetCurrentVertexAttributeFloat', 'SetCurrentVertexAttributeInt', 'SetCurrentVertexAttributeUint']
   NEW_SAMPLER_VIEWS             3 ->  1   ['BumpTextureBindGeneration']
   NEW_SO_TARGETS                2 ->  1   ['BeginTransformFeedback']
   NEW_INDEX_BUFFER / NEW_VERTEX_BUFFERS / NEW_VERTEX_ELEMENTS   7 -> 1 each  ['MarkVertexArrayForDeletion']
   NEW_FRAMEBUFFER               1 ->  0
   (the other 7 bits: 0 -> 0)
```

The 30 `ACC → UNDER-FIRING` are exactly the `NEW_PATCH_STATE` collapse the review measured
(its 35 was over 936 function names; 33 here is over the 73 mapped mutators). The 22
`ACC → UNDECIDED` and 138 `REJ → UNDECIDED` are the ten tainted mutators over the other bits.
**Zero** rejections became acceptances. The review's three named rows:

```
$ (derive_bit_answers on the tip, one synthetic row each)
UNDER-FIRING SetClearColor      <- NEW_PATCH_STATE   it writes nothing NEW_PATCH_STATE's shutter reads (shutter: m_parameters{PatchDefaultInnerLevel,PatchDefaultOuterLevel,PatchVertices}; it writes: m_parameters{ClearColor}, m_renderState{*}, m_version{*})
UNDER-FIRING SetScissorBox      <- NEW_PATCH_STATE   … it writes: m_parameters{ScissorBoxWrittenMask,ScissorBoxes}, …
UNDER-FIRING SetViewportIndexed <- NEW_PATCH_STATE   … it writes: m_parameters{Viewports}, …
UNDER-FIRING SetBlendEquation   <- NEW_PATCH_STATE   … it writes: m_parameters{BlendStates}, …
UNDER-FIRING SetCurrentVertexAttributeInt <- NEW_PIXEL_PACK      (control 7, still red)
UNDECIDED    SetNamedTransformFeedbackBinding <- NEW_SO_TARGETS  the write analysis is not complete for this mutator: Bind() writes 'Access', which it never declares …
SUPPORTED    SetPixelStoreParam <- NEW_PIXEL_PACK
```

The seven setters of MAJOR 1, resolved through the alias (`~/w7/p2r5-logs/seven.out`; the
`m_parameters`/`m_pixelStore*` tokens only, taint count is the whole reachable set):

```
SetBlendFunc             taint=0  m_parameters{BlendStates}
SetBlendFuncIndexed      taint=0  m_parameters{BlendStates}
SetBlendEquation         taint=0  m_parameters{BlendStates}
SetBlendEquationIndexed  taint=0  m_parameters{BlendStates}
SetStencilFunc           taint=0  m_parameters{StencilStates}
SetStencilMask           taint=0  m_parameters{StencilStates}
SetStencilOp             taint=0  m_parameters{StencilStates}
SetClearColor            taint=0  m_parameters{ClearColor}
SetScissorBox            taint=0  m_parameters{ScissorBoxWrittenMask,ScissorBoxes}
SetViewportIndexed       taint=0  m_parameters{Viewports}
SetPatchVertices         taint=0  m_parameters{PatchVertices}
SetPixelStoreParam       taint=0  m_pixelStorePackParameters{Alignment,ImageHeight,LSBFirst,RowLength,SkipImages,SkipPixels,SkipRows,SwapBytes}, m_pixelStoreUnpackParameters{…same eight…}
```

and the three sites the review called "luck, not a property", on their own bodies:

```
VertexArrayState.cpp:CreateVertexArrayObject  auto& vao = m_vertexArrays[   -> FIELD:m_vertexArrays.*, MEM:m_vertexArrays | taint=0
VertexArrayState.cpp:VertexArrayState         m_vertexArrays.push_back      -> FIELD:m_vertexArrays.*, MEM:m_vertexArrays | taint=0
VertexArrayState.cpp:CreateVertexArrayObject  m_vertexArrays.resize         -> (same)
ProgramState.cpp:UseProgram                   m_currentProgram.reset()      -> FIELD:m_currentProgram.*, MEM:m_currentProgram | taint=0
```

### 2.5 `--self-test`, 21 controls

| # | control | what it asserts | kind |
|---|---|---|---|
| 1–5 | unchanged (withheld mutator, stale row, bad answer, under-firing render bit, dropped publisher) | | real def |
| 6 | `SetNamedTransformFeedbackBinding ← NEW_SO_TARGETS` | `--check` goes red on it (UNDER-FIRING **or** unmarked UNDECIDED) and the verdict is never SUPPORTED — the mutator reaches `Bind()` by name, so the honest red is UNDECIDED | real tree |
| 6b | `BumpTextureBindGeneration ← NEW_GLOBAL_CONSTANTS` | UNDER-FIRING outright — the object-class family can still go red as a verdict | real tree |
| 7 | `SetCurrentVertexAttributeInt ← NEW_PIXEL_PACK` | UNDER-FIRING | real tree |
| 8 | `kPulledPartialShutter` naming no bit | BAD | real def |
| 9a | a mutator given a canned `TAINT:` token | UNDECIDED, never a verdict | synthetic |
| 9b | the same, row unmarked | `--check` fails with `UNDECIDED answer …`, no UNDER-FIRING line | synthetic |
| 9c | the same, row marked in the undecided list | no problem, still no verdict | synthetic |
| 10 | shutter member written outside the roots | UNDECIDED with that reason | synthetic |
| 11 | body writing `m_parameters.ClearColor` vs a patch-field shutter | UNDER-FIRING (design 6a) | synthetic body |
| 12 | `for (auto& blendState : m_parameters.BlendStates)` and `StencilFaceState& state = m_parameters.StencilStates[…]` | `MEM:m_parameters`+`FIELD:m_parameters.BlendStates`/`.StencilStates`, no taint, SUPPORTED against their shutter, UNDER-FIRING against the patch shutter (6b) | synthetic body |
| 13 | `auto& state = LookUpSomewhere(face); state.WriteMask = mask;` and a write through a `RenderStateParameters&` parameter | both tainted, both UNDECIDED (6c) | synthetic body |
| 14 | `m_parameters = fresh;` | `FIELD:m_parameters.*`, supports the patch, blend and `LineWidth` shutters (6d) | synthetic body |
| 15 | `SetPixelStoreParam` | tokens contain `FIELD:m_pixelStore{Pack,Unpack}Parameters.<leaf>` (or the mutator is tainted), never `paramNameTail`, never UNDER-FIRING (6e) | real tree |
| 16 | `X(SetClearColor, NEW_RENDER_STATE\|NEW_PATCH_STATE)` | `UNDER-FIRING answer NEW_PATCH_STATE for SetClearColor` — the row that was green at `9ff6061c` | real tree |
| 17 | a shutter of `MEM:m_parameters` alone (no fields) vs a field writer | COARSE: counted as 0 derived, 1 coarse, no problem | synthetic |
| 18 | an undecided mark on a pair the derivation decides | `STALE undecided mark` | real tree |
| + | positive: `SetPixelStoreParam ← NEW_PIXEL_PACK` SUPPORTED, no undecided line; the seven alias setters each carry `MEM:m_parameters` and their field with no taint | real tree |

---

## 3. Verification — every command and its actual output

All at `dddb60b0` in `~/w7/p2-tracker`. `cmake --build {build-linux,build-push,build-verify}
-j 12` → `ninja: no work to do.` ×3.

### 3.1 The package's own verification block (brief §C.1, with ID-4's errata)

| command | output |
|---|---|
| `python3 scripts/gen_pipe_dirty_surface.py --check` | rc 0 — §2.3 verbatim |
| `python3 scripts/gen_pipe_dirty_surface.py --self-test` | rc 0 — `21 negative controls, all tripped; positive controls OK …` |
| `python3 scripts/gen_pipe_dirty_surface.py --summary` | rc 0, 81 lines, **byte-identical** to `9ff6061c`'s (`diff` empty) — E's CI step keeps working |
| `python3 scripts/gen_pipe.py --check` / `--self-test` | rc 0 — `inventory 477 rows: 299 -> call, 5 client-resolved, 6 reverse-channel, 167 structural handle, 0 UNMAPPED`, `generated files are up to date`; `7 negative-control trip(s), positive control OK` |
| `symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | rc 0 — `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`; `.text 10792579 -> 10792739 (+160, +0.001%)`; both files 19100448 bytes; the four resized are ID-3's set (`RenderState::RenderState()` +148, `SetCapability` +77, `IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9) |
| `ctest --test-dir build-{linux,push,verify} -L unit --no-tests=error -j 8` | rc 0 ×3 — `100% tests passed, 0 tests failed out of 1529` ×3 |
| `ctest --test-dir build-push -L integration-gpu --no-tests=error -j 4` | rc=0 100% tests passed, 0 tests failed out of 878 |
| `MOBILEGL_PIPE_PUSH=0 ctest --test-dir build-push -L integration-gpu --no-tests=error -j 4` (the all-pull arm) | rc=0 100% tests passed, 0 tests failed out of 878 |
| `ctest --test-dir build-linux -L integration-gpu --no-tests=error -j 4` | rc=0 100% tests passed, 0 tests failed out of 878 |
| `ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4` | rc=0 100% tests passed, 0 tests failed out of 818 |
| `python3 ~/w7/retrace_gate.py --tree ~/w7/p2-tracker --lib build-push/libMobileGL.so --out ~/w7/retrace-out/v5-push -j 4` (G3) | **STILL RUNNING / NOT YET RUN when this file was written** - the orchestrator called for the result while `~/w7/p2r5-lanes.sh` was in flight; the outcome lands in `~/w7/p2r5-logs/lanes.out` (binary unchanged since round 4, where it was 79/79) |
| `MOBILEGL_PIPE_VERIFY=1 python3 ~/w7/retrace_gate.py … --lib build-verify/libMobileGL.so --out ~/w7/retrace-out/v5-verify -j 4` + `Fatal{` / `PipeVerifyDiffer` / `PipeResidualDiverged` / `UnmigratedPipeInput` / not-`MGPipe: verify armed` greps over `<out>/<case>/<backend>/output/mobilegl.log` (G4) | **STILL RUNNING / NOT YET RUN when this file was written** - the orchestrator called for the result while `~/w7/p2r5-lanes.sh` was in flight; the outcome lands in `~/w7/p2r5-logs/lanes.out` (binary unchanged since round 4, where it was 79/79, Fatal{ 0, not-armed 0) |

### 3.2 Section-A gates this tree can reach on its own

| gate | command | output |
|---|---|---|
| **G1** | above | 0/0/4/0, the admitted four |
| **G2** | ID-4's ctest-name extraction on `build-linux` and `build-push` | `2407` and `2407`, `diff` empty |
| **G5** | `for r in refs/tags/p2/contract HEAD; do git show $r:MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp \| awk '/^    namespace RenderStateImpl \{/,/^    \} \/\/ namespace RenderStateImpl/' \| sha256sum; done` | `d8fd1c48716056c536752fde09db3a9e5aa8494b70404798bfc15d11220efe27` on both sides **and** equal to `~/w7/p2-before-syncrenderstate.sha` |
| **G9** | §2.3 | green, and the three patch rows are now supported for the field they write |
| **G13** | `gen_pipe.py --check`/`--self-test` (above); `check_include_closure.py` → `4 probes, 0 skipped, 0 problem(s)`; `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` → empty | green |
| **G14** | `LC_ALL=C comm -23 <LC_ALL=C-sorted baseline> <build-linux names>` | **0** of 2363 baseline names removed; 44 added |

Not reachable from this tree: **G6/G7** (package A), **G8/G12** (package E), **G11**
(device). Not claimed.

### 3.3 Discipline

`git status --porcelain` empty; `git diff --summary refs/tags/p2/contract..HEAD | grep -c
'mode change'` → 0; `git log --format=%B refs/tags/p2/contract..HEAD | grep -ci co-authored`
→ 0; 16 commits, every subject `[Type] (Scope): …`, bodies `- ` bullets. Not pushed.

---

## 4. The review's nine minors

1. **`writers_outside()` shares the write analysis's blind spots** — still true and now
   *said* in its docstring and in the `.def` header ("a shutter member written outside … is
   undecided in the absence direction", "textual and file-wide"). It was not switched to
   the new extractor: an outside body's writes through a reference are its own object's,
   and treating every unplaceable outside write as a possible frontend writer would make
   every shutter undecided. Open, declared.
2. **`macro_table()` collects `#define`s only under the two roots** — open, unchanged.
3. **`expand_macros(…, rounds=4)` gives up silently** — open, unchanged.
4. **the decline path never fires on the real tree** — now it does, one level down: 157
   bodies are tainted and 10 mutators reach one, `--check` prints both counts, and the 22 +
   138 pairs of §2.4 are real UNDECIDED verdicts on the real tree (none of them a bit
   answer in the file). Controls 9a–c, 13 and 18 exercise the plumbing synthetically.
5. **"8 other bit answers" is pairs, not rows** — the line now says `(mutator, bit)
   answers`, and none of the eight is vacuous (§2.3).
6. **the three §A command defects** — unchanged; ID-4 records the corrected forms and §3
   uses them (the brief's `<out>/*/mobilegl.log` glob still matches nothing; the real path is
   `<out>/<case>/<backend>/output/mobilegl.log`).
7. **round 3's minors 3, 4, 5, 8** (raw-address context identity, `LiveContext()` in
   `MGPipeNoteAggregate`, the three emission globals, `MGPipeCsoCache` slot reclamation) —
   open, untouched, as before.
8. **no lane started skipping** — nothing compiled this round either; the two deliberate
   skip pairs are unchanged (G2's 2407 = 2407).
9. **the outstanding list** — restated in §6.

---

## 5. Deviations from the brief (new in round 5; 1–17 from `tracker-v2/3/4.md` stand)

18. **`DirtySurface.def` carries a second X-list, `MGP_DIRTY_SURFACE_UNDECIDED_LIST(X)`.**
    D16 shows one list. The fixed design's item (5) needs the file to *mark* which rows are
    derived; marking the exceptions (known-undecided pairs, with the reason) keeps the
    default the strong claim, and `--check` refuses a stale mark so the list cannot hide a
    row. It is empty today. `load_mapping()` splits the file at that `#define`; a consumer
    of the first list sees no change.
19. **`function_signatures()` no longer yields `if/for/while/switch/catch/else` blocks as
    functions.** `FUNCTION_RE` matched `for (...) {` as a function named `for`; as a "body"
    it had no enclosing declarations (every write in it tainted) and every function with a
    loop "called" it. This also changes `function_bodies()` for the scanner half, but
    `--summary` is byte-identical, so the report is unaffected.
20. **A non-read-only method call is a whole write of its object.** The design says "a
    mutating member call on `m_x`"; the script cannot tell mutating from not, so on a
    member-rooted lvalue every call except the standard library's `size()/begin()/find()`
    family is credited (widening), and on an unplaceable root it taints. No name prefix
    (`Get*`, `Is*`) is trusted: I measured both policies and they taint the same ten
    mutators, so the version with fewer assumptions shipped.
21. **FIELD tokens are two-level.** The design writes the alias case as
    `FIELD:m_x.y[.leaf]`; the token shipped is `FIELD:<member>.<first field>`, with deeper
    paths collapsed, because the shutter side reads at exactly that depth
    (`render.<Field>`) and a third level would make a writer of `m_x.y.z` and a reader of
    `m_x.y` miss each other by string. Prefix matching would have been the alternative; not
    invented.
22. **A tainted mutator is UNDECIDED for every bit, even where a visible direct write
    would support one.** Design (2) says "never a verdict" and that is what shipped. A
    support-first ordering (SUPPORTED is monotone under unknown extra writes) would be
    sound and changes nothing today — none of the eight supported rows is tainted — but it
    is the integrator's call, not mine.
23. **Const aliases.** A write or call through a `const`-qualified alias with `.` and a
    `const` raw pointer are taken as impossible (the language forbids them); `->`/`*`
    through a `const` **reference** is not, because a `const SharedPtr<T>&` reaches a
    mutable pointee. `mutable` members written through a const path escape this; declared.
24. **`--check` costs 3.0 s, up from 1.5 s** (the declaration parse per body).

---

## 6. Unfinished, and the hand-off

1. **By-name call resolution is what spreads taint.** `SetNamedTransformFeedbackBinding` is
   undecided because *some* body named `Bind` writes `Access`. Scoping a call to its
   receiver's type would recover the ten prose-row mutators and let control 6 be an
   UNDER-FIRING verdict again; no bit answer in the file needs it today, so it is recorded,
   not done.
2. **Members without the `m_` prefix cannot be placed** (`ImageUnitBinding::Access`,
   `ProgramLinkTask::artifacts`); they taint their bodies. A class-context pass (know which
   struct a body belongs to) would place them. Same status as 1.
3. **`writers_outside()` is still the regex scan** (review minor 1); the `.def` says so.
4. Unchanged from rounds 2–4: package A's `MGPipeApplySetVertexAttribDefaults` hand-off
   (the applier-repair `MGLOG_W_ONCE` still lands once per push/verify process), the 26
   derived render-state mirrors still pulled until A's `c1`, `kPulledEveryVerb` still an
   unchecked absence claim, G9 not a CI gate until E lands `pipe-gates`, the
   `MEASUREMENTS.md` items the integrator owns (four resized symbols; `T1 − T2`; the flake
   family; §3.4 of `tracker-v4.md`'s residual firing rate), `FormatWindowLine`, the six
   unwired `SetHashSuppressor` slots, `MGPipeWidenedCounter` at exactly 65536,
   `s_hashForTest`, the three header-only files' `list(APPEND)` line, no device work.

Logs kept under `~/w7`: `p2r5-logs/{lanes.out,flips.out,seven.out}` and the verify retrace
corpus `~/w7/retrace-out/v5-verify/`; every other intermediate file of this round was
deleted.
