# Engineering discipline and verification gates

> Reference. The design of the verification gates is in [`../design/07-memory-readback-verification.md`](../design/07-memory-readback-verification.md) §13.2; the per-phase gate results are in the phase notes (in git history).

## Every commit

- The default ALL target must build completely; instrumenting hot paths is not allowed in commits (a CI grep gate).
- **Every gate must be able to turn red for the reason it exists**: it has a negative control and has really run red once (R-16); what public GL cannot see is asserted white-box instead.
- Correctness gates are hard gates; **performance is recorded, not gated** (user, 2026-09-08), but the numbers must actually be measured. Dedicated optimization phases come after the roadmap; performance rounds after P13 use the `dev` build as the baseline (user, 2026-10-07).
- **Do not fix `dev` bugs along the way**: a defect found on the `dev` side becomes its own entry and its own PR.

## Every phase exit

- Run the five-part gate once: interface purity, semantic shadow comparison, behavioral A/B, performance record, coverage and handle discipline.
- Byte and name level equalities: **G5** protected regions are byte-identical; **G14** test names only grow. **G1** (the pull build's symbol set and `.text` identical to the baseline) and **G2** (the pull and push builds have the same set of test names) retired with the pull build in P13; their successors are: the zero-`MG_Remote`-symbol check on the shipped library, the skip census (newly appearing skips and disappearing names must be named in the baseline), the recorder golden (`MG_Test/Pipe/RecorderGoldenTest.cpp`: record streams from a fixed GL call sequence compared one by one; changing the golden requires saying why in the commit), and the link ratchet (the server half's references to the frontend only fall, and `# P13` annotations are not allowed).
- The performance criterion is **per-thread CPU time**; device comparisons use reboot-clean + same-thermal-window paired A/B, on the Redmi `2f7cbe2e` only (pin-verification record, in git history); a Windows machine is not a correctness gate.
- The close-out is reviewed once by a different model family; findings enter the first round of the next phase; implementers and reviewers use different model families; do not over-verify (user, 2026-09-16, ID-66).

## CTS

`gl44to46` has about 56,271 cases and is priced separately: each phase runs only the named blocks it can affect; the full caselist runs only at architecture boundaries and before merging into `dev`, in CI rather than on the critical path.
