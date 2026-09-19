# Wave 3 census — the strict lane at `3c13fa4b` (pa + mv + ID-124 merged)

Harness: `strict_census3.py`, which takes each entry's OWN command and environment from
`ctest --show-only=json-v1` and layers only `MOBILEGL_IPC_STRICT_ERRORS=1` and a private
`MOBILEGL_LOG_FILE_PATH` on top, last. The two earlier harnesses each produced a table that looked
complete and was not: v1 was defeated by lane-pinned log paths, v2 by re-running 14 of 109 entries
under an environment their lane does not use.

## The table

180 lane entries. 24 green, 156 red.

| marker | entries | admitted by | owner |
|---|---|---|---|
| `GetFramebufferBindingSlot@ReadPixels` | 113 | ID-116 disjunct 1 (ReadPixels is statically barriered) | trails to P8/P9 |
| `GetBufferBindingSlot@DrawArrays` | 18 | ID-125 disjunct 2 (retires P8 / P9 / P13) | P8 |
| `GetTransformFeedbackProgram@DrawArrays` | 11 | ID-125 disjunct 2, and ID-128 disjunct 3 | P3b/P4b |
| `GetTextureObject@CopyImageSubData` | 6 | ID-125 disjunct 2; ID-118 also makes disjunct 1 hold | P7 |
| `ValidateProgramName@ShaderStorageBlockBinding` | 2 | ID-125 disjunct 2 (retires P9) | P9 |
| `GetBoundVertexArray@DrawArrays` | 2 | **ID-128 disjunct 3 only** | P8 (client arrays) |
| `GetTextureUnitObject@CopyTexImage2D` | 2 | ID-116 disjunct 1 | trails |
| `GetFramebufferBindingSlot@Clear` (DirectVulkan) | 2 | ID-125 disjunct 2 on the Magma side | P7 |

**`GetProgramForDraw@DrawArrays` and `GetProgramForDispatch@DispatchCompute` are gone.** They were
62 and 7 at the wave's base and they were the per-draw pull that kept every draw barriered.

## Two rows that the wave created, and both were predicted

- **`GetBufferBindingSlot@DrawArrays`, 18 entries, all in `MultiDrawTierIndirect` and
  `MultiDrawTierMultiIndirect`.** Package `mv` NAMED this in its report as a latent row before it
  could fire: `BoundDrawIndirectBufferId`'s `GetBufferBindingSlot(DrawIndirect)`, which "would
  surface as `GetBufferBindingSlot@DrawArrays` if an indirect tier were ever forced under strict".
  It then gated the indirect tiers, which forced them. The field retires in P8 (`FieldOwnership.def:78`),
  so it is admitted, not owed. A latent row named and then made visible in the same package is the
  outcome you want.
- **`GetFramebufferBindingSlot@ReadPixels` grew from 19 to 113.** Unmasking: entries that used to
  die on the program row now get as far as their readback.

## The rule is now complete against this table

Three disjuncts, and every row above is covered:

1. the verb's wire op is statically barriered (ID-116);
2. the field's retiring phase does not name P5e (ID-125);
3. the record was barriered by ESCALATION rather than statically — `wireSaysBarriered && waitClass == kWaitNone` (ID-128).

Disjunct 3 exists because of exactly two entries, both `ClientVertexArrayScenario.*`, and it is the
principled one rather than a patch: escalation happens only for XFB-active and client-array draws,
and the contract puts both outside this phase.

**Hard green is therefore a checkable number: 180 of 180, eight rows reporting `Admitted{`, zero
reporting `Fatal{`.** Not an argument.

## What is NOT in this table

The census measures first faults under the strict knob. It says nothing about whether run-ahead is
correct, only about whether any record still reads client memory in a way P5e owes. The negative
controls, the red-once re-runs and the device exit are separate gates (`FLIP-CHECKLIST.md` phases C
to E), and BRIEF §3 item 5's evidence form had to be re-sequenced because every candidate was red
before the revert (ID-121).
