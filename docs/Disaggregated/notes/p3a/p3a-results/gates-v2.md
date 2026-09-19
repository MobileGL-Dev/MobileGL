# P3a package D — the gates, rework round 1 (`gates-v2`)

Branch `p3a/gates`, worktree `/home/swung/w7/p3a-gates`, **rebased onto `refs/heads/feat/disaggregated`
(`5cb826b0`)** per ID-9. HEAD **`2bafb316`**. Not pushed.

Answering `gates-review-v1.md`'s REWORK (four majors) plus the integrator's ID-9 / ID-10 / **ID-11**.
Everything in the review's §5 rework list is addressed; the optional minors (m5, m6) are done too.

| # | sha | message |
|---|---|---|
| — | `5cb826b0` | (base: contract `e01c0ccc` + the `dev@9eae9858` merge) |
| 1 | `9e08c563` | `[Test] (Pipe): reproduce the buffer handle ABA through public GL and prove the pre-rekey guards are what stops it` |
| 2 | `ecbddcea` | `[Test] (Pipe): assert a regrown adopted store costs one map-persistent round trip per storage definition, not one per draw` |
| 3 | `20dfb64c` | `[CI] (Pipe): gate that the buffer pool, the deferred-release drain and the three rings did not move` |
| 4 | `331bb425` | `[Test] (Pipe): count the map-persistent round trips over a re-specified adopted arena in both arms, now that the VAO rebind fix is in the branch` |
| 5 | `2dfac81a` | `[Fix] (Pipe): raise the four CSO stats lanes to the P3a subsystem mask so they stop asserting on a configuration nothing ships` |
| 6 | `854b1be3` | `[CI] (Pipe): select the P3a subsystem and map-persistent lanes in the only job that unpacks a push build` |
| 7 | `a9d4011a` | `[Fix] (Pipe): restore and rebuild on every exit path of the vertex-input negative control, and keep its logs out of the repository` |
| 8 | `2bafb316` | `[CI] (Pipe): add FlushPendingRangesNow's three-tier drain to G5's byte-identical set` |

Commits 1–3 are the original three, rebased (the duplicate contract patch dropped out automatically).
Commits 4–8 are the rework. Diffstat `20dfb64c..HEAD`: 6 files, +294 / −156.

---

## 1. The rebase (ID-9)

`git rebase refs/heads/feat/disaggregated` — **rc 0, no conflict**. Git merged
`LargeArenaAdoptionScenario.cpp` cleanly: `dev`'s two cases were appended in one hunk and gates' case in
another, far enough apart that the union is what git produced. It was verified by name rather than
assumed, because ID-9 names this file as where a name can be lost:

| side | `TEST_F(LargeArenaAdoptionScenario, …)` |
|---|---|
| gates pre-rebase `80f6bf1b` | `SubDataAfterAnInFlightDrawReachesTheNextDraw`, `ReadbackSeesTheLatestCpuWrite`, `GpuWriteIntoTheArenaIsReadBack`, `AnAdoptionCostsExactlyOneMapPersistentRoundtrip` (4) |
| dev side `5cb826b0` | `SubDataAfterAnInFlightDrawReachesTheNextDraw`, **`RespecifiedVertexArenaKeepsVaoBindings`**, **`RespecifiedIndexArenaKeepsVaoBinding`**, `ReadbackSeesTheLatestCpuWrite`, `GpuWriteIntoTheArenaIsReadBack` (5) |
| **HEAD** | all **6** — the union, nothing renamed, nothing dropped |

The 11 `PersistentBufferOrderingProbeTest.*` names from `9eae9858` are in a file this package does not
touch and arrived untouched. G14 below confirms both sets at the ctest level.

**`feat/disaggregated` has moved on since this rebase.** It was `5cb826b0` when this package rebased and
when every number below was taken; by the end of this round `~/w7/pipe` reads **`12e6bfcf`** (package A's
`wire` landed; `p3a/client` has also moved to `b6706362`). Under ID-2 gates integrates **last**, so one
more rebase is expected and is the integrator's; nothing in this package touches `MG_Pipe/**`,
`MG_Impl/**` or `MG_Backend/**`, so the only file with any chance of conflicting again is
`MG_IntegrationTest/CMakeLists.txt`. G1 and G14 must be re-measured against whatever pull baseline the
integrator holds at that point — the `~/w7/p3a-before-*` captures used here are `5cb826b0`'s.

---

## 2. Per-finding disposition

### M1 — the four CSO stats lanes still pinned `0x7f` — **FIXED** (`2dfac81a`)

`MG_IntegrationTest/CMakeLists.txt`, at the review's line numbers, now at HEAD's:

| lane | was | now | line at HEAD |
|---|---|---|---|
| `MGL_ITEST_GLES_CSO_ON_ENVIRONMENT` (`:1044`) | `0x7f` | `0x1ff` | `:1046` |
| `MGL_ITEST_GLES_CSO_OFF_ENVIRONMENT` | `0x800000000000007f` | `0x80000000000001ff` | `:1051` |
| `MGL_ITEST_VULKAN_CSO_ON_ENVIRONMENT` | `0x7f` | `0x1ff` | `:1056` |
| `MGL_ITEST_VULKAN_CSO_OFF_ENVIRONMENT` | `0x800000000000007f` | `0x80000000000001ff` | `:1061` |

The control semantics are unchanged: the only bit that separates the two arms is **63**
(`kMGPipeBehaviourNoCsoContentAddressing`, `MG_Pipe/MGPipe.h:90`); every other bit is the shipping mask
and now moves with the phase (`kMGPipeSubsystemsMigratedAtP3a = 0x1ff`, `MGPipe.h:95`). A comment block at
`:1031-1043` states that rule at the site, so the next phase's author does not have to rediscover it, and
closes contract-review item 11 explicitly for all three lane families.

**Re-run (M1's own requirement).** `ctest --test-dir build-push -R CsoContentAddressing` → **rc 0, 6
entries** (the 4 lanes pass; the 2 ambient entries skip by design). The arms are still distinguishable at
the new mask — the lane logs read `csom=3 csob=17` (content-addressed) against `csom=17 csob=17`
(no content addressing), i.e. 3 mints for 17 binds versus one mint per bind. The generated ctest
environment was checked directly: the CSO On lanes now carry `MOBILEGL_PIPE_PUSH=0x1ff` and no
`0x800000000000007f` string survives anywhere in the build tree.

**Every remaining pin in the file re-read** (`grep -n 'MOBILEGL_PIPE_PUSH=0' MG_IntegrationTest/CMakeLists.txt`),
which is the second half of the instruction. Thirteen value-carrying pins remain:

| line | value | verdict |
|---|---|---|
| `:930` `MGL_ITEST_HANDLES_ARM_KNOBS` | `0x1ff` | phase default |
| `:944`, `:947` `HandleRecycle.Legacy` (both backends) | `0` | **named control** — the pre-handle arm, documented at `:895-900` |
| `:950` `HandleRecycle.AbaControl` | `0` | **named control** — D18's lane verbatim |
| `:1046`, `:1056` CSO **On** | `0x1ff` | fixed here |
| `:1051`, `:1061` CSO **Off** | `0x80000000000001ff` | fixed here; bit 63 is the control |
| `:1130` `ResourceSubsystemControl.On` | `0x1ff` | phase default |
| `:1135` `ResourceSubsystemControl.Off` | `0x7f` | **named control** — G12's armed/cleared A/B |
| `:1164`, `:1169` `MapPersistentRoundtrips.` ×2 | `0x1ff` | phase default |
| `:1200` `ResourceSubsystemOn.` | `0x1ff` | phase default |
| `:1203` `ResourceSubsystemOff.` | `0x7f` | **named control** |

`HandleRecycle.AbaControlHandles` (`:953-956`) pins nothing of its own — it composes
`MGL_ITEST_HANDLES_ARM_KNOBS`, so it followed `0x1ff` automatically. **No lane runs with P3a's subsystems
off unless it is a named control.**

### M2 — the eight `ResourceSubsystemOn./Off.` entries never ran in a push build in CI — **FIXED** (`854b1be3`)

`.github/workflows/test.yml:615`, the `integration-verify` job's controls step:

```
-R 'HandleRecycle|CsoContentAddressing|ResourceSubsystem|MapPersistentRoundtrip'
```

`ResourceSubsystemControl` → `ResourceSubsystem` and the plural `MapPersistentRoundtrips` → singular, for
exactly the reasons the review gives. A 14-line comment above the step records that these alternatives are
test-name PREFIXES, that each is the shortest string that still selects only what it means to, and that
this is the only CI job unpacking a push build — so an entry the filter misses either never runs under the
P3a bits or runs only in the pull `integration` job where the mask steers nothing.

**Measured, in the job's own build directory (`build-verify`, `-L integration-gpu -R …`):**

| filter | entries selected |
|---|---|
| old (as reviewed) | **80** |
| new | **96** |
| of which P3a lane entries (`ResourceSubsystem{On,Off,Control}.` / `MapPersistentRoundtrips.`) | **16** |

The step run for real: **rc 0, 96/96 pass**. Of the 12 `ResourceSubsystemOn./Off.` entries, **10 assert and
2 skip** — the two skips are `AnAdoptionCostsExactlyOneMapPersistentRoundtrip` in each lane, which needs
package B's counter and says so; the other five cases per lane (including `dev`'s two
`Respecified*Arena*`) run on both arms today. That is review item 9 ("confirm the lanes actually assert")
discharged, at least for the pre-B tree.

**Every other new lane checked the same way.** All 38 names D adds are present in `ctest -N` on
`build-linux`, `build-push` and `build-verify`; the 22 that carry `ResourceSubsystem` or
`MapPersistentRoundtrip` in their names (16 lane entries + 6 ambient) are all inside the new filter. The
`HandleRecycle` families were already reachable and remain so.

### M3 — the G7 script's exit contract — **FIXED** (`a9d4011a`)

`scripts/p3a_vertex_input_negative_control.sh` restructured:

- **A single `repair()`** (`:103-123`): restore the header from the pre-patch byte copy, `cmake --build`,
  then re-run the suite to prove the tree went back. Idempotent, a no-op until the patch is applied.
- `PATCH_APPLIED=1` is set **before** the patcher runs (`:178`), not after, so a python that died mid-write
  is repaired too. `trap 'repair' EXIT` (`:174`) replaces `trap 'restore' EXIT`, so **mid-way failures
  repair**: the "patched header did not compile" branch (`:203`) and any interrupt now go through it.
- Step 4 (from `:212`) only **records** a verdict (`tripped` / `did-not-trip` / `wrong-reason`); step 5
  (from `:227`) calls `repair` and clears the trap **before** anything is printed or exited, and a failed
  repair downgrades the verdict to **2** ("a build directory that could not be put back is 'could not
  run'"). All three outcomes share one block.
- **No file is written into the repository.** `LOG_DIR` is `<build-dir>/p3a-g7-logs` (absolute, created
  with `mkdir -p`, falling back to `mktemp -d` if the build dir is unwritable); the header backup lives
  there too, and both former `cp … ./p3a-vertex-input-control-*.log` lines are gone. Every verdict prints
  the log path.
- The header's exit-code block (`:39-63`) now states the rebuild as part of the contract and explains why
  the "did not trip" path is the one that needs it most.

**Proved by forcing each path once.** A rehearsal ran a **byte-identical copy** of the shipped script
(`cmp` clean) inside a throwaway repo at `/tmp/g7rehearsal`, whose
`MobileGL/MG_Impl/Pipe/VertexInputEmit.h` and `VertexInputEmit.Roundtrip` ctest entry are tiny stand-ins —
so the control flow exercised is the real one, and only the two constants' *contents* differ:

| forced condition | rc | header after | binary after | repo root |
|---|---|---|---|---|
| the walk names `IsBgra` when it is zeroed | **0** | original | built from the restored header | clean |
| the walk stopped comparing (stays green) | **1** | original | **built from the restored header** | clean |
| the walk fails without naming the field | **1** | original | built from the restored header | clean |
| the patched header does not compile (`Bgra` enum class) | **2** | original | built from the restored header | clean |

In every case the transcript shows `restored …; rebuilding … / the tree is restored, rebuilt and green
again` **before** the verdict line (or, for the exit-2 case, from the trap), and `ls` of the fake repo root
shows nothing new. The rehearsal tree was deleted.

On the real tree the script still answers **rc 2** — `MG_Impl/Pipe/VertexInputEmit.h` is package B's and
does not exist yet — and leaves `git status` clean.

### M4 / ID-11 — `FlushPendingRangesNow` in G5 — **DONE** (`2bafb316`)

`scripts/p3a_untouched_regions.sh`: `FUNCTIONS` is now **ten** names, appended last so the first nine lines
of the D.0 baseline keep their positions. `EXPECTED_FUNCTION_COUNT=10` replaces the hard-coded `9`. The
header table gains the row and a paragraph recording *why* the tenth is there (ID-11 resolving
`BRIEF-P3A.md:420` / `:1708` against D-F's nine-function table) and *what byte-identical means for it*:
**the handle arm must CALL it, not carry a copy — a push arm that re-spells the ladder beside an untouched
`#else` copy satisfies G1 and defeats G5, because two ladders drift.**

**The self-test grew a control for it.** `SELF_TEST_FUNCTIONS="ClearBufferPool FlushPendingRangesNow"`, one
negative control each, each run from the pristine copy and each required to be *named* in the message.
`ClearBufferPool` is the easy shape (small, no forward declaration); `FlushPendingRangesNow` is the
opposite on purpose — 70 lines, three nested tiers, its own early returns — so an entry that silently
extracted the wrong extent cannot pass. Both trip:

```
[p3a-untouched] positive control: 10 bodies extracted from the working tree
[p3a-untouched] positive control: an untouched copy compares equal
[p3a-untouched] positive control: an edit outside the ten bodies is invisible
[p3a-untouched] negative control: a perturbed ClearBufferPool body is reported, and named
[p3a-untouched] negative control: a perturbed FlushPendingRangesNow body is reported, and named
[p3a-untouched] self-test passed
```

**The new D.0 baseline (`p3a_untouched_regions.sh HEAD`, rc 0):**

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
c65570022a5856c1afccfd0cb42fefac707689f5bb3154c3a8e17d2495ccdf8e  FlushPendingRangesNow
```

The integrator should replace `~/w7/p3a-before-untouched.sha` with this list (it is `44c2b5cf`'s too — the
gate is rc 0 between them).

### D6 — the second-arena workaround undone (`331bb425`)

ID-9 makes `d7655247` an ancestor of this branch, so both counting workloads now **re-specify** an adopted
store under a live VAO instead of routing around it:

- `LargeArenaAdoptionScenario.cpp:457-474` — `glBindBuffer(m_arena); glBufferData(kArenaBytes, nullptr, …)`
  inside the counted window, and the attribute pointers are **deliberately not re-declared** afterwards, so
  the draws below can only land if the backend VAO followed the new store on its own. `m_secondArena` and
  its TearDown lines are gone.
- `ResourceSubsystemControlScenario.cpp` — `DefineAnArenaAndDrawFromIt` → `DefineTheArenaAndDrawFromIt`
  (`:225`): index 0 creates the store and declares the attributes, every later index re-specifies the same
  buffer and declares nothing. `std::vector<GLuint> m_arenas` → a single `GLuint m_arena` (`:250`). Two
  storage definitions either way, so the counter's expectations (`mpr == 2` on, `0` off) are unchanged.

Both comments now say why the shape is the hard one and cite ID-9 rather than the old detour. Because the
first of these runs under `ResourceSubsystemOn.`/`Off.`, the respecify path is now exercised **on both arms
of the P3a A/B**, which is what ID-9 asks for; a handle arm that re-implemented the retire without the
rebind is a dead draw or a fault there rather than a divergence found on device.

### The minors

| # | disposition |
|---|---|
| **m1** (the `-V` artefact) | **Done.** Both logs regenerated post-rebase and kept: `~/w7/p3a-handlerecycle-before.log` (the D.2 command) **and** `~/w7/p3a-handlerecycle-before-verbose.log` (`ctest -V`, 158 KB, 22 `[ HandleRecycle ]` verdict lines), both copied to `~/w7/notes/p3a/p3a-results/`. The AbaControl green is now readable as a verdict line, not inferred from an exit code. |
| **m2** (D6 wording) | Moot — the workaround is gone. For the record the review was right: `StorageBufferRegrowScenario` never used a second arena (it re-specifies an SSBO with no VAO attributes). |
| **m3** (the stale workaround) | Fixed — see D6 above. |
| **m4** (G14 numbers) | Re-measured against `5cb826b0`'s baseline; see §3. D's contribution is **+38** (34 + the 4 new `ResourceSubsystem{On,Off}.…Respecified*Arena*` lane entries the union produced). |
| **m5** (digit separator) | **Done.** `mask()` skips a `'` preceded by a digit or a hex digit that itself follows a hex digit — a C++14 separator (`16'777'216`, `0xff'ff`) no longer opens a char literal that could blank a brace a function away. |
| **m6** (`perturb()` brace) | **Done.** The opening brace is located in the **masked** text and used as an offset into the original (`mask()` preserves length), so a brace in a comment or a default argument on the signature line cannot misdirect the perturbation. |
| **m7** (return type on its own line) | Not changed. Still true that none of the ten is written that way, `FlushPendingRangesNow` included (`void FlushPendingRangesNow(` is one line). |
| **m8** (`build-bench`) | Still not run — there is no `build-bench` in this worktree and no benchmark source changed. Integrator item, unchanged. |
| **m9** (`588d25e9`'s message) | Unchanged: the messages are C.3's verbatim and the rebase preserved them (`ecbddcea`). The D5 `RESOURCE_LOCK` fix is still carried by that commit; this report is the place it is written down. |
| **m10** (no DirectGLES ABA positive control) | Unchanged; carried to package C's scope as the review suggests. |

---

## 3. The G5 verdict against espryt's tree

**`git -C /home/swung/w7/p3a-espryt rev-parse HEAD` has moved: it is now `cdbd5535`**
(`[Test] (Espryt): extend the slot-table suite to the buffer kind and pin that its death crosses as
resource_destroy`), not the `203120fa` the brief names. **The gate was run against both, and answers
identically.**

```
$ bash scripts/p3a_untouched_regions.sh 44c2b5cf 203120fa      -> rc 2
$ bash scripts/p3a_untouched_regions.sh 44c2b5cf cdbd5535      -> rc 2
[p3a-untouched] FlushPendingRangesNow: expected exactly one definition, found 2
[p3a-untouched]   ...definition at line 1127, 3 lines
[p3a-untouched]   ...definition at line 1307, 70 lines
[p3a-untouched]   the handle arm must CALL the untouched FlushPendingRangesNow, not carry a copy of
[p3a-untouched]   it: two ladders drift (BRIEF-P3A.md:1708, ID-11)
```

**The gate was not weakened.** Instead the `len(hits) > 1` diagnostic was made to print the two extents and
the rule, because this is now the *expected shape* of the failure rather than a limitation of the
extractor.

**The exact hunk, for espryt's rework** (`MG_Backend/DirectGLES/Managers.cpp` at both refs; the whole region
is `#if MOBILEGL_PIPE_PUSH` at `:902` … `#else` at `:1188` … `#endif` at `:1424`):

| what | where | lines | sha256 (16) |
|---|---|---|---|
| **push arm** — `FlushPendingRangesFrom(GLESBufferResource&, const Uint8* hostBase, SizeT frontendSize)`, a **re-spelled copy of the whole three-tier ladder** | `:1051-1125` | 75 | `37fc94ffc5991923` |
| **push arm** — `FlushPendingRangesNow(GLESBufferResource&, BufferObject&)`, a thin forwarder to it | `:1127-1129` | 3 | `7ac74243aec7b6f8` |
| **`#else` (pull) arm** — `FlushPendingRangesNow(GLESBufferResource&, BufferObject&)` | `:1307-1376` | 70 | **`c65570022a5856c1`** = **byte-identical to `$BASE:1004`** |

New callers of the re-spelled core are at `:2000` and `:2541` (the handle arm); `:1708` and `:2677` still
call `FlushPendingRangesNow`.

**Read this precisely, because it is not "a body moved":**

1. **All nine original G5 functions are byte-identical at espryt's HEAD** — verified by extracting them
   from espryt's file directly; every sha matches `44c2b5cf`. The review's earlier rc 0 still holds for the
   nine.
2. **The pull arm's `FlushPendingRangesNow` is byte-identical too.** Nothing was edited in place.
3. What the gate objects to is that **inside a push build the ladder exists twice** — once as
   `FlushPendingRangesFrom` (75 lines, re-parameterised) and once as the `#else` copy that G1 freezes — and
   the gate cannot say which is "the" body, so it refuses (rc 2) rather than picking one.

**This is a real conflict between G1 and G5-with-the-tenth, and it is the integrator's to settle.** Espryt's
deviation D1 exists because refactoring these helpers in place resized five pull symbols
(`FlushPendingRangesNow +14` among them), which G1's empty admitted-resize set forbids; the `#if/#else`
split is what bought G1 back. The two obvious resolutions both cost something:

- **Espryt restores it byte-identical and the handle arm calls it.** This is what ID-11 asks for and it is
  what makes the risk row at `BRIEF-P3A.md:1708` true. It needs the handle arm to reach the same body
  without a `BufferObject&`, which is the constraint D1 was written around — so it is not a one-line
  change, and whether it is achievable without resizing a pull symbol is espryt's call, not this
  package's.
- **The tenth entry stays and the phase records a written, argued exception for it** — naming the two
  copies, stating that the `#else` copy is frozen by G1 and the push copy is covered by
  `CrossFrameBufferScenario`'s thirteen cases plus `StreamedArenaScenario`'s two, and that the exception
  retires with the pull path at P13. That is a decision, not a silence, and it is what D.5 would record.

What this package will **not** do is drop the tenth entry or teach the extractor to prefer one of two
definitions: either would turn `ROADMAP.md:19`'s headline p99 number back into a risk with a mitigation
that does not exist. The finding is recorded here so espryt's rework and the integrator can choose.

---

## 4. Verification transcript

All in `/home/swung/w7/p3a-gates`, `CCACHE_BASEDIR=/home/swung/w7`, at **`2bafb316`**.

| check | result |
|---|---|
| `cmake --build {build-push,build-linux,build-verify} -j 28` | rc 0, rc 0, rc 0 |
| **G1** `symbol_report.py --before ~/w7/p3a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0` | **rc 0 — Removed 0, Added 0, Resized 0, Renamed 0**. `0/0/0/0` against ID-9's re-captured `5cb826b0` baseline |
| **G2** `ctest -N` (ID-4 extractor), pull vs push | **identical**, 2540 each (`build-verify` 3382) |
| **G14** `comm -23 ~/w7/p3a-before-ctest-names.txt <build-linux names>` | **empty — 0 removed**. `comm -13` = **+38 added**, all new names; the union kept `dev`'s `LargeArenaAdoptionScenario.Respecified{Vertex,Index}Arena*` on both backends **and** in both new subsystem lanes |
| `ctest -L unit -j 12` on all three builds | rc 0 — **1582/1582** each |
| `ctest -L integration-gpu -j 8` on **build-push** | rc 0 — **958/958** |
| `ctest -L integration-gpu -j 8` on **build-linux** | rc 0 — **958/958** |
| `ctest -L integration-verify -j 4` on **build-verify** | rc 0 — **842/842** |
| **hard rule 4** `ctest --test-dir build-verify -R HandleRecycle --no-tests=error` | rc 0; only visible skips (the `.Handles` buffer arm and the `Verify.` prefixed copies, each naming what is missing) |
| **hard rule 4** `ctest --test-dir build-push -R 'StorageBufferRegrow\|LargeArenaAdoption\|ResourceSubsystem'` | rc 0; the only skips are the four counting entries + the two lane copies, all saying `no source under MobileGL/MG_Impl/Pipe/ names MapPersistentRoundtrips` |
| **M1 re-run** `ctest --test-dir build-push -R CsoContentAddressing` | rc 0 — 6 entries, 4 lanes assert, arms distinguishable (`csom=3` vs `csom=17` at 17 binds) |
| **M2 re-run** the CI step as CI runs it, on build-verify | rc 0 — **96/96**; 80 → 96 entries, the 16 P3a lane entries now included; 10 of the 12 On/Off entries assert, 2 skip naming package B |
| **G5** `p3a_untouched_regions.sh 44c2b5cf HEAD` | **rc 0** — ten bodies byte-identical |
| **G5** `p3a_untouched_regions.sh --self-test` | **rc 0** — 3 positive controls + **2** negative controls (both named) |
| **G5** vs `p3a/espryt` (`203120fa` **and** `cdbd5535`) | **rc 2**, naming the two definitions — §3 |
| **G7** `p3a_vertex_input_negative_control.sh build-push` on this tree | **rc 2** — package B's header absent, message says so |
| **G7** all four exit paths, forced on a byte-identical copy | **0 / 1 / 1 / 2**, tree restored **and rebuilt** on every one, nothing written to the repo root |
| `git status` | clean |

Two D.2 artefacts kept (`~/w7/` and copied to `~/w7/notes/p3a/p3a-results/`):
`handlerecycle-before.log` and `handlerecycle-before-verbose.log`. Every other log this round created —
`~/w7/p3a-gates-build.log`, the `/tmp` build and label logs, the `/tmp/g7rehearsal` tree — was deleted.
`ls ~/w7/p3a-gates-*` is empty.

---

## 5. What remains for the integrator

`gates-v1.md` §8's list stands, with items 8 (carry D6) and the G14 re-measurement discharged. What is
left, in the order it bites:

1. **Settle the `FlushPendingRangesNow` duplication with package C** (§3). This is the one item that is not
   mechanical: either espryt's handle arm calls the untouched body, or the phase writes down an argued
   exception in D.5. Until then G5 is **rc 2** on any tree carrying espryt, and rc 2 must not be read as
   "the script is broken" — the message names the two definitions and their line extents.
2. **Re-run `p3a_untouched_regions.sh 44c2b5cf HEAD` after every merge**, plus `--self-test`. Re-capture
   `~/w7/p3a-before-untouched.sha` with the **ten**-line listing above (§2, M4).
3. **`p3a_vertex_input_negative_control.sh build-push` on the tree carrying package B** — it must report
   `negative control tripped, naming IsBgra` (rc 0). Anything else is a finding about G6, and rc 2 now
   means something different from what it means here. Its logs land in `<build-dir>/p3a-g7-logs/`.
4. **The `.Handles` buffer arm must stop skipping** once package C registers `MGPipeResourceOps`; the four
   counting entries must stop skipping once package B emits `MapPersistentRoundtrips`, and then read
   `mpr == 3`, `1`, `2` (On) and `0` (Off). A green Off arm beside a skipping On arm is not the A/B
   (review item 11).
5. **`MOBILEGL_PIPE_PUSH=0x7f ctest -L integration-gpu -j 8` beside the default `0x1ff`** — the label-wide
   third arm. The two `ResourceSubsystemOn/Off.` lanes now cover the adopted store's whole life *including
   the respecify path* in both arms, but the label-wide A/B is still the integrator's.
6. **`cmake --build build-bench && ctest --test-dir build-bench -L benchmark`** once (C.3's block, m8).
7. **`gh workflow run test.yml --ref feat/disaggregated`** — `pipe-gates` runs the two G5 rows (branch
   scoped, `BASELINE: "44c2b5cf"`, `fetch-depth: 0`); the `integration-verify` controls step now selects
   96 entries instead of 80.
8. **The DirectGLES buffer ABA has no positive control** (review m10) — carry into package C's scope rather
   than losing it: `HandleRecycleScenario.cpp:319-321` names where a `Features.PipeHandleAbaControl`
   consumer over the resource slot table would go.
