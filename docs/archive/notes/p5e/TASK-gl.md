# TASK-gl — make the strict lane mean something, and make the flip survivable

Read `PACKAGE-PREAMBLE-P5E.md` and `PREAMBLE-ADDENDUM-WAVE3.md` first. Your tree is `~/w7/p5e-gl`,
branch `p5e/gl`, base `7c6f6886`. Your slug for the gate driver is `gl`.

You are not migrating a field family. You are fixing the **mechanism** that decides whether P5e is
finished, plus two defects that would break other backends the moment the phase's flags flip. Two
sibling packages are running concurrently (`pa` on the program family in `~/w7/p5e-pa`, `mv` on
multi-draw in `~/w7/p5e-mv`). Stay out of their trees and out of `MG_Backend/DirectGLES/DirectGLES.cpp`
and `MultiDraw.cpp` except where this brief names a line.

Do the items **in the order given**. Items 1 and 2 are correctness landmines and must land even if
you run out of room for the rest; commit each item separately.

---

## Item 1 — the barriered stamp must ask the server's own capability (ruling ID-111)

**This is a latent crash, not a cleanup.** `MGPipeRunAheadCapBitsFor` (`MG_Pipe/MGPipeTypes.h:160-166`)
never publishes `kCapRunAheadApply` on `BackendType::DirectVulkan`, because ruling ID-90 keeps Magma
in lockstep for all of P5e. But the server-side stamp in `PipeApplier::ApplyOne`
(`MG_Remote/Server/PipeApplier.cpp:1180-1182`) reads only the build constant:

```cpp
MG_Pipe::MGPipeApplierSetCurrentRecordBarriered(
    MG_Pipe::kMGPipeP5eClientWaitRuleLanded ? wireSaysBarriered : true);
```

`draw_vbo`, `blit`, `clear` and `launch_grid` are all `kWaitNone`. So the moment
`kMGPipeP5eClientWaitRuleLanded` flips, a **Magma** server stamps every draw record UNBARRIERED while
its client is still lockstep — and `CountBarrierPull` (`MG_Backend/MGPipe/PipeInputs.cpp:95-97`)
aborts unconditionally on an unbarriered pull, no knob involved. Magma dies on its first draw, and
`MagmaP7AllocatorDebtScope`'s exemption is withdrawn with it.

**Fix**: conjoin the server's own capability. `ServerPublishesRunAhead()` already exists at
`PipeApplier.cpp:40-44` and answers false for a session with no CallMask yet, which is the right
answer for the bring-up window.

```cpp
MG_Pipe::MGPipeApplierSetCurrentRecordBarriered(
    (MG_Pipe::kMGPipeP5eClientWaitRuleLanded && ServerPublishesRunAhead()) ? wireSaysBarriered : true);
```

Keep `wireSaysBarriered` computed unconditionally — that is ID-103's reason, the predicate must stay
exercised for the whole phase rather than first run on the day it starts deciding.

**Red-once**: a `RemoteClientTest` case that drives a `DrawVbo` through `ApplyOne` on a session whose
CallMask lacks bit 10 and asserts `MGPipeApplierCurrentRecordIsBarriered()` is true. Removing the new
conjunct makes it false. This is the server-side mirror of the client latch and it is what makes
ID-90 true in code rather than in prose.

## Item 2 — a compile-time trip wire on the residual fill (ruling ID-112)

`EmittedCallSuppliesTheWholeField` (`MG_Impl/Pipe/PipeFill.cpp:2499-2519`) is Magma's **only** source
for the seven pointer-backed fields, and Magma dereferences them immediately. Two of the seven rows,
`GetBoundVertexArray` and `GetBufferBindingPoint`, have no compile-time protection at all: a package
that "tidies up" by removing one would not break the build, it would SIGSEGV Magma on its first
draw on a device.

**Fix**: widen `NoP4aFamilyFieldIsWhollySupplied` (`PipeFill.cpp:2667-2682`) to cover all seven rows,
or add a second `static_assert` naming the two unprotected ones. About ten lines. This converts the
most likely mistake of every remaining package from a device crash into a build break, which is why
it is item 2 and not item 8.

**Red-once**: delete the `GetBoundVertexArray` row from the table and show the build fails with your
named assertion.

## Item 3 — the admission predicate, GENERATED (ruling ID-116)

BRIEF §3.3's "§7 allowlist" exists, in `MobileGL/MG_Remote/CONTRACT-P5E.md:475-486`. Package `ra`
also landed a **hand-kept 13-row copy** in `.github/workflows/test.yml:1207-1292`. That copy is wrong
in both directions and must be deleted, not corrected.

The rule, which is ID-84 expressed in tables that already exist: a `<field>@<verb>` pair is admitted
**iff** the field's row is `BARRIER_PULLED` **and** the verb's wire op is statically barriered
(`MGPipeWaitClassFor(op) != kWaitNone`), intersected with `kMGPipeClassFieldMask[kMGPipeVerbClass[verb]]`.

**Fix**: extend `scripts/gen_pipe_field_ownership.py` — it already joins `FieldOwnership.def`, already
emits `MG_Pipe/generated/PipeFieldOwnership.inc`, and already has `--check` / `--self-test` wired
into the `gens` gate step — to emit

```cpp
inline constexpr MGPipeFieldMask kMGPipeAdmittedPullMask[kMGPipeVerbCount] = { … };
constexpr Bool MGPipeBarrierPullAdmitted(MGPipeInputField f, MGPipeVerb v) {
    return MGPipeFieldMaskHas(kMGPipeAdmittedPullMask[SizeT(v)], f);
}
```

from `kMGPipeFieldOwnership`, `MGP_VERB_OP_LIST` (`FieldOwnership.def:255`) and `kMGPipeWaitClasses`
(`generated/PipeWire.inc:376`). Add `--print-admitted` so the shell reads the same table the C++ does.
Extend `--self-test` with negative controls in the style already there.

By this rule `GetFramebufferBindingSlot@ReadPixels` (21 entries, the largest survivor) is admitted,
and `GetTextureObject@CopyImageSubData` is not — see item 5.

## Item 4 — the strict knob becomes admission-aware (ruling ID-117)

Today `CountBarrierPull` aborts on **every** barrier-pulled read under `MOBILEGL_IPC_STRICT_ERRORS`,
admitted or not, so the CI's allowlist comparison is unreachable code and "hard green" is impossible
with the knob as written. Give it a third state, leaving the unbarriered arm exactly as it is:

```cpp
if (!MGPipeApplierCurrentRecordIsBarriered())
    StrictBarrierPullFatal(field, verb, "UNBARRIERED, the client did not fill it");   // unchanged
++g_residualPulls; …
if (MG_Config::Ipc.StrictErrors) {
    if (!MGPipeBarrierPullAdmitted(field, verb))
        StrictBarrierPullFatal(field, verb, "MOBILEGL_IPC_STRICT_ERRORS=1");          // unchanged
    AdmittedBarrierPullOnce(field, verb);   // MGLOG_W, deduped per (field, verb)
}
```

The admitted line keeps the marker grammar and changes only the leading tag:

```
MGPipe: Admitted{UnmigratedPipeInput, "<F>@<V>"} [BARRIER-PULLED, ADMITTED, retires in <phase>]
```

so every filter written since P5c that matches `Fatal{UnmigratedPipeInput` keeps meaning exactly
"red". **The dedupe is required**: an 852-draw frame must not write 852 lines.

Check, do not assume, that `FieldOwnershipTest.StrictErrorsTurnsABarrierPulledReadIntoANamedAbort`
and `StrictErrorsAlsoPromotesTheStickyForwards` still go red — both stamp `MGPipeVerb::DrawArrays`,
whose op is `kWaitNone`, so they should.

## Item 5 — `ResourceCopyRegion` becomes barriered (ruling ID-118)

`GetTextureObject` retires in **P7**, not P5e, but `ResourceCopyRegion` is `kWaitNone`
(`generated/PipeWire.inc:412`). After the flip that is a genuinely unbarriered record reading client
memory: an unconditional Fatal, no knob. The ruling is to **move the op to a barriered wait class**
rather than special-case the field into the allowlist — then item 3's derivation admits it with no
exception, and P7 can retire the field on its own schedule. `CopyImageSubData` is not on the
per-frame path (zero per frame in the Minecraft scene), so the wait costs nothing measurable. Say in
your report which wait class you chose and why.

## Item 6 — the lane reads every log, and the `rsp` pin is reworded (rulings ID-119, ID-115)

Three defects, all measured:

1. **The CI step greps `MobileGL/MG_IntegrationTest/split-logs/`**, which holds 90 of the lane's 116
   entries. The 26 it misses are exactly the `F1.` readback block, both `NamedBlit` pairs, the four
   `Ct.` entries and `PersistentMapArm` — i.e. precisely the readback population the allowlist is
   about. Drive the log set from ctest instead of from a directory: `split_log_paths.py:12-33`
   already parses `ctest --show-only=json-v1` into `{entry: absolute MOBILEGL_LOG_FILE_PATH}`. Add a
   `markers` mode that walks that map and classifies every `Fatal{` and `Admitted{` line, and make
   the lane a **two-sided ratchet**: an unadmitted marker fails, and an admitted pair that no longer
   appears must be removed from the expected set (so the lane cannot rot green).
2. **The `rsp` check greps for a counter no split lane emits.** And the pin itself is vacuous: the
   unbarriered arm of `CountBarrierPull` is `[[noreturn]]` and runs *before* `++g_residualPulls`, so
   `rsp` counts only barriered pulls and "`rsp` = 0 on unbarriered records" is true whatever the code
   does. Reword it to what is checkable: **`rsp` counts only admitted barriered pulls and must equal
   the admitted-marker tally**. Assert `rsp` against `draws` rather than against 0 — on the device
   `rsp` equalled the draw count under inproc and 0 under monolith, so the draw path's retirement is
   visible as `rsp` ceasing to scale with `draws`. `MG_IntegrationTest/Harness/PipeStatsWindow.h`
   already gives you `CounterOrAbsent(window, "rsp")` and `"draws"`; six scenarios already use it.
   You need one counting entry in the split lane with `MOBILEGL_PIPE_STATS=1`,
   `MOBILEGL_PIPE_STATS_PERIOD=1` and its **own private log path** — not a whole-lane env flip, which
   races under `-j` (`PipeStatsWindow.h:19-24` argues this).
3. **The lane has no positive control** (ruling ID-115). Of the 7 entries that pass strict today,
   three are `PushMonolithArm` (where `MGPipeServerStampVerbBoundary` never runs, so
   `CountBarrierPull` is structurally unreachable), two self-skip, one is a death test and one is a
   Python check. Not one is a record-carrying split GL scenario. So today's "green" is
   indistinguishable from "strict never armed". Add an assertion that at least one **drawing** inproc
   entry completed with a server-stamped verb boundary. Without it, 116/116 means nothing.

## Item 7 — partition the lane by backend, or admit Magma's row

Both `DirectVulkan.Split.NamedBlit.*` entries carry `LABELS "integration-split"`
(`MG_IntegrationTest/CMakeLists.txt:2270-2280`) and abort on `GetFramebufferBindingSlot@Clear`
(`VulkanRenderer.cpp:7648`), which P5e does not unbarrier — `GetFramebufferBindingSlot` retires in P7
on Magma. CONTRACT-P5E §7 already says in prose that Magma keeps the expected-red step. Give those
entries an `integration-split-magma` label with their own expected-red step, so the Espryt allowlist
stays a statement about Espryt. If you find a reason to prefer admitting the row instead, argue it.

## Item 8 — mark contract amendment 8 UNLANDED (ruling ID-120)

`CONTRACT-P5E.md:503-506` says `GetBufferBindingPointCount`'s sticky forward becomes FATAL under
split. `FieldOwnership.def:147` and `:179` both still say `BARRIER_PULLED`, and the generator refuses
a disagreeing field/forward pair (pinned by `FieldOwnershipTest.TheSevenStickyForwardsAgreeWithTheirFieldRows`).
The amendment is unlanded and landing it as written would change 10 pairs of the derived set. **Do
not land it.** Annotate it in the contract as UNLANDED with the phase that will land it (P7/P13), so
the divergence is stated rather than discovered.

---

## Gate before you report

`build` rc=0 · `unit` 2250/2250 (expect this number to move — say by how much and why) ·
`isplit` 116/116 · **`gpu` 1294/1294** · `gens` all rc=0, including your new generator arm and its
self-test · and the strict lane, which after your change should **complete every entry**. Report the
full marker table split into `Fatal{` (must be only what `pa` and `mv` still owe) and `Admitted{`.

**Expect the marker table to shift, not vanish.** The census is a first-fault histogram: entries that
abort today on `GetProgramForDraw@DrawArrays` will, once that row lands, advance to their next pull.
Do not read a changed table as a regression without checking which fault moved.

## Report

`~/w7/notes/p5e/reports/gl-v1.md`: per item, what you did, the red-once verbatim, and for any item
you did not reach, exactly where you stopped and why. Also state plainly whether, with your changes,
"the strict lane is hard green" is now a statement a script can evaluate — and if not, what is still
missing.
