# P3a implementation brief — handle wave 1 (Espryt): buffers and VAOs behind the `resource_*` / vertex calls

Tree: `feat/disaggregated @ 55d2af9b` (worktree `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`,
read-only for the scouts and for this brief). **`55d2af9b` is the P3a base ref**; every `file:line` below was re-opened
at it. `$BASE` = `55d2af9b` throughout.

WSL: `~/w7/pipe` is a worktree of the same repo on `feat/disaggregated`; `~/w7/base` is `dev@81b17c0b` (the performance
anchor, untouched). Tree creation is `~/w7/notes/tools/wsl_p2_tree.sh` copied to `wsl_p3a_tree.sh` with `p2`→`p3a`
throughout (it creates `~/w7/p3a-<slug>` on branch `p3a/<slug>`, initialises submodules, copies
`3rdparty/glslang/External` and the trace fixtures, then configures and builds `build-linux` (pull), `build-push`, and
with a third argument `verify` also `build-verify`).

Corrections to the scouts are **[correction]**; deliberate departures from the design documents are **[deviation]** and
carry their reason and the doc line the integrator must correct.

---

## Scout verification summary

All four scouts were read in full and their load-bearing claims re-opened at `55d2af9b`. What follows is wrong or
incomplete in them and is fixed here. Everything not listed was confirmed verbatim.

1. **[correction] `scout-docs-spec.md` mis-cites eight `ARCHITECTURE.md` lines.** The correct lines are:
   the call-flag legend incl. `kOptional`/`kReplySlot`/`kNeedsAck` and Magma's deliberate `BufferSubDataResident`
   omission is **`:94`** (not `:102`; `:102` is the "explicitly not migrated" list);
   the `MGPResourceDesc` prose gloss is **`:128`** (not `:127`, a table separator);
   the `resource_create`/`resource_respecify`/`resource_destroy` lifetime rule is **`:208`** (not `:204`);
   the fp64-vertex-narrowing emulation row is **`:225`** (not `:236`);
   the per-site `SyncPersistentMappedRange`/`SyncGpuWrites` reconcile table is **`:235-241`** with its gate sentence at
   **`:243`** (not `:239-243`);
   `OnGpuWritten` is **`:274`** and `OnBufferWriteback` **`:275`** (not `:271-273`);
   §9.1 "原样不动的东西" is the heading at **`:314`** and the Espryt list at **`:316`** (not `:315`);
   the two surviving byte-level equalities are **`:507`** (not `:508`);
   `SetIndexBuffer` independent of the VAO config version (D5) is **`:99`** (not `:101`);
   the `NEW_INDEX_BUFFER` dirty row is **`:165`** (not `:164`);
   the CSO content-addressing capacities (render-state 64 / vertex-elements 1024 / sampler 256 / sampler-view 4096) are
   **`:63`** (not `:57`); `VertexInputStateFactory::m_cache` as the server-side counterpart is **`:55`**.
2. **[correction] `ROADMAP.md` open question 10 (`ResidentSubData` asymmetry) is `:83`, not `:87`.** Questions 15/16/17
   are at `:88`/`:89`/`:90` as the scout says. The re-baseline table is `:60-68`: P3a's trigger row is `:64`, P7's note
   about P3a's checkpoint being blind to Magma is `:66`, and "任一触发，先跑 `inproc` 的证伪数字" is `:68` (not `:67`).
3. **[correction] Six `Managers.cpp` line numbers in `scout-espryt-buffers.md` are off by 2–8.** Verified:
   `Ops_OnDestroy` starts at **`:1417`** (not 1421); `g_deferredBufferReleases` is declared at **`:672`**;
   `struct PooledBuffer` at **`:687`**; `g_bufferPool` at **`:693`**; the three pool budget constants
   (`kMaxPoolableBufferBytes` 8 MiB / `kMaxPoolBytes` 64 MiB / `kMaxEntriesPerBucket` 32) at **`:696-698`**;
   `IsPoolable` at **`:700`**; `EnrollIntoPool` at **`:711`**; `AcquireFromPool` at **`:740`**;
   `g_bufferBackendIdGeneration`'s definition at **`:1503`**. `Ops_Respecify` `:1213`, `Ops_SubData` `:1266`,
   `Ops_ResidentSubData` `:1304`, `Ops_FlushMappedRange` `:1314`, `Ops_ReadbackFromGpu` `:1382`,
   `Ops_AcquirePersistentMap` `:1134`, the seven tracked wrappers `:1449-1480`, `g_glesBufferBackendOps` `:1482-1490`,
   `Register/UnregisterBufferBackendOps` `:1505`/`:1512`, `OnBackendContextDestroyed` `:1527`,
   `ProcessDeferredBufferReleases` `:1542`, `IsBufferDrawClean` `:1579`, `EnsureBufferResource` `:1602`,
   `TrimBufferPool` `:1882`, `ClearBufferPool` `:1913`, `FlushPendingRangesNow` `:1004`, `DrainResidentWritesNow`
   `:1084`, `RespecifyStorageNow` `:895`, `UploadRangeNow` `:946`, `UploadRingUsableNow` `:964` are all exact.
4. **[correction] `MEASUREMENTS.md`'s "6.5–9.3 accessor calls per draw" is `:64`, not `:66`.** `:87` (the persistent-map
   adoption baseline) and `:96` (the `create-indirect` pre-existing failure) are exact.
5. **[correction] The `map-persistent-roundtrips` counter does not exist in any form.** `PipeStats.h`'s `ByteClass`
   (`:46-77`) and `CallClass` (`:81-118`) carry no such member and `PipeStats.cpp:173-179`'s name table has no such
   string. P3a **mints it** (D-B2 below), and it must be `#if MOBILEGL_PIPE_PUSH`-guarded for the reason `PipeStats.h`
   `:101-106` already states for `RenderStateCsoMints`/`Binds`: growing the enum in a **pull** build resizes the counter
   arrays, the name table and `FormatWindowLine`, which is a G1 break for a counter that could never leave zero.
6. **[correction] `scout-espryt-vao.md`'s flag about the second view is right, and it is stronger than the scout says.**
   `MGPipeValueTypes.h:529-531` states it as a design fact in the tree — *"Attributes configured through the
   binding-point API are resolved eagerly into the flat VertexAttribute view above, so backends keep consuming resolved
   attributes and never see binding points."* — and `grep -rn 'VertexBufferBindingPoint\|GetAttributeBindingIndex\|
   GetAttributeRelativeOffset' MobileGL/MG_Backend/` returns **zero hits**. `ARCHITECTURE.md:130`'s justification for
   carrying both views ("pointer stride 0 = element size, binding stride 0 = same element") is therefore **stale**: the
   frontend already resolved it, and `MGPipeValueTypes.h:496-503` says a surviving zero can only have come from the
   binding model. D-G below carries both views anyway, for a different and still-valid reason, and the integrator
   corrects `ARCHITECTURE.md:130`'s stated reason.
7. **[correction] `scout-espryt-vao.md` §4.2's `baseInstance` question resolves in favour of a payload change, and the
   free `Pad0` it points at is the wrong one.** `MGPVertexBuffer` (`MGPipeTypes.h:356-364`) does have a `Uint32 Pad0`
   at `:362`, but the fetch shift is **one value per emitted set**, not per binding: putting it per entry lets a
   malformed record disagree with itself and gives the applier a case to police. `ROADMAP.md:19` names
   `set_vertex_buffers`, whose payload is `MGPVertexBuffers` (`:367-371`, 16 B, no padding). D-H takes that struct from
   16 to 24 bytes.
8. **[correction] `scout-pipe-and-tests.md` §5.1's "edit `PipeCalls.def` in place for `kNeedsAck`" understates the
   problem.** Flags are a **per-call** static property, and `ResourceRespecify` is the one call used for both
   `glBufferData` and `glBufferStorage`. A bare `kNeedsAck` on it would ack every `glBufferData` in Minecraft's world
   upload. D-A5 resolves it with a per-record predicate and a flag-legend edit.
9. **[correction] The four ID-3 admitted resizes do not apply to P3a's G1.** They are resizes measured against the
   **pre-P2** baseline and are already present in `$BASE`. Against `$BASE` the admitted set is **empty**: 0 added,
   0 removed, 0 renamed, **0 resized**. They reappear only when the report is run against `087685d1`, which is what CI's
   `monolith-symbol-report` job does by default (`.github/workflows/test.yml:14-21`). G1 below names both readings so
   neither can be confused for the other.
10. **Confirmed at `55d2af9b`, unchanged from the scouts**: all sixteen `PipeCalls.def` rows P3a lands
   (`:75-79`, `:95-97`, `:108-110`, `:128-132`) with exactly the payloads, classes and flags quoted;
   `MGP_CALL_LIST_DOCUMENTED_COUNT 71` (`:69`); `MGPResourceDesc` 88 B (`:150-176`) and `kMGPipeWholeBuffer` (`:177`);
   `MGPSubData` 72 B with the buffer-half convention and its three accessors (`:575-617`); `MGPFlushRange` 32 B
   (`:628-635`); `MGPReadback` 24 B (`:637-641`); `MGPVertexElements` 40 B (`:250-259`); `MGPIndexBuffer` 24 B
   (`:373-380`); `BufferBackendOps`' seven members and every doc comment quoted (`BufferObject.h:76-122`);
   `g_glesBufferBackendOps` setting all seven (`Managers.cpp:1482-1490`); `MGPipeCallbacks`' ten callbacks including
   `OnGpuWritten` and `OnBufferWriteback` (`MGPipeCallbacks.h:27-51`); the seven `MGPipeApply*` entry points and
   `MGPipeApplierState` (`PipeApply.h:96-149`, `:48-87`); `MGPipeDirty`'s 18 bits and `MGPipeSubsystemForDirty`'s
   `default: return 0` (`Tracker.h:57-135`); `MGPipeSuppressorSlot::SetVertexBuffers = 0` and the reserved hash 0
   (`SetHashSuppressor.h:36-59`); the six `NotifyStateObjectDestroyed` raisers and `Managers.cpp:212-215`'s
   `default:` arm naming `BufferBackendOps::OnDestroy`; the subsystem bits 0..6 and
   `kMGPipeSubsystemsMigratedAtP2 = 0x7f` (`MGPipe.h:72-85`); `SubsystemForEmitter`, its five `static_assert`s and
   `kMGPipeWiredSubsystems` (`PipeFill.cpp:624-672`); `Coverage.def:36-45` and `:128-130`; `FillPoints.def:60-64`
   recording the class-field-mask retirement as **P3a work**; the VAO twin's whole private state
   (`Managers.h:886-951`) and `SyncToBackend`'s gate (`Managers.cpp:2437-2472`); the three `ScopedFetchBaseInstance`
   sites (`DirectGLES.cpp:5135, 5172, 5224`) and `EmulatedFetchBaseInstance`/`UseNativeBaseInstance`
   (`:5122-5130`); `retrace_gate.py`'s five flags (`--tree --lib --out -j --only`, no `--ssim`, no `--backend`);
   the 40 trace cases, the two `coherent_as_flush: true` Create fixtures (`trace_cases.json:275, 283`) and the global
   `"ssim_threshold": 0.99` at `:6` with per-case overrides at `:292` (0.995) and `:303` (0.98); and all fifteen named
   scenario files under `MobileGL/MG_IntegrationTest/Scenarios/`.

---

## A. Goal and acceptance gate

`docs/Disaggregated/ROADMAP.md:19`, verbatim (all five cells):

> | **P3a** handle wave 1（Espryt）：buffer、VAO | 18–23 | 7 个 `BufferBackendOps` → `resource_*`、`buffer_subdata_resident`（可 null）、`resource_flush_range`、`resource_readback`、`map_persistent`（不碰实现）；pool 与延迟释放原样搬；vertex elements 三件（两个视图都带）；`set_vertex_buffers`（`baseInstance` 显式字段）；`set_index_buffer`；Adreno SIGSEGV workaround 保留 | 全套门；buffer/VAO 族场景（`LargeArenaAdoption`、`StorageBufferRegrow` 发布 `map-persistent-roundtrips`、`VertexAttribBinding`、`MultiDraw`、`PrimitiveRestart`…）；Create/rd12/26.3/sodium trace；MC 26.3 在 Adreno 上 p99 不变。**再基线检查点 1：超过 27 天必须重定基线** | P2 |

Binding context: `ARCHITECTURE.md:94` (the call-flag legend, including `kOptional`'s named exception), `:99` (D5:
`SetIndexBuffer` is independent of the VAO config version), `:116-122` (payload record conventions and the `MGPSubData`
buffer half), `:128-130` (the `MGPResourceDesc` and `MGPVertexElements` rows), `:149-155` (§5.1 — push happens at the
validate moment before a verb, **and the one exception: only the seven `BufferBackendOps` hooks push at GL-call
time**), `:157-174` (§5.2 — the dirty table, five aggregate generations, wrap widening), `:194` and `:200` (§5.4 — the
D-B3 ordering invariant and live resolution of indexed ranges), `:208` (§5.6 — the create/respecify/destroy lifetime
hooks and the three payload-expressed ordering constraints), `:221-225` and `:235-243` (§5.7 — emulation ownership and
the per-site reconcile table), `:267-288` (§8.1/8.2 — the reverse channel and the epoch-ordering rule), `:293` (§8.3 —
**the only entry allowed a synchronous ack is `glBufferStorage`; the `kNeedsAck` marker lands with P3a's buffer
path**), `:314-316` (§9.1 — the Espryt do-not-touch list: three rings, buffer pool, the Adreno disabled-attribute
SIGSEGV workaround), `:363` (§9.5 — the 21-memo census P3a pays into), `:367` (§9.6 — `MOBILEGL_PIPE_LEGACY_MEMOS`
stays a compile-time arm **through P3a/P4a**), `:383-389` (§10.3 — the index host mirror P8 builds out of P3a's three
calls), `:466` (D-B4 — `AcquirePersistentMap` untouched for the whole monolith conversion), `:479-485` (P5's persistent-map
push and the `coherent_as_flush` rule), `:497-507` (§13.2 — the five-part validation gate and the two surviving byte
equalities), `:515` (present/`eglSwapBuffers` strictly 1:1, which is what protects `TrimBufferPool`). `ROADMAP.md:7`:
默认 ALL target 必须完整构建；禁止提交热路径插桩；**每个门必须能因它存在的理由变红**；Windows 不是正确性门；设备对比走
reboot-clean + 同热窗口配对 A/B；每阶段出口跑一次五部分门；每阶段性能判据是逐线程 CPU 时间. `ROADMAP.md:34`: **P3a is one of
the five architecture boundaries that owe a full `gl44to46` caselist run (~56,271 cases) on both devices, off the
critical path.** `ROADMAP.md:64`: **P3a over 27 days means the narrow-handle premise was wrong and P4a re-baselines.**

### A.1 The user's standing rules (2026-09-08), applied

> (a) Advance the roadmap as fast as possible without a large performance regression. **Performance is RECORDED
> against the pull baseline (P2's A/B and DriverBench numbers in `MEASUREMENTS.md`), never a blocking gate in P3a.
> Correctness gates stay.**
> (b) Agents run on opus, so packages stay mechanical and well-specified.

Concretely: **part 4 of the five-part gate (`ARCHITECTURE.md:504`) becomes a measure-and-publish obligation, not a
pass/fail gate**, and so does `ROADMAP.md:19`'s "MC 26.3 在 Adreno 上 p99 不变". Parts 1, 2, 3 and 5 stay hard gates.
This rule is transcribed into `MEASUREMENTS.md`'s P3a section by the integrator (D.5) because it is not written in
`docs/Disaggregated/*`.

### A.2 The gate, decoded

Unless a row says otherwise, commands run in a P3a WSL worktree. ID-4's errata are applied throughout: ctest names are
extracted with `grep -E "^[[:space:]]*Test[[:space:]]+#[0-9]+:" | sed -E "s/^ *Test +#[0-9]+: //" | LC_ALL=C sort`
(the naive `^\s+Test #` drops every id under 1000); `retrace_gate.py` has **no** `--ssim` and **no** `--backend` (the
threshold is the per-case one in `trace_cases.json`); the verify arming line is
`MGPipe: verify armed - 63 fields, 69 verbs, fatal=1` and retrace case logs carry
`MGPipe verify: <case> <backend> armed, zero divergences, zero unmigrated reads`.

| # | statement | the exact command that checks it |
|---|---|---|
| **G1** | The **pull build** (`MOBILEGL_PIPE_PUSH=OFF`, `MOBILEGL_PIPE_VERIFY=OFF`, Release/INFO) is symbol-identical to `$BASE`: 0 added, 0 removed, 0 renamed, **0 resized**. P3a's admitted-resize set is **empty** — every P3a edit to `MG_State` / `MG_Pipe` / `MG_Backend` is inside `#if MOBILEGL_PIPE_PUSH` (D-J). The four ID-3 resizes (`RenderState::{RenderState,SetCapability,IsCapabilityEnabled}`, `_GLOBAL__sub_I_DirectGLES.cpp`) are already in `$BASE` and reappear only in the `087685d1` reading. | `python3 scripts/symbol_report.py --before ~/w7/p3a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0 --json` → `added==removed==renamed==resized==0`. The `087685d1` reading is CI's: `gh workflow run test.yml --ref feat/disaggregated -f baseline_sha=087685d1`, whose Resized table must be **exactly** those four. |
| **G2** | `ctest -L integration-gpu` is **name-for-name identical between the pull and the push build**, on both backends, and green in both. | `for d in build-linux build-push; do ctest --test-dir $d -N \| grep -E "^[[:space:]]*Test[[:space:]]+#[0-9]+:" \| sed -E "s/^ *Test +#[0-9]+: //" \| LC_ALL=C sort > ~/w7/p3a-names-$d.txt; done; diff ~/w7/p3a-names-build-linux.txt ~/w7/p3a-names-build-push.txt` → empty; then `ctest --test-dir build-linux -L integration-gpu --no-tests=error -j 8` and the same on `build-push`, both green, plus `MOBILEGL_PIPE_PUSH=0 ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8` (the all-pull arm). |
| **G3** | The 40-trace corpus replays **under push** at each case's own SSIM threshold on both backends (79 desktop cases: 40 × 2 minus `iterationrp × DirectGLES`). | `python3 ~/w7/retrace_gate.py --tree ~/w7/pipe --lib ~/w7/pipe/build-push/libMobileGL.so --out ~/w7/retrace-out/p3a-push -j 4` → exit 0. |
| **G3b** | The five **named** traces pass under push on both backends: `minecraft-1.21.1-neoforge-create-indirect-in-world`, `minecraft-1.21.1-neoforge-create-instancing-in-world`, `minecraft-1.21.4-rd12-odinlite-in-world`, `improved-transparency-minecraft-26.3`, `minecraft-1.21.4-fabric-sodium-in-world`; and the two `coherent_as_flush: true` Create fixtures take the **same buffer path** under push as under pull (`ARCHITECTURE.md:485`). | `python3 ~/w7/retrace_gate.py --tree ~/w7/pipe --lib .../build-push/libMobileGL.so --out ~/w7/retrace-out/p3a-named -j 4 --only 'create-indirect,create-instancing,rd12-odinlite,improved-transparency-minecraft-26.3,fabric-sodium'`. **`create-indirect` on device is excluded and blocked on a `dev` fix** — see the caveat below. |
| **G4** | The **verify** build shows zero divergence: no `Fatal{PipeVerifyDiffer`, no `Fatal{UnmigratedPipeInput`, no `Fatal{PipeResidualDiverged`, and **every case armed**. | `ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4`; `MOBILEGL_PIPE_VERIFY=1 python3 ~/w7/retrace_gate.py --tree ~/w7/pipe --lib ~/w7/pipe/build-verify/libMobileGL.so --out ~/w7/retrace-out/p3a-verify -j 4`; then `grep -l 'Fatal{' ~/w7/retrace-out/p3a-verify/*.log` empty and `grep -L 'MGPipe verify:' ~/w7/retrace-out/p3a-verify/*.log` empty. |
| **G5** | **"pool 与延迟释放原样搬" is literal.** Nine named functions in `Managers.cpp` are **byte-identical** to `$BASE`: `IsPoolable`, `EnrollIntoPool`, `AcquireFromPool`, `TrimBufferPool`, `ClearBufferPool`, `ProcessDeferredBufferReleases`, `CreateRingStorage`, `RingAvailable`, `RingAllocate`. | `bash scripts/p3a_untouched_regions.sh $BASE HEAD` (package D, C.4) — extracts each named function by brace matching, `sha256sum`s the list on both refs, diffs. Non-zero rc names the first function that moved. |
| **G6** | **Vertex-input emission consistency**: for every VAO configuration the client's emitted `MGPVertexElements` blob + `MGPVertexBuffers` set + `MGPIndexBuffer` reproduce **exactly** the values `BackendVertexArrayObject::SyncToBackend` reads from the frontend today, field by field, for all 32 attribute slots. | `ctest --test-dir build-push -R 'VertexInputEmit\.' --no-tests=error --output-on-failure`. |
| **G7** | **G6's negative control**: dropping one field from the wire attribute conversion (a break the compiler cannot see, because the struct still has the member and still asserts its size) turns G6 red **naming that field**. | `bash scripts/p3a_vertex_input_negative_control.sh build-push` — patches `VertexInputEmit.h` to stop copying `IsBgra`, rebuilds, expects `ctest -R 'VertexInputEmit\.'` to **fail** naming `IsBgra`, then reverts and rebuilds. Exit 0 = tripped and named; 1 = did not answer; 2 = could not run. |
| **G8** | **`HandleRecycleScenario` is green after the re-key and red before it for the two P3a kinds**, as always-on ctest entries: `.Handles` green, `.Legacy` green, `.AbaControl` green-by-asserting-the-corruption. P3a adds a buffer case and extends the VAO case to the vertex-input path. | `ctest --test-dir build-verify -R 'HandleRecycle' --no-tests=error --output-on-failure`. The "red before" evidence is recorded on the contract tree at D.2. |
| **G9** | `gen_pipe_dirty_surface.py` stays a gate **and its scan root is widened** to `MG_State/GLState` so the four `MGP_NOTE_MUTATION` sites in `TextureState.h` stop being outside it (`BRIEF-P2.md:969` records the widening as a P3a item). Both directions still fail: an unmapped mutator, and a row naming a mutator the scan no longer finds. | `python3 scripts/gen_pipe_dirty_surface.py --check` (rc 0) and `--self-test` (rc 0); `git diff --exit-code -- MobileGL/MG_Pipe/DirtySurface.def`. |
| **G10** | **`map-persistent-roundtrips` exists, is published, and can move.** `StorageBufferRegrowScenario` asserts N storage definitions of an adopted store publish exactly N roundtrips and **not** one per draw; a device window prints the counter non-zero. | `ctest --test-dir build-push -R 'StorageBufferRegrow' --no-tests=error --output-on-failure`; on device `grep 'MGPipe stats:' mobilegl.log \| tail -1` shows `mpr=` > 0. |
| **G11** | **RECORDED, NOT GATED (rule (a)).** Two devices, reboot-clean, same thermal window, paired A/B, per-thread CPU p50/p99 from `frameCpuTimesMs[]`; MC 26.3 p99 on Adreno published against `MEASUREMENTS.md:87`'s adoption baseline (p99 163→21 ms, 40→115 fps, ~400 MB); DriverBench `ns_per_op` T1/T2. A regression is written down and does **not** stop the phase. | D.4. |
| **G12** | The **subsystem A/B is real**: with bits 7 and 8 cleared the tree runs the legacy `BufferBackendOps` / `MGB_CTX`-reading twin arm and produces identical integration results; a ctest entry proves the switch actually changes behaviour rather than being dead. | `MOBILEGL_PIPE_PUSH=0x7f ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8` (P2's default = P3a's subsystems off) versus the default `0x1ff`; `ctest --test-dir build-push -R 'ResourceSubsystemControl' --no-tests=error`. |
| **G13** | Purity holds: `pGLContext` still appears zero times under `MG_Backend/`, the include closure holds, no stdio instrumentation, generators regenerate clean, **and no `MG_State::GLState` type appears in any `MGPipeResourceOps` signature**. | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` empty; `python3 scripts/check_include_closure.py --mode both --compiler clang++-20 --self-test --require-all`; `python3 scripts/gen_pipe.py --check && python3 scripts/gen_pipe.py --self-test`; `python3 scripts/symbol_report.py --self-test`; the `pipe-gates` stdio grep; `grep -n 'MG_State' MobileGL/MG_Pipe/PipeApply.h` shows no hit inside the `MGPipeResourceOps` block. |
| **G14** | Existing test names are never removed (additions only) and the full CI matrix is green. | `ctest --test-dir build-linux -N \| <ID-4 extractor> \| LC_ALL=C comm -23 ~/w7/p3a-before-ctest-names.txt -` empty; `gh workflow run test.yml --ref feat/disaggregated`. |
| **G15** | **P3a is one of the five architecture boundaries**: the full `gl44to46` caselist (~56,271 cases) runs on **both** devices, off the critical path, and per-backend conformance is within 0.5 pp of the `$BASE` reading. Report shape: rows = GL version/extension, columns = status counts, rate = Pass/(Pass+Fail), NS **not** in the denominator. | Scheduled at the `dev` merge (D.5), not before the phase verdict. |

**Baseline captures the integrator takes before any package lands** (in `~/w7/pipe` at `$BASE`):

```sh
cd ~/w7/pipe
cp build-linux/libMobileGL.so ~/w7/p3a-before-libMobileGL.so
ctest --test-dir build-linux -N | grep -E "^[[:space:]]*Test[[:space:]]+#[0-9]+:" \
  | sed -E "s/^ *Test +#[0-9]+: //" | LC_ALL=C sort > ~/w7/p3a-before-ctest-names.txt
bash scripts/p3a_untouched_regions.sh $BASE $BASE > ~/w7/p3a-before-untouched.sha   # the nine G5 shas
python3 scripts/gen_pipe_dirty_surface.py > ~/w7/p3a-before-dirty-surface.txt
```

**Two caveats that bind this gate and are not P3a's work to fix.**

- **`minecraft-1.21.1-neoforge-create-indirect-in-world` fails on both devices at `dev@81b17c0b`** — Adreno 830: Espryt
  black frame after ~4.5 min, Magma `VK_ERROR_DEVICE_LOST` at a texture-upload submit; Mali SSIM 0.85 / 0.45
  (`MEASUREMENTS.md:96`, `ROADMAP.md:90`). It is a **pre-existing `dev` bug, not this branch's**, and it must be fixed
  on `dev` before it can serve as a P3a/P8 acceptance case. **Decision: it stays in the desktop 79-case SSIM corpus
  (G3/G3b) and stays out of the device A/B (D.4), exactly as P2 did.** If it is still red on `dev` at the phase exit,
  the integrator records "G3b partial: create-indirect blocked on `dev` open question 17" rather than claiming a pass.
- **Re-baseline checkpoint 1**: if P3a exceeds **27 working days**, "窄句柄化"'s premise was wrong and P4a re-baselines
  (`ROADMAP.md:64`); `ROADMAP.md:68` says to run the `inproc` falsification numbers first. The integrator records
  elapsed days per package for D.4.6's Track H unit-cost census.

---

## B. Fixed design decisions

Nothing in this section is re-decided inside a package.

### D-A — The seven `BufferBackendOps` hooks become handle-shaped `resource_*` calls

#### D-A1 — Where they push, and what replaces the ops table

`ARCHITECTURE.md:155` is the whole rule and it is the **only** exception to push-at-validate:

> **只有今天就在 GL 调用时刻分发的资源 op 在 GL 调用时刻推送**——即 `BufferBackendOps` 的七个 hook。纹理 subdata 不在此列（§6）。

So: **`ResourceCreate`, `ResourceRespecify`, `ResourceSubData`, `BufferSubDataResident`, `ResourceFlushRange`,
`ResourceReadback`, `ResourceDestroy`, `MapPersistent` and `UnmapPersistent` are emitted at the GL call that causes
them, from the same `BufferObject` dispatchers that call the ops table today** (`BufferObject.cpp:39-43` `~BufferObject`,
`:45` `NotifyRespecify`, `:53` `NotifySubData`, `:63` `NotifyFlushMappedRange`, `:73` `NotifyContentWrite`, `:186`
`TryAdoptLargeStorage`, `:291` `SyncPersistentMappedRange`, `:319` `SyncGpuWrites`, `:371`
`LandBytesIntoResidentStore`, `:493` `EnsureGpuResidentStorage`). Nothing about buffers moves to validate time in P3a.

The conversion is **not** "rename seven functions". Every hook takes `BufferObject&` — a frontend heap reference — and
four of them read `MappedData()`. So P3a introduces a second, **handle-shaped** backend op table beside the existing
one:

```cpp
// MobileGL/MG_Pipe/PipeApply.h, inside #if MOBILEGL_PIPE_PUSH.
// Registered by the active backend at bring-up, cleared at shutdown, exactly as
// BufferBackendOps is (BufferObject.h:124-127). NO MG_State type appears here - that is
// gate B, and G13 greps for it.
struct MGPipeResourceOps {
    void  (*Create)      (MGPipeHandle res, const MGPResourceDesc& desc);
    void  (*Respecify)   (MGPipeHandle res, const MGPResourceDesc& desc, const void* initialBytes);
    void  (*SubData)     (MGPipeHandle res, const MGPSubData& record, const void* bytes);
    void  (*SubDataResident)(MGPipeHandle res, const MGPSubData& record, const void* bytes); // kOptional: may be null
    void  (*FlushRange)  (MGPipeHandle res, const MGPFlushRange& record, const void* bytes);
    void  (*Readback)    (MGPipeHandle res, const MGPReadback& record);
    void  (*Destroy)     (MGPipeHandle res);
    void* (*MapPersistent)  (MGPipeHandle res, Uint64 size, const void* seedBytes);
    void  (*UnmapPersistent)(MGPipeHandle res);
};
void MGPipeSetResourceOps(const MGPipeResourceOps* ops);
const MGPipeResourceOps* MGPipeGetResourceOps();
```

**The `const void*` companion argument is P2's precedent, not a new idea**: `MGPipeApplyCreateRenderState(const
MGPRenderStateDesc&, const void* chunkBytes)` (`PipeApply.h:102`) already carries a blob beside its POD because in
monolith a blob needs no `MGPBlobRef`. In monolith the pointer is the frontend shadow base (`PipeResource::Bytes()`,
`PipeResource.h:91`) — **zero copy, zero behaviour change**. How those bytes cross in split is P5's problem and P5's
flag edit; P3a does **not** add `kHasBlob` to `ResourceRespecify` (a `kHasBlob` record must own an `MGPBlobRef` member,
`ARCHITECTURE.md:119`, and `MGPResourceDesc` has none).

**Dispatch selection in `BufferObject.cpp`** (package B), in exactly this shape at every one of the eight dispatch
sites:

```cpp
#if MOBILEGL_PIPE_PUSH
    if (MGPipeResourceSubsystemEnabled()) { MGPipeEmitResourceRespecify(*this); return; }
#endif
    if (g_bufferBackendOps && g_bufferBackendOps->Respecify) g_bufferBackendOps->Respecify(*this);
```

`MGPipeResourceSubsystemEnabled()` is `(MG_Config::Features.PipePush & kMGPipeSubsystemResources) != 0 &&
MGPipeGetResourceOps() != nullptr`. Magma never registers `MGPipeResourceOps` (its buffer path is P7,
`ROADMAP.md:24`), so a Magma context keeps dispatching `BufferBackendOps` verbatim and P3a cannot touch it. That is
also what makes the integration order in D.1 always-green: package `client` can land with nothing registered.

#### D-A2 — Payload and semantics, hook by hook

Every row below states what the client fills, what the applier stores, and what Espryt does. Espryt's **behaviour** is
unchanged in every row; only where it reads its inputs from changes.

| hook (today) | call | payload the client fills | what the applier stores | what Espryt does |
|---|---|---|---|---|
| *(none)* | `ResourceCreate` (`PipeCalls.def:75`) | `MGPResourceDesc` with `Resource` = the freshly minted `{slot,gen}` of kind `Buffer`, `Target = Buffer`, `StorageKind = Buffer`, `BindMask = 0`, `Usage`/`StorageFlags`/`Width` = 0, `Immutable = 0`, `HasDefinedContent = 0`, `GlNameForDiag = GetExternalIndex()`, everything else zero. Emitted from the `BufferObject` **constructor** (`BufferObject.cpp:35-37`), per `ARCHITECTURE.md:208`. | marks the slot Live, stores the descriptor. | **nothing** — storage is defined lazily by `ResourceRespecify`, and Espryt's `EnsureBufferResource` already tolerates a null resource (`BufferObject.h:72-75`). |
| `Respecify` (`BufferObject.h:80`) | `ResourceRespecify` (`:76`) | the same descriptor with `Width` = `GetSize()`, `Usage` = `GetUsage()`, `StorageFlags` = the `glBufferStorage` flags (0 for `glBufferData`), `Immutable` = 1 iff the store came from `glBufferStorage*`, `HasDefinedContent` = `HasDefinedContent()`, `BindMask` = D-A3's live mask. `initialBytes` = `MappedData()` when `HasDefinedContent`, else `nullptr`. | replaces the stored descriptor. | `Ops_Respecify` verbatim (`Managers.cpp:1213-1264`) with `bufferObject.GetSize()/GetUsage()/HasDefinedContent()/MappedData()` replaced by `desc.Width / desc.Usage / desc.HasDefinedContent / initialBytes` and `GetChangeSerial()` by the server-side `ResourceSerial` (D-A4). **`RespecifyStorageNow` (`:895-936`) keeps its `glBufferData(target, size, initialData, usage)` single call** — no orphan-then-upload split. `InvalidateIndexedBufferBindingShadowsForId` on an extent move (`:933`) stays. |
| `SubData` (`:82`) | `ResourceSubData` (`:128`) | `MGPSubData` with `Res`, `Target = Buffer`, and the range encoded **only** through `MGPipeSetSubDataBufferRange(record, offset, size)` (`MGPipeTypes.h:603-611`). `bytes` = `MappedData() + offset`. **A `false` return means the emitter must split** — one record caps at offset 2^31-1 and size 2^32-1, and at `SEG_STAGE`'s 32 MiB the ring's half-capacity bound is far tighter (`:579-586`). | nothing per record (contents are the backend's). | `Ops_SubData` verbatim (`:1266-1297`): all four arms preserved — `pendingRespecify` early-out, the off-thread/stale queue, the adopted zero-copy no-GL stamp (`:1278`), the `EsprytDisableUploadRing` kill-switch immediate `UploadRangeNow`, and the **default queue-only path that is the Mali WAR-stall fix** (`:1284-1296`, `Managers.h:806-826`). |
| `ResidentSubData` (`:95`) | `BufferSubDataResident` (`:129`, `kOptional`) | the same `MGPSubData` shape; `bytes` is the app's staging store, **valid for the duration of the call only** (`BufferObject.h:84-85`). | nothing. | `Ops_ResidentSubData` verbatim (`:1304-1312`): copies into `pendingResidentWrites` under `pendingMutex`, issues no GL, stays thread-agnostic. Drained by `DrainResidentWritesNow` (`:1084`) at draw sync (`:1670`) or readback (`:1391`). |
| `FlushMappedRange` (`:99`) | `ResourceFlushRange` (`:131`) | `MGPFlushRange{Res, Offset, Size, AccessFlags}` — `AccessFlags` is the **application's real** `Flags<BufferMappingAccessBit>`, not a normalised subset (`MGPipeTypes.h:628`). `bytes` = `MappedData() + Offset`. | nothing. | `Ops_FlushMappedRange` verbatim (`:1314-1379`), including the kill-switch-only arm that honours `InvalidateRange`/`InvalidateBuffer`/`Unsynchronized` (`:1353-1375`) and the adopted-store stamp-and-return (`:1334-1337`). |
| `OnDestroy` (`:103`) | `ResourceDestroy` (`:77`) | `MGPHandleOnly{Handle, Kind = Buffer}`, emitted from `~BufferObject` (`BufferObject.cpp:39-43`) **before** the client frees the slot. | clears Live, drops the record; then the **client** calls `MGPipeSlots().Free(MGPipeKind::Buffer, handle)`. | `Ops_OnDestroy` verbatim (`:1417-1447`) with the moved `SharedPtr<BackendBufferResource>` replaced by the server-side slot table's own entry: the three outcomes (stale generation → zero the id; on-thread → `IsPoolable`/`EnrollIntoPool` else scrub+`glDeleteBuffers`; off-thread → `g_deferredBufferReleases` + the lock-free flag) are unchanged. |
| `AcquirePersistentMap` (`:114`) | `MapPersistent` (`:78`, `kReplySlot\|kOptional`) / `UnmapPersistent` (`:79`) | `MGPHandleOnly` plus `size` and `seedBytes` (the shadow, still live at this point). | bumps `MapPersistentRoundtrips` (D-B2). | **`Ops_AcquirePersistentMap` (`:1134-1210`) is untouched below its signature** — D-B4, `ARCHITECTURE.md:466`. See D-E. |
| `ReadbackFromGpu` (`:121`) | `ResourceReadback` (`:132`, `kReplySlot`) | `MGPReadback{Res, Offset = 0, Size = GetSize()}` — the contract is whole-buffer (`BufferObject.h:115-116`). | nothing; the answer travels back (D-D). | `Ops_ReadbackFromGpu` verbatim (`:1382-1419`) except that `bufferObject.WritebackFromBackend(...)` (`:1410`) becomes the reverse callback of D-D. Both branches survive: the adopted-store `DrainResidentWritesNow` + `glFinish()` (`:1387-1394`) and the map/`WritebackFromBackend`/unmap path preceded by `FlushPendingRangesNow` (`:1404`). |

#### D-A3 — `BindMask`, and the one bit P8 keys on

`MGPResourceDesc::BindMask` (`MGPipeTypes.h:154-157`) is
`VERTEX|INDEX|CONSTANT|SHADER_BUFFER|INDIRECT|SAMPLER|SHADER_IMAGE|RENDER_TARGET|DEPTH_STENCIL|STREAM_OUTPUT|ATOMIC|ELEMENT_ARRAY`.
**The `ELEMENT_ARRAY` bit is the D-B7 switch: with `kCapNeedsHostIndexBytes` set the server mirrors this resource**
(`ARCHITECTURE.md:385`). Getting it wrong silently disables restart rewriting and multi-draw flattening in split, and it
is invisible in monolith — no P3a gate can see it.

**Decision:** the client maintains a per-`BufferObject` sticky `everBoundAs` mask, ORed at every `glBindBuffer` /
`glBindBufferBase` / `glBindBufferRange` / VAO element-slot bind / `glVertexAttribPointer` with a bound
`GL_ARRAY_BUFFER`, and **`BindMask` is emitted on both `ResourceCreate` and every `ResourceRespecify`** (P8 expectation
1). It is sticky, never cleared, exactly like `ImageBindableHint`'s `everImageBound` (`MGPipeTypes.h:165`). The
`BufferTarget` → bit mapping is a single `constexpr` table in `MG_Impl/Pipe/ResourceTracker.h` with a
`static_assert` that it covers every `BufferTarget` enumerator, so a new target cannot be silently unmapped.

**Test that can see it in monolith:** `ResourceDescriptorTest.EveryBufferTargetSetsItsBindMaskBit` walks every
`BufferTarget`, binds a buffer to it, and asserts the applier's stored descriptor carries the bit — on both
`ResourceCreate` and a following `ResourceRespecify`.

#### D-A4 — The server-side resource record, and what identity replaces

`GLESBufferResource` (`Managers.h:592-644`) moves out of `PipeResource::m_backend` and into the **seventh** Espryt slot
table, `BufferImpl::g_backendBufferResources` of kind `MGPipeKind::Buffer`. It is the first table keyed by a
**client-minted handle carried in the call**, rather than resolved from a `SharedPtr`'s `GetLifetimeId()` — which
discharges, for these two kinds, the debt `SlotTables.h:70-77` records against itself
(*"this header is under `MG_Backend/` and it mints handles … the minting and the resolution belong on the client"*).
`BackendSlotTable` gains a `GetOrCreate(MGPipeHandle)` / `FindByHandle(MGPipeHandle)` overload pair that never calls
`MGPipeSlots()`.

Fields of `GLESBufferResource` that change owner:

| field | `Managers.h` | P3a |
|---|---|---|
| `id`, `storageSize`, `storageInitialized` | `:598-600` | unchanged, server-side |
| `contextGeneration` vs `g_bufferContextGeneration` | `:603`, `Managers.cpp:646`, bumped `:1529` | unchanged, server-side |
| `std::atomic<Uint64> syncedChangeSerial`, mirroring the frontend's `GetChangeSerial()` | `:608` | **re-keyed**: mirrors the applier's `ResourceRecord::Serial`, a server-owned `Uint64` incremented on every `ResourceRespecify` / `ResourceSubData` / `ResourceFlushRange` / `BufferSubDataResident` the applier applies. It is an `MGGen`-class counter (`ARCHITECTURE.md:44-49`) and **never crosses the line**. |
| `pendingRespecify`, `pendingRanges`, `pendingResidentWrites`, `pendingMutex` | `:614-625` | unchanged, server-side |
| `drawCleanEpoch` | `:631` | unchanged, server-side |
| `persistentMapped`, `persistentPtr`, `immutableStorage` | `:637-644` | unchanged, server-side |

`IsBufferDrawClean` (`Managers.cpp:1579-1601`, decl `Managers.h:665-675`) today reads five frontend facts per draw per
buffer — `GetBackendResource()`, `IsMapped()`, `GetSize()`, `GetChangeSerial()`. Under P3a it reads: the slot table
entry (identity), `record.Desc.Width` (size), `record.Serial` vs `resource->syncedChangeSerial`, and
`record.HasLiveHostWrites` for the map state. **`HasLiveHostWrites` is stored on the record, is always `false` in P3a,
and is written by nobody** — it exists so P5's client-side persistent-map push can set it with *zero new record kinds*
(`ARCHITECTURE.md:481`, P8 expectation 5). A `MOBILEGL_PIPE_VERIFY` assertion pins that it is false, so P5 cannot land
a silent semantic change under it.

**`g_bufferBackendIdGeneration` (`Managers.cpp:1503`, decl `Managers.h:701-707`) survives untouched and stays
server-local.** It answers "did *I* re-mint the driver id" — a server-owned `MGGen` — and it is the *only* thing that
catches a re-mint at persistent-map adoption (`:1173`) or immutable-store retire (`:1639`) that moves no client-side
version. A `set_vertex_buffers` keyed on the client content hash alone reintroduces the bug commit `d7655247` fixed
(scout-espryt-vao trap 1). D-G4 keeps it in the VAO twin's gate.

#### D-A5 — `kNeedsAck`, first use in the tree, and why it is not a bare flag

`ARCHITECTURE.md:293`: *"**唯一允许同步 ack 的入口是 `glBufferStorage`（真同步分配）** … 目录里目前没有条目携带
`kNeedsAck`（`ResourceRespecify` 是 `kNone`），标记随 P3a 的 buffer 路径落地."* `grep -rn kNeedsAck` confirms no
`X(...)` row carries it; it is defined at `MGPipe.h:42` and named in `PipeCalls.def:18`'s legend and
`Doorbell.h:19`'s prose only.

Flags are a **per-call** static property, and `ResourceRespecify` serves both `glBufferData` and `glBufferStorage`. A
bare flag would ack every `glBufferData` in Minecraft's chunk upload.

**Decision:** P3a adds `kNeedsAck` to `ResourceRespecify`'s flag word **and** lands the per-record predicate that
decides:

```cpp
// MGPipeTypes.h, next to MGPipeSetSubDataBufferRange.
// kNeedsAck on a call means "records of this call MAY require an acknowledgement; this
// predicate decides per record". glBufferStorage is a real synchronous allocation and is the
// only entry allowed one (ARCHITECTURE.md:293); glBufferData through the same call is not.
inline Bool MGPipeResourceRespecifyNeedsAck(const MGPResourceDesc& desc) { return desc.Immutable != 0; }
```

`PipeCalls.def:18`'s legend gains that sentence in the same commit. In monolith the ack is `((void)0)` — the applier is
one function call away. P5 wires the doorbell to the predicate. `PipeCatalogueTest` gains
`ResourceRespecifyAcksOnlyImmutableStorage`, which pins the predicate against both idioms and is the negative control
for a future flag that over-acks. Opcodes do **not** move: a flag-word edit changes the generated tables, so
`python3 scripts/gen_pipe.py` + `git diff --exit-code -- MobileGL/MG_Pipe/generated` is mandatory in the same commit.

### D-B — `buffer_subdata_resident` nullability, and the `map-persistent-roundtrips` counter

#### D-B1 — `BufferSubDataResident` stays nullable and stays asymmetric

`ARCHITECTURE.md:94`: *"`kOptional`(O，后端表里可为 null：Magma 故意不注册 `BufferSubDataResident` 与
`SetSwapInterval`)."* Verified in the tree: `g_vulkanBufferBackendOps` (`DirectVulkan/Renderer/VkBufferManager.cpp:105-112`)
sets `.Respecify`, `.SubData`, `.FlushMappedRange`, `.OnDestroy`, `.AcquirePersistentMap`, `.ReadbackFromGpu` and
**omits `.ResidentSubData`**. `ROADMAP.md:83` (open question 10) is a standing hands-off: *"null 项保住今天的行为；给
Magma 补真实现是行为变更，独立 `dev` PR."*

**Decision:** `MGPipeResourceOps::SubDataResident` may be null, the frontend checks it exactly as it checks
`g_bufferBackendOps->ResidentSubData` today (`BufferObject.cpp:371` `LandBytesIntoResidentStore`), and **P3a does not
give Magma an implementation**. `kCapResidentSubData` (`MGPipeTypes.h:110`, bit 2) is the split-mode spelling of the
same fact and is not wired in P3a.

#### D-B2 — `map-persistent-roundtrips`: definition, home, and why it is non-zero in monolith

`ROADMAP.md:19` demands `StorageBufferRegrow` **publish** it; `ROADMAP.md:21` (P5) and `:28` (P11) demand it again;
`ARCHITECTURE.md:474` sets its meaning: T1 costs *"**每次存储定义一次 round trip**（不是每 store 一次）"*.

A counter defined as "round trips actually taken" is 0 by construction in monolith and can never go red — which
violates `ROADMAP.md:7`'s **每个门必须能因它存在的理由变红**.

**Decision — and this is a correction to the reading in `scout-docs-spec.md` §1.2(b):**
**`map-persistent-roundtrips` counts every `MapPersistent` emission, i.e. every acquisition attempt (mint *or*
decline), because every one of them requires an answer from the resource owner.** In monolith that answer is free; in
split it is a real round trip. The number is therefore identical in both modes and equals "one per storage definition"
exactly as `ARCHITECTURE.md:474` requires, it is **non-zero and assertable today**, and a regression that acquires
per-draw instead of per-definition shows up immediately.

Home: `MG_Util/Metrics/PipeStats.h`, `enum class CallClass`, new member `MapPersistentRoundtrips`, short name **`mpr`**,
**inside the existing `#if MOBILEGL_PIPE_PUSH` block at `:101-116`** — for the reason that block already states: growing
the enum in a pull build resizes the counter arrays, the name table and `FormatWindowLine`, which is a G1 break.
`PipeStats.cpp:173-179`'s `kCallClassNames` gains `"map-persistent-roundtrips"`; `FormatWindowLine` prints
`mpr=<n>` beside `csom`/`csob` (`PipeStats.cpp:424-427`); `MG_Test/Util/PipeStatsTest.cpp` pins the name in
`CounterNamesAreStable` (`:260`) and `SummaryLineCarriesEveryClassAndGate` (`:137`).

The counting site is the **client** emitter (`MG_Impl/Pipe/ResourceTracker.h`), one `AddCalls` behind the standard
`if (PipeStats::Enabled())` shape (`PipeStats.h:29`) so an off build pays one predicted branch. **No timer anywhere**
(`ROADMAP.md:7`, `Tracker.h:35-38`).

### D-C — `resource_flush_range`

`MGPFlushRange` (`MGPipeTypes.h:628-635`, 32 B) is landed and unchanged:

```cpp
// Carries the application's REAL access flags, not a normalized subset.
struct MGPFlushRange { MGPipeHandle Res; Uint64 Offset, Size; Uint32 AccessFlags; Uint32 Pad0; };
MGP_ASSERT_POD(MGPFlushRange, 32);
```

`AccessFlags` is `Flags<BufferMappingAccessBit>` **as the application passed it** — the frontend must not merge
`INVALIDATE_RANGE` / `INVALIDATE_BUFFER` / `UNSYNCHRONIZED` before emitting, because Espryt's kill-switch arm
(`Managers.cpp:1353-1375`) reads them per call to choose between `glMapBufferRange(WRITE | INVALIDATE_RANGE? |
UNSYNCHRONIZED?) + Memcpy + glUnmapBuffer` and `UploadRangeNow`. `NotifyFlushMappedRange` (`BufferObject.cpp:63-70`)
already has them; the emitter passes them through with a `static_assert` that
`sizeof(Flags<BufferMappingAccessBit>) <= sizeof(Uint32)`.

The default (ring-enabled) arm still just queues the range (`:1344-1348`), the adopted-store arm still stamps and
returns (`:1334-1337`), and `FlushPendingRangesNow`'s three-tier drain (`:1004-1072`) is a G5-protected function:
tier 1 `glMapBufferRange(WRITE | INVALIDATE_BUFFER|INVALIDATE_RANGE)` + memcpy for a whole-buffer flush or a range
`>= kInvalidateRangeMinBytes` (128 KiB, `:975`); tier 2 upload ring + `glCopyBufferSubData`; tier 3 `UploadRangeNow`.
**Ranges are flushed as queued, never collapsed to their union** (`:1000-1003`) — collapsing re-copies whole chunk
arenas.

### D-D — `resource_readback` and its reverse-channel reply

`MGPReadback{MGPipeHandle Res; Uint64 Offset, Size;}` (`MGPipeTypes.h:637-641`, 24 B), flagged `kReplySlot`
(`PipeCalls.def:132`) = *"答进 `MGPReplySlot`，永不阻塞"* (`ARCHITECTURE.md:94`).

Espryt's `Ops_ReadbackFromGpu` today ends in `bufferObject.WritebackFromBackend({mapped, size}, 0)`
(`Managers.cpp:1410`) — the backend reaching into the frontend's address space. Under handles that is
`MGPipeCallbacks::OnBufferWriteback(MGPipeHandle res, Uint64 offset, MGPBlobRef bytes)`
(`MGPipeCallbacks.h:32`), which is landed and unused.

**The applier-side reply, in exactly this order** (`ARCHITECTURE.md:288` is a correctness requirement, not a
preference — *"写回的 epoch bump 必须在任何后续读该 handle 的命令之前被 server 应用。**反向通道需要与正向通道相同的有序保证。**"*):

1. Espryt maps the store read-only and produces the bytes.
2. Espryt invokes `OnBufferWriteback(res, 0, {Seg = kMGHostSpanSegNone, Offset = (address), Size})`.
3. The **client**'s implementation of that callback resolves `res` → `BufferObject*` and calls
   `WritebackFromBackend({bytes, size}, offset)`.
4. Espryt unmaps, stamps `syncedChangeSerial` (`:1413-1418`) and calls `BumpBufferMutationEpoch()` — **the epoch bump
   stays server-side and happens after the writeback, never before**.
5. The reply slot is stamped; in monolith the whole sequence is synchronous inside
   `MGPipeApplyResourceReadback`, so `SyncGpuWrites`'s caller sees the reconciled shadow on return, exactly as today.

**The handle → object resolution is the client's**, and it is new: `MG_Impl/Pipe/ResourceTracker.h` keeps a
per-kind `Vector<BufferObject*>` indexed by `MGPipeHandle::Slot`, written at `ResourceCreate` and cleared at
`ResourceDestroy`. A **raw** pointer is exact here and a `WeakPtr` would be wrong: the entry exists only between
create and destroy, both of which are emitted from the object's own constructor and destructor, and a readback is only
ever issued for a live, bound buffer. A `MGPipeHandle::Gen` compare against the allocator (`MGPipeSlots().GenOfSlot`)
guards a stale handle in debug builds.

**The other reverse callback P3a wires: `OnGpuWritten(res, rangeCount, ranges)`** (`MGPipeCallbacks.h:30-31`),
replacing the three Espryt `MarkGpuWritten` sites (`DirectGLES.cpp:500`, `:544`, `:2012`). `ARCHITECTURE.md:274` calls
it a **narrowing** channel — *"client 在每个 draw/dispatch 发射点**保守自建** pending 集"* — so the client marks
conservatively at emission and the callback only ever removes. In P3a the client's conservative set is exactly what
`MarkShaderStorageBuffersGpuWritten` / `SyncAtomicCounterBuffers` / `MarkWritableImageBufferTexturesGpuWritten` mark
today, so the observable behaviour is identical and P8/P9's narrowing has its channel (P8 expectation 4).

The four other writeback shapes stay on `WritebackFromBackend` for now and are **explicitly out of scope**: the
`glReadPixels` PBO path (`DirectGLES.cpp:9406-9468`), the `glGetTexImage` PBO path (`:9832-9880`),
`StoreReadbackRowsToClient` (`:7876-7906`) and `Utils.cpp:2297-2354`, and the XFB capture/scatter pair (`:838-878`,
`:918-985`). They are texture- and XFB-shaped, they are P4a/P8/P9's, and each already pairs its writeback with a
`BumpBufferMutationEpoch()` (`:872, 978, 7903, 9462, 9872`; `Utils.cpp:2352`).

### D-E — `map_persistent`: the call exists, the implementation is not touched

`ARCHITECTURE.md:466` (D-B4), verbatim: *"`AcquirePersistentMap` 是永久的地址空间捐赠（返回 host-visible coherent
指针，成为该 buffer 的唯一真相源；≥16 MiB 可变 store 由 `TryAdoptLargeStorage` 自动走到，实测 MC 26.3 p99 163→21 ms、
40→115 fps、省 ~400 MB）。**整个 monolith 改造期一动不动**（D-B4），只有 IPC 那一步会打破它。"*

**Decision: `Ops_AcquirePersistentMap` (`Managers.cpp:1134-1210`) changes in exactly two ways and in no other.**
(1) Its signature becomes `(MGPipeHandle res, Uint64 size, const void* seedBytes)`. (2) The three reads it made through
`bufferObject` become those parameters: `bufferObject.GetSize()` (`:1139`) → `size`; `bufferObject.MappedData()`
(`:1184`, the `glBufferStorageEXT` seed) → `seedBytes`; `bufferObject.SetBackendResource(...)` (`:1146`) → the slot
table's `GetOrCreate(res)`. Every other statement — the four-way capability gate (`:1135-1138`), the stale-generation
wipe **before** the new stamp (`:1151-1160`), the idempotency hit (`:1163-1165`), the fresh-id sequence with
`++g_bufferBackendIdGeneration` (`:1169-1179`), the locally defined `0x0040/0x0080/0x0100` bit constants (`:1130-1132`),
`immutableStorage = true` set as soon as the store exists (`:1188`), the `MGLOG_E_ONCE` decline (`:1189-1197`) and the
success stamps (`:1198-1209`) — is byte-identical. The tracked wrapper still bumps the epoch **even on decline**
(`:1470-1476`).

Nothing of the tiering work is inherited: T0/T1/T2/T3, the POST probe, `SEG_ADOPT`, `MOBILEGL_IPC_ADOPT_TIER` and the
client-side push triplet are **P5/P11** (`ARCHITECTURE.md:468-486`, `ROADMAP.md:28`). P3a routes the call, publishes
`map-persistent-roundtrips`, and stops.

The frontend side is likewise untouched: `PipeResource::AdoptPersistentMap` (`PipeResource.h:115`) still drops the CPU
shadow; `TryAdoptLargeStorage` (`BufferObject.cpp:186-196`) keeps its 16 MiB threshold and its
`MG_Config::Features.DisableLargeBufferAdoption` kill switch; `MOBILEGL_COHERENT_AS_FLUSH` keeps working
(`ARCHITECTURE.md:485` — the two `coherent_as_flush: true` Create fixtures must take the same buffer path in both
modes, which is what makes G3b's name-by-name compare meaningful).

### D-F — Pools and deferred release move as-is, and G5 makes that literal

`ARCHITECTURE.md:316` puts *"三条 persistent-mapped ring 与 `PersistentRing` 算法、**buffer pool**"* in §9.1's
do-not-touch list, and `:515` explains what protects them: *"**`present` 与 `eglSwapBuffers` 严格 1:1**：… Espryt
三个 ring 与 `TrimBufferPool` 的 retire 只在 `Present` 内发生，批量会饿死它们."*

**Decision: nine functions in `Managers.cpp` are byte-identical after P3a, and G5 checks it by sha256.**

| function | line | why it survives verbatim |
|---|---|---|
| `IsPoolable(const GLESBufferResource&)` | `:700` | takes the server-side resource, never the frontend object |
| `EnrollIntoPool(GLESBufferResource&)` | `:711` | same; the `retireSerial = CurrentFrameSerial() + 1` stamp (`:737-739`) is load-bearing and explained in place |
| `AcquireFromPool(SizeT)` | `:740` | hands back only entries with `retireSerial <= CompletedFrameSerial()` (`DirectGLES.cpp:10859-10860`) |
| `TrimBufferPool()` | `:1882` | called once per frame from `Present` (`DirectGLES.cpp:10928`) |
| `ClearBufferPool()` | `:1913` | context loss |
| `ProcessDeferredBufferReleases()` | `:1542` | drained per draw from `SyncNeccessaryBuffers`/`SyncComputeBuffers`, fast-outs on the atomic `g_hasDeferredBufferReleases` flag |
| `CreateRingStorage` | `:1930` | `glBufferStorageEXT` + persistent|coherent map, retires the old store at `CurrentFrameSerial()+1` |
| `RingAvailable` | `:1989` | self-heals a stale context generation via `ResetRingForNewContext` (`:866-876`) |
| `RingAllocate` | `:2016` | the fast path is on the hot upload route |

The budget constants (`kMaxPoolableBufferBytes` 8 MiB, `kMaxPoolBytes` 64 MiB, `kMaxEntriesPerBucket` 32,
`Managers.cpp:696-698`), `struct PooledBuffer` (`:687`), `g_bufferPool` (`:693`) and `g_deferredBufferReleases`
(`:672`) are likewise untouched. The `EnsureBufferResource` pool reseed path (`:1657-1685`) changes only its source of
bytes (`bufferObject->MappedData()` → the applier's stored `initialBytes` / the shadow pointer the client last sent),
and is **not** in the G5 set for that reason.

**Recorded but not fixed:** `OnBackendContextDestroyed` (`:1537-1538`) resets `g_uboRing` and `g_unpackRing` but not
`g_uploadRing`; `RingAvailable` (`:1996-1998`) heals it on first use, so it is benign. **P3a preserves the asymmetry
deliberately** and records it as a `dev`-side follow-up rather than fixing it in flight (`ROADMAP.md:88`'s standing
rule about not taking unrelated fixes into the split work).

### D-G — Vertex elements: three calls, both views, and how the CSO is keyed

#### D-G1 — [deviation] The vertex-elements CSO is identity-addressed in P3a, not content-addressed

`ARCHITECTURE.md:63` puts vertex-elements CSOs in the client-side content-addressed scheme with capacity 1024, and
`:55` names `VertexInputStateFactory::m_cache` as the server-side counterpart.

**That is right for Magma and wrong for Espryt, and P3a is Espryt-only.** Espryt has no vertex-elements CSO at all: it
has a **per-VAO twin** (`BackendVertexArrayObject`, `Managers.h:826-955`) that owns one driver VAO name
(`m_backendVAOId`, `:913`), 32 client-array scratch buffer ids (`:914`), 32 converted-fp64 scratch ids (`:918`) and
their memos (`:919-920`). Two frontend VAOs with identical *format* cannot share that object, because a driver VAO also
holds the element-array binding and the per-attribute buffer bindings that `set_vertex_buffers` / `set_index_buffer`
re-emit into it. A shared CSO would force those to be re-emitted on every `BindVertexElements` — strictly more work
than today.

**Decision:** in P3a a `VertexElementsCso` handle is minted **per frontend `VertexArrayObject`**, off `GetLifetimeId()`
through `MGPipeSlots().Acquire(MGPipeKind::VertexElementsCso, lifetimeId)` **on the client**, and freed on the VAO's
existing death notice (`VertexArrayObject.cpp:53`, consumed at `Managers.cpp:209-211`). `CreateVertexElements` is
**re-issued on the same handle** whenever the configuration moves — legal, because `MGPipeHandle::Gen` increments only
on slot reuse and never on respecify (`MGPipeHandles.h:44-48`). The client-side content-addressed vertex-elements cache
and its 1024 capacity are **not built in P3a**; they are recorded as **P7** work, when Magma's
`VertexInputStateFactory` takes the CSO over and content addressing is what its `VkPipelineVertexInputStateCreateInfo`
actually wants. The integrator annotates `ARCHITECTURE.md:63` accordingly.

Consequences that make the package smaller, and they are the point: no hash, no probe, no `memcmp`, no LRU, no
`DeleteVertexElements`-on-evict, and no second twin resolve per draw — which also protects
`BackendSlotTable`'s single-entry `m_memoLifetimeId`/`m_memoHandle` front memo (`SlotTables.h:407-431`), whose comment
names `ResolveVaoTwin` as one of the two callers it exists for (scout-espryt-vao trap 7).
`DeleteVertexElements` is emitted from **one** place: the death notice.

#### D-G2 — The two views, and what they carry on the wire

`MGPVertexElements` (`MGPipeTypes.h:253-259`, 40 B) is unchanged:

```cpp
struct MGPVertexElements { MGPipeHandle Cso; Uint32 AttributeCount; Uint32 BindingPointCount; MGPBlobRef Blob; };
MGP_ASSERT_POD(MGPVertexElements, 40);
```

`ARCHITECTURE.md:130` demands the blob carry the resolved `VertexAttribute[]` **and** `VertexBufferBindingPoint[]`.
Neither can travel as the `MG_State::GLState` struct: `VertexAttribute` holds a `SharedPtr<BufferObject> Buffer`
(`MGPipeValueTypes.h:516`) and `VertexBufferBindingPoint` holds one too (`:533`), and **a payload never contains a
pointer** (`ARCHITECTURE.md:116`). P3a therefore adds two POD wire forms to `MGPipeValueTypes.h`:

```cpp
// The resolved flat attribute view. Buffer identity does NOT travel here - it travels in
// set_vertex_buffers, which is what keeps this record stable while buffers change under it.
// Stride is the RESOLVED distance and a surviving 0 can only have come from the binding model
// (MGPipeValueTypes.h:496-503); collapsing it back into the element size is what made
// KHR-GL43.vertex_attrib_binding.basic-input-case7/8 read past the buffer.
struct MGPVertexAttribWire {
    Uint64 Offset;       //  0
    Int32  Stride;       //  8
    Uint32 Type;         // 12  DataType
    Uint8  Size;         // 16  1..4; GL_BGRA keeps 4
    Uint8  Enabled;      // 17
    Uint8  Normalized;   // 18
    Uint8  IsInteger;    // 19
    Uint8  IsLong;       // 20  CARRIED SEPARATELY from Type == Float64 (ARCHITECTURE.md:130)
    Uint8  IsBgra;       // 21
    Uint8  BindingIndex; // 22  which MGPVertexBuffer entry feeds it (< MAX_VERTEX_ATTRIBS = 32)
    Uint8  Pad0;         // 23
};
MGP_ASSERT_POD(MGPVertexAttribWire, 24);

// The ARB_vertex_attrib_binding view. Buffer identity is again in set_vertex_buffers.
struct MGPVertexBindingPointWire {
    Uint64 Offset;  // 0
    Int32  Stride;  // 8   GL 4.6 core table 23.4: the INITIAL value is 16, not 0
    Uint32 Divisor; // 12
};
MGP_ASSERT_POD(MGPVertexBindingPointWire, 16);
```

`Divisor` is **not** in the attribute view: it is resolved per binding point and travels in
`MGPVertexBuffer::Divisor`, which is where Espryt's `glVertexAttribDivisor(attribIndex, …)` (`Managers.cpp:2613-2615`)
now reads it. `LegacyStride`/`LegacyPointer` (`MGPipeValueTypes.h:518-526`) **stay client-side** — they are the
`glGetVertexAttrib*` query answers and nothing but the query path reads them (`ARCHITECTURE.md:130`).

Blob layout: `MGPVertexAttribWire[AttributeCount]` immediately followed by
`MGPVertexBindingPointWire[BindingPointCount]`, both in ascending index order, `AttributeCount` and
`BindingPointCount` each ≤ `VertexArrayObject::MAX_VERTEX_ATTRIBS` (32). The applier refuses a record whose declared
counts do not match the blob's declared size — `Fatal{ProtocolCorruption}` (`ARCHITECTURE.md:119`).

**Why the second view travels even though nothing reads it.** `grep -rn 'VertexBufferBindingPoint|
GetAttributeBindingIndex|GetAttributeRelativeOffset' MobileGL/MG_Backend/` returns **zero hits** — neither backend has
ever read a binding point, and `MGPipeValueTypes.h:529-531` says so as a design statement. So
`ARCHITECTURE.md:130`'s stated reason (pointer stride 0 vs binding stride 0) is stale: the frontend already resolves
it. The reason that survives: `MGPVertexElements` **declares** `BindingPointCount`, `PipeFields.def:71-72` names it,
and a record whose declared counts do not describe its own blob is a shape the applier's bounds gate would have to
police forever. Carrying both keeps the record self-describing, and the cost is paid **once per configuration change,
not per draw** — the CSO is a create/bind pair, and the blob rides only on `CreateVertexElements`. The integrator
corrects `ARCHITECTURE.md:130`'s stated reason to this one and records the measured per-frame blob bytes in
`MEASUREMENTS.md`; trimming the second view is a P13 retune item, not P3a's.

#### D-G3 — What the tracker emits, and when

Dirty bit 5 `NewVertexElements` already has its exact shutter and it does not change:
`vaoIdentity = MGPipeMixShutter(vao->GetLifetimeId(), vao->GetConfigVersion())`, 0 when unbound
(`Tracker.h:210-213`). It fires both when the bound VAO changed and when the bound VAO's configuration changed, so the
emitter keeps two latches and distinguishes:

```
if (vao == nullptr)                                   -> BindVertexElements(kMGPipeNullHandle)
else if (lifetimeId != m_lastBoundLifetimeId)         -> (re)Create if configVersion moved, then Bind
else if (configVersion != m_lastEmittedConfigVersion) -> CreateVertexElements on the same handle (no rebind)
```

`m_lastEmittedConfigVersion` is stored **per handle**, not globally, in `MG_Impl/Pipe/VertexInputEmit.h`'s
slot-indexed table, so ping-ponging between two VAOs does not re-create either. A `Uint32` config version
(`VertexArrayObject.h:107`) does not wrap in any realistic run and is compared directly; the tracker's
`MGPipeWidenedCounter` (`Tracker.h:159-177`) is **not** needed for it.

Order (`ARCHITECTURE.md:194`, D-B3): all `set_*`/`bind_*` of a verb complete before the verb; between them the only
ordering requirement is *"资源 create 先于对它的 bind"*. The recommended emission order's vertex segment is
*"… render state/dynamic → **vertex elements/buffers/index/attrib defaults** → patch/XFB → verb"* and it is code
organisation, not contract. P3a emits in exactly that order inside `MGPipeValidateForVerb`'s step 3
(`PipeFill.cpp:1087-1128`), after the four P2 emitters.

#### D-G4 — What the applier stores, and what it retires in the twin

`MGPipeApplierState` (`PipeApply.h:48-87`) gains, all per context:

```cpp
struct MGPipeResourceRecord {                 // indexed by MGPipeHandle::Slot, kind Buffer; slot 0 never live
    Uint32 Gen; Bool Live;
    MGPResourceDesc Desc;                     // the last create/respecify, verbatim
    Uint64 Serial;                            // server-owned MGGen: ++ on every applied mutation
    Bool HasLiveHostWrites;                   // always false in P3a; P5 sets it (ARCHITECTURE.md:481)
};
struct MGPipeVertexElementsRecord {           // indexed by MGPipeHandle::Slot, kind VertexElementsCso
    Uint32 Gen; Bool Live;
    Uint32 AttributeCount, BindingPointCount;
    Array<MGPVertexAttribWire, 32>       Attributes;
    Array<MGPVertexBindingPointWire, 32> BindingPoints;
    Uint64 ContentSerial;                     // server-owned MGGen: ++ on every CreateVertexElements
};
Vector<MGPipeResourceRecord>       Resources;
Vector<MGPipeVertexElementsRecord> VertexElementsCsos;
MGPipeHandle             BoundVertexElements;
Array<MGPVertexBuffer, 32> VertexBuffers; Uint32 VertexBufferStart, VertexBufferCount;
Uint32 VertexFetchBaseInstance;               // the explicit field of D-H
Uint64 VertexBuffersSerial;                   // server-owned MGGen: ++ on every SetVertexBuffers
MGPIndexBuffer IndexBuffer; Uint64 IndexBufferSerial;
Uint64 MapPersistentRoundtrips;
```

The three `Serial` counters are **`MGGen`-class: server-owned, monotone `Uint64`, and they never cross the line**
(`ARCHITECTURE.md:44-49`; the norm is *"任何 MGPipe 调用不得要求 client 提供或知晓 `MGGen`"*). They are what retires
the twin's wrapping-`Uint16`-plus-identity patches. `BackendVertexArrayObject::SyncToBackend`'s gate
(`Managers.cpp:2437-2472`) becomes:

| today | after P3a |
|---|---|
| `m_hasSyncedConfigVersion` + `m_syncedConfigVersion` (`Managers.h:937-938`) vs `stateVAOObject->GetConfigVersion()` | `m_syncedElementsHandle` + `m_syncedElementsSerial` vs `BoundVertexElements` + `rec.ContentSerial` |
| `Array<VertexAttributeVersion,32> m_syncedAttributeVersions` (`:939-940`) | **deleted** — the applier's stored `Attributes[]` *is* what was last pushed, so a per-attribute version compare has nothing left to prove |
| `m_syncedIndexBufferVersion` (`Uint16`, `:926`) + `m_syncedIndexBufferObject` (raw pointer, `:931`) | `m_syncedIndexSerial` vs `IndexBufferSerial` — one monotone `Uint64`, no wrap, no identity patch. **This is the Track H re-key `ARCHITECTURE.md:363` counts.** |
| `m_syncedFetchBaseInstance` (`:946`) + `baseInstanceDirty` (`Managers.cpp:2462-2464`) | **deleted from the gate**: a baseInstance change is a `ContentHash` input (D-H), so it moves `VertexBuffersSerial`. The field survives as the record of *what was last emitted*, which is what the next sync must correct — `Managers.h:941-946`'s note holds verbatim. |
| `m_syncedBufferIdGeneration` (`:952`) vs `BufferImpl::g_bufferBackendIdGeneration` (`:2446-2447`) | **unchanged**. Server-local, and the only thing that catches a driver-id re-mint no client counter moves. |
| `m_hasConvertedFloat64Attribute` (`:924`, set `:2530`, cleared `:2473`) | **unchanged**. A narrowed fp64 stream is derived from buffer *content*, which no version covers, so the early-out must not be trusted while one is live (scout trap 3). |

The per-attribute walk (`Managers.cpp:2477-2617`) reads `rec.Attributes[i]` and `st.VertexBuffers[i]` instead of
`GetAllAttributeVersions()` / `GetAllAttributes()`. **Its every branch survives**: the `SwitchVersion`
enable/disable block, the fp64 narrowing with its Adreno disable (D-I), the zero-stride binding-API path
(`:2545-2560`), `BindAttributeBuffer` (`:2330-2346`) resolving the driver id from the slot table instead of
`attrib.Buffer`, the `glVertexAttribPointer`/`IPointer` at `fetchOffset` (`:2586-2600`), the BGRA refusal probe
(`:2568-2611`) and `glVertexAttribDivisor` (`:2613-2615`).

`ResolvedDrawBuffers` (`Managers.h:833-869`) keeps its shape and re-keys: `Entry::frontend` (a raw `BufferObject*`)
becomes `MGPipeHandle`, `iboFrontend` likewise, and `configVersion` becomes the pair
`{elementsHandle, elementsSerial}`. `vboCleanEpoch`/`iboCleanEpoch` against `CurrentBufferMutationEpoch()` and the
per-entry `IsBufferDrawClean` re-probe (`DirectGLES.cpp:605-621`) are unchanged.

### D-H — `set_vertex_buffers` with an explicit `baseInstance`

#### D-H1 — The payload change

`MGPVertexBuffers` (`MGPipeTypes.h:367-371`) goes from 16 to **24** bytes:

```cpp
// Var-tail header: MGPVertexBuffer[Count] follows.
struct MGPVertexBuffers {
    Uint32 Start, Count;
    // The vertex-FETCH base instance these offsets are valid for. Draw state, not VAO state
    // (Managers.h:941-946), and NOT the same thing as MGPDrawInfo::StartInstance, which is the
    // GL draw's baseInstance and feeds gl_BaseInstance. The server decides whether to emulate
    // the fetch shift or let GL_EXT_base_instance do it (ARCHITECTURE.md:112: emulation is
    // server-owned). It is a ContentHash input - see MGPipeTypes.h's suppressor note.
    Uint32 BaseInstance;
    Uint32 Pad0;
    Uint64 ContentHash;
};
MGP_ASSERT_POD(MGPVertexBuffers, 24);
```

`MGPVertexBuffer` (the per-entry struct, `:356-364`, 32 B) is **unchanged** — its `Pad0` at `:362` stays padding. Per
entry the shift is redundant and would let a malformed record disagree with itself.

Same-commit consequences, none optional: `MGP_FIELDS_MGPVertexBuffers` (`PipeFields.def:99-100`) gains
`F(BaseInstance)` (`gen_pipe.py` asserts every field list names exactly the struct's direct members, `Pad<n>` excluded,
`PipeFields.def:16-19`); `python3 scripts/gen_pipe.py` regenerates `MG_Pipe/generated/*.inc` including G3's wire-record
`static_assert`s; `git diff --exit-code -- MobileGL/MG_Pipe/generated` is the gate. **No opcode moves** — a payload
size change is not a catalogue edit (`PipeCalls.def:20-23`).

#### D-H2 — What replaces the ambient global, and what stays server-side

Today the shift is driven by a **process global**: `g_pendingFetchBaseInstance` (`Managers.cpp:2358`),
`SetPendingFetchBaseInstance`/`GetPendingFetchBaseInstance` (`:2360-2366`), scoped by
`VertexArrayImpl::ScopedFetchBaseInstance` around **exactly three** draw entry points —
`DrawElementsInstancedBaseVertexBaseInstance` (`DirectGLES.cpp:5135`), `DrawElementsInstancedBaseInstance` (`:5172`),
`DrawArraysInstancedBaseInstance` (`:5224`) — with the value from `EmulatedFetchBaseInstance(baseinstance)` (`:5128-5130`),
which is `UseNativeBaseInstance() ? 0 : baseinstance` (`:5122-5124`). It is consumed in the VAO sync at
`Managers.cpp:2463`, `:2524`, `:2589` (`attrib.Offset + BaseInstanceByteShift(attrib, fetchBaseInstance)`) and inside
`SyncFloat64AttributeAsFloat32` (`:2800-2806`). `BaseInstanceByteShift` (`:2377-2383`) is
`baseInstance * attrib.Stride`, only for `Divisor != 0`, zero for a stride-0 array.

**An ambient process global cannot cross a pushed boundary.** Decision, in three parts:

1. **The client emits the draw's raw `baseInstance` in `MGPVertexBuffers::BaseInstance`.** A new one-line client
   setter `MGPipeSetPendingBaseInstance(Uint32)` in `MG_Impl/Pipe/PipeFill.h` is called immediately before
   `MGP_FILL(...)` at the frontend entry points corresponding to those three, and **`MGPipeLeaveVerb` resets it to 0**
   (`PipeFill.cpp:607`). The reset is what keeps
   `DrawParametersScenario.APlainDrawAfterABaseInstancedOneSeesZeroAgain` (`:233`) true.
2. **The server decides whether to shift.** Espryt's applier computes
   `fetchBaseInstance = UseNativeBaseInstance() ? 0 : hdr.BaseInstance` and stores it in
   `MGPipeApplierState::VertexFetchBaseInstance`. `UseNativeBaseInstance()` is a backend capability and stays in
   `MG_Backend` — emulation is server-owned (`ARCHITECTURE.md:112`, `:225`).
   `g_pendingFetchBaseInstance`, `SetPendingFetchBaseInstance`, `GetPendingFetchBaseInstance` and
   `ScopedFetchBaseInstance` are **deleted**, along with the three scope objects at `:5135`, `:5172`, `:5224`.
   `EmulatedFetchBaseInstance` (`:5128`) and `BaseInstanceByteShift` (`Managers.cpp:2377`) survive and move inside the
   applier arm.
3. **`BaseInstance` is a `ContentHash` input, and this is a hard requirement, not a nicety.**
   `MGPipeSuppressorSlot::SetVertexBuffers` (`SetHashSuppressor.h:37`) suppresses on an unchanged hash. If
   `BaseInstance` moved but the buffer set did not, and the hash did not include it, the emission would be suppressed
   and the server would keep the previous shift — exactly the bug `baseInstanceDirty` (`Managers.cpp:2462-2464`)
   exists to prevent. The hash is `xxHash(MGPVertexBuffer[Count]) ⊕ mix(Start, Count, BaseInstance)`; hash 0 stays
   reserved for "never emitted" (`SetHashSuppressor.h:23-25, 54`).
   Test: `VertexInputEmitTest.ABaseInstanceChangeAloneStillEmitsTheVertexBufferSet`, and its counterpart
   `…AnUnchangedSetWithAnUnchangedBaseInstanceEmitsNothing`.

**Indirect draws are untouched.** None of the three `ScopedFetchBaseInstance` sites is in an indirect loop —
`DrawElementsIndirect` (`DirectGLES.cpp:5216`) and the multi-draw tiering never used the emulation. Per-command
baseInstance stays exactly where it is: resolved server-side out of the indirect commands, zero wire traffic
(`ARCHITECTURE.md:112`: *"multi-draw 分档与 restart 重写永远由 server 拥有"*). The client emits `BaseInstance = 0` for
every indirect path.

**`MGPDrawInfo::StartInstance` (`MGPipeTypes.h:719`, `PipeFields.def:189`) is a different field and stays.** It is the
GL draw's baseInstance — what `SetCurrentBaseInstance` feeds to `gl_BaseInstance`
(`BaseInstanceInjectionTest.cpp:77-155`) and what the native `GL_EXT_base_instance` entry point consumes. The two
being separate and both explicit is the disambiguation `ROADMAP.md:19` asks for. `DrawParametersScenario` (all eight
cases, `:213-327`) is the gate on the pair.

#### D-H3 — What the emitted set contains

Espryt consumes **resolved attributes**, so the client emits one `MGPVertexBuffer` per **enabled attribute** with
`BindingIndex = attribIndex`:

```
Res           = the attribute's buffer handle, or kMGPipeNullHandle for a client-memory array
Offset        = 0                      (the attribute's own byte offset lives in MGPVertexAttribWire::Offset)
Stride        = the RESOLVED stride    (0 is meaningful: binding model, every vertex reads the same element)
Divisor       = the resolved divisor
BindingIndex  = the attribute index
```

`Start` = 0, `Count` = `MAX_VERTEX_ATTRIBS` truncated to the highest enabled attribute + 1, matching the 32-attribute
prefix walk `ARCHITECTURE.md:164` specifies for dirty bit 9.

**Client-memory arrays** (`Res == kMGPipeNullHandle`) are the one shape whose store does not exist at emission time:
`SyncClientSideAttributesForDrawArrays` (`Managers.cpp:2651-2755`) runs **after** `PrepareForDraw`, at the draw entry
point, and only for `DrawArrays`/`MultiDrawArrays` (`DirectGLES.cpp:4805-4821`, `:4837-4866`). The push ordering
contract permits this (`ARCHITECTURE.md:194` requires only "create before bind" *for resources that are named*), and a
null handle is exactly how the server learns "this attribute is client-sourced, upload it yourself". P3a moves nothing
here: the uploader, its `uploadSize = (first + count - 1) * stride + elementSize` formula (`:2726`), its
`StageVertexClient` byte accounting (`:2701-2704`, `:2740-2743`) and its two call sites are unchanged. Moving that
resolution to the client is **P8** (`HostResolve.cpp`, `ROADMAP.md:25`).

`SyncNeccessaryBuffers` (`DirectGLES.cpp:572-707`) also does SSBO binding points and
`MarkShaderStorageBuffersGpuWritten()` at `:705-706` — **unrelated to vertex input and must not be dragged into
`set_vertex_buffers`** (scout trap 6). Those rows are dirty bits 15/16/17 and belong to P4b.

#### D-H4 — Dirty bit 9, narrowed

`Tracker.h:243-244` today: `now[NewVertexBuffers] = MGPipeMixShutter(ctx.GetAnyVaoAttributeGeneration(), vaoIdentity)`.
That is already exact for P3a: `VertexArrayState::m_anyVaoAttributeGeneration` (`VertexArrayState.h:36-44`) is bumped by
`MGP_NOTE_AGGREGATE(VaoAttribute)` inside all three `Bump*Version` functions (`VertexArrayObject.cpp:317, 324, 331`),
which are the only writers of an attribute's format, buffer or enable state. A driver-id re-mint that moves no client
counter is caught server-side by `g_bufferBackendIdGeneration` (D-A4). Redundant emissions are killed by the
suppressor. **Keep the shutter; wire the emission; add `BaseInstance` to the hash.**

`Tracker.h:69-78`'s comment labels bits 9 and 10 "object class: computed and counted, emitted from P3b/P4b on".
**`ROADMAP.md:19` puts both in P3a.** Package B corrects the comment in the same commit that wires them.

### D-I — `set_index_buffer`, and the two restore traps

`MGPIndexBuffer` (`MGPipeTypes.h:373-380`, 24 B) is unchanged and carries its own rule in place:
*"An independent call, NOT a subset of the VAO configuration version (D5)."* `ARCHITECTURE.md:99` states it in prose and
the code already agrees — the index slot is explicitly not covered by `m_configVersion` (`Managers.h:932-936`).

Emission: `Res` = the handle of `GetIndexBufferBindingSlot().GetBoundObject()`, `Offset` and `IndexSize` from the draw
(0 and 0 when no draw is pending — the applier stores them and the draw verb overrides). Shutter, **narrowed** (this is
the narrowing `Tracker.h:245-248` records as owed — today bit 10 is `MixShutter(buffers, vaoIdentity)` and over-fires
on *any* buffer write anywhere):

```
now[NewIndexBuffer] = MGPipeMixShutter(vaoIdentity,
                          MGPipeMixShutter(widen(slot.GetVersion()), boundObject ? boundObject->GetLifetimeId() : 0));
```

`slot.GetVersion()` is a **wrapping `Uint16`** (`MG_Util/Types.h:198`, bumped only on a real change at `:189-195`), so
it goes through `MGPipeWidenedCounter` (`Tracker.h:159-177`) at the tracker boundary; the bound object's lifetime id
joins it because identity is what closes the wrap hole (`Managers.h:927-931` explains the same reasoning for the
twin). This is exactly `ARCHITECTURE.md:165`'s *"索引 slot 版本 + 绑定对象 `{slot,gen}`"*.

**Two traps that any re-emit must preserve, and P3a preserves them by not touching them:**

1. `ScopedRestartIndexSubstitution::~ScopedRestartIndexSubstitution` (`DirectGLES.cpp:4788-4791`) restores the exact
   previous GL name captured at `:4776` via `BoundElementArrayBufferId()` (`:4629-4634`).
2. `MultiDrawImpl` does the same at `MultiDraw.cpp:101-109` (whose comment states the memo hazard verbatim) and
   `:929-937`.

Both operate on **driver ids**, which stay server-side, and both exist *because the twin memoised that it already
synced the binding*. The memo is now `m_syncedIndexSerial`, and the restore must leave it consistent: the two dtors
restore the id **without** touching the serial, which is correct — the serial records what the *applier* last said, and
the scope restored what the applier said. `ResidentIndexScenario`'s five cases (`:186, 225, 271, 311, 351`) and
`PrimitiveRestartScenario`'s seven (`:215, 236, 263, 281, 306, 332, 368`) are the gate.

Restart rewriting and multi-draw flattening stay **server-owned with zero wire traffic** (`ARCHITECTURE.md:112`,
`:222`), including `kMaxRestartRewriteBytes` = 64 MiB (`DirectGLES.cpp:4510`), the whole-buffer EBO rewrite
(`:4691-4721`), the `UBYTE→USHORT→UINT` widening (`:4746-4764`), `kMaxFlattenedIndices` = 1<<24 (`MultiDraw.cpp:74`)
and the six-tier ladder (`MultiDraw.h:15-31`). **P3a must not move either into the client as a convenience**
(P8 expectation 8). The two CPU index reads that precede them —
`indexBuffer->SyncPersistentMappedRange(); indexBuffer->SyncGpuWrites(); MappedData();` at
`DirectGLES.cpp:4709-4713` and `MultiDraw.cpp:508-514` — stay exactly where they are; moving them is P8's
`HostResolve.cpp` plus the index host mirror.

### D-J — The Adreno SIGSEGV workarounds, and everything else that must NOT change

`ROADMAP.md:19` says "Adreno SIGSEGV workaround 保留"; `ARCHITECTURE.md:316` repeats it. `grep SIGSEGV
MobileGL/MG_Backend/DirectGLES/` returns three hits and they are **two distinct workarounds plus one cross-reference**.
Both live inside `BackendVertexArrayObject::SyncToBackend`, i.e. inside P3a's own vertex-elements half:

1. **A `GL_DOUBLE`/`IsLong` attribute with no buildable stream** (`Managers.cpp:2517-2540`, rationale `:2516-2521`,
   forward reference `Managers.h:887-895`). Becoming long bumps `FormatVersion`, not `SwitchVersion`, so the
   enable/disable block above does not re-run and an already-enabled array would stay enabled with no pointer and no
   `GL_ARRAY_BUFFER` binding — *"the Adreno driver dereference[s] null inside the next draw and take[s] the process
   with it (SIGSEGV in libGLESv2_adreno, KHR-GL43.vertex_attrib_binding.basic-input-case4)"*. Fix: try
   `SyncFloat64AttributeAsFloat32` (`:2757-2874`), and on failure `glDisableVertexAttribArray(attribIndex)` (`:2537`)
   with an `MGLOG_W_ONCE`. The re-enable at `:2529` is explicitly non-redundant.
2. **A `GL_BGRA` vertex size the driver refuses** (`:2568-2612`, rationale `:2568-2582`): drain `glGetError`
   (`:2585-2588`), issue `glVertexAttribPointer` with `GL_BGRA` as the size (`:2593-2596`), re-check (`:2603`),
   disable on refusal. Deliberately **only** for `attrib.IsBgra` — the per-draw sync must not grow a `glGetError`
   round trip for real formats.

Both depend on observing a **driver** error at attribute-declaration time. **Decision: both stay server-side verbatim,
and the disable is a server-local decision, never a client one.** The wire enabler is already in place —
`MGPVertexAttribWire` carries `IsLong` and `Type == Float64` **separately** (D-G2, `ARCHITECTURE.md:130`), and fp64
narrowing remains a server-side emulation (`ARCHITECTURE.md:225`) advertised by `kCapFloat64VertexAttrib`
(`:106`). `ConvertedFloat64Stream` (`Managers.h:901-909`) re-keys its `sourceLifetimeId` to the buffer handle's
`{slot,gen}` — one of `ARCHITECTURE.md:363`'s eleven direct deletions (*"`ConvertedVertexStreamKey` 的 `sourcePin`"*) —
and keeps `sourceChangeSerial` (now the applier's `Serial`), `sourceOffset`, `sourceStride`, `componentCount`,
`elementCount`, and its rule of **never being trusted for a persistently mapped buffer** (`Managers.cpp:2812-2820`).

**The complete must-not-change list for package C**, each with an existing test:

| thing | where | pinned by |
|---|---|---|
| `AcquirePersistentMap`'s body (D-B4) | `Managers.cpp:1134-1210` | D-E; `BufferTest.cpp`'s adopted-store family (`:1658, 1724, 1952, 1990, 2029, 2058, 2093, 2165-2316, 2451-2943`) |
| the nine pool / deferred-release / ring functions | D-F's table | **G5** |
| the two Adreno SIGSEGV workarounds | above | `VertexAttribBindingScenario` (`:241-734`), `DoublePrecisionScenario` (`:863, 916`) |
| the Mali WAR-stall fix (queue-only `SubData`) | `Managers.cpp:1284-1296`, `Managers.h:806-826` | `CrossFrameBufferScenario` (`:409-518`), `StreamedArenaScenario` (`:675, 731`) |
| `InvalidateIndexedBufferBindingShadowsForId` on an extent move | `Managers.cpp:933` | `StorageBufferRegrowScenario.AGrownStoreIsVisibleThroughItsExistingIndexedBinding` (`:124`) |
| the element-array binding restores | `DirectGLES.cpp:4788-4791`, `MultiDraw.cpp:101-109, 929-937` | `ResidentIndexScenario`, `PrimitiveRestartScenario`, `MultiDrawScenario` (10 cases, `:225-526`) |
| `EnsureProcessTeardownSentinel()` armed on the slot table's first insertion | `Managers.h:52-60`, `Managers.cpp:161-170` | `SanityTest.cpp:2650-2665, 2765-2790` |
| the twin destructors' `g_backendContextGeneration` compare | `Managers.h:64-70` | `SanityTest.cpp:2650-2665, 2786-2790` |
| `TempBufferTarget` = `GL_ARRAY_BUFFER` and its redundant-bind cache | `Managers.h:552`, `Managers.cpp:631-632` | `SanityTest.cpp:2535` (`PixelPackBindingCacheSkipsRedundantBindsAndRestsAtZero`); **any new path that binds `GL_ARRAY_BUFFER` behind `BindBufferId`'s back silently corrupts it** (scout trap 7) |
| `g_bufferBackendIdGeneration` server-local | `Managers.cpp:1503`, read `:2446`, `:2649` | scout trap 1; commit `d7655247` is the bug it prevents |
| no `CollectGarbageIfNeeded` tick on the slot arm | `DirectGLES.cpp:1275`, `SlotTables.h:37-48` | scout trap 8 |
| the four PBO/XFB writeback shapes and their epoch bumps | `DirectGLES.cpp:9406-9468, 9832-9880, 7876-7906, 838-878, 918-985`; `Utils.cpp:2297-2354` | `PackedWordReadbackScenario` (`:148, 167, 194`), `XfbCaptureBufferReuseScenario` (`:182-283`), `AtomicCounterScenario` (`:172, 201, 242`), `BufferTextureScenario` (`:162, 238, 311`) |

### D-K — How the pull build stays inert (G1)

Every P3a edit outside `MG_Backend` is inside `#if MOBILEGL_PIPE_PUSH`, following P2's precedent exactly
(`PipeMutation.h:46-48`: counters are members of the owning container *"all under `MOBILEGL_PIPE_PUSH` so the pull
build's state objects do not change size (G1)"*). Specifically:

- `MGPipeResourceOps`, `MGPipeSetResourceOps`, `MGPipeGetResourceOps`, every new `MGPipeApply*` and the applier's new
  records are inside `PipeApply.h`'s existing `#if MOBILEGL_PIPE_PUSH` (`:30`).
- The eight `BufferObject.cpp` dispatch branches are `#if MOBILEGL_PIPE_PUSH` guarded and fall through to the
  unmodified `g_bufferBackendOps` call in a pull build.
- `CallClass::MapPersistentRoundtrips` is inside `PipeStats.h`'s existing push-only block (`:101-116`).
- `MG_Impl/Pipe/{ResourceTracker.h, VertexInputEmit.h}` compile to nothing in a pull build, like `Tracker.h`,
  `CsoCache.h` and `SetHashSuppressor.h`.
- `MGPVertexAttribWire` / `MGPVertexBindingPointWire` are new **types**, not new members of an existing type, so they
  add no size anywhere. `MGPVertexBuffers` growing 16 → 24 is a push-only payload; the pull build never instantiates it.

**[deviation] `PipeResource::m_backend`, `SetBackendResource`, `ReleaseBackend` and `BackendBufferResource` are NOT
deleted in P3a**, although `ARCHITECTURE.md:284` says *"`SetBackendResource` 删除（server 拥有资源表）"*. Deleting them
changes `sizeof(BufferObject)` and its symbol set in the **pull** build, which is a straight G1 break, and it would
also break the `MOBILEGL_PIPE_LEGACY_MEMOS` arm that `ARCHITECTURE.md:367` requires to stay alive through P3a/P4a.
**Under push they simply stop being written**: `SetBackendResource` is never called, `m_backend` stays null, and
`~BufferObject`'s `OnDestroy` branch (`BufferObject.cpp:40-42`) therefore never fires — the death crosses as
`ResourceDestroy` instead. Cost: one dead pointer per `BufferObject` in a push build. The deletion retires with the
pull path at **P13**; the integrator records that against `ARCHITECTURE.md:284`.

### D-L — The buffer death signal: `ResourceDestroy`, not a seventh raiser

`scout-pipe-and-tests.md` §1.5 leaves this open. Today six kinds raise `NotifyStateObjectDestroyed`
(`StateObjectDeathNotice.h:55`) — Framebuffer, ShaderCso, Renderbuffer, SamplerCso, Texture, VertexElementsCso — and
`Managers.cpp:212-215`'s `default:` arm says *"Buffer already has its own death signal
(`BufferBackendOps::OnDestroy`)"*.

**Decision: the buffer's death crosses as `ResourceDestroy` (`PipeCalls.def:77`), which is the catalogue call for it.
No seventh `NotifyStateObjectDestroyed` raiser is added.** `StateObjectDeathNotice.h` exists precisely for kinds that
have *no* such call (`:23-27`: it carries `{kind, lifetimeId}` and not the object, and one entry point serves all kinds
because the backend's answer is the same — free the slot, drop the twin). `SlotTables.h:38`'s *"All six re-keyed object
classes raise …"* therefore stays true, and package C updates `Managers.cpp:213-214`'s comment to
*"Buffer death crosses as ResourceDestroy (P3a)"*.

The **order** at `~BufferObject` is fixed and is not negotiable, because `MGPipeSlotAllocator::Free` erases its
`lifetimeId → slot` mapping (`SlotAllocator.h:60`) and a notice resolved twice finds nothing the second time
(`SlotTables.h:49-59`, `Managers.cpp:180-185`):

```
~BufferObject()  ->  MGPipeEmitResourceDestroy(handle)      // applier clears Live, calls ops->Destroy(handle)
                 ->  MGPipeSlots().Free(MGPipeKind::Buffer, handle)
```

The `Gen` bump happens on the **next handout** of the slot, not in `Free` (`SlotAllocator.h:60`), so a double free
cannot skip a generation. `HandleRecycleScenario` gains
`ABufferAtARecycledAddressDoesNotInheritItsPredecessorsContents`, shaped on the existing
`AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexInput` (`:483`), with all three arms
(`.Handles` / `.Legacy` / `.AbaControl`).

### D-M — Subsystem bits, the A/B, and `MOBILEGL_PIPE_LEGACY_MEMOS`

`MGPipe.h:79`: *"bits 7..62 reserved for the later phases, allocated in ROADMAP order"* and never reused — an
operator's recorded `0x7f` has to keep meaning what it meant. P3a takes the next two:

```cpp
inline constexpr Uint64 kMGPipeSubsystemResources   = 1ull << 7; // P3a: the resource_* family
inline constexpr Uint64 kMGPipeSubsystemVertexInput = 1ull << 8; // P3a: vertex elements / buffers / index
inline constexpr Uint64 kMGPipeSubsystemsMigratedAtP3a = 0x1ffull; // bits 0..8
```

`ConfigLoader.cpp:245-256`'s push-build default moves from `kMGPipeSubsystemsMigratedAtP2` to
`kMGPipeSubsystemsMigratedAtP3a`; `Config.h:319-358`'s bit-list comment gains the two rows.
`MGPipeSubsystemForDirty` (`Tracker.h:119-135`) gains three arms — bit 5 → `kMGPipeSubsystemVertexInput`,
bit 9 → `kMGPipeSubsystemVertexInput`, bit 10 → `kMGPipeSubsystemVertexInput` — and `PipeFill.cpp`'s
`SubsystemForEmitter` gains the matching arms, **plus the pairing `static_assert`s** (`:645-663`), **plus**
`kMGPipeWiredSubsystems` (`:669-672`) gaining both bits. All four in one commit — see the merge trap in C.5.

`ARCHITECTURE.md:367` requires `MOBILEGL_PIPE_LEGACY_MEMOS` (default ON, tri-state) to keep the pre-handle arm alive
*"在 P3a/P4a 期间"*, because after phase C a cleared subsystem bit is no longer a true A/B on its own. P3a's legacy arm
is: the frontend dispatch falls through to `g_bufferBackendOps`; Espryt's `Ops_*` keep their `BufferObject&` bodies;
the VAO twin keeps `m_syncedConfigVersion` / `m_syncedAttributeVersions` / `m_syncedIndexBufferObject` and reads
`MGB_CTX->GetBoundVertexArray()`. Both arms compile in every push build and every `§C` verification runs the suite
twice, so the legacy arm cannot silently stop compiling or stop passing.

### D-N — What P3a does **not** do

Written down so nobody re-opens it mid-package:

- No client-side content-addressed vertex-elements cache, no 1024-entry LRU (D-G1 — P7).
- No `hasLiveHostWrites` producer; the field exists and stays false (D-A4 — P5).
- No `MGHostSpan`, no `SEG_*`, no chunked G3 path, no `kCapNeedsHostIndexBytes`, no index host mirror (P8).
- No move of `SyncPersistentMappedRange` / `SyncGpuWrites` off their 8 + 3 Espryt sites
  (`DirectGLES.cpp:297, 4710, 4964, 4965, 5066, 5067`, `Managers.cpp:1700`, `MultiDraw.cpp:511`; and
  `DirectGLES.cpp:4711`, `Managers.cpp:2774`, `MultiDraw.cpp:512`) — the per-site attribution table
  (`ARCHITECTURE.md:235-241`) is P8's to execute, and `ROADMAP.md:88` forbids "fixing" the `*IndirectCount`
  gap in flight.
- No Magma work of any kind. Magma's buffer path is P7 (`ROADMAP.md:24`); its VAO memo re-key landed in P2.
- Nothing added to `ResidualValueBlock`. `MGL_RESIDUAL_BLOCK_SIZE` is **8** (`MGPipeTypes.h:546`), it only ever goes
  **down**, and growing it at all is a build break by design (`:535-539`). Anything P3a is not ready to carry stays in
  the per-verb residual **fill loop** (`kMGPipeFieldEmittedBy` / `CopyField`), which is a different mechanism.
- No new namespace. Everything is `MobileGL::MG_Pipe` (`BRIEF-P2.md:456-458`), and
  `python3 scripts/check_include_closure.py` runs after any file is added to `MG_Pipe/`; if the closure gate objects,
  the file moves to `MG_Impl/Pipe/`.
- No `Tracker.cpp`. `Tracker.h` is header-only for an ownership reason (`Tracker.h:40-44`): the root `CMakeLists.txt`
  that would name a new `.cpp` belongs to the contract package. P3a's new client files are headers for the same reason,
  except those the contract package itself adds to `CMakeLists.txt`.
- No hot-path timer anywhere (`ROADMAP.md:7`, `Tracker.h:35-38`). Absolute ns comes from DriverBench, from outside the
  library.

---

## C. Packages

**Four packages on four branches, four WSL worktrees.** The split is chosen by the code: the payloads, the applier, the
op table and the generated interfaces are one shared contract everything else compiles against; the client emitters and
the tracker touch no backend file; Espryt is one backend directory; the gates, scenarios, CI file and measurement
plumbing are cross-cutting tooling that must be able to land before the thing it gates.

**Branch points.** `p3a/contract` from `feat/disaggregated@55d2af9b`. Its single commit `c0` is the contract;
`p3a/{client, espryt, gates}` all branch from the **tag** `p3a/contract`, so their push builds compile and link while A
finishes. `p3a/wire` continues on from `c0`.

**Tree creation** (`~/w7/notes/tools/wsl_p2_tree.sh` → `wsl_p3a_tree.sh`, `p2`→`p3a` throughout):

```sh
bash ~/w7/wsl_p3a_tree.sh contract 55d2af9b   verify    # first, and it must finish before the rest
bash ~/w7/wsl_p3a_tree.sh wire     p3a/contract verify
bash ~/w7/wsl_p3a_tree.sh client   p3a/contract verify
bash ~/w7/wsl_p3a_tree.sh espryt   p3a/contract
bash ~/w7/wsl_p3a_tree.sh gates    p3a/contract verify
```

Each tree gets the same three build directories, flags identical to `~/w7/pipe/build-linux` (the `COMMON` line of
`wsl_p2_gate.sh:10`):

```sh
# pull (the G1 build)
cmake -S . -B build-linux -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_BUILD_TYPE=Release \
  -DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_INFO -DMOBILEGL_BUILD_TEST=ON -DMOBILEGL_BUILD_INTEGRATION_TEST=ON \
  -DMOBILEGL_BUILD_BENCHMARK=OFF -DMOBILEGL_ITEST_EGL_VENDOR=/usr/share/glvnd/egl_vendor.d/50_mesa.json \
  -DMOBILEGL_ITEST_VK_ICD=/usr/share/vulkan/icd.d/lvp_icd.json -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
# push
cmake -S . -B build-push   ... same ... -DMOBILEGL_PIPE_PUSH=ON
# verify
cmake -S . -B build-verify ... same ... -DMOBILEGL_PIPE_VERIFY=ON -DMOBILEGL_ITEST_REQUIRE_GPU=ON
# benchmark (package D and the integrator only; DriverBench is Linux-desktop only, CMakeLists.txt:33-35
# force-disables the whole benchmark tree on Android)
cmake -S . -B build-bench  ... same ... -DMOBILEGL_BUILD_BENCHMARK=ON
```

`export CCACHE_BASEDIR=/home/swung/w7` in every shell.

### C.0 Package A — `p3a/contract` then `p3a/wire` (trees `~/w7/p3a-contract`, `~/w7/p3a-wire`)

**Commit `c0` — the contract. Nothing in it may change after the tag without telling the other three packages.**

| action | file | what |
|---|---|---|
| MODIFY | `MobileGL/MG_Pipe/MGPipeValueTypes.h` | D-G2 — `MGPVertexAttribWire` (24 B) and `MGPVertexBindingPointWire` (16 B), placed next to `VertexAttribute` / `VertexBufferBindingPoint` (`:491-538`) with the trip-wire size asserts in the `:547-...` block |
| MODIFY | `MobileGL/MG_Pipe/MGPipeTypes.h` | D-H1 — `MGPVertexBuffers` gains `Uint32 BaseInstance; Uint32 Pad0;`, `MGP_ASSERT_POD` 16 → **24**, with the comment naming the `ContentHash` requirement and the `MGPDrawInfo::StartInstance` distinction; D-A5 — `MGPipeResourceRespecifyNeedsAck(const MGPResourceDesc&)` next to `MGPipeSetSubDataBufferRange` (`:601-617`) |
| MODIFY | `MobileGL/MG_Pipe/PipeCalls.def` | D-A5 — `ResourceRespecify` (`:76`) flags `kNone` → `kNeedsAck`; the legend at `:18` gains *"`kNeedsAck` on a call means records of this call MAY require an ack; a per-record predicate decides"*. **No row is added, moved or reordered**; `MGP_CALL_LIST_DOCUMENTED_COUNT` stays 71 |
| MODIFY | `MobileGL/MG_Pipe/PipeFields.def` | `MGP_FIELDS_MGPVertexBuffers` (`:99-100`) gains `F(BaseInstance)`; new `MGP_FIELDS_MGPVertexAttribWire` and `MGP_FIELDS_MGPVertexBindingPointWire`; both appended to `MGP_VERIFY_PAYLOAD_LIST` (`:300-315`) — **`gen_pipe.py` reads that list to know what to emit** |
| MODIFY | `MobileGL/MG_Pipe/MGPipe.h` | D-M — `kMGPipeSubsystemResources` (bit 7), `kMGPipeSubsystemVertexInput` (bit 8), `kMGPipeSubsystemsMigratedAtP3a = 0x1ff` |
| MODIFY | `MobileGL/MG_Pipe/PipeApply.{h,cpp}` | D-A1 — `MGPipeResourceOps` + `MGPipeSetResourceOps`/`MGPipeGetResourceOps`; D-G4 — `MGPipeResourceRecord`, `MGPipeVertexElementsRecord` and the ten new `MGPipeApplierState` members; the **nine new apply entry points** with complete signatures and **stub bodies** so C and D link: `MGPipeApplyResourceCreate`, `…ResourceRespecify`, `…ResourceSubData`, `…BufferSubDataResident`, `…ResourceFlushRange`, `…ResourceReadback`, `…ResourceDestroy`, `…MapPersistent`, `…UnmapPersistent`, plus `…CreateVertexElements`, `…BindVertexElements`, `…DeleteVertexElements`, `…SetVertexBuffers`, `…SetIndexBuffer`. `MGPipeApplierReset()` (`:93`) clears all of them |
| MODIFY | `MobileGL/MG_Pipe/Coverage.def` | the **complete** P3a rows: `MGP_COVERAGE_EMITTED_LIST` gains the vertex-input fields; `GetBufferBindingSlot`'s accessor row (`:42`) splits per target now that the target argument is known (`:37-41` says the split waits on the re-vendored inventory — **P3a supplies the target from the emission site instead, and the comment is rewritten to say so**); the `Buffer ops delta → ResourceRespecify` delta row (`:130`) is left alone |
| MODIFY | `MobileGL/MG_Impl/Pipe/PipeFill.cpp` | **contract-commit only**: the generated-enum-coupled block — `SubsystemForEmitter`'s new arms (`:624-638`), the new pairing `static_assert`s (`:645-663`), `kMGPipeWiredSubsystems` (`:669-672`) — plus **stub emitter functions that emit nothing**. See the merge trap in C.5 |
| MODIFY | `scripts/gen_pipe.py` | whatever the two new payloads and the flag-word change require; regenerate |
| MODIFY | `MobileGL/MG_Pipe/generated/*.inc` | regenerate and commit (all eight; the build never depends on python, `ARCHITECTURE.md:424`) |
| MODIFY | `MobileGL/MG_Backend/DirectGLES/SlotTables.h` | D-A4 — `BackendSlotTable::GetOrCreate(MGPipeHandle)` / `FindByHandle` overloads that never call `MGPipeSlots()`, so the client can mint. **Header-only, both arms compile, nothing uses the new one yet.** [This is the one `MG_Backend` file A touches; C owns the rest of the directory and branches from the tag.] |
| MODIFY | `MobileGL/MG_Util/Metrics/PipeStats.{h,cpp}` | D-B2 — `CallClass::MapPersistentRoundtrips` **inside the existing `#if MOBILEGL_PIPE_PUSH` block** (`:101-116`), name `"map-persistent-roundtrips"`, short name `mpr` in `FormatWindowLine` |
| MODIFY | `MobileGL/MG_Test/Util/PipeStatsTest.cpp` | pin `mpr` in `CounterNamesAreStable` (`:260`) and `SummaryLineCarriesEveryClassAndGate` (`:137`) |
| MODIFY | `MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp` | `MGPVertexBuffers` size 16 → 24; the new `ResourceRespecifyAcksOnlyImmutableStorage` case (D-A5) |
| MODIFY | root `CMakeLists.txt`, `MobileGL/Config.h`, `MobileGL/ConfigLoader.cpp` | the new client sources inside `if (MOBILEGL_PIPE_PUSH)` (`:462-468`); the push default `0x7f` → `0x1ff`; the bit-list comment |
| CREATE (stubs) | `MobileGL/MG_Test/Pipe/{ResourceEmitTest, VertexInputEmitTest}.cpp` | each a compiling file with one placeholder `TEST`, so the package that owns their contents never edits `MG_Test/Pipe/CMakeLists.txt` |
| MODIFY | `MobileGL/MG_Test/Pipe/CMakeLists.txt` | register the two stubs, `LABELS unit` |

**Ordered steps for `c0`**: value types and payloads first (build pull; confirm `symbol_report --threshold 0` is
clean and the two new `MGP_ASSERT_POD`s hold) → `PipeCalls.def` flag word + `PipeFields.def` + generator + regenerate +
`git diff --exit-code -- MobileGL/MG_Pipe/generated` → `MGPipe.h` / `Config` / `CMakeLists` → `PipeApply.{h,cpp}` with
complete signatures and stub bodies → `PipeFill.cpp`'s enum-coupled block with stub emitters → `SlotTables.h`
overloads → `PipeStats` + the three tests. **Then tag `p3a/contract` and tell the other three packages.**

Message: `[Feat] (Pipe): land the P3a contract - the handle-shaped resource op table, the applier's resource and vertex-elements records, the two vertex wire views, an explicit baseInstance on set_vertex_buffers, and the first kNeedsAck`

**Then `p3a/wire` continues** with the work only A can do:

- `c1`: the nine `resource_*` apply entry points in full — the record updates, the `Serial` bumps, the bounds gate that
  refuses a blob outside its declared segment (`Fatal{ProtocolCorruption}`, `ARCHITECTURE.md:119`), and the dispatch
  into `MGPipeResourceOps`. Message:
  `[Feat] (Pipe): apply the resource calls into a per-context slot-indexed record and dispatch them to the backend by handle`
- `c2`: the five vertex-input apply entry points — `CreateVertexElements`'s blob unpack with its count validation,
  `BindVertexElements`, `DeleteVertexElements`, `SetVertexBuffers` (including `VertexFetchBaseInstance`) and
  `SetIndexBuffer`, each bumping its `Serial`. Message:
  `[Feat] (Pipe): apply vertex elements, vertex buffers and the index buffer into the server's own working state`
- `c3`: `MG_Test/Pipe/ResourceEmitTest.cpp` in full — the applier's record lifecycle: a create marks the slot live; a
  respecify replaces the descriptor and bumps `Serial`; a destroy clears Live and a stale handle's `Gen` compare fails;
  `HasLiveHostWrites` is false for every path P3a has; `MGPipeSetSubDataBufferRange` returns false exactly at the
  2^31-1 / 2^32-1 bounds and the emitter's splitter produces contiguous, non-overlapping records that reassemble to the
  original range. Message:
  `[Test] (Pipe): pin the resource record's lifecycle and the buffer sub-data range encoding at both of its bounds`

**Verification (`~/w7/p3a-wire`)**

```sh
python3 scripts/gen_pipe.py --check && python3 scripts/gen_pipe.py --self-test
python3 scripts/check_include_closure.py --mode both --compiler clang++-20 --self-test --require-all
cmake --build build-linux  --parallel 28 && ctest --test-dir build-linux  -L unit --no-tests=error --output-on-failure
python3 scripts/symbol_report.py --before ~/w7/p3a-before-libMobileGL.so --after build-linux/libMobileGL.so \
        --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0
#   expect added 0 / removed 0 / renamed 0 / resized 0 - P3a's admitted set is EMPTY (G1)
cmake --build build-push   --parallel 28 && ctest --test-dir build-push   -L unit --no-tests=error --output-on-failure
cmake --build build-verify --parallel 28 && ctest --test-dir build-verify -L unit --no-tests=error
bash scripts/p3a_untouched_regions.sh 55d2af9b HEAD          # G5: nine functions unmoved
```

### C.1 Package B — `p3a/client` (tree `~/w7/p3a-client`, from `p3a/contract`)

**Files**

| action | file | what |
|---|---|---|
| CREATE | `MobileGL/MG_Impl/Pipe/ResourceTracker.h` | D-A3's sticky `BindMask` table and its `static_assert` over `BufferTarget`; the `lifetimeId → slot` mint and the `slot → BufferObject*` inverse (D-D); the nine `MGPipeEmitResource*` helpers; the `MGPSubData` range splitter; the `mpr` counting site |
| CREATE | `MobileGL/MG_Impl/Pipe/VertexInputEmit.h` | the `VertexAttribute`/`VertexBufferBindingPoint` → wire conversion (D-G2), the per-handle `m_lastEmittedConfigVersion` table (D-G3), the `MGPVertexBuffer[]` build (D-H3), the `ContentHash` including `BaseInstance` (D-H2.3), and `MGPipeEmit{VertexElements,VertexBuffers,IndexBuffer}` |
| MODIFY | `MobileGL/MG_Impl/Pipe/Tracker.h` | D-M — three `MGPipeSubsystemForDirty` arms; D-I — bit 10's narrowed shutter; the comment at `:64-78` corrected (bits 9/10 are **P3a**, not P3b/P4b) and `:245-247`'s "P3b narrows it" note replaced by what P3a did |
| MODIFY | `MobileGL/MG_Impl/Pipe/PipeFill.{h,cpp}` | the three emitter bodies replacing A's stubs, in `ARCHITECTURE.md:194`'s order after the four P2 emitters (`:1117-1128`); `MGPipeSetPendingBaseInstance` + its reset in `MGPipeLeaveVerb` (`:607`); `EmittedCallSuppliesTheWholeField` (`:693+`) extended |
| MODIFY | `MobileGL/MG_Impl/Pipe/SetHashSuppressor.h` | comment only: slot `SetVertexBuffers` (`:37`) is marked wired at P3a instead of P3b |
| MODIFY | `MobileGL/MG_State/GLState/BufferState/BufferObject.{h,cpp}` | D-A1's eight `#if MOBILEGL_PIPE_PUSH` dispatch branches at `:39-43, 45, 53, 63, 73, 186, 291, 319, 371, 493`; `ResourceCreate` from the constructor (`:35-37`); `ResourceDestroy` + `MGPipeSlots().Free` from `~BufferObject` in that order (D-L) |
| MODIFY | `MobileGL/MG_State/GLState/BufferState/BufferState.{h,cpp}` | the sticky `everBoundAs` mask's bump points at the buffer bind entry points (D-A3), all `#if MOBILEGL_PIPE_PUSH` |
| MODIFY | `MobileGL/MG_Pipe/DirtySurface.def` | rows for anything the widened scan root (G9) newly sees; `MarkBufferObjectForDeletion → kExplicitDestroy` (`:252`) and `MarkVertexArrayForDeletion → kExplicitDestroy` (`:260`) are now **true** rather than aspirational, and their comments say which call publishes them |
| MODIFY | `scripts/gen_pipe_dirty_surface.py` | G9 — widen the scan root from `MG_Impl/GLImpl` to `MG_Impl/GLImpl` + `MG_State/GLState`; keep `--check` failing in both directions and `--self-test`'s two canned negative controls |
| MODIFY | `MobileGL/MG_Pipe/FillPoints.def` | discharge the item `:60-64` records as P3a's: run `MOBILEGL_PIPE_POISON_OMIT` across the full CTS caselist on both devices, then either retire the class-field-mask rows the omission proves dead or record per row why it stays. **Off the critical path — the verdict lands with G15, and the def's comment is updated either way.** |
| MODIFY | `MobileGL/MG_Test/Pipe/{ResourceEmitTest, VertexInputEmitTest}.cpp` | contents where A's `c3` did not already cover them (the stubs and the CMake registration are A's) |

**Ordered steps**

1. `b1`: `ResourceTracker.h` + the `BindMask` table + the `BufferObject` dispatch branches, **emitting into the applier
   but with `MGPipeGetResourceOps() == nullptr`**, so every dispatch still falls through to `g_bufferBackendOps` and
   the tree behaves exactly as before. This step is the safety net: it proves the emission is semantically free before
   anything is switched over. Message:
   `[Feat] (Pipe, State): mint a {slot, gen} handle for every buffer object and publish its create, respecify, sub-data, flush, readback and destroy as pipe calls`
2. `b2`: `VertexInputEmit.h` + the three emitters + `MGPipeSetPendingBaseInstance` + the tracker's three subsystem arms
   and bit 10's narrowed shutter. Still nothing consumes them. Message:
   `[Feat] (Pipe): push the bound VAO's format, its vertex buffers with an explicit baseInstance, and its index binding as their own calls`
3. `b3`: `gen_pipe_dirty_surface.py`'s widened scan root and the `DirtySurface.def` rows it produces. Message:
   `[Feat] (Pipe): widen the dirty-surface scan to MG_State so the MGP_NOTE_MUTATION sites stop being outside the gate`
4. `b4`: the two unit-test files.

**Tests to add**

- `ResourceEmitTest.EveryBufferTargetSetsItsBindMaskBit` — D-A3, on create **and** on a following respecify.
- `ResourceEmitTest.ABindMaskBitIsStickyAcrossARespecifyThatDoesNotRebind`.
- `ResourceEmitTest.ADestroyedBufferReleasesItsSlotAndAStaleHandleResolvesToNothing` — D-L's order.
- `ResourceEmitTest.AWholeBufferSubDataBeyondTheRecordBoundIsSplitIntoContiguousRecords`.
- `VertexInputEmitTest.EveryAttributeFieldSurvivesTheWireConversion` — **G6**: all 32 slots, every member of
  `VertexAttribute` that the wire form carries, driven through the format entry points, the binding-model entry points
  and the legacy pointer entry points.
- `VertexInputEmitTest.ABindingModelStrideOfZeroSurvivesAsZero` — the
  `KHR-GL43.vertex_attrib_binding.basic-input-case7/8` regression (`MGPipeValueTypes.h:496-503`).
- `VertexInputEmitTest.IsLongAndFloat64TravelSeparately` — `ARCHITECTURE.md:130`, and the input to D-J's workaround 1.
- `VertexInputEmitTest.ABaseInstanceChangeAloneStillEmitsTheVertexBufferSet` and
  `…AnUnchangedSetWithAnUnchangedBaseInstanceEmitsNothing` — D-H2.3, the suppressor trap.
- `VertexInputEmitTest.RebindingTheSameVaoEmitsABindAndNoCreate` and
  `…PingPongingBetweenTwoVaosNeverRecreatesEither` — D-G3's per-handle latch.
- `TrackerTest.TheIndexBufferBitDoesNotFireOnAnUnrelatedBufferWrite` — **the narrowing of D-I, and the one case the old
  shutter could not pass.**
- `TrackerTest.TheIndexBufferBitFiresWhenTheSlotVersionWrapsOntoADifferentBuffer` — the wrap hole identity closes.

**Constraints**: B touches **no** file under `MG_Backend/`. B does not edit
`MG_Pipe/{MGPipeValueTypes.h, MGPipeTypes.h, MGPipe.h, PipeCalls.def, PipeFields.def, Coverage.def, PipeApply.*,
generated/*.inc}` or `scripts/gen_pipe.py` (A owns them), and does not edit `.github/workflows/test.yml` or
`MG_IntegrationTest/**` (D owns them) — so B's scanner change must keep `--summary` working until D lands.

**Verification (`~/w7/p3a-client`)**

```sh
python3 scripts/gen_pipe_dirty_surface.py --check && python3 scripts/gen_pipe_dirty_surface.py --self-test
python3 scripts/gen_pipe.py --check
cmake --build build-linux --parallel 28 && python3 scripts/symbol_report.py \
  --before ~/w7/p3a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change
cmake --build build-push  --parallel 28 && ctest --test-dir build-push -L unit --no-tests=error --output-on-failure
ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8
MOBILEGL_PIPE_PUSH=0     ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8   # all-pull arm
MOBILEGL_PIPE_PUSH=0x7f  ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8   # P3a subsystems off (G12)
cmake --build build-verify --parallel 28 && ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4
MOBILEGL_PIPE_VERIFY=1 python3 ~/w7/retrace_gate.py --tree ~/w7/p3a-client --lib build-verify/libMobileGL.so \
  --out ~/w7/retrace-out/p3a-client -j 4
grep -l 'Fatal{' ~/w7/retrace-out/p3a-client/*.log      # empty
grep -L 'MGPipe verify:' ~/w7/retrace-out/p3a-client/*.log   # empty: every case armed
```

### C.2 Package C — `p3a/espryt` (tree `~/w7/p3a-espryt`, from `p3a/contract`)

**Files**: `MobileGL/MG_Backend/DirectGLES/{DirectGLES.cpp, Managers.h, Managers.cpp, MultiDraw.cpp, SlotTables.h,
Utils.cpp}` and `MobileGL/MG_Test/SanityTest.cpp`. **Nothing else.**

**Ordered steps**

1. `e1`: the seventh slot table — `BufferImpl::g_backendBufferResources` of kind `MGPipeKind::Buffer`, holding
   `GLESBufferResource`, keyed by the client-minted handle through A's `GetOrCreate(MGPipeHandle)` overload
   (D-A4). `EnsureProcessTeardownSentinel()` arming moves onto its first insertion (`Managers.h:52-60`,
   `Managers.cpp:161-170` — a destructor hook is wrong and `:52-57` says why). Both arms compile; nothing uses the new
   one yet. Message:
   `[Refactor] (Espryt): give the buffer resource its own {slot, gen} table instead of hanging it off the frontend object`
2. `e2`: the nine `MGPipeResourceOps` implementations and `MGPipeSetResourceOps` at bring-up
   (`BackendObject_DirectGLES.cpp:849` and `DirectGLES.cpp:10402`, beside `RegisterBufferBackendOps`), cleared at
   shutdown beside `UnregisterBufferBackendOps` (`Managers.cpp:1512`). Each body is its `Ops_*` counterpart with the
   substitutions D-A2's table names and **nothing else**; the seven `*Tracked` wrappers and their epoch bumps
   (`:1449-1480`) are duplicated for the new table, including `AcquirePersistentMap`'s bump-even-on-decline. The old
   table and its bodies stay compiled under `MOBILEGL_PIPE_LEGACY_MEMOS`. **This is the commit that switches the buffer
   path over.** Message:
   `[Refactor] (Espryt): take the buffer ops by handle and payload instead of by frontend object reference`
3. `e3`: the reverse channel — `OnBufferWriteback` in `Ops_Readback` (D-D) replacing
   `bufferObject.WritebackFromBackend` at `Managers.cpp:1410`, in the fixed order writeback → unmap → serial stamp →
   `BumpBufferMutationEpoch()`; `OnGpuWritten` replacing the three `MarkGpuWritten` sites
   (`DirectGLES.cpp:500, 544, 2012`). Message:
   `[Refactor] (Espryt): answer a readback through the reverse channel and bump the mutation epoch after it, never before`
4. `e4`: the VAO half — `SyncToBackend`'s gate re-keyed onto the applier's three `Serial`s (D-G4's table), the
   per-attribute walk reading `rec.Attributes[]` / `st.VertexBuffers[]`, `BindAttributeBuffer` resolving the driver id
   from the slot table, `ResolvedDrawBuffers` re-keyed onto handles, and the deletion of
   `g_pendingFetchBaseInstance` / `SetPendingFetchBaseInstance` / `GetPendingFetchBaseInstance` /
   `ScopedFetchBaseInstance` and its three scopes (`DirectGLES.cpp:5135, 5172, 5224`) in favour of
   `MGPipeApplierState::VertexFetchBaseInstance` (D-H2). `m_syncedConfigVersion`, `m_syncedAttributeVersions`,
   `m_syncedIndexBufferVersion` and `m_syncedIndexBufferObject` are deleted; `m_syncedBufferIdGeneration`,
   `m_hasConvertedFloat64Attribute` and `m_syncedFetchBaseInstance` stay. Every one of them stays compiled under
   `MOBILEGL_PIPE_LEGACY_MEMOS`. Message:
   `[Refactor] (Espryt): drive the driver VAO from the pushed vertex-elements record and retire the wrapping index-slot version and its identity patch`
5. `e5`: `ConvertedFloat64Stream`'s re-key from `sourceLifetimeId` to the buffer handle (`Managers.h:901-909`,
   `Managers.cpp:2812-2820`) — `ARCHITECTURE.md:363`'s *"`ConvertedVertexStreamKey` 的 `sourcePin`"* deletion — plus
   `Managers.cpp:213-214`'s comment update (D-L). Message:
   `[Refactor] (Espryt): key the narrowed fp64 vertex stream on the buffer's handle instead of its lifetime id`
6. `e6`: `SanityTest.cpp` — extend the `DirectGLESSlotTable` suite (`:3311-4044`) to the buffer kind:
   `EveryReKeyedObjectClassAnnouncesItsOwnDeath` (`:3706`) gains the buffer arm,
   `EverySwitchedOverKindResolvesItsTwinThroughTheHandleArm` (`:3857`) likewise, and
   `PixelPackBindingCacheSkipsRedundantBindsAndRestsAtZero` (`:2535`) keeps passing unchanged.

**Must not break** — D-J's table, every row with its named test. In addition: `IsBufferDrawClean`'s five reads become
four applier reads with **identical** semantics (D-A4); `SyncNeccessaryBuffers`' memo hit/miss/repair structure
(`DirectGLES.cpp:601-706`) is unchanged in shape; the four `EnsureBufferResource` call sites in `Managers.cpp`
(`:2337, 2393, 2622, 4724`) and the twelve in `DirectGLES.cpp` (`:411, 463, 526, 560, 612, 645, 681, 4185, 4247, 4631,
9411, 9837`) all keep their behaviour, taking a handle where they took a `SharedPtr`.

**Verification (`~/w7/p3a-espryt`)**

```sh
cmake --build build-linux --parallel 28 && python3 scripts/symbol_report.py \
  --before ~/w7/p3a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change
grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'          # empty (G13)
grep -n 'MG_State' MobileGL/MG_Pipe/PipeApply.h                    # no hit inside the MGPipeResourceOps block (G13)
bash scripts/p3a_untouched_regions.sh 55d2af9b HEAD                # G5: the nine functions are byte-identical
grep -n 'g_pendingFetchBaseInstance\|ScopedFetchBaseInstance' MobileGL/MG_Backend/DirectGLES/*.{h,cpp}   # empty
grep -n 'm_syncedConfigVersion\|m_syncedAttributeVersions\|m_syncedIndexBufferObject' \
     MobileGL/MG_Backend/DirectGLES/Managers.h
#   every remaining hit must be inside a MOBILEGL_PIPE_LEGACY_MEMOS arm
cmake --build build-push --parallel 28
ctest --test-dir build-push -L unit --no-tests=error --output-on-failure
ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8 -R DirectGLES
MOBILEGL_PIPE_PUSH=0    ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8 -R DirectGLES  # legacy arm
MOBILEGL_PIPE_PUSH=0x7f ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8 -R DirectGLES  # G12
ctest --test-dir build-push -L integration-gpu --no-tests=error -j 4 \
  -R 'LargeArenaAdoption|StorageBufferRegrow|VertexAttribBinding|MultiDraw|PrimitiveRestart|CrossFrameBuffer|ResidentIndex|BufferTexture|AtomicCounter|XfbCaptureBufferReuse|PackedWordReadback|DoublePrecision|VertexArrayEnableDisable|DrawParameters'
python3 ~/w7/retrace_gate.py --tree ~/w7/p3a-espryt --lib build-push/libMobileGL.so --out ~/w7/retrace-out/p3a-espryt -j 4
```

### C.3 Package D — `p3a/gates` (tree `~/w7/p3a-gates`, from `p3a/contract`)

Cross-cutting tooling. D branches from the contract and integrates **last**, but writes the extended
`HandleRecycleScenario` arms **first**, so the buffer arm's `AbaControl` red is proven before C exists — that is the
"重键前红" evidence, recorded with its run log.

| action | file | what |
|---|---|---|
| MODIFY | `MobileGL/MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp` | `ABufferAtARecycledAddressDoesNotInheritItsPredecessorsContents`, shaped on the VAO case at `:483`, in all three arms (`.Handles` / `.Legacy` / `.AbaControl`); the VAO case extended to cover the vertex-input path (a recycled VAO must not inherit a predecessor's `set_vertex_buffers`) |
| MODIFY | `MobileGL/MG_IntegrationTest/Scenarios/StorageBufferRegrowScenario.cpp` | **G10** — N storage definitions of an adopted store publish exactly N `map-persistent-roundtrips` and **not** one per draw; `AGrownStoreIsVisibleThroughItsExistingIndexedBinding` (`:124`) is unchanged and keeps guarding `InvalidateIndexedBufferBindingShadowsForId` |
| MODIFY | `MobileGL/MG_IntegrationTest/Scenarios/LargeArenaAdoptionScenario.cpp` | its three cases (`:204, 227, 245`) run in both arms and additionally assert `mpr` moves exactly once per adoption |
| CREATE | `MobileGL/MG_IntegrationTest/Scenarios/ResourceSubsystemControlScenario.cpp` | **G12** — two entries, `MOBILEGL_PIPE_PUSH` with and without bits 7|8, asserting the emission counters move in the expected direction and the pixels do not. A dead switch must not pass silently |
| MODIFY | `MobileGL/MG_IntegrationTest/CMakeLists.txt` | register the new scenario and its environments through `mgl_itest_join_environment(... ${MGL_ITEST_COMMON_ENV})` (`:311, 464-471`) — **ctest `ENVIRONMENT` replaces rather than appends, `;` must be escaped, and the property overrides the job env** (`ARCHITECTURE.md:566`); follow the SKIP-warning precedent at `:418, 431, 445, 456, 459` so a configuration that cannot run an arm says so |
| CREATE | `scripts/p3a_untouched_regions.sh` | **G5** — takes two refs, extracts the nine named functions from `Managers.cpp` by brace matching, `sha256sum`s them, diffs. Non-zero rc names the first function that moved. `--self-test` proves it can go red by perturbing one extracted body in a temp copy |
| CREATE | `scripts/p3a_vertex_input_negative_control.sh` | **G7** — patches `VertexInputEmit.h` to stop copying `IsBgra`, rebuilds, expects `ctest -R 'VertexInputEmit\.'` to fail naming `IsBgra`, reverts and rebuilds. Exit 0 = tripped and named; 1 = did not answer (green on a dropped field, or red without naming it — both are findings about the test, and both leave the tree restored and rebuilt); 2 = could not run |
| MODIFY | `.github/workflows/test.yml` | `pipe-gates` (`:1538`) gains `p3a_untouched_regions.sh $BASELINE HEAD` and its `--self-test`; the two new scenario entries; the `include-graph-check` job (`:738`) is unchanged |
| CREATE | `~/w7/notes/tools/wsl_p3a_gate.sh` | D.3 — `wsl_p2_gate.sh` adapted; see D.3 for the exact diff |
| MODIFY | `~/w7/notes/tools/p2_ab.sh` → `p3a_ab.sh` | D.4 — the four default cases replaced by P3a's named set; everything else (reboot-clean, the three-try pin with `check` after each and exit 43, the `mkdir` device-lock mutex with its 900 s timeout and exit 42, the `trap … EXIT` unpin, `--benchmark-repeats 3 --benchmark-tail-frames 200`, the `AB_DONE` terminator) verbatim |

Messages: `[Test] (Pipe): reproduce the buffer handle ABA through public GL and prove the pre-rekey guards are what stops it`;
`[Test] (Pipe): assert a regrown adopted store costs one map-persistent round trip per storage definition, not one per draw`;
`[CI] (Pipe): gate that the buffer pool, the deferred-release drain and the three rings did not move`

**Verification (`~/w7/p3a-gates`)**

```sh
cmake --build build-verify --parallel 28
ctest --test-dir build-verify -R HandleRecycle --no-tests=error --output-on-failure
#   on the contract tree, before C lands: .Legacy and .AbaControl pass, .Handles SKIPs with
#   "subsystem not implemented on this tree" - a visible skip, never a vanishing test
bash scripts/p3a_untouched_regions.sh --self-test
bash scripts/p3a_vertex_input_negative_control.sh build-push      # must report "tripped, named IsBgra"
cmake --build build-bench --parallel 28 && ctest --test-dir build-bench -L benchmark --no-tests=error
```

### C.4 What each package may assume of the others

- **B assumes** A's `MGPipeApply*` signatures, `MGPipeResourceOps`' shape, the two wire structs, `MGPVertexBuffers`'
  24-byte layout, the two subsystem bits and `MGPipeResourceRespecifyNeedsAck`. It assumes **nothing registers**
  `MGPipeResourceOps`, so every dispatch falls through to `g_bufferBackendOps` and the tree is behaviourally
  unchanged. That is what makes B landable on its own.
- **C assumes** the same contract plus A's `BackendSlotTable::GetOrCreate(MGPipeHandle)` overload. It assumes **the
  applier's records are populated** — which is only true after B lands, hence the integration order in D.1. C is
  written against the contract and rebased onto B.
- **D assumes** both, and its `HandleRecycleScenario` buffer arm is written and proven red on the contract tree first.

### C.5 File-ownership table

No two packages edit the same file **after the tag**. The contract is one commit that all three other branches descend
from, so the integrator's rebase never sees an edit on both sides of a file.

**Generated and `.def` interfaces are owned artefacts and are listed as such**, because of the semantic-merge trap
INTEGRATOR-DECISIONS records and `Coverage.def:484-494` documents from commit `bb2a236d`: *`Coverage.def`'s emitted
list is generated-enum input.* The spans work (`d1a7c5f1`) removed `GetPixelStoreParameters` from the emitted list, so
the generated `MGPipeFieldEmitter` **no longer had a `SetPixelPackState` enumerator** — but `PipeFill.cpp`'s
`SubsystemForEmitter` switch and its `static_assert`, written against the contract, still named it, and **the push and
verify builds did not compile on the integrated tree**. Green on two branches separately is not green on the merge.

**The rule this imposes on P3a**: *a package that retires a `.def` row must not leave another package referencing the
generated enumerator.* It is discharged structurally — **package A owns `Coverage.def` AND, in the contract commit
only, the enum-coupled block of `PipeFill.cpp` (`SubsystemForEmitter`, its `static_assert`s, `kMGPipeWiredSubsystems`).
Package B owns `PipeFill.cpp` from the tag onward.** A's edit is already in B's base, so the two can never diverge, and
a mistake is a compile error at `c0`, not at the merge.

| file / glob | A contract+wire | B client | C espryt | D gates |
|---|---|---|---|---|
| `MobileGL/MG_Pipe/{MGPipeValueTypes.h, MGPipeTypes.h, MGPipe.h, PipeApply.{h,cpp}}` | **owner** | – | – | – |
| `MobileGL/MG_Pipe/PipeCalls.def` *(generated interface: opcodes + flag words → `PipeTables.inc`, `PipeThunks.inc`, `PipeWire.inc`)* | **owner** | – | – | – |
| `MobileGL/MG_Pipe/PipeFields.def` *(generated interface: field lists + `MGP_VERIFY_PAYLOAD_LIST` → `PipeVerify.inc`)* | **owner** | – | – | – |
| `MobileGL/MG_Pipe/Coverage.def` *(generated interface: `MGP_COVERAGE_EMITTED_LIST` → `MGPipeFieldEmitter` enumerators in `PipeFilled.inc`)* | **owner** | – | – | – |
| `MobileGL/MG_Pipe/generated/*.inc` *(all eight, committed; CI diffs them)* | **owner** | – | – | – |
| `scripts/gen_pipe.py` | **owner** | – | – | – |
| `MobileGL/MG_Impl/Pipe/PipeFill.{h,cpp}` — the `SubsystemForEmitter` / `static_assert` / `kMGPipeWiredSubsystems` block | **owner (contract commit only)** | **owner (after the tag)** | – | – |
| `MobileGL/MG_Pipe/{FillPoints.def, PipeMutation.h, DirtySurface.def}` | – | **owner** | – | – |
| `scripts/gen_pipe_dirty_surface.py` | – | **owner** | – | – |
| `scripts/{p3a_untouched_regions.sh, p3a_vertex_input_negative_control.sh}` | – | – | – | **owner** |
| `MobileGL/MG_Impl/Pipe/{ResourceTracker.h, VertexInputEmit.h, Tracker.h, SetHashSuppressor.h}` | – | **owner** | – | – |
| `MobileGL/MG_Impl/Pipe/{SlotAllocator.*, CsoCache.*}` | **owner** (unchanged in P3a; listed so nobody edits them) | – | – | – |
| `MobileGL/MG_State/GLState/BufferState/**` | – | **owner** | – | – |
| `MobileGL/MG_State/GLState/VertexArrayState/**` | – | – | – | – (unchanged in P3a) |
| `MobileGL/MG_Backend/DirectGLES/SlotTables.h` | **owner (contract commit only)** | – | **owner (after the tag)** | – |
| `MobileGL/MG_Backend/DirectGLES/**` (everything else) | – | – | **owner** | – |
| `MobileGL/MG_Backend/DirectVulkan/**` | – | – | – | – (**untouched in P3a**) |
| `MobileGL/MG_Util/Metrics/PipeStats.{h,cpp}`, `MobileGL/MG_Test/Util/PipeStatsTest.cpp` | **owner (contract)** | – | – | – |
| root `CMakeLists.txt`, `MobileGL/Config.h`, `MobileGL/ConfigLoader.cpp` | **owner (contract)** | – | – | – |
| `MobileGL/MG_Test/Pipe/CMakeLists.txt` and the two stubs' creation | **owner (contract)** | – | – | – |
| `MobileGL/MG_Test/Pipe/{ResourceEmitTest, VertexInputEmitTest}.cpp` contents | **owner** (`c3`) | **owner** (`b4`, disjoint cases) | – | – |
| `MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp` | **owner (contract)** | – | – | – |
| `MobileGL/MG_Test/SanityTest.cpp` | – | – | **owner** | – |
| `MobileGL/MG_IntegrationTest/**`, `.github/workflows/test.yml`, `tools/**`, `android-plugin/**`, `MobileGL/MG_Benchmark/**` | – | – | – | **owner** |
| `docs/Disaggregated/*.md` | – | – | – | – (integrator only) |
| everything else | nobody | nobody | nobody | nobody |

The one file two packages both *touch conceptually* is `MG_Test/Pipe/ResourceEmitTest.cpp`: A's `c3` writes the
applier-side cases, B's `b4` writes the emitter-side cases. **They are disjoint `TEST` bodies in one file and A lands
first**, so B's rebase is an append. If that ever collides, it is a rebase conflict at integration, which is exactly the
shape the P2 wave-1/wave-2 collision took (`cts-waves-landed`) and it is resolved by union, never by choosing a side.

---

## D. Integration order, and what the integrator runs

### D.0 Preconditions

- The §A baseline captures (`~/w7/p3a-before-libMobileGL.so`, `~/w7/p3a-before-ctest-names.txt`,
  `~/w7/p3a-before-untouched.sha`, `~/w7/p3a-before-dirty-surface.txt`), taken in `~/w7/pipe` at `55d2af9b`
  **before any package lands**.
- `~/w7/pipe` clean at `55d2af9b`; fixture binaries hidden the way `wsl_p3a_tree.sh` does it
  (`git update-index --assume-unchanged` on `tools/trace_replay/fixtures`) — **a fixture left visible gets pulled back
  to its LFS pointer by a `rebase --abort`**.
- `~/w7/retrace_gate.py` present (flags: `--tree --lib --out -j --only`; **no `--ssim`, no `--backend`**).
- Both devices reachable and **not** locked by the GL4.6 Wave-7 campaign
  (`ls .../8ab6b3cb-.../scratchpad/w7/locks/`).
- A worktree created for a package must have its submodules initialised and `3rdparty/glslang/External` copied, or the
  build fails in a way that looks like a code error.

### D.1 Order: **contract → client → espryt → gates**

Reason, and it is not P2's by default:

- `contract` first because everything else compiles against it, and because its `PipeFill.cpp` block is what makes the
  generated-enum coupling a compile error at `c0` rather than at a merge (C.5).
- `client` second because **it is the only package that can land without changing behaviour**: with nothing registering
  `MGPipeResourceOps`, every buffer dispatch still falls through to `g_bufferBackendOps` and the VAO twin still reads
  `MGB_CTX`. If the emission is wrong, the verify lane says so before any backend has been touched — P2's `b2` logic,
  applied to a phase where the backend change is the risky half.
- `espryt` third: it is the commit that switches the path over, and it lands against a client whose emission is already
  proven by the verify lane.
- `gates` last, so its lanes assert against the finished tree — except the `HandleRecycleScenario` buffer arm, whose
  red is recorded on the contract tree at D.2.

Copy `~/w7/notes/tools/wsl_integrate_p2.sh` → `wsl_integrate_p3a.sh` (`p2`→`p3a`). Per slug: rebase `p3a/<slug>` onto
`feat/disaggregated` in its own worktree, `git merge --ff-only` in `~/w7/pipe`, then **after every merge**:

```sh
cmake --build build-linux --parallel 28 && ctest --test-dir build-linux -L unit --no-tests=error --output-on-failure
cmake --build build-push  --parallel 28   # push AND verify, both, every time - that is where bb2a236d surfaced
cmake --build build-verify --parallel 28
python3 scripts/gen_pipe.py --check && python3 scripts/gen_pipe.py --self-test
python3 scripts/gen_pipe_dirty_surface.py --check          # from `client` onward
python3 scripts/check_include_closure.py --mode both --compiler clang++-20 --self-test --require-all
python3 scripts/symbol_report.py --before ~/w7/p3a-before-libMobileGL.so --after build-linux/libMobileGL.so \
        --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0 --markdown ~/w7/p3a-symbol-<slug>.md
#   0 added / 0 removed / 0 renamed / 0 RESIZED after every merge. P3a's admitted set is EMPTY (G1).
grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'                        # empty
bash scripts/p3a_untouched_regions.sh 55d2af9b HEAD                              # G5, after EVERY merge
ctest --test-dir build-linux -N | grep -E "^[[:space:]]*Test[[:space:]]+#[0-9]+:" \
  | sed -E "s/^ *Test +#[0-9]+: //" | LC_ALL=C sort | LC_ALL=C comm -23 ~/w7/p3a-before-ctest-names.txt -   # empty
```

**Any symbol resize outside an admitted set is a defect, attributed per symbol in the commit message, never waved
through** (ID-3's precedent). P3a's admitted set is empty; if a resize appears, the cause is an edit that escaped a
`#if MOBILEGL_PIPE_PUSH` guard (D-K).

### D.2 On the contract tree, before `client`: record the "red before" evidence

```sh
cd ~/w7/p3a-gates && cmake --build build-verify --parallel 28
ctest --test-dir build-verify -R HandleRecycle --no-tests=error --output-on-failure | tee ~/w7/p3a-handlerecycle-before.log
#   the buffer arm: .AbaControl PASSES (it asserts the corrupted contents), .Legacy PASSES, .Handles SKIPs
```

That log is the artefact `ROADMAP.md`'s "重键前红" asks for, for the **buffer** kind. Keep it; it goes into
`MEASUREMENTS.md`.

### D.3 After every package has landed: the five-part gate on `~/w7/pipe`

`~/w7/notes/tools/wsl_p3a_gate.sh` is `wsl_p2_gate.sh` with these changes and **no others**:

- `p2`→`p3a` in every path and in the `P2_GATE_DONE` terminator (→ `P3A_GATE_DONE`).
- **The fail-fast rule at `:4` and `:19` is kept verbatim**: the three builds are made first and a non-zero rc prints
  `BUILD FAILED - gate aborted (no stale-binary verdicts)` and exits 1. *Results from a stale binary must never look
  like a verdict.*
- `:24`'s echo of the admitted resizes becomes `G1 symbols (admitted resizes: NONE for P3a)`.
- `:27-28`'s `SyncRenderState` sha comparison is **replaced** by `bash scripts/p3a_untouched_regions.sh 55d2af9b HEAD`
  (G5). The `RenderStateImpl` sha is P2's invariant and stays true trivially; the P3a invariant is the pool/ring set.
- `:34`'s `ctest -R 'RenderStateSpans\.|Residual'` gains `|VertexInputEmit\.|ResourceEmit\.` (G6).
- `:45`'s render-state-sensitive subset is replaced by the **buffer/VAO family**:
  `-R 'LargeArenaAdoption|StorageBufferRegrow|VertexAttribBinding|MultiDraw|PrimitiveRestart|CrossFrameBuffer|ResidentIndex|BufferTexture|AtomicCounter|XfbCaptureBufferReuse|PackedWordReadback|DoublePrecision|VertexArrayEnableDisable|DrawParameters'`
  — called out by name because these are where a vertex-input or buffer mistake shows up first.
- `:46`'s `g7_negative_control.sh build-push` is kept **and** joined by
  `bash scripts/p3a_vertex_input_negative_control.sh build-push` (G7).
- `:47`'s `CsoContentAddressing` is joined by `ResourceSubsystemControl` (G12).
- `:44` gains a third arm: `MOBILEGL_PIPE_PUSH=0x7f ctest -L integration-gpu` (P3a's subsystems off) beside the
  existing `MOBILEGL_PIPE_PUSH=0` (everything off).
- The two `retrace_gate.py` sweeps at `:54, 57` are unchanged in form; a third, named sweep is added:
  `--only 'create-indirect,create-instancing,rd12-odinlite,improved-transparency-minecraft-26.3,fabric-sodium'`
  (G3b), whose summary is copied out before the output trees are deleted.
- ID-4's ctest-name extractor at `:8` and the arming-line greps at `:55` are kept **verbatim**.

**Part 1 — interface purity.**
```sh
python3 scripts/check_include_closure.py --mode both --compiler clang++-20 --self-test --require-all   # gate A
nm --undefined-only build-linux/libMobileGL.so | grep -E 'MG_State::GLState::|glslang'                  # gate B (n/a in monolith; recorded)
grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'                                               # gate C: empty
grep -n 'MG_State' MobileGL/MG_Pipe/PipeApply.h                                                         # no hit in the ops block (G13)
ctest --test-dir build-verify -R HandleRecycle --no-tests=error --output-on-failure                     # the {slot,gen} memo-key assertion (G8)
```

**Part 2 — the semantic shadow comparison (the decisive one).**
```sh
ctest --test-dir build-verify -L unit --no-tests=error --output-on-failure
ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4 --output-on-failure
MOBILEGL_PIPE_VERIFY=1 python3 ~/w7/retrace_gate.py --tree ~/w7/pipe \
  --lib ~/w7/pipe/build-verify/libMobileGL.so --out ~/w7/retrace-out/p3a-verify -j 4
grep -L 'MGPipe verify:' ~/w7/retrace-out/p3a-verify/*.log        # empty: every case armed
grep -l 'Fatal{'         ~/w7/retrace-out/p3a-verify/*.log        # empty
```
A `Fatal{UnmigratedPipeInput, "F@V"}` is a missing `FillPoints.def` row — add the row, **never a sticky row**
(`Coverage.def:106-115`, `:465-466`: none of the version/generation accessors is sticky and `gen_pipe.py` refuses a
name that is not an accessor), regenerate, rerun. A `Fatal{PipeVerifyDiffer, …, where=read}` is a real push/pull
divergence and names its field; it is fixed, never silenced. A `Fatal{ProtocolCorruption}` is a malformed
`CreateVertexElements` blob or an out-of-range `MGPSubData` box (D-G2, D-A2).

**Part 3 — behavioural A/B.** As `wsl_p3a_gate.sh` parts 3 above, plus the three retrace sweeps.

**Part 4 — performance. RECORDED, NOT GATED (rule (a)).** D.4.

**Part 5 — coverage, poison, handle discipline.**
```sh
python3 scripts/gen_pipe.py --check && python3 scripts/gen_pipe.py --self-test                 # 0 UNMAPPED
python3 scripts/gen_pipe_dirty_surface.py --check && python3 scripts/gen_pipe_dirty_surface.py --self-test
ctest --test-dir build-push -R 'RenderStateSpans\.|Residual|VertexInputEmit\.|ResourceEmit\.' \
  --no-tests=error --output-on-failure
bash scripts/p3a_untouched_regions.sh --self-test
```

**CTS (G15).** P3a **is** one of the five architecture boundaries (`ROADMAP.md:34`), so the **full `gl44to46` caselist
(~56,271 cases) runs on both devices**, scheduled off the critical path at the `dev` merge. Per-backend conformance
within 0.5 pp of the `$BASE` reading; report shape per the house rule (rows = GL version/extension, columns = status
counts, rate = Pass/(Pass+Fail), NS not in the denominator). The `MOBILEGL_PIPE_POISON_OMIT` sweep that
`FillPoints.def:60-64` assigns to P3a rides the same caselist run.

**CI.** `gh workflow run test.yml --ref feat/disaggregated`, then once green
`gh workflow run test.yml --ref feat/disaggregated -f baseline_sha=55d2af9b` for `monolith-symbol-report`, and once more
with `-f baseline_sha=087685d1` for the four-resize reading of G1. Record all three run URLs. Expected green:
`pipe-gates`, `include-graph-check`, `flatc-check`, `test`, `integration`, `benchmark`, `build-linux-verify`,
`integration-verify`, `retrace`, `retrace-verify`, `monolith-symbol-report`. **The exit evidence is the local WSL run
(`~/w7/pipe`), not CI; CI green is required before the `dev` merge, not before the phase verdict**
(`BRIEF-P2.md:965`).

**A decision the CI file forces, carried over from P2.** `test.yml:520-526` records that `integration`'s second,
`MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1` filtered pass (186 entries) is not run under the comparator; P2 deferred
it to P3b saying *"the 186 entries are a buffer/readback filter that P2 changes nothing in"*. **P3a changes exactly
that filter's subject.** Decision: **P3a runs it, once, under push (not under the 5–10× comparator)**, as a named step
in part 3 — `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1 ctest --test-dir build-push -L integration-gpu -j 8` — because
it is the kill-switch arm of `FlushPendingRangesNow`'s tier 1 (`Managers.cpp:1017`) and P3a rewrites that function's
caller. Running it under the comparator stays deferred to P3b, and the integrator records the split decision in
`MEASUREMENTS.md` rather than leaving the note dangling a second time.

### D.4 The measurement runs P3a owes — recorded, not gated

Rule (a): **every number below is published; none of them can stop the phase.** A regression is written into
`MEASUREMENTS.md` with its suspected cause and carried into P4a's planning.

#### D.4.1 The pull baseline to record against

Already in `MEASUREMENTS.md`, no new collection needed:
- §3, the two-device / two-backend / four-trace boundary-counter table (`:53-62`), whose headline is **steady-state
  dynamic accessor cost 6.5–9.3 per draw** (`:64`) — for `improved-transparency-minecraft-26.3` specifically, a
  1200-frame window at 1320 draws/frame with Espryt acc/draw **8.44** and Magma **6.53**, Espryt buf 333 K B/f, gates
  ers 156925/2791, etl 148606/11110, eub 148246/11470.
- The six memo gates' hit/miss pairs and the `stage-*` byte classes, including `stage-ubo-named`'s 331 KB/frame
  asymmetry (`:68`) and the union-box vs region-list 635 K vs 40 K, 16× (`:70`).
- **The one number `ROADMAP.md:19` names**: `MEASUREMENTS.md:87` — *"**persistent map 采纳的既有基线**（`dev`，MC 26.3,
  Adreno）：≥16 MiB 可变 store 定义时采纳为 coherent persistent map 后 p99 163→21 ms、稳态 40→115 fps、省 ~400 MB"*.
- P2's own §8 table (the two-device p50/p99, T1/T2, the blend-toggle table, the CSO negative control).

**The caveat that must travel above the numbers, not below them** (`BRIEF-P2.md:417-419`, D17):
`CallClass::AccessorCalls` is a set of **static tallies at ~10 hot entry points** (`PipeStats.cpp:16-100`, and *"那份
清单是契约"*), so a change that merely **relocates** reads scores a lower `acc/draw` without removing work. Lead with
the **gate hit/miss pairs** and the **CPU-time series**; quote `acc/draw` only alongside a re-audit of the tally
constants at `DirectGLES.cpp:1421, 1524, 2043, 2053, 2977` and `VulkanRenderer.cpp:5009, 5016, 5274, 5927, 5934, 6416,
6449, 6462`.

#### D.4.2 The paired two-device A/B

Protocol per device, unchanged from P2 and driven by `p3a_ab.sh`:

- **reboot-clean**: `adb -s $SER reboot`, wait for `sys.boot_completed`, poll for root (24 × 5 s), sleep 20, wake +
  unlock, uninstall the package, then **pin up to 3 times with a `check` after each and exit 43 if none verified**.
- **CPU/GPU pinned** via `tools/device_bench/pin_device.sh <serial> pin|unpin|check` — **not** `bench.sh`'s
  `pin_freqs()`, which is MediaTek-legacy-only and **writes into nothing and exits 0 on both campaign devices**.
  `check` is three-valued: **0 PINNED**, **1 DRIFT** (*the dangerous state — a run overlapping it is not comparable,
  discard it*), **2 UNPINNED**. Sequence: `pin` → `bench.sh … --no-pin` → `check || "PIN DRIFTED - discard this run"` →
  `unpin`. Big 1.96 GHz / little 1.55 GHz, GPU maxed, 40 °C gate.
- **serially from one tree**: `run_android_retrace_local.py` shares one `.trace-work/android-retrace-result` root per
  tree and `rmtree`s it per invocation (`MEASUREMENTS.md:93`), so the two devices **must not** be driven concurrently
  from one worktree.
- **device lock** ≤ 25 min per acquisition, `owner` file written, `rm -rf` on the way out; a lock held > 60 min is not
  broken — report "blocked by device lock".
- `MSYS_NO_PATHCONV=1` on every invocation (`/data/...` inside an `--env` value is path-converted otherwise); but **do
  not export `MSYS_NO_PATHCONV` / `MSYS2_ARG_CONV_EXCL` globally in `p3a_ab.sh`** — the runner passes `/c/...` paths to
  Windows python and relies on MSYS converting them.
- **ColorOS traps** on `3B159D009VZ00000`: the first install of a not-yet-installed package blocks on
  `com.oplus.appdetail InstallGuideActivity` until "继续安装" is tapped; a foreign-signed APK must be uninstalled first.
- Git Bash cannot create directories on `//wsl.localhost`; outputs go to a Windows-local scratchpad and `copy_ab.sh`
  moves them into `~/w7/notes/p3a/ab/<serial>/`.

Arms: **{pull APK, push APK} × {`--benchmark-no-finish`, finish} × 4 cases × 2 backends**, per device. Cases, chosen to
match `ROADMAP.md:19`'s named set minus the blocked one:

```
improved-transparency-minecraft-26.3            # the p99 number ROADMAP.md:19 names
minecraft-1.21.4-rd12-odinlite-in-world
minecraft-1.21.4-fabric-sodium-in-world
minecraft-1.21.1-neoforge-create-instancing-in-world   # a coherent_as_flush fixture
```

**Excluded**: `minecraft-1.21.1-neoforge-create-indirect-in-world` — it fails on both devices on the `dev@81b17c0b`
baseline APK itself (`MEASUREMENTS.md:96`, `ROADMAP.md:90`), so it is not this branch's regression. It stays in the
desktop SSIM corpus and out of the device A/B, exactly as in P2.

```sh
LOCKS=.../8ab6b3cb-.../scratchpad/w7/locks
ANDROID_SERIAL=$SER MSYS_NO_PATHCONV=1 python3 tools/trace_replay/run_android_retrace_local.py \
  --case "$c" --backend "$b" --benchmark --benchmark-no-finish --benchmark-repeats 3 --benchmark-tail-frames 200 \
  --env MOBILEGL_PIPE_STATS=1 --env MOBILEGL_PIPE_STATS_PERIOD=120
```

Then, host-side, reduce `frameCpuTimesMs[]` over the trailing 200 frames to **p50 and p99** per arm and publish the
delta `push − pull` per (device, case, backend). `benchmark.json` carries the whole array on purpose, so p99 is a
**host-side reduction over an artefact that already exists**; `nearest_rank_percentile` uses nearest rank so every
number printed is a frame that was actually observed, and `series_median` recomputes p50 by the device's own rule so it
agrees with `medianFrameCpuMs` rather than merely sitting beside it. `--benchmark-no-finish` is the **primary** arm
because P3a's question is CPU cost; the finish-ON arm is the sanity check that GPU time did not move.

Read out of the same runs: the six memo gate hit/miss pairs (`ers etl eub mfp mpm mdt`) from the **last complete
window** of `grep 'MGPipe stats:' mobilegl.log`, the `stage-buffer` / `stage-vertex-client` / `stage-index-client` byte
classes, and **`mpr`** (G10).

**Trap**: `MOBILEGL_PIPE_STATS_FILE` never produces a file on device — the JSON is written from `PipeStats::Shutdown()`
which runs from `MobileGL::Destroy`, and the trace app never reaches that teardown. Only the periodic `mobilegl.log`
lines exist, and a run shorter than one period yields nothing; hence `MOBILEGL_PIPE_STATS_PERIOD=120`, or lower for a
short fixture.

#### D.4.3 The headline number: MC 26.3 p99 on Adreno

Published as: p99 and p50 of `frameCpuTimesMs[]` for `improved-transparency-minecraft-26.3` on `35d0befa`, both
backends, pull vs push, against `MEASUREMENTS.md:87`'s adoption baseline (p99 163→21 ms, 40→115 fps, ~400 MB) and
against P2's own §8 reading of the same case. **Recorded, not gated** (rule (a)). If p99 moved, the report says by how
much, on which arm, and names the suspected cause; the phase proceeds.

#### D.4.4 DriverBench T1/T2

```sh
cd ~/w7/pipe && cmake --build build-bench --parallel 28
for lib in build-linux build-push; do for be in espryt magma; do
  DRIVERBENCH_EGL_LIB=$PWD/$lib/libMobileGL.so bash MobileGL/MG_Benchmark/Driver/run_driver_bench.sh $be $PWD/$lib/libMobileGL.so
done; done
bash MobileGL/MG_Benchmark/Driver/run_driver_bench.sh native      # the control
```

- **T1** = `ns_per_op(push, default bitmask 0x1ff) − ns_per_op(pull)` on `mc_vanilla_draw` — the whole boundary cost per
  draw.
- **T2** = `ns_per_op(push, MOBILEGL_PIPE_PUSH=0x7f) − ns_per_op(pull)` — P2's boundary alone, so **T1 − T2 isolates
  exactly what P3a added and what it removed.**
- `mc_state_toggle` is published beside them as the non-P3a control (a blend toggle touches no buffer and no VAO, so it
  must not move).

`DriverBench` `dlopen`s exactly one EGL provider through `DRIVERBENCH_EGL_LIB` with **no `LD_LIBRARY_PATH` shadowing**,
and pins the vendor json / ICD explicitly — a bare `libEGL.so.1` on a glvnd system resolves to Mesa/llvmpipe, i.e. *a
software rasteriser silently replacing the GPU under a benchmark*. `wsl_p3a_gate.sh` configures with
`-DMOBILEGL_BUILD_BENCHMARK=OFF`, so this runs in `build-bench`, not in the gate build. **No ceiling is pinned and none
is enforced** (rule (a)); the numbers are published so P4a can pin one if it wants to.

#### D.4.5 The Track H unit-cost census

`ROADMAP.md:56`'s criterion is *"Track H 单位成本不超出估计的 50%"*, measured in calendar days. Record the elapsed days
per package, the diff sizes, and the memos actually retired against `ARCHITECTURE.md:363`'s census (11 direct
deletions, 2 moved to client debounce, 7 re-keys, 1 unchanged). **P2 paid 11 of the 11 direct deletions and 2 of the 7
re-keys.** P3a pays: the twelfth-through-thirteenth entries of the direct-deletion list are already gone, so P3a's
contribution is **`ConvertedVertexStreamKey`'s `sourcePin`** (the last of the 11, D-J) plus **two of the seven
re-keys** — the VAO twin's index-slot `{Uint16 version + raw pointer}` → one server `Serial`, and
`ResolvedDrawBuffers`' raw `BufferObject*` entries → `{slot, gen}`. Also record the **27-day trip wire**
(`ROADMAP.md:64`): if P3a exceeded it, `ROADMAP.md:68` says to run the `inproc` falsification numbers before P4a.

#### D.4.6 What is published even though it is not asked for

- `CreateVertexElements` blob bytes per frame on the four A/B cases — the cost of D-G2's second view, so P13's trim
  decision starts from data rather than from the argument in this brief.
- The per-dirty-bit fire rates for bits 5, 9 and 10 before and after the narrowing, so P3b/P4b's object-class bits
  start from data the way P2's table let P3a start from data (`BRIEF-P2.md:956`).
- Peak RSS, push vs pull, over the 79-case retrace — the seventh slot table is new memory and a leak in it would be
  invisible to every correctness gate.

### D.5 Docs, and the merge to `dev` (integrator, one commit)

`scripts/check_doc_citations.py docs/Disaggregated/*.md` must stay clean — every `file:line` a doc edit adds has to be
true. The scout corrections in this brief's preamble are **not** doc bugs (the scouts mis-cited, the docs are right)
except where noted below.

- `README.md:3` — status → P3a landed, with the hash.
- `ROADMAP.md:19` — ✅, with the real numbers: the nine `resource_*`/vertex calls wired, the seventh slot table, the
  memos retired, `map-persistent-roundtrips`' first readings, and the MC 26.3 p99 pair.
- `ROADMAP.md:83` (open question 10) — restated: P3a kept the `ResidentSubData` asymmetry and did **not** give Magma an
  implementation.
- `ROADMAP.md:90` (open question 17) — the `create-indirect` state at the phase exit, honestly: fixed on `dev`, or
  still blocking G3b.
- `ARCHITECTURE.md:63` — **[deviation] D-G1**: the vertex-elements CSO is identity-addressed for Espryt; the
  content-addressed 1024-entry cache is P7's, when Magma's `VertexInputStateFactory` takes the CSO over.
- `ARCHITECTURE.md:130` — the stated reason for carrying both views is corrected: the frontend already resolves
  pointer-stride-0 (`MGPipeValueTypes.h:496-503`), so the reason both views travel is record self-consistency
  (`BindingPointCount` describes its own blob), not stride disambiguation.
- `ARCHITECTURE.md:284` — **[deviation] D-K**: `SetBackendResource` is not deleted in P3a; under push it is simply
  never called, and the deletion retires with the pull path at P13 (G1).
- `ARCHITECTURE.md:293` — `kNeedsAck` has landed on `ResourceRespecify` **with a per-record predicate**, and the
  sentence "目录里目前没有条目携带 `kNeedsAck`" is now false and is replaced.
- `ARCHITECTURE.md:367` — `MOBILEGL_PIPE_LEGACY_MEMOS`'s P3a arm is named.
- `ARCHITECTURE.md:474` — `map-persistent-roundtrips`' definition is written down: **every `MapPersistent` emission,
  mint or decline**, which is why it is non-zero and assertable in monolith (D-B2).
- `ARCHITECTURE.md:504` — the citation `tools/bench.sh` → `tools/device_bench/bench.sh` if P2's integrator did not
  already fix it.
- `MEASUREMENTS.md` — a new numbered section with: the D.4.2 two-device p50/p99 table (with D.4.1's `acc/draw` caveat
  **above** it), the MC 26.3 p99 pair against `:87`, T1/T2 and the `mc_state_toggle` control, the per-dirty-bit fire
  rates for bits 5/9/10, the `mpr` readings, the `CreateVertexElements` blob bytes, the D.2 "red before" log, peak RSS,
  the Track H unit-cost census, the `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH` decision of D.3, **and the user's
  2026-09-08 rule (a) transcribed verbatim**, since it is not written anywhere in `docs/Disaggregated/`.
- `MEASUREMENTS.md` also records the `g_uploadRing`-not-reset asymmetry (D-F) as a `dev`-side follow-up that P3a
  deliberately preserved.

Then `[Merge]` to `dev` per the milestone flow, and schedule the full `gl44to46` caselist on both devices **off the
critical path** (`ROADMAP.md:34`, G15). Commit messages are single-line `[Type] (Scope): description`; **never** a
`Co-Authored-By` or any other attribution line.

---

## E. Risks and mitigations

| risk | mitigation |
|---|---|
| **`MGPVertexBuffers` growing 16 → 24 breaks something no test covers** — a wire `static_assert`, a `PipeFields.def` row, a generated table. | It is a **build break at `c0`**, not a silent drift, and `c0` is the first thing built. `gen_pipe.py` refuses a field list that does not name exactly the struct's direct members (`PipeFields.def:16-19`) **in both modes, hence in `pipe-gates`**, and `git diff --exit-code -- MobileGL/MG_Pipe/generated` catches an unregenerated table. The `MGP_VERIFY_PAYLOAD_LIST` row is the one a compiler cannot catch — hence its explicit line in C.0's table. |
| **The `Coverage.def` ↔ `SubsystemForEmitter` semantic-merge trap fires again** (commit `bb2a236d`: an emitted row removed on one branch, its generated enumerator still named on another, and the push and verify builds did not compile on the integrated tree). | Discharged **structurally**: A owns `Coverage.def` **and**, in the contract commit only, `PipeFill.cpp`'s enum-coupled block; B branches from the tag, so A's edit is already in B's base and the two cannot diverge (C.5). On top of that, **every merge in D.1 builds push AND verify**, which is the only place the trap surfaces, and `gen_pipe.py --self-test`'s control at `:1230-1234` refuses an emitted row naming a call that does not exist. |
| **A `kNeedsAck` on `ResourceRespecify` acks every `glBufferData`** — Minecraft's chunk upload turns into a round trip per store in split. | The flag is declared on the call, but the **per-record predicate** `MGPipeResourceRespecifyNeedsAck(desc) == (desc.Immutable != 0)` decides (D-A5), the legend at `PipeCalls.def:18` says so, and `PipeCatalogueTest.ResourceRespecifyAcksOnlyImmutableStorage` pins it against both idioms. In monolith the ack is `((void)0)`, so P3a cannot ship the mistake even if the predicate were wrong; P5 is where it would bite, and P5 inherits a pinned test. |
| **The vertex-elements CSO is identity-addressed, deviating from `ARCHITECTURE.md:63`** — and P7 then finds it cannot content-address Magma's without redoing P3a's work. | The deviation is *narrower*, not wider: P3a mints one CSO per VAO, which is a **valid content-addressed cache with a hit rate of one**. P7 adds the hash-probe-`memcmp` layer above the same `CreateVertexElements`/`BindVertexElements`/`DeleteVertexElements` calls and the same slot allocator; nothing in P3a's wire shape or applier record forbids it. The reason it is not done now is that Espryt's twin is a driver VAO name plus 64 scratch buffer ids that two frontend VAOs cannot share, so content addressing would be strictly slower on the only backend P3a touches. Recorded against `ARCHITECTURE.md:63` at the phase exit. |
| **`BaseInstance` is not in the `ContentHash`, so a baseInstance-only change is suppressed and the server keeps the previous fetch shift** — silently wrong geometry on instanced draws, and no SSIM gate on a desktop corpus that may not exercise it. | It is stated as a hard requirement in D-H2.3 with two named unit tests (`ABaseInstanceChangeAloneStillEmitsTheVertexBufferSet`, `AnUnchangedSetWithAnUnchangedBaseInstanceEmitsNothing`), and the behavioural gate is `VertexAttribBindingScenario`'s two dedicated cases — `BaseInstanceMovesTheInstancedArraysStartElement` (`:406`) and `BaseInstanceLeavesPerVertexArraysWhereTheyWere` (`:451`) — which capture the vertex shader's inputs with XFB under `GL_RASTERIZER_DISCARD` from a poison-prefilled buffer, plus all eight `DrawParametersScenario` cases (`:213-327`). |
| **Deleting the ambient `g_pendingFetchBaseInstance` breaks a path nobody remembered** — an indirect or multi-draw entry point that relied on it. | `grep` says there are **exactly three** scope sites (`DirectGLES.cpp:5135, 5172, 5224`) and all three are direct `*BaseInstance` entry points; `DrawElementsIndirect` (`:5216`) and the multi-draw tiering never used it. The check is mechanical and is in C.2's verification (`grep -n 'g_pendingFetchBaseInstance\|ScopedFetchBaseInstance' … # empty`), and `MultiDrawScenario`'s ten cases plus `DrawParametersScenario`'s `MultiDrawElementsIndirectCarriesEveryCommandsParameters` (`:301`) are the behavioural backstop. |
| **`BindMask & ELEMENT_ARRAY` is set wrong and nothing in P3a can see it** — it only matters in split, where a wrong bit silently disables restart rewriting and multi-draw flattening (P8 expectation 1). | The mask is built from one `constexpr` `BufferTarget` → bit table with a `static_assert` that it covers every enumerator, so a new target cannot be silently unmapped; `ResourceEmitTest.EveryBufferTargetSetsItsBindMaskBit` walks all of them on create **and** on respecify; and it is sticky, so a buffer bound as an EBO once keeps the bit forever. This is the one P3a deliverable whose only real gate is a unit test, and it is called out here so a reviewer knows to look at it. |
| **The seventh slot table has no garbage collector, so a buffer nobody destroys leaks its twin forever** — worse than today's `weak_ptr`-in-`PipeResource` ownership, which dies with the frontend object. | `ResourceDestroy` is emitted from `~BufferObject` **unconditionally**, not from a sweep, and the ordering (emit, then `MGPipeSlots().Free`) is fixed in D-L with `HandleRecycleScenario`'s three arms as the gate. Peak RSS over the 79-case retrace, push vs pull, is published (D.4.6) precisely so a leak is visible. `Managers.h:306-314` quantifies what the old lazy sweep cost, which is the comparison. |
| **The reverse-channel `OnBufferWriteback` resolves a stale or dangling handle** — the client's `slot → BufferObject*` inverse is a raw pointer. | The entry exists only between `ResourceCreate` (constructor) and `ResourceDestroy` (destructor), and a readback is only ever issued for a live, bound buffer; a `MGPipeHandle::Gen` compare against `MGPipeSlots().GenOfSlot` guards it in debug builds. If that assumption is ever wrong it is a debug abort, not silent corruption. |
| **The epoch bump lands before the writeback** and the server's draw-clean memo goes stale behind it. | `ARCHITECTURE.md:288` makes the order a correctness rule and D-D writes it as a five-step sequence. The existing coverage is `AtomicCounterScenario.SubDataAfterDispatchSurvivesAnImmediateReadback` (`:242`) and `LargeArenaAdoptionScenario.GpuWriteIntoTheArenaIsReadBack` (`:245`), both of which fail if the bump moves. `Managers.h:556-590` is the exhaustive bump-site contract and must be read before touching anything in that file. |
| **`AcquirePersistentMap` is "untouched" in principle but its signature change perturbs it in practice** — and D-B4 is the one thing the whole monolith conversion is supposed not to break, worth p99 163→21 ms and ~400 MB on the headline device. | D-E enumerates the **only two** permitted changes and names every statement that must stay byte-identical, down to the locally defined `0x0040/0x0080/0x0100` constants and the stale-generation wipe ordering. The behavioural gate is `BufferTest.cpp`'s ~30-case adopted-store family (`:1658-2943`), `LargeArenaAdoptionScenario`'s three cases, and MC 26.3's p99 on Adreno. A reviewer should diff the function against `$BASE` by eye; it is 76 lines. |
| **`FlushPendingRangesNow`'s three-tier drain silently changes tier** because its caller now supplies bytes differently — tier 1 (`INVALIDATE_BUFFER` map + memcpy) is taken only for a whole-buffer flush or ≥128 KiB, and dropping into tier 3 costs the Mali WAR stall back. | The function is in **G5's byte-identical set** and its kill switch (`Features.EsprytDisableInvalidateFlush`, `Managers.cpp:1017`) gets its own integration pass in D.3. `CrossFrameBufferScenario`'s thirteen cases (`:409-518`) plus `StreamedArenaScenario`'s two recycle cases (`:675, 731`) are the behavioural gate, and MC 26.3's p99 is the number that moved when this last regressed. |
| **`create-indirect` is still red on `dev` at the phase exit**, so a named acceptance case cannot be run. | It is a **pre-existing `dev` bug** (`MEASUREMENTS.md:96`, `ROADMAP.md:90`), reproduced on the `dev@81b17c0b` baseline APK, and it is not P3a's to fix. G3b is recorded as "partial, blocked on open question 17" rather than claimed as a pass, the fixture stays in the desktop SSIM corpus, and it stays out of the device A/B. P8 asserts `roundtrips-per-frame == 0` on precisely that fixture, so the `dev` fix is on someone's critical path — just not P3a's. |
| **The 27-day trip wire fires.** | `ROADMAP.md:64` and `:68`: run the `inproc` falsification numbers, then decide. The desktop half of every correctness gate can NO-GO the phase without touching a device, so the schedule's long pole (two shared devices under a `mkdir` lock, ~32 lock holds of ~10 min for D.4.2) sits **after** the verdict, not before it. Budget two days for the device work and hold ≤ 25 min per acquisition. |
| **A gate goes green for the wrong reason** — `ROADMAP.md:7`'s **每个门必须能因它存在的理由变红**. | Every P3a gate ships its negative control: G5 has `p3a_untouched_regions.sh --self-test`; G6 has G7's scripted field-drop; G8 has the `.AbaControl` arm that asserts the corruption and whose D.2 log proves it was red before the re-key; G9 has the scanner's two canned controls; G10's `mpr` would read zero if the counter were never incremented, which the scenario asserts against; G12 has `ResourceSubsystemControlScenario` proving the switch changes behaviour rather than being dead; G4 has the verify lane's existing `MOBILEGL_PIPE_VERIFY_CORRUPT` and `MOBILEGL_PIPE_POISON_OMIT` controls, whose steps pass **when ctest fails**. |
| **A test that drives a backend helper directly aborts on the per-verb poison** because no verb filled the block. | `MG_Test/ScopedPipeVerb.h` exists for exactly this and eleven tests already use it (`ef6227e1`). Any new test declares its verb the same way; the poison is never weakened and **no test name changes** (G14). |
| **A worktree builds against a stale `.cxx` or a half-initialised submodule** and a green looks like a verdict. | `wsl_p3a_gate.sh` keeps `wsl_p2_gate.sh`'s fail-fast rule verbatim: three builds first, and any non-zero rc prints `BUILD FAILED - gate aborted (no stale-binary verdicts)` and exits 1. Tree creation initialises submodules and copies `3rdparty/glslang/External`; fixtures are hidden with `git update-index --assume-unchanged`, because a `rebase --abort` on a visible fixture pulls it back to its LFS pointer. |
| **Performance regresses and someone stops the phase for it.** | Rule (a) is explicit and is transcribed into `MEASUREMENTS.md` (D.5): **performance is recorded against the pull baseline, never a blocking gate in P3a; correctness gates stay.** A regression is written down with its suspected cause and carried into P4a's planning. The one thing that is *not* negotiable is that the number is actually collected — an unmeasured regression is not a recorded one. |

