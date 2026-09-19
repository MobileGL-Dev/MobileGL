# P3a package D — the gates (`gates-v1`)

Branch `p3a/gates`, worktree `/home/swung/w7/p3a-gates`, parent `39722687` (tag `p3a/contract`, package A's `c0`).
Three commits, the brief's messages verbatim, single line, no body. **Not pushed.**

| # | sha | message |
|---|---|---|
| 1 | `1ef91dec` | `[Test] (Pipe): reproduce the buffer handle ABA through public GL and prove the pre-rekey guards are what stops it` |
| 2 | `588d25e9` | `[Test] (Pipe): assert a regrown adopted store costs one map-persistent round trip per storage definition, not one per draw` |
| 3 | `80f6bf1b` | `[CI] (Pipe): gate that the buffer pool, the deferred-release drain and the three rings did not move` |

Files touched are C.5's package-D set and nothing else: `MobileGL/MG_IntegrationTest/**`,
`scripts/p3a_*.sh`, `.github/workflows/test.yml`. `tools/**`, `android-plugin/**` and
`MobileGL/MG_Benchmark/**` needed no change. `wsl_p3a_gate.sh` / `p3a_ab.sh` are the integrator's
(ID-3) and were not written.

---

## 1. Per-file summary

| file | commit | what changed |
|---|---|---|
| `MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp` | 1 | **+`ABufferAtARecycledAddressDoesNotInheritItsPredecessorsContents`** (the buffer ABA, all three arms) and **+`AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexBufferSet`** (the recycled VAO must not inherit a predecessor's `set_vertex_buffers`; see deviation D1 for why it is a case rather than a second window in the existing one). New helper `ThisBackendsResourceRekeyHasLanded()` + the fixture's `SkipUnlessTheResourceHandlePathIsAssertableHere()`; `MakePositionBuffer` / `MakeColorBuffer` / `ConfigureSplitQuadVao`. Header block extended with what P3a adds. Existing cases untouched. |
| `MG_IntegrationTest/CMakeLists.txt` | 1 | Two new build-time capability probes inside the existing `if (MOBILEGL_PIPE_PUSH)` block: `MGITEST_HANDLE_REKEY_RESOURCES_<backend>` (content probe for `MGPipeResourceOps` under each backend directory) and `MGITEST_PIPE_RESOURCE_EMITTER_PRESENT` (content probe for `MapPersistentRoundtrips` under `MG_Impl/Pipe`). `MGL_ITEST_HANDLES_ARM_KNOBS`'s pin **`MOBILEGL_PIPE_PUSH=0x7f` → `0x1ff`**, with the comment rewritten to name the phase constant rather than P2's. |
| `MG_IntegrationTest/CMakeLists.txt` | 2 | `ResourceSubsystemControlScenario.cpp` added to the sources; six new registrations (§3); `RESOURCE_LOCK` on the four pinned lanes that share one log path between several cases (deviation D5). |
| `MG_IntegrationTest/Harness/PipeStatsWindow.h` | 2 | **new**, header-only. Reads the library's last `MGPipe stats:` window out of the lane's private log and pulls one counter out of it by short name, with the separator included so `draws` cannot match `draws/f=`. The three counting cases share it instead of triplicating `CsoContentAddressingScenario`'s parser. |
| `MG_IntegrationTest/Scenarios/StorageBufferRegrowScenario.cpp` | 2 | **G10** — `+NStorageDefinitionsCostNMapPersistentRoundtripsNotOnePerDraw`: three definitions of an adopted (≥16 MiB) SSBO store with four dispatches against each must publish `mpr=3`, never 12 and never 0. `AGrownStoreIsVisibleThroughItsExistingIndexedBinding` is untouched. |
| `MG_IntegrationTest/Scenarios/LargeArenaAdoptionScenario.cpp` | 2 | **G10** — `+AnAdoptionCostsExactlyOneMapPersistentRoundtrip`: one storage definition plus five draws must publish `mpr=1`. The three existing cases are unchanged and now also run under both arms of the P3a A/B (§3). |
| `MG_IntegrationTest/Scenarios/ResourceSubsystemControlScenario.cpp` | 2 | **new, G12** — one case, two lanes: with bits 7\|8 set the window's `mpr` equals the number of storage definitions; with them cleared it must be **0**; the pixels are the same green in both. A dead switch reads non-zero in the Off lane. |
| `scripts/p3a_untouched_regions.sh` | 3 | **new, G5** — the nine named bodies out of `Managers.cpp` at two refs, sha256'd and diffed. |
| `scripts/p3a_vertex_input_negative_control.sh` | 3 | **new, G7** — drops the `IsBgra` copy from `MG_Impl/Pipe/VertexInputEmit.h`, rebuilds, requires `ctest -R 'VertexInputEmit\.'` to go red **naming the field**, restores and rebuilds. |
| `.github/workflows/test.yml` | 3 | `pipe-gates` gains `BASELINE: "44c2b5cf"`, `fetch-depth: 0` on its checkout, and two steps: the G5 comparison and its `--self-test`. The `integration-verify` job's always-on controls step gains `ResourceSubsystemControl` and `MapPersistentRoundtrips` beside `HandleRecycle|CsoContentAddressing`. `include-graph-check` untouched; the TEMPORARY `feat/disaggregated` trigger lines untouched. |

**34 ctest names added, none removed, pull and push name-identical** (2521 in both; the baseline at
`44c2b5cf` was 2482, the contract commit added 5).

---

## 2. What each scenario asserts, and how it skips on an unimplemented tree

Every skip is decided by the BUILD (a `file(GLOB ... CONFIGURE_DEPENDS)` content probe over the
directory the owning package owns), never by a hand-maintained list, and every skip names what is
missing. Nothing vanishes from `ctest -N` in any build (G2/G14).

| entry | on the finished tree | on this tree (contract only) |
|---|---|---|
| `HandleRecycle.*.ABufferAtARecycledAddressDoesNotInheritItsPredecessorsContents` | `.Handles`: the replacement buffer's own bytes are drawn although the GL name and the `{slot, gen}` were recycled inside one frame. `.Legacy`: the same, through today's `SharedPtr`/lifetime-id guards. `.AbaControl`: **the corruption is the assertion** — with the identity half of Magma's vertex-input key defeated the dead store's bytes come back. | `.Legacy` **passes**, `.AbaControl` **passes asserting the corruption**, `.Handles` **SKIPs**: *"subsystem not implemented on this tree: … no source under `MG_Backend/<backend>` naming `MGPipeResourceOps`"*. |
| `HandleRecycle.*.…VertexBufferSet` | The replacement VAO shares its predecessor's POSITION buffer and differs only in the COLOUR buffer, so an inherited `set_vertex_buffers` shows as its own geometry in the dead VAO's colour. `.AbaControl` asserts that inheritance. | Runs in all three arms already (the vertex-input key it rides is P2's): Handles/Legacy FRESH, AbaControl STALE. |
| `StorageBufferRegrow.…NotOnePerDraw` | `mpr == 3` for three definitions × four dispatches. | **SKIPs**: no `MG_Impl/Pipe` source names `MapPersistentRoundtrips`, so `mpr=` is structurally zero (package B). Also skips outside its lane, in a pull build, and with no private log path — each with its own message. |
| `LargeArenaAdoption.AnAdoptionCostsExactlyOneMapPersistentRoundtrip` | `mpr == 1` for one definition and five draws. | **SKIPs**, same probe. Skips in the two arm lanes too, saying they configure no private log. |
| `LargeArenaAdoption.{SubDataAfterAnInFlightDraw…, ReadbackSeesTheLatestCpuWrite, GpuWriteIntoTheArenaIsReadBack}` | run under `MOBILEGL_PIPE_PUSH=0x1ff` **and** `0x7f`: the handle path and the legacy `BufferBackendOps` path must agree about an adopted store's whole life. | **run and pass in both arms today** (both arms are the legacy path until C lands). |
| `ResourceSubsystemControl.{On,Off}` | On: `mpr == 2`. Off: `mpr == 0`. Both: the same green quad. | **SKIPs**, same probe, in both lanes. |

Verified skip texts (from `ctest -V`), one per family:

```
subsystem not implemented on this tree: the buffer's Handles arm needs this backend's resource_*
op table, and the build's capability probe found no source under MobileGL/MG_Backend/DirectGLES
naming MGPipeResourceOps. …  (P3a package C for DirectGLES; Magma's buffer path is P7)
subsystem not implemented on this tree: no source under MobileGL/MG_Impl/Pipe/ names
MapPersistentRoundtrips, so nothing emits map_persistent and mpr= is structurally zero. …
```

### 3. The new ctest lanes

| prefix | filter | mask / knobs | why |
|---|---|---|---|
| `DirectGLES.ResourceSubsystemControl.On.` | `ResourceSubsystemControlScenario.*` | `0x1ff`, stats, private log | G12's armed arm |
| `DirectGLES.ResourceSubsystemControl.Off.` | same | `0x7f`, stats, private log | G12's control arm |
| `DirectGLES.MapPersistentRoundtrips.` | `StorageBufferRegrowScenario.NStorage…` | `0x1ff`, stats, private log | G10, one case per lane |
| `DirectGLES.MapPersistentRoundtrips.` | `LargeArenaAdoptionScenario.AnAdoption…` | `0x1ff`, stats, private log | G10, one case per lane |
| `DirectGLES.ResourceSubsystemOn.` | `LargeArenaAdoptionScenario.*` | `0x1ff` | the three behavioural cases, bits set |
| `DirectGLES.ResourceSubsystemOff.` | `LargeArenaAdoptionScenario.*` | `0x7f` | the same three, bits cleared |

Every environment list is joined through `mgl_itest_join_environment(... ${MGL_ITEST_COMMON_ENV})`
so the EGL-vendor pinning survives (a ctest `ENVIRONMENT` property REPLACES the job environment);
every log-reading entry names ONE case and owns its log path. DirectGLES only — Magma's buffer path
is P7 and registers no ops, so a DirectVulkan arm would measure the client emitter against a
backend nobody asked to change.

---

## 4. The two scripts

### 4.1 `scripts/p3a_untouched_regions.sh` (G5)

```
p3a_untouched_regions.sh <ref-a> <ref-b>   compare the nine bodies at two refs
p3a_untouched_regions.sh <ref>             list the nine shas (D.0's baseline capture is a redirect)
p3a_untouched_regions.sh --self-test       prove the comparison can go red
```

stdout is always the `<sha256>  <function>` list in the fixed order; diagnostics go to stderr.
Exit **0** identical · **1** at least one moved, the first named on stderr · **2** could not run
(bad ref, missing file, a name not defined exactly once, a self-test control that did not answer).

Extraction masks comments and string/char/**raw**-string literals to spaces first (a shader source
in this file is full of braces), then takes the ONE occurrence of `<name> (` whose closing
parenthesis is followed — past `const`/`noexcept` — by `{`; that is what separates the definition
from the forward declarations and from the call sites. Zero or two matches is exit 2, never a
silent pass. The body is hashed from the ORIGINAL text, so a comment change inside one of the nine
counts as a move: "byte-identical" is the claim.

Self-test output on this tree (rc 0):

```
[p3a-untouched] positive control: 9 bodies extracted from the working tree
[p3a-untouched] positive control: an untouched copy compares equal
[p3a-untouched] positive control: an edit outside the nine bodies is invisible
[p3a-untouched] negative control: a perturbed ClearBufferPool body is reported, and named
[p3a-untouched] self-test passed
```

The third control is the one P3a needs: the rest of `Managers.cpp` is going to be rewritten, so a
gate that fired on any edit to the file would be switched off in the week it landed.

The gate itself, `44c2b5cf` → `HEAD`, rc 0, with the nine shas (this is also the D.0 baseline):

```
ba79b92dc6ebf2cb2af6aa79f23ba1941818d131812965e9f96a89caffc8e90a  IsPoolable
6563286e007650a5abb0a1bbdd7acddd901776f3e55d3c0087c8fee60aa6cc4c  EnrollIntoPool
dc9d3066b2429208e18adc27c639d16260e74b7f17f7c3f290d54f58534f089b  AcquireFromPool
12656cfd716dec50a4a4893459a7a6aaa0a635f79dda8fa51b88a819da27e5e0  TrimBufferPool
c44a12746bcd82e55d8a635adf766e1fe2cf961ded5d9d745896fd65535bde16  ClearBufferPool
15259e547fb514dcdcf087bbde1d048fc2d9e7901ea6c6c94a1109fb7987073a  ProcessDeferredBufferReleases
2daaa5254c38db63742ef8d6dacd25f8868c7fcd3d9745956862134cf76911d5  CreateRingStorage
6221df2e4c0b185fbac061e956253dacd8f8d886bb3f3341cd76a26b5e1f84b1  RingAvailable
709b6f4190c735517034b9c5d62967a9ae61f2198d2eb441dfe4e7af437cb784  RingAllocate
```

A nonexistent ref exits 2 (verified) rather than reporting a difference.

### 4.2 `scripts/p3a_vertex_input_negative_control.sh` (G7)

`p3a_vertex_input_negative_control.sh <build-dir>`. Exit **0** tripped and named `IsBgra` ·
**1** did not answer (green on the dropped field, or red without naming it — both leave the tree
restored AND rebuilt) · **2** could not run.

The break is chosen to be invisible to the compiler: the record keeps the member, keeps its 24-byte
`static_assert`, keeps its `PipeFields.def` row and its generated comparator — only the VALUE stops
being copied, and `Size` stays 4 for `GL_BGRA` (D-G2) so nothing else moves either. It is applied by
regex (`.IsBgra = <expr>[;,]` → `0`) because `VertexInputEmit.h` is package B's file and its
spelling is B's to choose.

**On this tree it returns 2, and that is the honest answer**: `MG_Impl/Pipe/VertexInputEmit.h` does
not exist yet.

```
[p3a-g7] MobileGL/MG_Impl/Pipe/VertexInputEmit.h does not exist on this tree.
[p3a-g7] It is P3a package B's file (BRIEF-P3A.md C.5) … Until it lands there is no field copy to
[p3a-g7] drop, so this control cannot run and MUST NOT report a pass.
rc=2
```

Three exit paths were exercised: the missing header (above); a header present that assigns no
`.IsBgra` (a temporary probe file, removed straight after — also 2, with the message that this
would mean G6 is already broken); and the patcher itself, on a scratch copy carrying both
spellings, which neutralised 2 of 2 assignments:

```
    wire.IsBgra = 0 /* G7 NEGATIVE CONTROL: was attrib.IsBgra ? 1 : 0 */;
        .IsBgra = 0 /* G7 NEGATIVE CONTROL: was static_cast<Uint8>(a.IsBgra) */,
```

The "suite must exist and be green first", "must still compile", "must name the field" and
"restore then rebuild then re-run" steps are `g7_negative_control.sh`'s, kept verbatim in shape.

---

## 5. D.2 — the "red before" record

`cmake --build build-verify` then
`ctest --test-dir build-verify -R HandleRecycle --no-tests=error --output-on-failure`, on the
contract-based tree, rc **0**, 60 entries. Kept at `~/w7/p3a-handlerecycle-before.log` and copied to
`~/w7/notes/p3a/p3a-results/handlerecycle-before.log`.

```
28/60 Test #2479: DirectGLES.HandleRecycle.Legacy…ABufferAtARecycledAddress…Contents ......   Passed
34/60 Test #2485: DirectVulkan.HandleRecycle.Legacy…ABufferAtARecycledAddress…Contents ....   Passed
40/60 Test #2491: DirectVulkan.HandleRecycle.AbaControl…ABufferAtARecycledAddress…Contents    Passed
46/60 Test #2497: DirectVulkan.HandleRecycle.AbaControlHandles…ABufferAtARecycledAddress…      Passed
        2467 - DirectGLES.HandleRecycle.Handles…ABufferAtARecycledAddress…Contents (Skipped)
        2473 - DirectVulkan.HandleRecycle.Handles…ABufferAtARecycledAddress…Contents (Skipped)
```

The per-arm verdict line is printed on EVERY arm whether or not the case passes, so the AbaControl
green is readable as "the corruption is still reproducible" rather than inferred from an exit code
(`ctest -V`):

```
[ HandleRecycle ] arm=AbaControl expected=STALE observed=STALE - the draw after the buffer was recycled inside one frame        (x2 lanes)
[ HandleRecycle ] arm=AbaControl expected=STALE observed=STALE - the draw after a VAO reading a two-buffer vertex set …         (x2 lanes)
[ HandleRecycle ] arm=Legacy     expected=FRESH observed=FRESH - the draw after the buffer was recycled inside one frame        (x2 lanes)
[ HandleRecycle ] arm=Handles    expected=FRESH observed=FRESH - the draw after a VAO reading a two-buffer vertex set …         (x2 lanes)
```

That is the artefact `ROADMAP.md`'s "重键前红" asks for, for the buffer kind: **the corruption is
reproducible today with the identity guards defeated, and the pre-re-key guards are what stop it.**

---

## 6. Deviations, each with its reason

**D1 — the vertex-buffer-set window is a NEW case, not a second window inside the existing VAO
case.** C.3 says "the VAO case extended to the vertex-input path". Written as a second phase it
turned the AbaControl arms red for a reason that is not about identity, and the measurement is in
the file: gtest_discover_tests gives each case its own PROCESS, the ABA knob collapses every VAO in
a process onto one memo entry, and that entry carries the resolved vertex-input LAYOUT as well as
the bindings. The split VAOs deliberately do not share the first window's layout (interleaved
stride 20 vs two tight arrays), so in one process the AbaControl arm read the split VAOs' vertices
through the interleaved layout and every draw in the phase — priming and warm-ups included — came
back as garbage (`"should be all green … first offender is blue"`). In its own process, where every
VAO carries the same layout and only the buffer identity differs, the window asserts exactly what it
claims. A name was added, none removed (G14).

**D2 — the buffer case's `.Handles` arm reads a NEW marker, not P2's.** `MGITEST_HANDLE_REKEY_<backend>`
answers "is this backend's VERTEX-INPUT memo keyed on {slot, gen}", which P2 landed and which is set
on this tree; a buffer only travels as a handle once `resource_*` does. Reading P2's marker would
have armed the buffer arm on a tree where nothing about a buffer is keyed on a handle — a green
asserting nothing. The new probe looks for `MGPipeResourceOps` under each backend's own directory
(content, not filename, so package C keeps its file layout).

**D3 — the buffer case's AbaControl expectation is a measurement.** It was written expecting FRESH
(the guards the knob leaves standing include VkBufferManager's slice epoch, which MOVES when the
replacement buffer is created inside the window by construction) and the tree answered STALE over
100% of the viewport on both AbaControl lanes. The expectation follows the measurement, and the
reasoning is recorded at the assertion.

**D4 — `MGL_ITEST_HANDLES_ARM_KNOBS` moves `0x7f` → `0x1ff`.** As instructed. The comment now names
the PHASE constant (`kMGPipeSubsystemsMigratedAtP3a`) rather than P2's, and says why: pinned at
`0x7f` after P3a, the Handles arm would assert the P2 shape while the handles it is about stayed
switched off. `0x7f` survives as `ResourceSubsystemControl`'s Off lane.

**D5 — [finding, fixed here] four pre-existing lanes needed a `RESOURCE_LOCK`.** Three runs of the
full `integration-gpu` label at `-j 8` produced 4, 0 and 2 failures, always one of the
`…IsActuallyArmedWhenTheEnvironmentPinsItOn` cases, never the same set twice; each passes alone. The
mechanism is the one this file already documents: those lanes give a WHOLE scenario one
`MOBILEGL_LOG_FILE_PATH`, the library opens it `fopen(path, "w")`, and a sibling case truncates it
while the arming case reads. P2's gate run was green (916 entries); this package's +34 entries
changed the scheduling odds. Re-filtering — the rule the file states — would RENAME the arming
entries, which G14 forbids, so the fix is a ctest `RESOURCE_LOCK` named after each log on
`UnlocatedIoBlocks`, `PrimGenReroute` and the two `PointSizeDemotion` lanes. Three consecutive
full-label runs afterwards: 950/950, 950/950, 950/950, with the name list unchanged.

**D6 — [finding, NOT fixed] `dev@d7655247` is not in `feat/disaggregated`, and it bites here.**
"rebind VAOs when an adopted buffer is respecified — the immediate retire path forgot the buffer-id
generation, so cached vertex and element bindings kept the deleted store" is on `dev` and is **not**
an ancestor of this branch. Re-specifying an adopted (≥16 MiB) store that a VAO's attributes still
read is a hard SIGSEGV inside the vertex fetch on the first draw after it (reproduced twice on
llvmpipe, backtrace in JIT code). Both counting workloads were rewritten to define a SECOND arena
instead of re-specifying the first — the same number of STORAGE DEFINITIONS, which is what
`ARCHITECTURE.md:474` prices — with the reason written at both sites. `ROADMAP.md:88` says unrelated
fixes do not ride the split work, so **this is recorded for the integrator, not fixed**: it is a
`dev` crash that the branch will inherit at the next merge, and a P3a scenario that carried it would
be red for a reason that is not P3a's.

**D7 — `pipe-gates` gains `fetch-depth: 0` and a `BASELINE` env, and the two G5 steps are branch
scoped.** The script reads `Managers.cpp` at `BASELINE` with `git show`, which a depth-1 checkout
does not have. `BASELINE` is `44c2b5cf` (ID-1) and deliberately NOT the workflow's `baseline_sha`
input, which is the SYMBOL baseline (`087685d1`) and is empty on a push. The two steps carry
`if: github.ref == 'refs/heads/feat/disaggregated' || github.event_name == 'workflow_dispatch'`,
because on `dev` — where unrelated buffer fixes land on their own schedule — the comparison asks a
question nobody posed. They belong with the TEMPORARY trigger lines and retire with them.

**D8 — the G5 script also accepts ONE ref.** D.0's baseline capture is
`p3a_untouched_regions.sh $BASE $BASE > ~/w7/p3a-before-untouched.sha`; a single-ref listing mode
makes that a plain redirect and is what the two-ref mode prints anyway.

**D9 — a new harness header.** `MG_IntegrationTest/Harness/PipeStatsWindow.h` (package D's tree)
holds the summary-line reader the three counting cases share.
`CsoContentAddressingScenario`'s own copy was left alone: churn in a file this package has no other
reason to touch.

**D10 — script file modes are 644**, matching `scripts/g7_negative_control.sh`; both are invoked as
`bash scripts/...`.

**D11 — `MG_Benchmark`, `tools/**`, `android-plugin/**` unchanged.** C.3 lists them as D's to own,
not as work: nothing in P3a's gate needs a benchmark or a device-side change.
`cmake --build build-bench` from C.3's verification block was not run — there is no `build-bench`
directory in this worktree (the tree script builds `build-linux`, `build-push`, `build-verify`) and
no benchmark source changed.

---

## 7. Verification transcript

All commands in `/home/swung/w7/p3a-gates`, `CCACHE_BASEDIR=/home/swung/w7`, at `80f6bf1b`.

| check | result |
|---|---|
| `cmake --build {build-linux,build-push,build-verify} -j 28` | rc 0, rc 0, rc 0 |
| **G1** `symbol_report.py --before ~/w7/p3a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | rc 0 — `27799 -> 27799 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed`, total `17209979 -> 17209979 (+0)`. **0/0/0/0**, as expected: none of this package's files reaches the library. |
| **G2 names** `ctest -N` (ID-4 extractor) pull vs push | identical, 2521 each |
| **G14** `comm -23 ~/w7/p3a-before-ctest-names.txt <build-linux names>` | empty — nothing removed; +39 over the 2482 baseline (5 are the contract's, 34 are this package's) |
| `ctest -L integration-gpu --no-tests=error -j 8` on **build-push** | rc 0 — 950/950 (three consecutive runs after the D5 fix: 950, 950, 950) |
| `ctest -L integration-gpu --no-tests=error -j 8` on **build-linux** | rc 0 — 950/950 |
| `ctest -L integration-verify --no-tests=error -j 4` on build-verify | rc 0 — 838/838 |
| **hard rule 4** `ctest --test-dir build-verify -R HandleRecycle --no-tests=error` | rc 0 — 60 entries, only visible skips |
| **hard rule 4** `ctest --test-dir build-push -R 'StorageBufferRegrow\|LargeArenaAdoption\|ResourceSubsystemControl' --no-tests=error` | rc 0 — 26 entries, only visible skips |
| `ctest -L unit -j 12 --no-tests=error` on all three builds | rc 0 — 1571/1571 each |
| **G5** `bash scripts/p3a_untouched_regions.sh 44c2b5cf HEAD` | rc 0 |
| **G5** `bash scripts/p3a_untouched_regions.sh --self-test` | rc 0 (three controls, above) |
| **G7** `bash scripts/p3a_vertex_input_negative_control.sh build-push` | rc **2** — package B's header is absent; the message says so |
| `test.yml` parses; `pipe-gates` carries the two new steps with their `if:`, `env.BASELINE`, `fetch-depth: 0`; the controls step's `-R` carries the four families | verified by parsing the workflow |
| `git status` | clean; three commits; nothing pushed |

**Plumbing evidence for the three counting cases** (they skip on this tree, so the workload, the
window parser and the pixel assertion were exercised with `MGITEST_PIPE_RESOURCE_EMITTER_PRESENT=1`
forced by hand, outside ctest): in every one the summary line was found and parsed, the draws
landed, and the ONLY failure was the count itself reading 0 — `ResourceSubsystemControl`'s **Off**
lane passed outright, which is the arm whose expectation is `mpr == 0`.

---

## 8. What the integrator must re-run once B, C and the rest have landed

1. `bash scripts/p3a_untouched_regions.sh 44c2b5cf HEAD` **after every merge** (D.1) — this is the
   gate that catches a pool or ring body that moved while `Managers.cpp` was being rewritten, and
   it is the one check in the five-part gate that no test can stand in for. Plus `--self-test`.
2. `bash scripts/p3a_vertex_input_negative_control.sh build-push` on the tree that carries package
   B: it must report **"negative control tripped, naming IsBgra"** (rc 0). Anything else is a
   finding about G6, and rc 2 now means something different from what it means here.
3. `ctest --test-dir build-verify -R HandleRecycle --no-tests=error --output-on-failure` — the
   buffer arm's `.Handles` entries must **stop skipping** and pass once package C registers
   `MGPipeResourceOps`. If they still skip, the probe's answer is a real statement: no backend
   source names the op table.
4. `ctest --test-dir build-push -R 'StorageBufferRegrow|LargeArenaAdoption|ResourceSubsystemControl'`
   — the four counting entries must **stop skipping** once package B emits
   `MapPersistentRoundtrips`, and then: `mpr == 3`, `mpr == 1`, `mpr == 2` (On) and `mpr == 0`
   (Off). A non-zero Off is the dead-switch reading and blocks the phase's G12 claim.
5. `MOBILEGL_PIPE_PUSH=0x7f ctest --test-dir build-push -L integration-gpu -j 8` beside the default
   `0x1ff` (D.3's third arm) — the two `ResourceSubsystemOn/Off.LargeArenaAdoption.` lanes already
   cover the adopted store's own life in both arms, but the label-wide A/B is still the integrator's.
6. The full `integration-gpu` label on **both** builds, name-diffed first (G2), and
   `integration-verify` on the verify build.
7. `gh workflow run test.yml --ref feat/disaggregated` — `pipe-gates` now runs the G5 rows; they are
   scoped to that branch and to `workflow_dispatch`, so a dev-branch run will not execute them.
8. Carry **D6** (`dev@d7655247` absent from this branch: respecifying an adopted store under a live
   VAO is a SIGSEGV) into the phase's findings. Two P3a scenarios route around it; the next
   `dev` → `feat/disaggregated` merge is what actually fixes it.

Intermediate logs this package created under `~/w7/p3a-gates-*` were deleted. Kept:
`~/w7/p3a-handlerecycle-before.log` (D.2's artefact, for `MEASUREMENTS.md`) and its copy at
`~/w7/notes/p3a/p3a-results/handlerecycle-before.log`.
