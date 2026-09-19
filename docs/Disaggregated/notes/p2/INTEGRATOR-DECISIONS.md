# P2 integrator decisions (feat/disaggregated, 2026-09-07)

## ID-1 Ownership grant for the six destructor notifications (closes espryt review v4's major)
Package C (espryt) is GRANTED the four frontend destructor sites that brief C.5 gives to B and D:
`MG_State/GLState/{TextureState/TextureObject.{h,cpp}, FramebufferState/FramebufferObject.{h,cpp},
SamplerState/SamplerObject.{h,cpp}, VertexArrayState/VertexArrayObject.{h,cpp}}` - one include and one
out-of-line destructor calling `NotifyStateObjectDestroyed` each, nothing else. B (tracker) and D (magma)
were implemented on `p2/contract` before this grant and will be rebased across it by the integrator.
Reviewers: this is a deliberate, recorded waiver of C.5, not an undeclared deviation.

## ID-2 Integration order (replaces brief D.1)
contract -> spans -> espryt -> tracker -> magma -> gates.
Reason: espryt now carries the destructor edits in files B and D also touch (different regions); landing
espryt first makes B and D the sides that rebase, which the integrator resolves. G5 (the RenderStateImpl
sha) and G1 (pull-build symbols) are re-checked after every merge.

## ID-3 G1 admitted resize set
The four symbols the contract review measured: `RenderState::RenderState()`, `RenderState::SetCapability`,
`RenderState::IsCapabilityEnabled`, and `_GLOBAL__sub_I_DirectGLES.cpp` (the static initialiser of
`g_syncedRenderStateParameters`, -9 bytes). Anything else must be attributed per symbol.

## ID-4 Section-A command errata (apply when running the gates)
- ctest names: `grep -E "^[[:space:]]*Test[[:space:]]+#[0-9]+:" | sed -E "s/^ *Test +#[0-9]+: //" | LC_ALL=C sort`
  on both sides (the brief's `^\s+Test #` drops every id under 1000).
- `retrace_gate.py` has no `--ssim` / `--backend`; the SSIM threshold is the per-case one in `trace_cases.json`.
- The verify arming line is `MGPipe: verify armed - 63 fields, 69 verbs, fatal=1`; retrace case logs carry
  `MGPipe verify: <case> <backend> armed, zero divergences, zero unmigrated reads`.

## ID-5 Durable locations (the Windows scratchpad was wiped once already)
Brief: `~/w7/notes/p2/BRIEF-P2.md`; scouts: `~/w7/notes/p2/scout-*.md`; results and reviews:
`~/w7/notes/p2/p2-results/`; helper scripts: `~/w7/notes/tools/`. Write there (through a WSL script or
the `//wsl.localhost/Arch/home/swung/w7/...` UNC path), not only to the scratchpad. Delete your own
intermediate logs under `~/w7` when a round ends; keep only the logs your result file cites.
