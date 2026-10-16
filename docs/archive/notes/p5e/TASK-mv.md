# TASK-mv — the multi-draw path stops reading the frontend VAO

Read `PACKAGE-PREAMBLE-P5E.md` first. Your tree is `~/w7/p5e-mv`, branch `p5e/mv`, base `7c6f6886`.
Your slug for the gate driver is `mv`.

## 1 What is measured

Per-entry marker census at your base (`~/w7/p5e-logs/strict-census/census2.tsv`): **9 entries**
abort with `Fatal{UnmigratedPipeInput, "GetBoundVertexArray@DrawArrays"}`, and every one of them is
a multi-draw case:

```
DirectGLES.Split.IndexedDrawFamilyScenario.MultiDrawElementsBaseVertexPaintsEverySubDraw
DirectGLES.Split.MultiDrawScenario.BaseVertexBatchMatchesUnrolledDraws
DirectGLES.Split.MultiDrawScenario.BaseVertexBeyondIndexTypeRangeMatchesUnrolledDraws
DirectGLES.Split.MultiDrawScenario.PlainBatchMatchesUnrolledDraws
DirectGLES.Split.MultiDrawScenario.PrimitiveRestartStripBatchMatchesUnrolledDraws
DirectGLES.Split.MultiDrawScenario.PrimitiveRestartUnsignedShortBatchMatchesUnrolledDraws
DirectGLES.Split.MultiDrawScenario.UnsignedByteBatchMatchesUnrolledDraws
DirectGLES.Split.MultiDrawScenario.UnsignedShortBatchMatchesUnrolledDraws
DirectGLES.Split.MultiDrawScenario.ZeroCountSubDrawsMatchUnrolledDraws
```

That confinement is the point: package vi already retired the bound-VAO row on the ORDINARY draw
path. `SyncCurrentVertexAttributeValues` takes `fromRecords ? noVao : MGB_CTX->GetBoundVertexArray()`
(`DirectGLES.cpp:2291`) and `PrepareForDraw` takes
`vertexInputFromRecords ? noFrontendVao : MGB_CTX->GetBoundVertexArray()` (`:5986`). What is left is
a path vi did not own.

## 2 The site

`MobileGL/MG_Backend/DirectGLES/MultiDraw.cpp:93-98`:

```cpp
const SharedPtr<MG_State::GLState::BufferObject>& BoundIndexBuffer() {
    static const SharedPtr<MG_State::GLState::BufferObject> none;
    const auto& vao = MGB_CTX->GetBoundVertexArray();          // <-- unconditional, no arm test
    if (!vao) return none;
    return vao->GetIndexBufferBindingSlot().GetBoundObject();
}
```

and its caller `BoundIndexBufferId()` just below, which turns it into a GL name through
`BufferImpl::EnsureBufferResource`. Read the whole file first: the multi-draw tiers swap in a
scratch index buffer and restore the original binding, and the comment at `:102-105` explains why
restoring the exact name matters (the VAO twin memoises that it already synced this index binding).

Audit `MultiDraw.cpp` for any OTHER frontend read on the apply thread while you are in there, and
report what you find even if you do not change it.

## 3 The wire already carries the answer

No wire change. The applier state already holds the index buffer:

- `MGPIndexBuffer IndexBuffer` and `Uint64 IndexBufferSerial` in the applier state
  (`MobileGL/MG_Pipe/PipeApply.h:688-690`), applied by `MGPipeApplySetIndexBuffer` (`:1247`).

So the handle arm answers `BoundIndexBuffer` from `MGPipeApplier().IndexBuffer` and resolves the
buffer resource by handle, the way the other by-handle resolvers do. Find the buffer-side
by-handle resolver that package vi / sb landed (`BufferImpl::EnsureBufferResourceForHandle` is one;
read `Managers.h`'s block of `Pipe*RecordForHandle` declarations around `:735`) and use it rather
than minting a new one.

If it turns out the multi-draw tiers need something from the VAO that the index-buffer record
genuinely cannot answer, STOP and report rather than inventing a wire row. The integrator wants to
hear it.

## 4 Requirements on the arm selection

1. **The arm is decided by `MG_Config::Transport`**, through the family's existing selector
   (`BufferImpl::VertexInputReadsRecords()` is the one this path's siblings use; check what it
   resolves to and use the same one), **never inferred from a pointer being null**. This is ruling
   ID-81. The phase has already paid for breaking it: fix1 (`66621767`) and rulings ID-107 /
   ID-109 / ID-110 exist because one seam read "a handle was noted" as "the record arm is
   selected". Read `docs/Disaggregated/CURRENT_STAGE_PROGRESS.md` §2.5 before you write it.
2. **A missing record on the handle arm is a named refusal, not a silent fall-back to the
   frontend.** Copy `RefuseNullFrontendTextureOffTheHandleArm`'s shape (search `Managers.cpp`).
3. **The pull build gains no call**; **the monolith arm keeps its bytes.**

## 5 Red-once (required, quoted verbatim in your report)

1. Revert the arm selection: the strict lane goes red again with `GetBoundVertexArray@DrawArrays`
   on those 9 entries.
2. Make the record lookup miss on the handle arm: your named refusal fires, quoted from the entry's
   own log (`MOBILEGL_LOG_FILE_PATH` — `MGLOG_F` does not reach ctest's captured stdout; the
   working recipe is `~/w7/notes/p5e/strict_census2.sh`).
3. Put both back and show green.

## 6 Gate before you report

`build` rc=0 · `unit` 2250/2250 · `isplit` 116/116 · **`gpu` 1294/1294** (new step, ID-109: the
monolith runtime arm, and how an ID-81 violation gets caught) · `gens` all rc=0 · `strict`: the
full per-entry marker census via `strict_census2.sh`. You succeed when
`GetBoundVertexArray@DrawArrays` is gone from that table and nothing new has appeared.

Pay particular attention to the multi-draw correctness lane: `MultiDrawScenario` asserts that a
batch matches unrolled draws, which is exactly the thing a wrong index buffer would break quietly.

## 7 Report

`~/w7/notes/p5e/reports/mv-v1.md`: the site inventory you actually found, the design and why, the
three red-onces verbatim, the before/after marker census, the gate table, and anything left undone
with the reason.
