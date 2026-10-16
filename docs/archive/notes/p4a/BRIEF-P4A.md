# P4a implementation brief — handle wave 2 (Espryt): framebuffer, texture, renderbuffer, sampler, program identity and descriptors

Tree: `feat/disaggregated @ 37da3c3a` (the P3a docs commit; the P3a **landing** hash is `fde5fda3`). Worktree
`C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`, **read-only** for the scouts and for this
brief. **`37da3c3a` is the P4a base ref**; every `file:line` below was re-opened at it. `$BASE` = `37da3c3a`
throughout. (ID-1's lesson applies: if `dev` is merged or a version bump lands before a package branches, the
integrator restates `$BASE` in INTEGRATOR-DECISIONS and every package re-opens the files it edits.)

WSL: `~/w7/pipe` is a worktree of the same repo on `feat/disaggregated`; `~/w7/base` is `dev@81b17c0b` (the performance
anchor, untouched). Tree creation is `~/w7/notes/tools/wsl_tree.sh p4a <slug> <base> [verify]`, integration
`~/w7/notes/tools/wsl_integrate.sh p4a <slug>...` (ID-3: the phase is a parameter, not a copy of the script).

Corrections to the scouts are **[correction]**; deliberate departures from the design documents are **[deviation]** and
carry their reason and the doc line the integrator must correct. Every ruling this brief makes is labelled `D-<letter>`
and **is not re-decided inside a package**.

---

## Scout verification summary

All five scouts (`scout-docs-spec.md`, `scout-espryt-framebuffer.md`, `scout-espryt-sampler-program.md`,
`scout-pipe-and-client.md`, `scout-frontend-state.md`) were read in full and their load-bearing claims re-opened at
`37da3c3a`. What follows is wrong or incomplete in them and is fixed here. Everything not listed was confirmed
verbatim — including `ROADMAP.md:20`'s five cells, `README.md:3`, every `MGP_ASSERT_POD` size, every
`ARCHITECTURE.md` line quoted in §2 of `scout-docs-spec.md`, and every `Managers.cpp` / `DirectGLES.cpp` function
address in `scout-espryt-framebuffer.md` §1–§4.

1. **[correction] `scout-espryt-framebuffer.md` §0 and §7.3 claim a `DrawBuffers[8]` width mismatch. There is none.**
   `FramebufferObject::MAX_DRAW_BUFFERS == MobileGL::kMGMaxDrawBuffers == 8` (`MGPipeValueTypes.h:30`,
   `FramebufferObject.h:107`), so `MGPFramebufferState::DrawBuffers[8]` matches the frontend array exactly. The real
   and *only* width question is `MGPSurface Color[8]` against `MAX_COLOR_ATTACHMENT_SLOTS = Color31 - Color0 + 1 = 32`
   (`Managers.h:1579-1581`, `FramebufferObject.h:66-69`), and 32 is a **token space**, not an attachment-point count:
   `ValidateColorAttachmentInRange` (`Validators.h:17-20`) rejects anything at or above
   `GetDynamicParameters().MaxColorAttachments`, which is the driver's raw ES cap
   (`BackendObject_DirectGLES.cpp:1518`, default 8 at `BackendObject.h:407`) and is **not clamped to 8** on the GLES
   path. D-C3 rules on it.
2. **[correction] `scout-espryt-framebuffer.md` §0's `PipeCalls.def` line numbers for the verb group are off by one.**
   Verified rows: `SetShaderImages` `:119` (not `:120`), `Blit` `:143`, `Clear` `:144`, `ReadPixels` `:145` (not
   `:142/:143/:144`). Everything else in that list is exact. The authoritative opcode/line map is D-A1's table below,
   generated from the file.
3. **[correction] `scout-espryt-framebuffer.md` §0 lists `GenerateMipmap`, `GetTextureImage`, `ResourceCopyRegion`,
   `Blit`, `Clear` and `ReadPixels` as "rows P4a owns". They are not P4a's.** `ROADMAP.md:20`'s column-3 cell names
   eleven deliverables and none of them is a transfer verb or a readback; `ROADMAP.md:23` puts **"回读 / pack state"**
   in the **P3b/P4b** row, and the transfer verbs ride P8's `draw_vbo`/`HostResolve` work (`ROADMAP.md:25`). D-Q
   states the scope boundary explicitly, because getting this wrong triples the phase.
4. **[correction] `scout-espryt-sampler-program.md` §1.7 and §6.7 cite `ARCHITECTURE.md:322` for
   `g_rawDepthFetchSamplerState`; it is `:323`, and the ruling goes the other way.** `:323` groups it with Magma's
   placeholder textures, and `ROADMAP.md:23` assigns *"raw-depth-fetch sampler 原生化"* to **P3b/P4b**, not P4a. D-F3
   keeps it where it is and says why the purity gates still pass.
5. **[correction] `scout-espryt-sampler-program.md` §1.9 and §3.5 cite `ARCHITECTURE.md:315` for the Espryt
   do-not-touch list and the UBO ring; the list is at `:318` and the one item moved out of it is at `:321`.**
6. **[correction] `scout-docs-spec.md` §4.1 attributes the no-drive-by rule to `ROADMAP.md:100`'s citation of
   `ROADMAP.md:7`. `:7` does not contain it.** The standing statements are `ROADMAP.md:98` (open question 15,
   *"独立 `dev` 问题，拆分不得借机顺手修"*) and `MEASUREMENTS.md:519`, which cites `:98` correctly. `ROADMAP.md:100`'s
   internal citation of `:7` is itself a doc bug; D.5 has the integrator fix it.
7. **[correction] `scout-frontend-state.md` §5.1 says the dirty-surface gate "will fail the moment the scan is re-run
   against a `UseProgram` call site it now recognises". It will not, and it never has.**
   `gen_pipe_dirty_surface.py:72-77` matches `pGLContext->` followed by a name beginning with
   `Add|Set|Mark|Bump|Allocate|Truncate|Record|Notify|Begin|End`. `UseProgram` begins with `Use`, so
   `GL_Program.cpp:1467` and `:1480` are invisible to the scanner and `--check` is green today. The hole is real but it
   is a **coverage hole in the prefix heuristic**, not a red gate; D-K5 closes it deliberately and names the exact
   widening.
8. **[correction] `scout-espryt-sampler-program.md` §1.5 and §3.5 give `BindCurrentUnitSamplers` at
   `DirectGLES.cpp:3593` and `BindCurrentProgramWithResources` at `:3765`.** Verified: the definitions are `:3595` and
   `:3766` (the forward declaration of the latter is `:3310`); `BindCurrentUnitSamplers` is called from `:3753`.
   `SyncCurrentProgram` is `:3059`, not `:3104` (`:3104-3128` is the twin-resolution block inside it).
9. **Confirmed at `37da3c3a`, unchanged from the scouts, and load-bearing:**
   `MGP_CALL_LIST_DOCUMENTED_COUNT 71` (`PipeCalls.def:75`) with exactly 71 `X(...)` rows and the numbering rule at
   `:26-29`; every payload size (`MGPResourceDesc` 88 `:176`, `MGPSamplerDesc` 32 `:287`, `MGPSamplerView` 36 `:304`,
   `MGPTextureParams` 32 `:318`, `MGPProgramDesc` 192 `:335`, `MGPSurface` 24 `:353`, `MGPFramebufferState` 304 `:372`,
   `MGPBoundView` 24 `:438`, `MGPSamplerViews` 16 `:444`, `MGPSamplerStates` 16 `:451`, `MGPImageView` 24 `:462`,
   `MGPShaderImages` 16 `:468`, `MGPGlobalConstants` 40 `:513`, `MGPSubRegion` 40 `:614`, `MGPSubData` 72 `:648`);
   `MGPipeKind`'s fourteen kinds (`MGPipeHandles.h:24-40`), `kMGPipeDefaultFramebuffer{0,1}` (`:73`) and the composite
   band (`:86-97`); the subsystem bits 0..8 and both phase constants (`MGPipe.h:72-95`); `MGPipeDirty`'s eighteen bits
   in the order the scouts list them and `MGPipeSubsystemForDirty`'s `default: return 0` (`Tracker.h:57-155`);
   `MGPipeSuppressorSlot`'s seven slots with their P3b/P4b labels (`SetHashSuppressor.h:43-53`);
   `kMGPipeMaxResourceSlots = 1<<20` / `kMGPipeMaxVertexElementsSlots = 1<<16` and the never-a-resize argument
   (`PipeApply.h:97-113`); `MGPipeResourceOps`' nine members (`PipeApply.h:74-91`); the six `TwinRegistry` instances
   (`Managers.h:1181, 1521, 1612, 2130, 2230, 2257`) and P3a's seventh table (`:691`);
   `OnFrontendStateObjectDestroyed`'s six arms (`Managers.cpp:198-245`); `ClassifyPipeSubsystemArm` /
   `StopOnArmlessPipeSubsystem` and the **bit-8-requires-bit-7 refusal** (`Managers.cpp:2348-2410`);
   `sizeof(SamplerParameters) == 100` with **three bytes of trailing padding, no `MGP_FIELDS_SamplerParameters` and no
   `MGP_VERIFY_PAYLOAD_LIST` row** (`MGPipeValueTypes.h:462-486`, `:615`; `PipeFields.def:315-331`);
   `ProgramArtifacts.h`'s four libstdc++ size pins and the **inert** libc++ branch (`:558-565`);
   `PADDING_MEMBER_RE = ^Pad\d*$` in `gen_pipe.py:306` (so a `Pad0` renamed to a real name **must** gain a
   `PipeFields.def` row); `PipeStats`' `CallClass` tail inside `#if MOBILEGL_PIPE_PUSH` ending at
   `MapPersistentRoundtrips` (`PipeStats.h:105-127`) and **`ByteClass` with no such guard** (`:46-77`);
   `MipmapStorage::kMaxDirtyRects = 96` (`MipmapStorage.h:75`); `ScopedDefaultUnpackState::s_synced` written at
   `Managers.cpp:4878`, read at `:4875`, and **invalidated nowhere**; the seven `HandleRecycleScenario` cases at
   `:521, 568, 678, 768, 869, 920, 1015`; `PipeSlotKind`'s two members (`PipeSlotPeek.h:30-33`); the itest phase-mask
   pins at `CMakeLists.txt:949, 1065, 1070, 1075, 1080, 1149, 1154, 1183, 1188, 1219, 1222`; and
   `scripts/p3a_untouched_regions.sh`'s eleven functions with `PINNED_BASELINE_REF=3e298c9a`.

---

## A. Goal and acceptance gate

`docs/Disaggregated/ROADMAP.md:20`, verbatim (all five cells):

> | **P4a** handle wave 2（Espryt）：FBO / 纹理 / sampler / program 身份与描述符 | 26–34 | `set_framebuffer_state`（解析后的 `ReadSurface`、内联格式、`ContentHash`、`{0,1}`）；sampler CSO（含 `borderColorForm`）；sampler view + `set_texture_params`；`set_sampler_views`/`bind_sampler_states`/`set_shader_images`；shader CSO（SPIR-V + 归档）；`set_draw/dispatch_program`；`set_global_constants`；`CompositeResolver`；纹理/renderbuffer 的 `resource_*`。emulation 在 split 下显式 Fatal 直到 P8 | 全套门；framebuffer/纹理/program 族场景；**新增"只作 attachment / image 单元 / CopyImage 端点的纹理其 `glTexParameter` 生效"场景（落地前必须红）**；两台设备 `KHR-GL46.direct_state_access.framebuffers*` 与整个 `packed_pixels` 块（~3300 例，句柄复用压力测试）。**再基线检查点 1b：超过 39 天** | P3a |

Binding context, all re-opened at `$BASE`: `ARCHITECTURE.md:39` (reserved handles `{0,0}` and `{0,1}`, and the
composite band), `:63` (the client-side content-addressed CSO capacities: render-state 64 / vertex-elements 1024 /
sampler 256 / sampler-view 4096 / shader follows `ProgramObject`), `:65` (P3a's own `[deviation] D-G1`, the precedent
for D-F2/D-F3 below), `:99` (**no stage dimension on `SetSamplerViews`/`BindSamplerStates`**), `:100` (**D10 — the
design statement the mandatory new scenario tests**), `:102` (`SetGlobalConstants` covers the default uniform block
only), `:104` (the explicit non-migrations), `:118` (flat POD, explicit padding, exact size, never a pointer),
`:130` (one discriminated `MGPResourceDesc` for buffers, every texture target and renderbuffers; **renderbuffer stays
an independent class**), `:133-136` (the four payload rows P4a fills), `:143` (`(ShaderCso, Version)` keyed, at most
once per program per frame), `:157` (the GL-call-time push rule **and its exception: texture subdata is not in it**),
`:196` (D-B3 — every `set_*`/`bind_*` before the verb; lazy specialisation at the verb), `:200` (the four merge rules,
including the set-hash suppressor), `:206` (sampler-view resolution moves client-side; **two backend post-processings
stay on the server**), `:210` (the create/respecify/destroy lifetime rule and the three payload-expressed ordering
constraints, two of them P4a's), `:212` (the composite resolver, in full), `:249-256` (§6 — the texture path, seven
statements), `:258-265` (§7 — shader state), `:269-286` (§8.1 — the reverse channel), `:290` (§8.2 — writeback then
epoch bump, a correctness rule), `:294-295` (**textures do not ack; the only synchronous ack is `glBufferStorage`**),
`:299-308` (§8.4 — the only new stall class and its four mitigations), `:318` (§9.1 — the Espryt do-not-touch list),
`:321` (**the one item moved out of it: the sub-rect upload decision and stride computation**), `:323` (the two
`MG_State`-type usages that must really change, both Magma's, plus Espryt's small sibling), `:363-365` (§9.5 — the
21-memo census), `:369` (§9.6 — `MOBILEGL_PIPE_LEGACY_MEMOS` stays a compile-time arm **through P3a/P4a**, +1 day per
phase), `:507-517` (§13.2 — the five-part gate and the two surviving byte equalities), `:562` (`CompositeResolver`'s
home), `:574` (`pDefaultFramebufferInfo` retires into `{0,1}` + `OnSurfaceChanged`), `:577-578` (test-wiring traps and
CI), `:590`/`:598`/`:599` (the three knobs). `ROADMAP.md:7`: 默认 ALL target 必须完整构建；禁止提交热路径插桩；**每个门
必须能因它存在的理由变红**；Windows 不是正确性门；设备对比走 reboot-clean + 同热窗口配对 A/B；每阶段出口跑一次五部分
门；每阶段性能判据是逐线程 CPU 时间. `ROADMAP.md:34`: **P4a is one of the five architecture boundaries that owe a full
`gl44to46` caselist run (~56,271 cases) on both devices, off the critical path, plus the named `packed_pixels` block.**
`ROADMAP.md:67`: **P4a over 39 working days means the narrow-handle premise was wrong and the next phase re-baselines**
(`:66`'s action; `:70` says to run the `inproc` falsification numbers first). `ROADMAP.md:98`: **拆分不得借机顺手修
`dev` 的问题.**

### A.1 The user's standing rules, applied

`MEASUREMENTS.md:324`, verbatim in the user's own words, and it carries into P4a unchanged (ID-6):

> (a) advance the roadmap as fast as possible without a large performance regression; performance is RECORDED
> against the pull baseline, never a blocking gate in P3a; correctness gates stay.

(b) Package agents run on **opus** (ID-4), so packages stay mechanical and well-specified; one adversarial review per
package, at most two rework rounds, declared minors do not block integration.

Concretely: **part 4 of the five-part gate (`ARCHITECTURE.md:514`) is a measure-and-publish obligation, not a
pass/fail gate**, and so is every P4a analogue of `ROADMAP.md:19`'s p99 clause. Parts 1, 2, 3 and 5 stay hard gates.
`MEASUREMENTS.md:503` hands P4a the decision *"让 P4a 自己决定要不要钉"* (whether to pin a DriverBench ceiling):
**P4a does not pin one either.** The reason is written into `MEASUREMENTS.md` at D.5: P3a's `+699 / +422` ns/draw was
paid by per-draw vertex-input emission, and P4a's per-verb sampler/image/framebuffer emission is the same shape of
risk; a ceiling set before the suppressors are measured would either be met trivially or stop the phase for a cost the
optimisation phase is already scheduled to remove. What P4a *does* add is a **budget statement** with a named
owner-of-record: every new per-verb emission goes through a version-first skip **before** it hashes anything
(D-D5), and D.4.4 publishes the per-emission cost so the optimisation phase starts from data.

### A.2 The gate, decoded

Unless a row says otherwise, commands run in a P4a WSL worktree with `export CCACHE_BASEDIR=/home/swung/w7`. ID-4's
errata are applied throughout: ctest names are extracted with
`grep -E "^[[:space:]]*Test[[:space:]]+#[0-9]+:" | sed -E "s/^ *Test +#[0-9]+: //" | LC_ALL=C sort` (the naive
`^\s+Test #` drops every id under 1000); **`retrace_gate.py --only` is a REGEX**, so a named sweep is spelled
`--only 'a|b|c'` and never a comma list (ID-15: P3a's named sweep silently matched 0 cases); the verify arming line is
`MGPipe: verify armed - <N> fields, 69 verbs, fatal=1` and retrace case logs carry
`MGPipe verify: <case> <backend> armed, zero divergences, zero unmigrated reads`.

| # | statement | the exact command that checks it |
|---|---|---|
| **G1** | The **pull build** (`MOBILEGL_PIPE_PUSH=OFF`, `MOBILEGL_PIPE_VERIFY=OFF`, Release/INFO) is symbol-identical to `$BASE`: 0 added, 0 removed, 0 renamed, **0 resized**. P4a's admitted-resize set is **EMPTY** — every P4a edit to `MG_State` / `MG_Pipe` / `MG_Backend` is inside `#if MOBILEGL_PIPE_PUSH` (D-P). | `python3 scripts/symbol_report.py --before ~/w7/p4a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0 --json` → `added==removed==renamed==resized==0`. CI's second reading is `gh workflow run test.yml --ref feat/disaggregated -f baseline_sha=087685d1`, whose Resized table must be **exactly** ID-3's four (`RenderState::{RenderState,SetCapability,IsCapabilityEnabled}`, `_GLOBAL__sub_I_DirectGLES.cpp`). |
| **G2** | `ctest -L integration-gpu` is **name-for-name identical between the pull and the push build**, on both backends, and green in both. | `for d in build-linux build-push; do ctest --test-dir $d -N \| <ID-4 extractor> > ~/w7/p4a-names-$d.txt; done; diff ~/w7/p4a-names-build-linux.txt ~/w7/p4a-names-build-push.txt` → empty; then `ctest --test-dir build-linux -L integration-gpu --no-tests=error -j 8` and the same on `build-push`; plus `MOBILEGL_PIPE_PUSH=0 ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8`. |
| **G3** | The 40-trace corpus replays **under push** at each case's own SSIM threshold on both backends (79 desktop cases: 40 × 2 minus `iterationrp × DirectGLES`). | `python3 ~/w7/retrace_gate.py --tree ~/w7/pipe --lib ~/w7/pipe/build-push/libMobileGL.so --out ~/w7/retrace-out/p4a-push -j 4` → exit 0. |
| **G3b** | The **named** traces pass under push on both backends. P4a's named set is P3a's five **plus `photon-v1.3b`** — the only fixture that has ever caught an image-binding-semantics regression, and it must be run on **llvmpipe desktop retrace, never on the Adreno** (memory `photon-broken-on-adreno`). | `python3 ~/w7/retrace_gate.py --tree ~/w7/pipe --lib .../build-push/libMobileGL.so --out ~/w7/retrace-out/p4a-named -j 4 --only 'create-indirect\|create-instancing\|rd12-odinlite\|improved-transparency-minecraft-26.3\|fabric-sodium\|photon-v1.3b'` — **a regex, not a comma list** (ID-15). `create-indirect` on device stays excluded and blocked on a `dev` fix; G3b is recorded as "desktop green, device partial", never as a device pass. |
| **G4** | The **verify** build shows zero divergence: no `Fatal{PipeVerifyDiffer`, no `Fatal{UnmigratedPipeInput`, no `Fatal{PipeResidualDiverged`, no `Fatal{ProtocolCorruption`, and **every case armed**. Verify additionally runs the **retain mode** comparison for texture dirty rects (`ARCHITECTURE.md:512`) and the **program-archive round trip** (D-H3). | `ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4`; `MOBILEGL_PIPE_VERIFY=1 python3 ~/w7/retrace_gate.py --tree ~/w7/pipe --lib ~/w7/pipe/build-verify/libMobileGL.so --out ~/w7/retrace-out/p4a-verify -j 4`; then `grep -l 'Fatal{' ~/w7/retrace-out/p4a-verify/*.log` empty and `grep -L 'MGPipe verify:' ~/w7/retrace-out/p4a-verify/*.log` empty. |
| **G5** | **The Espryt do-not-touch list is literal.** P3a's **eleven** functions stay byte-identical (the ten against the P3a baseline, `FlushPendingRangesFrom` against its pinned `3e298c9a` sha — ID-15), and P4a adds **six** of its own against `$BASE`: `StageBlocksIntoUnpackRing`, `UnpackRingAvailable`, `UnpackRingAllocate` (the unpack PBO ring), `RecomputeBackendColorSlots` (the attachment permutation), `DepthStencilSamplingReadImpl` (the D24S8 sampling emulation core) and `ShouldUseCaveatTextureFormat` (the format handler P4a's descriptors feed). **Seventeen functions across three files.** | `bash scripts/p4a_untouched_regions.sh $BASE HEAD` (package D, C.3) → rc 0. Non-zero rc names the first function that moved. `bash scripts/p4a_untouched_regions.sh --self-test` → rc 0, and it must report **four** negative controls (P3a's two plus `RecomputeBackendColorSlots` and `StageBlocksIntoUnpackRing`). |
| **G6** | **Descriptor-emission consistency**: for every framebuffer configuration, texture object, sampler object and program the client's emitted `MGPFramebufferState` / `MGPResourceDesc` / `MGPTextureParams` / `MGPSamplerDesc` / `MGPSamplerView` / `MGPProgramDesc` reproduce **exactly** the values Espryt's `SyncToBackend` family reads from the frontend today, field by field. | `ctest --test-dir build-push -R 'FramebufferEmit\.\|TextureEmit\.\|SamplerEmit\.\|ProgramEmit\.\|ImageEmit\.' --no-tests=error --output-on-failure`. |
| **G7** | **G6's negative controls**, one per new conversion, each a break the compiler cannot see. | `bash scripts/p4a_descriptor_negative_control.sh build-push` — patches `FramebufferEmit.h` to stop copying `MGPSurface::Layered`, and `SamplerEmit.h` to stop copying `SamplerParameters::borderColorForm`; each must turn its suite red **naming that field**; then reverts and rebuilds. Exit 0 = both tripped and named; 1 = a control did not answer; 2 = could not run. P2's `g7_negative_control.sh` and P3a's `p3a_vertex_input_negative_control.sh` both keep running unchanged. |
| **G8** | **`HandleRecycleScenario` covers every kind P4a mints, in all three arms** (`.Handles` green, `.Legacy` green, `.AbaControl` green-by-asserting-the-corruption). P4a adds `armExpectsCorruption=true` controls for **Texture**, **Framebuffer**, **Renderbuffer**, **SamplerCso**, **SamplerViewCso** and **ShaderCso** — the two existing texture/framebuffer cases (`:869`, `:920`) are *not* ABA controls today and say so at `:865-868`. | `ctest --test-dir build-verify -R 'HandleRecycle' --no-tests=error --output-on-failure`. The "red before" evidence is recorded on the contract tree at D.2. |
| **G8b** | **A leak test per new kind.** `PipeSlotKind` grows six members and each gets a churn case asserting `liveAfter == liveBefore`, `highWaterAfter == highWaterBefore` and `peakLive - liveBefore <= 1` over `kChurn = 48` rounds after two warm-ups — the `DestroyedVertexArraysReturnTheirVertexElementsSlots` shape (`HandleRecycleScenario.cpp:1015-1110`). **The composite ShaderCso gets its own case**, because its slot comes from the reserved band and is freed by two independent paths (D-H7). A peek that returns false must **SKIP**, never pass. | `ctest --test-dir build-verify -R 'HandleRecycle.*ReturnTheir' --no-tests=error --output-on-failure`. |
| **G9** | **The mandatory red-before scenario.** `TextureParamsWithoutASamplerViewScenario` proves a `glTexParameter` on a texture that is only an FBO attachment, only an image-unit binding or only a `glCopyImageSubData` endpoint reaches the driver. Its `AReadAttachmentOnlyTexturesDepthStencilModeReachesTheDriver` case is **red on the contract tree and on the pull build** (the pre-existing gap `scout-espryt-framebuffer.md` §2.6 documents) and green after `espryt` lands. | `ctest --test-dir build-push -R 'TextureParamsWithoutASamplerView' --no-tests=error --output-on-failure`. The red-before log is D.2's artefact and goes into `MEASUREMENTS.md`. |
| **G10** | `gen_pipe_dirty_surface.py --check` and `--self-test` stay green **with the widened mutator-prefix set** (`Use`, `Bind` added, D-K5) and with the four rows that widening surfaces. Both directions still fail: an unmapped mutator, and a row naming a mutator the scan no longer finds. | `python3 scripts/gen_pipe_dirty_surface.py --check` (rc 0), `--self-test` (rc 0, and it must report **more** negative controls than P3a's 21), `git diff --exit-code -- MobileGL/MG_Pipe/DirtySurface.def`. |
| **G11** | **The new counters exist, are published, and can move.** `texture-upload-emissions`, `texture-upload-box`, `texture-upload-rect`, `texture-upload-jobs` already exist as `CallClass` members and must now move **on the client side too**; P4a mints `cso-blob-bytes` (`ByteClass`, push-guarded — D-L) so `MEASUREMENTS.md:465`'s *"留给 P4a 给汇总行加类"* is discharged, plus `framebuffer-emissions`, `sampler-view-emissions`, `sampler-state-emissions` and `shader-image-emissions` (`CallClass`, push-guarded) so the four suppressors' hit rates are readable. | `ctest --test-dir build-push -R 'PipeStats' --no-tests=error --output-on-failure` (names pinned in `CounterNamesAreStable` and `SummaryLineCarriesEveryClassAndGate`); `ctest --test-dir build-push -R 'TextureUploadShape' --no-tests=error`; on device `grep 'MGPipe stats:' /sdcard/MG/latest.log \| tail -1` shows the new short names non-zero. |
| **G12** | The **subsystem A/B is real**: with P4a's bits cleared the tree runs the legacy `MGB_CTX`-reading arms and produces identical integration results; a ctest entry proves the switch changes behaviour rather than being dead; and the **dependency refusals** are exercised in both directions. | `MOBILEGL_PIPE_PUSH=0x1ff ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8` (**P3a's default = P4a's subsystems off**, the T2 arm) versus the default `0x1fff`; `ctest --test-dir build-push -R 'ObjectSubsystemControl' --no-tests=error`; and `MOBILEGL_PIPE_PUSH=0x9ff` (samplers set, texture resources clear) must log the refusal ERROR and run the legacy arm, asserted by `ObjectSubsystemControlScenario.ASamplerBitWithoutTheTextureBitIsRefusedAndNamed`. |
| **G13** | Purity holds: `pGLContext` still appears zero times under `MG_Backend/`, the include closure holds, no stdio instrumentation, generators regenerate clean, **and no `MG_State::GLState` type appears in any `MGPipeResourceOps` signature**. P4a adds **no member to any backend op table** (D-B1), so the ops block is unchanged. | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` empty; `python3 scripts/check_include_closure.py --mode both --compiler clang++-20 --self-test --require-all`; `python3 scripts/gen_pipe.py --check && python3 scripts/gen_pipe.py --self-test`; `python3 scripts/symbol_report.py --self-test`; the `pipe-gates` stdio grep; `grep -n 'MG_State' MobileGL/MG_Pipe/PipeApply.h` shows no hit inside the `MGPipeResourceOps` block. |
| **G13b** | **Every unmigrated emulation is named and greppable.** `MGPipeUnmigratedEmulation("<name>")` is called from every site D-M enumerates, its monolith body is a no-op, and the list of names is pinned. | `grep -rc 'MGPipeUnmigratedEmulation' MobileGL/MG_Backend/DirectGLES/` equals the count `PipeCatalogueTest.EveryUnmigratedEmulationIsNamedOnce` asserts; `ctest --test-dir build-push -R 'PipeCatalogue' --no-tests=error --output-on-failure`. |
| **G14** | Existing test names are never removed (additions only) and the full CI matrix is green. | `ctest --test-dir build-linux -N \| <ID-4 extractor> \| LC_ALL=C comm -23 ~/w7/p4a-before-ctest-names.txt -` empty; `gh workflow run test.yml --ref feat/disaggregated`. |
| **G15** | **The device CTS obligation, off the critical path**: (a) the two **named** blocks — `KHR-GL46.direct_state_access.framebuffers*` and the **whole** `packed_pixels` block (~3300 cases, run whole because `ROADMAP.md:20` calls it a **handle-reuse stress test**) — on **both** devices; (b) the full `gl44to46` caselist (~56,271 cases) on both devices at the `dev` merge, per-backend conformance within **0.5 pp** of the `$BASE` reading. Report shape: rows = GL version/extension, columns = status counts, rate = Pass/(Pass+Fail), **NS not in the denominator**. The `MOBILEGL_PIPE_POISON_OMIT` sweep that `FillPoints.def:65-70` still owes rides the same caselist run. | Scheduled at D.5, **after** the phase verdict, never before it. Devices: Adreno 830 `35d0befa`, Mali `3B159D009VZ00000` (`ROADMAP.md:50`). |
| **G16** | **RECORDED, NOT GATED (rule (a)).** Two devices, reboot-clean, same thermal window, paired **three-arm** A/B (pull / `0x1ff` / P4a's mask), per-thread CPU p50/p99 from `frameCpuTimesMs[]`; DriverBench `ns_per_op` T1/T2/T3. A regression is written down and does **not** stop the phase. | D.4. |
| **G17** | **The 39-day checkpoint (1b).** Elapsed calendar days per package are recorded; if the phase exceeds **39 days**, `ROADMAP.md:66`'s action applies to the *next* phase and `ROADMAP.md:70` says to run the `inproc` falsification numbers first. | The integrator writes the per-package day count into `MEASUREMENTS.md`'s Track H section, the way `MEASUREMENTS.md:417` records P3a's **1 day**. |

**Baseline captures the integrator takes before any package lands** (in `~/w7/pipe` at `$BASE`):

```sh
cd ~/w7/pipe
cp build-linux/libMobileGL.so ~/w7/p4a-before-libMobileGL.so
ctest --test-dir build-linux -N | grep -E "^[[:space:]]*Test[[:space:]]+#[0-9]+:" \
  | sed -E "s/^ *Test +#[0-9]+: //" | LC_ALL=C sort > ~/w7/p4a-before-ctest-names.txt
bash scripts/p3a_untouched_regions.sh $BASE $BASE > ~/w7/p4a-before-untouched-p3a.sha   # the eleven, still true
python3 scripts/gen_pipe_dirty_surface.py > ~/w7/p4a-before-dirty-surface.txt
awk '/^    namespace RenderStateImpl \{/,/^    \} \/\/ namespace RenderStateImpl/' \
  MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp | sha256sum > ~/w7/p4a-before-syncrenderstate.sha
```

**Three caveats that bind this gate and are not P4a's work to fix.**

- **`minecraft-1.21.1-neoforge-create-indirect-in-world` fails on both devices at `dev@81b17c0b`** (`MEASUREMENTS.md:96`,
  `ROADMAP.md:100`). Pre-existing `dev` bug. **It stays in the desktop 79-case SSIM corpus (G3/G3b) and stays out of the
  device A/B (D.4.2)**, exactly as P2 and P3a did. If it is still red on `dev` at the phase exit the integrator records
  "G3b partial: create-indirect blocked on `dev` open question 17" rather than claiming a pass.
- **`rd12` on Magma on the Xiaomi aborts on every arm, pull included** (ID-23, a scudo map error inside a `calloc` from
  `libMobileGL`). Pre-existing, excluded from the device table, task chip open. Its side effect matters:
  **a crash resets the GPU pwrlevel range to 2..5**, so every row taken after it on that device carries a DRIFT verdict
  on both arms and must be re-run or reported as DRIFT.
- **`IntegerBorderColorScenario` carries a known ASan stack-buffer-overflow** (ID-21 follow-up, a test bug). It sits
  squarely in P4a's sampler test set and P4a will run it a great deal. **P4a does not fix it** (`ROADMAP.md:98`); it
  records that the scenario is green under the normal lanes and names the open ASan finding beside every sampler result.
---

## B. Fixed design decisions

Nothing in this section is re-decided inside a package. Where a decision departs from a design document it is marked
**[deviation]** and D.5 names the doc line the integrator corrects.

### D-A — The calls P4a wires, and the ones it deliberately does not

`PipeCalls.def` is **closed**: `MGP_CALL_LIST_DOCUMENTED_COUNT 71` (`:75`) and `:26-29` — *"RECORD NUMBERING NEVER
CHURNS … The wire opcode is the 1-based position in this list, so reordering is a protocol break."* **P4a adds no row,
moves no row and retires no row.** It changes exactly one payload member name (D-C2) and one flag word (none — see
D-A2). Opcodes and line numbers below were regenerated from the file at `$BASE`.

#### D-A1 — The fifteen rows P4a wires

| opcode | row | `PipeCalls.def` | payload | class | flags | P4a family |
|---|---|---|---|---|---|---|
| 2 | `ResourceCreate` | `:81` | `MGPResourceDesc` 88 | `kScreen` | `kNone` | textures + renderbuffers |
| 3 | `ResourceRespecify` | `:82` | `MGPResourceDesc` 88 | `kScreen` | `kNeedsAck` | textures + renderbuffers |
| 4 | `ResourceDestroy` | `:83` | `MGPHandleOnly` 16 | `kScreen` | `kNone` | textures + renderbuffers |
| 23 | `CreateSamplerState` | `:104` | `MGPSamplerDesc` 32 | `kCtxCso` | `kNone` | samplers |
| 24 | `DeleteSamplerState` | `:105` | `MGPHandleOnly` 16 | `kCtxCso` | `kNone` | samplers |
| 25 | `CreateSamplerView` | `:106` | `MGPSamplerView` 36 | `kCtxCso` | `kNone` | samplers |
| 26 | `DeleteSamplerView` | `:107` | `MGPHandleOnly` 16 | `kCtxCso` | `kNone` | samplers |
| 27 | `CreateShaderState` | `:108` | `MGPProgramDesc` 192 | `kCtxCso` | `kHasBlob` | programs |
| 28 | `BindShaderState` | `:109` | `MGPHandleOnly` 16 | `kCtxCso` | `kNone` | programs |
| 29 | `DeleteShaderState` | `:110` | `MGPHandleOnly` 16 | `kCtxCso` | `kNone` | programs |
| 31 | `SetFramebufferState` | `:113` | `MGPFramebufferState` 304 | `kCtxState` | `kNone` | framebuffer |
| 35 | `SetSamplerViews` | `:117` | `MGPSamplerViews` 16 + `MGPBoundView[]` | `kCtxState` | `kVarTail` | samplers |
| 36 | `BindSamplerStates` | `:118` | `MGPSamplerStates` 16 + `MGPipeHandle[]` | `kCtxState` | `kVarTail` | samplers |
| 37 | `SetShaderImages` | `:119` | `MGPShaderImages` 16 + `MGPImageView[]` | `kCtxState` | `kVarTail` | samplers |
| 40 | `SetGlobalConstants` | `:122` | `MGPGlobalConstants` 40 | `kCtxState` | `kHasBlob` | programs |
| 44 | `SetDrawProgram` | `:126` | `MGPHandleOnly` 16 | `kCtxState` | `kNone` | programs |
| 45 | `SetDispatchProgram` | `:127` | `MGPHandleOnly` 16 | `kCtxState` | `kNone` | programs |
| 47 | `SetTextureParams` | `:133` | `MGPTextureParams` 32 | `kCtxObject` | `kNone` | textures |
| 48 | `ResourceSubData` | `:134` | `MGPSubData` 72 + `MGPSubRegion[]` | `kCtxObject` | `kHasBlob\|kVarTail` | textures |

(Nineteen rows; the "fifteen" of the heading counts the roadmap's eleven deliverables, several of which are more than
one row.)

#### D-A2 — [ruling] Textures do not widen `kNeedsAck`

`ARCHITECTURE.md:294`: texture allocation OOM is already deferred to sync time in monolith (`glTexImage*`/
`glTexStorage*` only `MarkStorageDirty`; even `glRenderbufferStorage*` allocates lazily inside `SyncToBackend`), so
splitting changes no observable behaviour and **this batch does not ack**. `:295`: the only entry allowed a synchronous
ack is `glBufferStorage`.

`ResourceRespecify` already carries `kNeedsAck` (P3a) and textures travel on the same row, so the answer is the
**per-record predicate**, not the flag word: `MGPipeResourceRespecifyNeedsAck(desc) == (desc.Immutable != 0)`
(`MGPipeTypes.h:680`). For a texture, `Immutable != 0` means `glTexStorage*`. **P4a must NOT widen the predicate, and
must NOT let a `glTexStorage*` descriptor set `Immutable` and thereby ask for an ack.** Decision: the client sets
`Immutable = 1` for an immutable-storage texture (it is a real descriptor fact the backend reads) **and** the predicate
is narrowed to name the buffer target explicitly:

```cpp
// MGPipeTypes.h, replacing the body at :680. The flag on the CALL means "records of this call
// MAY require an ack"; this predicate decides per record. glBufferStorage is a real synchronous
// allocation and is the only entry allowed one (ARCHITECTURE.md:295). glTexStorage* also sets
// Immutable - it is a descriptor fact the backend reads - but texture allocation is lazy in
// monolith and stays lazy in split (:294), so it must not ack.
inline Bool MGPipeResourceRespecifyNeedsAck(const MGPResourceDesc& desc) {
    return desc.Immutable != 0 && desc.Target == kMGPipeResourceTargetBuffer;
}
```

`PipeCatalogueTest.ResourceRespecifyAcksOnlyImmutableStorage` (`:552-585`) gains a third and fourth idiom:
`glTexStorage2D` and `glRenderbufferStorage` must both answer **false**. That test is the negative control for a future
widening.

#### D-A3 — [ruling] The `MGPResourceDesc::Target` enum is minted in the contract commit

`ResourceTracker.h:150-162` records that P3a *"minted no enum for the first list"* and that `Buffer` is 0 only because
it leads a comment. A second producer now exists, so the contract commit mints it in `MG_Pipe/MGPipeTypes.h` beside
the field, as `enum MGPipeResourceTarget : Uint8`, in the order `MGPipeTypes.h:152` already writes:

```
Buffer = 0, Tex1D, Tex2D, Tex3D, Tex1DArray, Tex2DArray, TexCube, TexCubeArray,
Tex2DMS, Tex2DMSArray, Renderbuffer, TexBuffer, Count
```

with (a) `static_assert(kMGPipeResourceTargetBuffer == static_cast<Uint8>(MGPipeResourceTarget::Buffer))` so P3a's
constant and the new enum cannot drift, and (b) a **`constexpr` mapping table with no `default:` arm** from
`MobileGL::TextureTarget` to it, plus the `MGPipeEveryTextureTargetIsMapped()` `static_assert` in the exact shape
`ResourceTracker.h:96-149` uses for `BufferTarget` — so adding a `TextureTarget` is a build break, not a silently
unmapped descriptor. `StorageKind` stays `== TextureStorageType` and is named, never open-coded.

#### D-A4 — [ruling] `MGPipeBindBit` moves to `MGPipeTypes.h` in the contract commit

`ResourceTracker.h:64-66` says it in as many words: *"The integrator moves them beside the field when a second producer
appears (P4a's texture family)."* The twelve enumerators move verbatim into `MGPipeTypes.h` next to
`MGPResourceDesc::BindMask`; `ResourceTracker.h` keeps a `using` alias so package B's existing code is unchanged.
The four bits nothing sets today get their producers in P4a:

| bit | set by |
|---|---|
| `kMGPipeBindSampler` (1<<5) | any texture the sampler-view resolution names in an emitted `MGPBoundView` |
| `kMGPipeBindShaderImage` (1<<6) | any texture named in an emitted `MGPImageView` — **and this is what feeds `ImageBindableHint`** |
| `kMGPipeBindRenderTarget` (1<<7) | any texture/renderbuffer attached to a colour attachment point |
| `kMGPipeBindDepthStencil` (1<<8) | any texture/renderbuffer attached to Depth, Stencil or DepthStencil |

The mask is **sticky, ORed, never cleared** and is emitted on **both** `ResourceCreate` and every `ResourceRespecify`,
exactly as P3a's buffer mask is (`ResourceTracker.h:355-374`). `ImageBindableHint` is
`(BindMask & kMGPipeBindShaderImage) != 0`, which is the client-side `everImageBound` `MGPipeTypes.h:165` asks for and
is the **prevention half** of §8.4's four mitigations (`ARCHITECTURE.md:302` item 1).

### D-B — Where each call lands, and why P4a adds no backend op table member

#### D-B1 — [ruling] Zero new backend op-table members. Zero new op tables.

This is the single most important structural decision in P4a and it falls out of what Espryt already does.

`ARCHITECTURE.md:157` is the whole push-timing rule and its exception: *"**只有今天就在 GL 调用时刻分发的资源 op 在 GL
调用时刻推送**——即 `BufferBackendOps` 的七个 hook。**纹理 subdata 不在此列**（§6）."* Nothing in P4a's families
dispatches to the backend at GL-call time today:

| family | what Espryt does today | consequence |
|---|---|---|
| texture storage | `glTexImage*`/`glTexStorage*` only `MarkStorageDirty`; Espryt allocates lazily in `SyncMipmapsToBackend` (`Managers.cpp:5672`) | record-only |
| texture sub-data | accumulated in `MipmapStorage`, uploaded at sync time (`ARCHITECTURE.md:249`) | record-only |
| texture params | `SyncTextureParamsToBackend` (`Managers.cpp:6782`) runs from `SyncTextureObjectToBackend` at draw sync, gated on a version | record-only |
| renderbuffer storage | `BackendRenderbufferObject::SyncToBackend` (`Managers.cpp:10621`) allocates lazily, memoised on a four-field cache | record-only |
| sampler objects | the twin is created lazily **only** from the program pass (`DirectGLES.cpp:4014-4020`); there is no eager `glGenSamplers` | record-only |
| sampler views / image units / unit bindings | resolved at `BindCurrentTextures` / `SyncImageTextureBindingsForDraw` | working state |
| framebuffer | `SyncCurrentFBO` → `SyncToBackend` at `PrepareForDraw` | working state |
| programs | `SyncCurrentProgram` → `BindCurrentProgramWithResources` at `PrepareForDraw` | working state + record |

**Decision: every P4a call is either an OBJECT RECORD the applier stores, or WORKING STATE the applier stores; neither
is dispatched to a backend function pointer.** Espryt reads the applier at the sync points it already has, keyed on a
server-owned `Serial` instead of a frontend version. This is exactly the shape P3a's five vertex-input entry points
took (`PipeApply.cpp:1157-1171`: *"these five dispatch to nobody"*), and it means:

- `MGPipeResourceOps` is **unchanged** — nine members, same signatures, same `PipeApply.h:58-61` purity claim, so G13
  is trivially true and the ops block does not grow an `MG_State` type by accident;
- **`MGPipeApplyResourceCreate` / `…Respecify` / `…SubData` / `…Destroy` branch on `record.Desc.Target`**: a buffer
  target dispatches into `MGPipeResourceOps` exactly as P3a wrote it; every other target stores and returns. The branch
  is one comparison against `kMGPipeResourceTargetBuffer` and it is where a mis-typed descriptor becomes visible;
- a `MOBILEGL_PIPE_PUSH` build with P4a's bits clear registers nothing new, so the A/B stays a pure configuration
  question and not a bring-up-order one (`Managers.cpp:2532-2537`'s rule).

The one thing this costs: **`glGenSamplers` still runs lazily on the driver**, at the first bind rather than at
`glGenSamplers`. That is the behaviour-preserving answer and `scout-espryt-sampler-program.md` §1.6's worry about
`MGLOG_E_ONCE("Failed to generate sampler object.")` (`Managers.cpp:10424`) moving is answered: it does not move.

#### D-B2 — Object records vs working state, per kind

`PipeApply.h:194-218` is the ruling P4a inherits and must restate for its own kinds: **object records describe
share-group objects and survive a make-current; working state is per-context and `MGPipeApplierReset` clears it.**

| kind / call | applier storage | lives across make-current? |
|---|---|---|
| Texture, Renderbuffer (`resource_*`) | `Vector<MGPipeResourceRecord> Resources` — **the same vector P3a's buffers use**, discriminated by `Desc.Target` | **yes** |
| `SamplerCso` (`create/delete_sampler_state`) | `Vector<MGPipeSamplerCsoRecord> SamplerCsos` | **yes** |
| `SamplerViewCso` (`create/delete_sampler_view`) | `Vector<MGPipeSamplerViewRecord> SamplerViews` | **yes** |
| `ShaderCso` (`create/bind/delete_shader_state`) | `Vector<MGPipeShaderCsoRecord> ShaderCsos` | **yes** |
| texture params (`set_texture_params`) | on the texture's `MGPipeResourceRecord` (a `MGPTextureParams Params` member + `ParamsSerial`) | **yes** — it is per-object state, not per-context |
| `set_framebuffer_state` | `MGPFramebufferState DrawFramebuffer, ReadFramebuffer` + `FramebufferSerial` | **no** |
| `set_sampler_views` / `bind_sampler_states` / `set_shader_images` | three fixed `Array`s of 192 entries + `Start`/`Count`/`Serial` each | **no** |
| `set_draw_program` / `set_dispatch_program` / `bind_shader_state` | `MGPipeHandle DrawProgram, DispatchProgram, BoundShaderCso` | **no** |
| `set_global_constants` | on the `MGPipeShaderCsoRecord` (`Version`, the block image, `GlobalConstantsSerial`) | **yes** — `(ShaderCso, Version)` keyed, `ARCHITECTURE.md:143` |

**Sharing `Resources` between buffers and textures is deliberate**, and it is what makes `kMGPipeMaxResourceSlots`
still one bound: the slot spaces of kinds `Buffer`, `Texture` and `Renderbuffer` are **independent** (the allocator is
per kind, `SlotAllocator.h`), so three different objects can hold slot 7. **The applier therefore indexes
`Resources` by `(kind, slot)`, not by slot alone** — one `Vector` per resource kind, three vectors, each bounded by
`kMGPipeMaxResourceSlots` and each grown only to its own dense high-water mark. This is the correction to the naive
reading of `PipeApply.h:105-113` that `scout-pipe-and-client.md`'s "What P4a has to decide" item 3 asks about: the
bound does not change, the number of tables does.

#### D-B3 — Serials, and what they replace

Every record carries a **server-owned, monotone `Uint64 Serial`**, an `MGGen`-class counter that never crosses the
line (`PipeApply.h:121-127`, `ARCHITECTURE.md:44-49`). It is bumped **before** the backend is told anything, for the
reason `PipeApply.cpp:578-582` records (the backend stamps its own synced serial from inside the hook, so a bump
afterwards leaves the twin one mutation behind). What each replaces in Espryt:

| Espryt memo today | file:line | P4a |
|---|---|---|
| `m_syncedFrontendAttachmentVersions[41]` + `m_syncedBackendIdGeneration` | `Managers.h:1604`, `:1609` | `MGPFramebufferState::ContentHash` + a server-private `attachmentRemintEpoch` (this is `ARCHITECTURE.md:365`'s `StampSyncedFBO` 四元组 re-key) |
| `g_fboSyncedSlotVersions/ObjectVersions/Objects/BackendIdGenerations` | `Managers.h:1690-1710` | one `FramebufferSerial` compare |
| `m_syncedTextureParamsVersion` + `m_forceTextureParamsResync` | `Managers.h:1503`, `:1507` | `ParamsSerial` + the record's `ForceResync` byte |
| `m_syncedSamplerVersion` (texture-owned sampler) | `Managers.h:1502` | the texture's `SamplerCso` handle + that CSO's `Serial` |
| `m_syncedSamplerVersion` (`BackendSamplerObject`) | `Managers.h:2222` | the CSO record's `Serial` |
| `m_syncedContentVersion`, `m_syncedShapeGeneration`, `m_syncedShapeParamsVersion` | `Managers.h:1468`, `:1481-1483` | the resource record's `Serial` |
| `m_cacheInternalFormat/Width/Height/Samples` (renderbuffer) | `Managers.h:2251-2254` | the resource record's `Desc` + `Serial` |
| `GetSyncedLinkVersion()` / `GetSyncedImageUnitVersion()` | `Managers.h:2004`, read `DirectGLES.cpp:3150-3151` | the `ShaderCso` record's `Serial` |

**`g_attachmentBackendIdGeneration` (`Managers.cpp:7763`) survives untouched and stays server-local.** It answers
"did *I* re-mint a driver texture id", which no client-side version can answer, and it is what
`RecreateBackendTexture` (`Managers.cpp:4797`) bumps and `SyncToBackend`'s re-arm (`Managers.cpp:7632-7636`) and
re-entrancy loop (`:7726-7736`) read. Dropping it would reintroduce exactly the class of bug commit `d7655247` fixed
on the buffer side. The same applies to `g_bufferBackendIdGeneration` (P3a's, unchanged) and to
`g_backendContextGeneration`.

### D-C — `set_framebuffer_state`

#### D-C1 — What the client resolves before emitting

The record goes out fully resolved. Nothing in it requires a lookup on the far side (`ARCHITECTURE.md:136`).

| field | source | note |
|---|---|---|
| `Fbo` | `kMGPipeDefaultFramebuffer` when the bound object is `FramebufferImpl::pDefaultFramebufferInfo->defaultFBO`, else the client-minted `Framebuffer` handle | retires the four identity comparisons `MGPipeHandles.h:69-71` names |
| `Color[i]`, `Depth`, `Stencil` | `fbo->GetAttachment(type)` → `IsTexture()`/`IsRenderbuffer()`, `GetTexture()`/`GetRenderbuffer()` → that object's handle; `GetTextureLevel()`, `GetTextureLayer()`, `IsLayered()`, `GetTextureUploadTarget()` (`FramebufferObject.cpp:53-87`) | an empty point is `{Res = kMGPipeNullHandle, Kind = None}` and every other field zero |
| `MGPSurface::InternalFormat` | `ITextureObject::GetFormat()` or `RenderbufferObject::GetInternalFormat()` | **inline**, so the four cross-object masks fall out at push time with no lookup (`MGPipeTypes.h:341-342`) |
| `ReadSurface` | `fbo->GetReadBuffer()` (`FramebufferObject.h:137`) → `fbo->GetAttachment(readBuffer)` → the same `MGPSurface` build | D-C2 |
| `DrawBuffers[8]` | `fbo->GetDrawBuffers()` (`FramebufferObject.h:135`), converted to the attachment **index** with `-1` for `FramebufferAttachmentType::None` | widths match exactly (`MAX_DRAW_BUFFERS == kMGMaxDrawBuffers == 8`) |
| `Width/Height/Layers/Samples/FixedSampleLocations` | the attachments' common `GetSize()`; for a no-attachment FBO, `GetDefaultWidth/Height/Layers/Samples/FixedSampleLocations` (`FramebufferObject.h:141-145`) | |
| `IsDefault` | `Fbo == kMGPipeDefaultFramebuffer` | redundant with `Fbo` on purpose: it is what a server reads without knowing the reserved value |
| `Complete` | **`FramebufferObject::CheckCompleteness()`** (`FramebufferObject.cpp:163-193`) | D-C3 |
| `Target` | D-C2 | the renamed padding byte |
| `ContentHash` | D-C4 | |

#### D-C2 — [ruling] `Pad0` becomes `Uint8 Target`, and the record is emitted per bound target

GL has two independent framebuffer bindings; `MGPFramebufferState` carries one `Fbo` and one `ReadSurface`. Espryt's
`SyncCurrentFBO` walks `{Draw, Read}` in that order (`DirectGLES.cpp:2237`) and has a "same FBO as draw" skip whose
one surviving job is `SyncReadBufferToBackend` (`:2286-2302`) — the `read-buffer shared FBO` defect's habitat.

**Decision:** `MGPFramebufferState::Pad0` (`MGPipeTypes.h:365`) is renamed `Uint8 Target` with values
`0 = Draw, 1 = Read, 2 = Both`, and the record is emitted **once per bound target that moved**, or **once with
`Target = Both`** when the two bindings name the same object. The struct's size is unchanged (`MGP_ASSERT_POD 304`
still holds); `gen_pipe.py:306`'s `PADDING_MEMBER_RE = ^Pad\d*$` means a member no longer called `Pad0` **must** gain
a `PipeFields.def` row, so `MGP_FIELDS_MGPFramebufferState` gains `F(Target)` in the same commit or `pipe-gates` goes
red. The applier keeps `DrawFramebuffer` and `ReadFramebuffer`; `Target = Both` writes both.

Consequences, and they are the point:

- `ReadSurface` is resolved from **the read framebuffer's own read buffer**, so the shared-FBO case cannot lose it.
  `SyncReadBufferToBackend`'s comment at `Managers.cpp:7424-7426` (*"When this is reached from `SyncCurrentFBO`'s 'same
  FBO as draw' skip path"*) describes a hazard that becomes unrepresentable.
- The draw-buffer array is applied **only for the draw target** — `Managers.cpp:7544-7551` records the Minecraft 26.x
  OIT bug where a READ-only sync landed `glDrawBuffers` on the wrong framebuffer and falsely stamped the memo. Under
  P4a the record says which target it is, so that mistake is a one-line `Target != Read` test rather than a call-site
  discipline.

#### D-C3 — [ruling] `Complete` is the backend-free answer, and `Color[8]` is bounded by a bring-up refusal

**`Complete` carries `FramebufferObject::CheckCompleteness()`, not `glCheckFramebufferStatus`'s answer.**
`CheckFramebufferStatus_State` (`GL_Framebuffer.cpp:2265-2296`) additionally consults
`ActiveBackendRejectsDistinctDepthStencil()` (`:2291-2294`) and `HasNonRenderableColorAttachment` (`:2288`), which read
the backend's probed format-capability cache. A client that emitted the GL-visible status would be reading the backend
from the client side — the exact coupling P4a exists to remove. `glCheckFramebufferStatus` keeps answering from the
frontend exactly as it does today; the payload comment at `MGPipeTypes.h:364` is amended to say which answer `Complete`
is, so a later phase cannot assume the stronger one.

**`Color[8]` vs `MaxColorAttachments`.** The wire array is 8; `MaxColorAttachments` is the driver's raw ES cap
(`BackendObject_DirectGLES.cpp:1518`) and is not clamped. Every campaign device reports 8 and ES 3.2's minimum is 8,
but a driver reporting more would silently truncate the record. Decision:

```cpp
// MGPipeTypes.h, beside MGPFramebufferState.
inline constexpr Uint32 kMGPipeMaxColorAttachments = 8;
static_assert(kMGPipeMaxColorAttachments == MobileGL::kMGMaxDrawBuffers,
              "MGPFramebufferState::Color[] and DrawBuffers[] are one array width");
```

and `ResolveFramebufferSubsystemArm()` (D-K3) **refuses bit 9 with one `MGLOG_E` naming the cap** when
`GetDynamicParameters().MaxColorAttachments > kMGPipeMaxColorAttachments`, falling back to the legacy arm. That is the
same shape as the bit-8-requires-bit-7 refusal (`Managers.cpp:2393-2410`) and it is the honest answer: widening the
payload is a wire change nobody has evidence for, and truncating silently is the bug class this phase is closing.
`FramebufferEmitTest.AnAttachmentPointAboveTheWireWidthIsRefusedNotTruncated` pins it.

#### D-C4 — `ContentHash`, and the one input it must not swallow

`ContentHash` is XXH64 over the whole record **with `ContentHash` itself zeroed**, i.e. over
`{Fbo, Color[8], Depth, Stencil, ReadSurface, DrawBuffers[8], Width, Height, Layers, Samples,
FixedSampleLocations, IsDefault, Complete, Target}` — every field the record carries, per
`VertexInputEmit.h:118-125`'s hard requirement, computed field-wise over a zero-initialised staging copy so no padding
byte enters the hash.

**It must cover `Fbo`.** A recycled framebuffer handle whose successor happens to carry an identical attachment set
would otherwise be suppressed against its predecessor; `Fbo` carries `Gen`, so it cannot be.

**The fragColor-broadcast trap** (`scout-espryt-framebuffer.md` working note 4). `g_fragColorBroadcastCount` is derived
from `drawFBO->GetDrawBuffers()` (`DirectGLES.cpp:3079-3102`), is read **from the frontend deliberately** so a program
relinks in the same draw (`:3076-3078`), and enters the program-staleness condition at `:3154`. Under P4a the server
derives it from `DrawBuffers[8]` of the record it holds — which is **in the hash**, so a suppressed
`set_framebuffer_state` provably means the draw-buffer array did not move, which provably means the broadcast count did
not move. The ordering `ARCHITECTURE.md:196` demands (all `set_*` before the verb, lazy specialisation at the verb) is
what makes the derivation legal at the verb rather than at the FBO sync. `FragmentOutputArrayIndexScenario` and the
Iris MRT traces are the behavioural gate; `FramebufferEmitTest.ADrawBufferChangeAloneStillMovesTheContentHash` is the
unit gate.

**Suppression.** `MGPipeSuppressorSlot` has no `SetFramebufferState` entry; **P4a appends one before `Count`**
(the enum is client-only and is not a wire opcode, so appending is safe). Hash 0 stays reserved for "never emitted"
and a computed 0 is remapped to 1 (`SetHashSuppressor.h:31-33`).

### D-D — Textures and renderbuffers: `resource_*`, sub-data accumulation, and upload-shape ownership

#### D-D1 — Create and respecify

`ARCHITECTURE.md:210`: create at construction, storage defined lazily by respecify, destroy at destruction.

- **`ResourceCreate`** is emitted from the object's constructor (`TextureObject.cpp:56`,
  `RenderbufferObject.cpp:28`), with `Target` from D-A3's table, `StorageKind = TextureStorageType`,
  `BindMask = 0`, every extent zero, `Immutable = 0`, `HasDefinedContent = 0`,
  `GlNameForDiag = GetExternalIndex()` (**diagnostics only** — `MGPipeTypes.h:167-170`), and `ViewOf`
  set for a `TextureObjectView` from `GetViewStorageOwner()`'s handle (one hop always reaches storage,
  `TextureObject.h:90-101`).
- **`ResourceRespecify`** is emitted from every storage-defining entry point: for textures, after
  `AllocateStorage(...)` in `TexImage{1,2,3}D_State`, `TexImage{2,3}DMultisample_State`,
  `CompressedTexImage{1,2,3}D_State`, `TexStorage{1,2,3}D`, `TexStorage{2,3}DMultisample`, `TextureView`,
  `TextureBuffer`/`TextureBufferRange`, and the generated-mip storage grow at `GL_Texture.cpp:528-539`; for
  renderbuffers, from `AllocateRenderbufferStorage_State` (`GL_Framebuffer.cpp:844-858`) — **which today publishes
  nothing at all** (D-D2).
- `InternalFormat` is *"already resolved to an uncompressed fallback by the client"* (`MGPipeTypes.h:158`);
  **compressed textures never reach the backend** (`ARCHITECTURE.md:256`), so a compressed upload emits the
  uncompressed fallback descriptor exactly as the frontend already keeps beside it
  (`MipmapStorage.h:84-92`: the compressed data is kept *beside*, not instead of, the uncompressed shadow).
- `BufferForTexBuffer` / `BufOffset` / `BufSize` for a `TextureObjectBuffer`, with `kMGPipeWholeBuffer = ~0ull` where
  the range is the whole buffer — and the frontend already resolves the size live
  (`TextureObjectBuffer.h:35-41`), which is the *"范围实时解析"* `ARCHITECTURE.md:130` asks for. **This is why
  P4a's texture-resource bit requires P3a's bit 7** (D-K3).

#### D-D2 — [ruling] The renderbuffer publication hole is closed by emission, not by a new version

`RenderbufferObject::{SetInternalFormat, AllocateStorage, SetSamples}` (`RenderbufferObject.cpp:96-109`) bump no
version and raise no notice, and bit 11's shutter (`anyAttachmentGeneration ⊕ drawFboBindVersion`) does not move when
an **already-attached** renderbuffer is re-storaged. So `glBindRenderbuffer; glRenderbufferStorage(newSize)` on an
attached renderbuffer is invisible to the framebuffer bit today.

**Decision: option (b) of `scout-frontend-state.md` §5.2 — emit `resource_respecify` straight from the storage entry
point**, the way `BufferObject::NotifyRespecify` does. No new version counter is added to `RenderbufferObject`
(a new member would resize the pull build's object and is a G1 break waiting to happen), and no aggregate generation is
added. The emission is unconditional in a push build and gated by the subsystem bit at the dispatch site.
`RenderbufferBlendFormatScenario` plus a new
`FramebufferEmitTest.ARestoragedAttachedRenderbufferPublishesItsNewExtent` are the gates.

#### D-D3 — Sub-data: accumulate client-side, emit one record at the validate point

`ARCHITECTURE.md:249-251`, and it is the explicit exception to `:157`'s GL-call-time rule.

**What accumulates.** Nothing new: `MipmapStorage` already keeps, per level, a union box (`m_dirtyRegions`,
`MarkDirtyRegion` at `:58`) **and** a disjoint rect list behind it (`m_dirtyRects`, `kMaxDirtyRects = 96` at `:75`,
`GetDirtyRects` at `:82`), and every write funnels through `MarkDirty`/`MarkDirtyRegion` into **both**
representations (`:62-81`). `GetDirtyRects` returning 0 means "upload the union box instead" and covers every reason
at once (`:76-81`). P4a reads that model; it does not change it.

**What is emitted.** At the validate point, for every dirty `(storageOwner, uploadTarget, level)` on the drain list
(D-D4), **one** `ResourceSubData`:

| field | value |
|---|---|
| `Res` | the **storage owner's** `Texture` handle |
| `Target` | the owner's upload target (`MGPipeResourceTarget` from D-A3, plus the cube-face upload target in `MGPSurface`-style terms) |
| `Level` | the owner's level index |
| `SourceIsVerbatimLevelShadow` | **1**, always, on the client side: the conversion fallbacks (`PrepareFallbackUpload`, `PreparePackedNormUpload`, `PrepareImageWidenedUpload`) are Espryt's and run on the server, so the bytes the client declares *are* the level shadow. The server keeps its own conversion branch and clears the flag internally when it converts — which is exactly the pointer comparison `uploadData == mipData` (`Managers.cpp:6241-6246`) turned into a carried fact (`ARCHITECTURE.md:252`) |
| `UnionBox` | `GetStorageDirtyRegion(level)` (`TextureObject.h:264-267`; the base fallback returns the whole level) |
| `RegionCount` + `MGPSubRegion[]` | `GetStorageDirtyRects(level, out, kMaxDirtyRects)`; **0 is legal and means "the union box is the whole story"** |
| `Blob` | `Blob.Seg = kMGHostSpanSegNone`, `Blob.Offset = (address of the level shadow base)`, `Blob.Size = 0` — **"this record does not declare its blob"**, which is what a monolith emission is (`MGPipeTypes.h:619-631`, ID-8b's "one Blob rule") |

**Strides are carried, never inferred** (`ARCHITECTURE.md:252`). Every `MGPSubRegion` sets `SrcOffset` to the byte
offset of its first texel **inside the level shadow**, `SrcRowStride` to the **level's** row pitch in bytes and
`SrcSliceStride` to the level's slice pitch — not 0, because a sub-rect's rows are not contiguous in the shadow. A
whole-level region leaves both 0 (tightly packed). This is the shape `UnpackStagingBlock`
(`Managers.cpp:4934-4942`) already consumes, so Espryt's staging planner changes its *source of strides*, not its
algorithm — which is the one item `ARCHITECTURE.md:321` moved out of the do-not-touch list, and nothing else moves.

The frontend has **already** resolved unpack (`PixelStoreProcessor::ProcessTexturePixelsDataUnpack`, eight call sites
in `GL_Texture.cpp`), which is why `set_pixel_unpack_state` does not exist (`ARCHITECTURE.md:104`) and why the level
shadow is tightly packed. `ScopedDefaultUnpackState`'s process-wide shadow (`Managers.cpp:4851-4918`) is **untouched
and stays in the do-not-touch list**; D-O records that its `s_synced` has no invalidation path as an inherited
`dev`-side hazard that P4a does not fix and does not make worse (the ring path still issues no `glPixelStorei`, and the
one outside writer at `Managers.cpp:6400` is still guarded by `!ringStaged`).

#### D-D4 — [ruling] The drain list, and where the emission cursor stops being P4a's

`ROADMAP.md:23` puts *"dirty 归属反转（按存储属主键控的发射游标）"* in the **P3b/P4b** cell, and `ARCHITECTURE.md:253-254`
describes the full cursor with view/owner index remapping. P4a needs *something* at the validate point, because walking
every live texture per verb is the cost `scout-frontend-state.md` risk 9 forbids.

**Decision, and it splits the item cleanly:**

- **P4a lands the drain list**: a flat, per-context `Vector<{MGPipeHandle owner, Uint16 uploadTarget, Uint16 level}>`
  on the client, appended **once** on the first dirty mark of a level (a `Bool m_onDrainList` beside the level's
  existing `m_isDirty`, inside `#if MOBILEGL_PIPE_PUSH`) and cleared at emission. It is keyed on the **storage owner**
  from day one: `TextureObjectView::GetViewStorageOwner()` (`TextureObjectView.h:49`) resolves it in one hop and the
  base returns a static null (`TextureObject.cpp:338-343`), so an upload through a view and an upload through the owner
  land on the same key. This is correct and complete for P4a **because view and owner already share one dirty state**
  (`ARCHITECTURE.md:254`: *"`TextureObjectView` 把 dirty 查询/清除全部转发给属主"*).
- **P3b/P4b lands the cursor**: the index remapping between a view's level/layer space and the owner's, the aliasing
  scenario `ROADMAP.md:23` names (*"view/owner 发射游标别名场景"*), and `TextureUploadShapeScenario`'s gold-standard
  shape comparison. P4a **builds** `TextureUploadShapeScenario` (it is meaningless without P4a's records) and runs it
  as a **recorded** comparison; P3b/P4b turns it into a gate with the Mali frame-time delta published. That resolves
  `scout-docs-spec.md` trap 2 in both directions.

#### D-D5 — [ruling] Who clears the dirty flag, and why a bail cannot lose texels

`ARCHITECTURE.md:253` inverts the ownership: *"client 保留 rect 模型、维护一份发射游标、发射后清自己的标志，server 从不
碰 client 的标志"*, and the safety argument is that `MG_Impl` contains no `IsStorageDirty`/`GetStorageDirtyRects`/
`GetStorageDirtyRegion` call site (the frontend never reads its own dirty state). Verified by grep at `$BASE`.

Today Espryt clears the flag from inside its upload loop, and Espryt's loop has bail arms — an incomplete texture
returns early (`Managers.cpp:5734-5738`), a multisample target refreshes and skips — that today leave the flag set.
Naively moving the clear to the client loses those texels.

**Decision, three steps, and the applier is the safety net:**

1. the client clears its own `m_isDirty`/`m_dirtyRects`/`m_dirtyRegions` for the level **at emission**, and only for
   levels whose record the applier **accepted** (the emit helper returns "applied", the P3a
   `MGPipeEmitResourceDestroyAndFree` shape at `PipeFill.cpp:777-799`);
2. the applier **accumulates** the emitted shape into a per-record pending-upload set (union box + region list per
   `(uploadTarget, level)`), which is *server-side state* and survives any number of bails;
3. Espryt consumes and clears the applier's pending set only where it actually uploads. A bail leaves it intact and the
   next sync retries.

`SampledSetStalenessScenario`, `ImageSizeAfterRespecScenario`, `ClearTexImageUndefinedLevelZeroScenario` and the GUI /
atlas traces (`rei`, `xaero-*`, `journeymap`, `modernui`) are the behavioural gates; the shape-level gate is verify's
**retain mode** (`ARCHITECTURE.md:512`): a consume-and-clear set cannot be recomputed after emission, so the tracker
retains the pre-clear set and the comparator compares the emitted
`(UnionBox, RegionCount, Regions[])` against it, field by field. **That retain-mode comparison is P4a's work and is
part of G4.**

#### D-D6 — Who picks the upload shape: the server, and Espryt's choice is unchanged

`ARCHITECTURE.md:251`: *"**同时携带 union box 与 region 列表，由 server 选上传形状**：决策留在付 GPU 代价的那一侧"*,
with the measured 635 KB/frame (box) vs 40 KB/frame (rect), 16×, and Mali's per-**job** pricing (~100 sprite rects
against one union box = **+6 ms/frame**).

Espryt's existing decision moves verbatim from "read the frontend" to "read the record": `subRectEligible`
(`Managers.cpp:6241-6246`) becomes a test of `SourceIsVerbatimLevelShadow` and the record's own box against the level
extent; **`dirtyRectCount` is still forced to 0 whenever the unpack ring is available** (`:6280-6282`, "One box, one
job"); the three staging shapes (`:6310-6322` N rects, `:6323-6331` one box, `:6332-6353` whole level) are unchanged;
`StageBlocksIntoUnpackRing` is **byte-identical** (G5). The four `PipeStats` counters
(`TextureUploadEmissions`, `…Box`, `…Rect`, `…Jobs`, `Managers.cpp:6360-6395`) keep counting on the server, and P4a
adds their client-side twins (D-L) so an emission-side regression is visible without a GPU.
### D-E — `set_texture_params`, and the payload gap the contract commit closes

#### D-E1 — [ruling] `MGPTextureParams` grows 32 → 40 bytes and gains `BuiltinSampler` and `SamplerResync`

`MGPTextureParams` (32 B, `MGPipeTypes.h:307-318`) carries `BaseLevel/MaxLevel/Swizzle[4]/DepthStencilMode/
ForceResync/MinLod/MaxLod/LodBias` and **no filter, no wrap, no compare mode and no border colour**. Espryt pushes all
of those onto the texture object through `SyncBuiltinSamplerToBackend` (`Managers.cpp:6671-6781`, `glTexParameter*`
after `Bind(target)`) plus the border-colour block inside `SyncTextureParamsToBackend` (`Managers.cpp:6911-6954`,
which compares all four of `m_cacheBorderColor{,I,UI,Form}`). That is the real gap
`scout-espryt-sampler-program.md` §1.8 and trap 2 name, and it must be closed in the contract commit.

The state itself already exists in the right shape: **every `ITextureObject` owns a `SamplerObject(0)`**
(`TextureObject.h:208`, constructed at `TextureObject.cpp:58`), and `glTexParameteri(GL_TEXTURE_MIN_FILTER)` runs
`SamplerObject::BumpVersion` through *that* object (`TextureObject.cpp:125-179`). So the texture's sampling state is
already a `SamplerParameters` value; what the payload lacks is a way to name the CSO that carries it. Widening
`MGPTextureParams` with a filter/wrap/border block would duplicate `SamplerParameters` on the wire and give two
authorities for one value.

**Decision:**

```cpp
// Per texture OBJECT, independent of any view. BuiltinSampler is the CSO carrying the
// SamplerParameters of the SamplerObject every ITextureObject owns (TextureObject.h:208) -
// GL 4.6 table 23.18 makes filter/wrap/compare/border sampler state, and Espryt pushes it
// with glTexParameter* onto the texture rather than with glBindSampler onto the unit, which
// is behaviour P4a preserves exactly.
struct MGPTextureParams {
    MGPipeHandle Res;             //  0
    MGPipeHandle BuiltinSampler;  //  8   NEW - kind SamplerCso, kMGPipeNullHandle is illegal
    Uint16 BaseLevel, MaxLevel;   // 16
    Uint8 Swizzle[4];             // 20
    Uint8 DepthStencilMode;       // 24
    Uint8 ForceResync;            // 25   mirrors m_forceTextureParamsResync (Managers.h:1507)
    Uint8 SamplerResync;          // 26   NEW - mirrors m_forceSamplerResync (Managers.h:1516)
    Uint8 Pad0;                   // 27
    Float MinLod, MaxLod, LodBias;// 28
};
MGP_ASSERT_POD(MGPTextureParams, 40);
```

Mechanics, all in the contract commit and all mechanically gated: the size assert moves 32 → **40**;
`MGP_FIELDS_MGPTextureParams` (`PipeFields.def:81-83`) gains `F(BuiltinSampler)` and `F(SamplerResync)` — and
**`Pad0` stays unlisted**, because `gen_pipe.py:306`'s `PADDING_MEMBER_RE = ^Pad\d*$` excludes it; `gen_pipe.py` is
re-run and `git diff --exit-code -- MobileGL/MG_Pipe/generated` is mandatory in the same commit;
`PipeCatalogueTest` pins the new size. This is P3a's `MGPVertexBuffers` 16 → 24 precedent, one for one.

**Why this is the right split rather than a wider payload.** The two pushes stay two pushes:
`SyncTextureParamsToBackend` reads `MGPTextureParams`, `SyncBuiltinSamplerToBackend` reads the CSO record's
`SamplerParameters`, and the asymmetry that must survive survives — the texture path resolves its min filter with
`IsAngleLlvmpipeRenderer()` while the sampler-object path uses `ShouldAvoidSamplerMipmapMinFilterOnAngleLlvmpipe()`
(`Managers.cpp:6732-6744` vs `:10482-10490`), which is a **server-side choice made from the same value**, so nothing
about it moves. The border colour keeps being pushed on the *texture params* version rather than the sampler version
(`Managers.cpp:6911-6954`), which is the behaviour Espryt has and P4a does not change; the record makes that explicit
instead of implicit.

`BuiltinSampler` may never be `kMGPipeNullHandle`: every texture has a sampler object, so a null handle is
`Fatal{ProtocolCorruption}`. `TextureEmitTest.EveryTexturesParamsNameItsBuiltinSamplerCso` and
`…TwoTexturesWithIdenticalSamplingShareOneBuiltinCso` pin it.

**[deviation]** against `ARCHITECTURE.md:134`, whose row says the texture-object half carries
*"base/max level、swizzle、depth-stencil mode、LOD 钳、`ForceResync`"*. It still carries exactly those; what is added is
the handle of the CSO that carries the rest, plus the second resync bit. D.5 has the integrator amend `:134`.


#### D-E2 — `ForceResync` and `SamplerResync`

`MGPTextureParams::ForceResync` (`MGPipeTypes.h:314`) is already the wire spelling of `m_forceTextureParamsResync`
(`Managers.h:1507`): *"the widened-channel carrier needs a swizzle override that the frontend params version does not
move for"*. `m_forceSamplerResync` (`Managers.h:1516`) has **no** wire spelling and the failure mode it guards is not
mis-filtering but an *incomplete texture that samples (0,0,0,1)* after a driver re-mint. D-E1's `SamplerResync` byte
closes it.

Both are **set by the server, not the client**: `RequireImageBindableStorage` (`Managers.cpp:4787`) and
`RecreateBackendTexture` (`:4829-4839`) set them on the twin. The wire bytes exist so the **client** can force a
resync it knows about — today the only such case is the widened-channel swizzle override, and the client sets
`ForceResync = 1` on the `set_texture_params` it emits after an `ImageBindableHint` transition (D-A4). The applier
stores them on the record; the twin ORs them into its own flags and clears its own copy after the sync. **The client
never clears a server flag** and the server never clears the client's — the D-D5 inversion, applied to two bits.

#### D-E3 — Which textures get their params pushed, and the gap P4a closes on purpose

`scout-espryt-framebuffer.md` §2.6 is the authoritative table of how a texture reaches
`SyncTextureParamsToBackend`/`SyncBuiltinSamplerToBackend` today, and it has one hole:

> a texture that is an attachment of the **READ** framebuffer only reaches `SyncCurrentFBO` → `SyncToBackend` →
> `SyncAttachmentObject`, which calls **`SyncMipmapsToBackend` and nothing else** (`Managers.cpp:7161`); the
> `SyncNeccessaryTextures` attachment list walks the **DRAW** slot only (`DirectGLES.cpp:1944`).

`ARCHITECTURE.md:100` (D10) is precisely about this class, and `ROADMAP.md:20` makes the scenario mandatory and
red-before. **Decision: P4a closes the gap deliberately.** `set_texture_params` is addressed by resource and is
independent of any binding, so the record exists for every texture the moment its parameters move; Espryt's
`SyncAttachmentObject` reads the record and applies params for **any** attachment, draw or read. This is a behaviour
change, it is **the deliverable**, and it is not a drive-by `dev` fix (`ROADMAP.md:98`) — it is what the roadmap's
mandatory scenario asks for.

`TextureParamsWithoutASamplerViewScenario` (package D, G9), four cases:

| case | today | after |
|---|---|---|
| `AnAttachmentOnlyTexturesSwizzleReachesTheDriver` (DRAW attachment) | green | green |
| **`AReadAttachmentOnlyTexturesDepthStencilModeReachesTheDriver`** | **RED** | green |
| `AnImageUnitOnlyTexturesSwizzleSurvivesARequireImageBindableStorageRemint` | green (`RequireImageBindableStorage` sets `m_forceTextureParamsResync`, `Managers.cpp:4787`) | green |
| `ACopyImageEndpointOnlyTexturesParamsReachTheDriver` | green (`MakeGLESCopyImageEndpoint` → `SyncTextureObjectToBackend`, `DirectGLES.cpp:7588`) | green |

Three of the four are green today and stay green — they are the regression net around the change. The second is the
`落地前必须红` artefact and its D.2 log is what `ROADMAP.md:20` asks for.

### D-F — Sampler CSOs and sampler views: identity rules

#### D-F1 — [ruling] Sampler CSOs are content-addressed, at capacity 256, hashed field-wise over a canonical copy

`ARCHITECTURE.md:63` puts sampler CSOs in the client-side content-addressed scheme at capacity 256, and unlike P3a's
vertex-elements CSO **that is right here**: a `SamplerObject` is a pure 100-byte value with no driver-side per-object
binding state, two identical samplers can share one CSO with no extra work on either side, and Espryt's
`BackendSamplerObject` is a driver name plus a parameter shadow — nothing a second frontend object would have to
re-establish. `MG_Impl/Pipe/CsoCache.h` is the template: version-first skip (`:14-25`), hash **plus `memcmp` confirm**
(`:84-98` — *"a bare 64-bit equality would alias two states"*), `Mint` (`:138-172`), LRU `Evict` (`:174-183`) that
emits `delete_*` and frees the slot, and the `kMGPipeBehaviourNoCsoContentAddressing` negative control (`:30-37`).

**The padding trap, and it is the reason this is a ruling and not an instruction.** `SamplerParameters` is
`sizeof == 100` with **3 bytes of trailing padding** (96 bytes of members + the 1-byte `borderColorForm`), verified at
`MGPipeValueTypes.h:462-486` / `:615`. A hash or a `memcmp` over those 100 bytes reads uninitialised padding. In
practice the member is value-initialised inside `SamplerObject` and never rewritten, so it is stable — *in practice* is
not a contract, and the whole point of the CSO cache is that a false miss mints a fresh CSO per call. Decision, three
parts:

1. **The contract commit adds `MGP_FIELDS_SamplerParameters` to `PipeFields.def` and a `P(SamplerParameters)` entry to
   `MGP_VERIFY_PAYLOAD_LIST`** (the pattern `MGP_FIELDS_MGPVertexAttribWire` `:306-308` set), so `MOBILEGL_PIPE_VERIFY`
   compares it **field by field** and the three padding bytes can never false-differ. Without this the blob is compared
   as bytes and G4 is a coin flip.
2. **The cache hashes and confirms over a zero-initialised canonical copy**: one stack `SamplerParameters canon{};`
   plus a field-wise assignment (not a `memcpy`), hashed with XXH64 over the copy. One copy per *mint attempt*, never
   per draw, because the version-first skip runs first.
3. `SamplerEmitTest.TwoIdenticalSamplersShareOneCso`, `…ABorderColorFormChangeAloneMintsANewCso` and
   `…PaddingCannotChangeTheHash` (constructed by writing garbage into the padding through a byte pointer) pin all
   three.

Capacity is `kMGPipeSamplerCsoCacheCapacity = 256` (`ARCHITECTURE.md:63`). LRU eviction emits `delete_sampler_state`,
bumps `gen` and frees the slot — the `CsoCache.h:174-183` shape.

#### D-F2 — [deviation] Sampler views are identity-addressed per texture object in P4a

`ARCHITECTURE.md:63` gives sampler views a content-addressed cache at capacity 4096. **That is right for Magma's
`VkImageView` and wrong for Espryt in P4a**, for the same reason P3a's D-G1 gave for vertex elements: MobileGL has no
frontend sampler-view object at all, so *every* sampled texture needs one minted, and a content-addressed mint per
texture per verb is the per-draw cost `MEASUREMENTS.md:501` and ID-19 forbid.

**Decision: one `SamplerViewCso` handle per `ITextureObject`, minted off its lifetime id, `create_sampler_view`
re-issued on the same handle whenever the restrictions move** — legal because `MGPipeHandle::Gen` increments only on
slot reuse and never on a respecify (`MGPipeHandles.h:44-48`), and it is exactly P3a's `CreateVertexElements`
re-issue shape (`VertexInputEmit.h:145-193`). The restrictions come from the texture itself: for a
`TextureObjectView`, `m_viewMinLevel/NumLevels/MinLayer/NumLayers` and the aliasing `m_internalFormat`
(`TextureObject.h:228-231`); for an ordinary texture, the full range and its own format.

The mint is gated by a **version-first skip before any hash**: a per-handle latch of
`(GetTextureParamsVersion(), GetShapeVersion())`; unchanged means no re-issue, no hash, no copy. That is the
`CsoCache.h:14-25` discipline applied to an identity-addressed slot, and it is what keeps the per-verb cost at one
`Uint64` compare per touched unit.

The content-addressed 4096-entry cache stays **P7's**, when Magma's image-view factory takes the CSO over and content
addressing is what its `VkImageView` create-info actually wants. The integrator annotates `ARCHITECTURE.md:63`
accordingly, beside P3a's `:65` deviation, in the same shape.

#### D-F3 — Sampler-view resolution moves to the client; two post-processings stay on the server

`ARCHITECTURE.md:206`, verbatim: GL is one binding per unit per target; which one the shader sees depends on the
sampler uniform's type, mipmap completeness (`IsMipmapCompleteForFilter`, `SamplesAsIncompleteTexture`) and
`IsUndefinedDefaultTexture`; gallium's one-view-per-slot **is** the resolved form; **the resolution moves to the
client with its own memo (~40 lines)**; and *"两处后端特定后处理留在 server、作用于已解析集合：Espryt 的 raw-depth-fetch
sampler 替换、Magma 的 feedback-loop 检测"*.

**Decision:** P4a moves the resolution and **leaves both post-processings on the server**, acting on the resolved set.
Specifically:

- The client's resolver reads, per program-resolved unit (`GetUniformSamplerOrImageUnitIndex`), the sampler uniform's
  type → target mapping, `TextureUnit::m_slots[target]`, the unit's or the texture's `SamplerObject`, and the two
  completeness predicates (`MG_State::GLState::SamplesAsIncompleteTexture`, `TextureObject.cpp:489-512`;
  `IsMipmapCompleteForFilterCached`, `:298-307`). It emits **only the program-resolved set**
  (`ARCHITECTURE.md:200`, third merge rule).
- Espryt's **raw-depth-fetch sampler substitution** (`DirectGLES.cpp:4001-4010`, `GetRawDepthFetchSampler` `:207-219`,
  `NeedsRawDepthFetchSampler` `:221-235`) stays on the server and now reads the resolved set: the view's
  `InternalFormat` answers `IsDepthFormatInternalFormat`, and the sampler CSO record's `SamplerParameters` answers
  compareMode/minFilter/mipmapMode/magFilter. **[correction] Nativising `g_rawDepthFetchSamplerState` is NOT P4a's** —
  `ROADMAP.md:23` assigns *"raw-depth-fetch sampler 原生化"* to P3b/P4b, and `ARCHITECTURE.md:323` groups it with
  Magma's placeholder textures, which are P7's. It keeps constructing a frontend `SamplerObject(0)` inside
  `MG_Backend` (`DirectGLES.cpp:209-214`), and G13 still passes because the purity gates grep for `pGLContext` and for
  `MG_State` inside `PipeApply.h`, neither of which this is. **P4a records it as the largest surviving
  `MG_State`-type usage in `MG_Backend/DirectGLES`, with its owner (P3b/P4b) named**, so the include-graph A gate at
  P13 has a known list rather than a surprise.

#### D-F4 — `borderColorForm` crosses, and all four values are compared

`ARCHITECTURE.md:133` and `MGPipeValueTypes.h:449-460`: all three border-colour representations are always numerically
populated, so the value alone cannot say which driver entry point to use. `MGPSamplerDesc::Parameters` carries
`SamplerParameters` **byte for byte including `borderColorForm`** (`MGPipeTypes.h:279-282`). Espryt's redundancy filter
compares **all four** (float, int, uint, form) at `Managers.cpp:10518-10521` and its texture-side twin compares four
cached members `m_cacheBorderColor{,I,UI,Form}` (`Managers.h:1488-1493`); both keep doing so, reading the CSO record
instead of the frontend object. `IntegerBorderColorScenario` is the behavioural gate (and carries ID-21's open ASan
finding, which P4a records and does not fix).

### D-G — The three `kVarTail` unit sets

#### D-G1 — [ruling] P4a emits them **and** wires their suppressor slots

`SetHashSuppressor.h:45-50` labels `SetSamplerViews`/`BindSamplerStates` as **P3b** and `SetShaderImages` as **P4b**,
while `ROADMAP.md:20` gives all three *calls* to P4a. `scout-docs-spec.md` trap 1 asks which.

**Decision: P4a emits all three with `ContentHash` populated AND wires all three suppressor slots.** Two reasons, both
load-bearing:

- `MGPipeTypes.h:369-373` makes the suppressor **mandatory** for every `kVarTail set_*`: *"The same pattern is
  mandatory for every kVarTail set_* below, or 26.2's redundant `glBindSampler` traffic reappears as a
  variable-length record per batch."* `GetTextureBindGeneration()` bumps on a redundant rebind (MC 26.2 rebinds the
  same sampler at every texture-unit switch), so an unsuppressed set is a several-hundred-byte variable-length record
  per batch. Emitting without the suppressor would be shipping the regression the design names.
- ID-19's budget: P3a already costs +699 / +422 ns/draw and rd12 moved +30% / +27% on device. A 192-entry walk per
  verb without a latch is not affordable.

What `ROADMAP.md:23` assigns to P3b/P4b is the **~175-line backend debounce removal**
(`UnitBindingsSnapshot`/`CaptureUnitBindings`/`UnitBindingsUnchanged` at `DirectGLES.cpp:1594-1727`,
`g_unitTextureSyncList`/`g_fboTextureSyncList` at `:1814-1830`), for which the suppressor is only the *carrier*
(`ARCHITECTURE.md:200`: *"最后一条是从后端搬到 client 的 ~175 行去抖…的载体"*). P4a wires the carrier; P3b/P4b deletes
the backend copy. The integrator corrects the three enum comments in `SetHashSuppressor.h` to say
`P4a - wired` and to name P3b/P4b as the owner of the backend deletion.

#### D-G2 — What each set carries

All three are `{Start, Count, ContentHash}` + a tail, **no stage dimension** (`ARCHITECTURE.md:99`,
`MGPipeTypes.h:429-431`, `PipeCalls.def:182-183`: MobileGL's texture-unit space is one merged array of
`MAX_TEXTURE_IMAGE_UNITS = 192`; per-stage 32 is an advertised number, the same unit can be sampled by two stages, and
stage is derived server-side from the reflection archive only where the target API needs it).

| set | `Start` | `Count` | tail entry |
|---|---|---|---|
| `set_sampler_views` | 0 | `GetMaxTouchedTextureUnit() + 1`, clamped to 192 | `MGPBoundView{View, Texture, Unit}` — `View = kMGPipeNullHandle` for a unit the program does not resolve, `Texture = kMGPipeNullHandle` for an unbound or undefined-default unit |
| `bind_sampler_states` | 0 | same | `MGPipeHandle` of the unit's sampler CSO, `kMGPipeNullHandle` when the unit has no `SamplerObject` (the texture's built-in sampler then applies, exactly as today) |
| `set_shader_images` | 0 | the image-unit high-water mark + 1 | `MGPImageView{Res, Unit, InternalFormat, Layer, Level, Layered, Access}` from `GetImageTextureBinding(unit)` |

`GetMaxTouchedTextureUnit` / the image high-water mark are **directly the `count` argument**
(`ARCHITECTURE.md:200`, second merge rule) and stay in the tracker walk; they are not re-derived.

**The var-tail window is the bound, and entries outside it are not cleared** — `Start + Count > 192` is
`Fatal{ProtocolCorruption}` and the record is *"the last set as received"* (`PipeApply.cpp:1303-1324`). Every P4a set
inherits that verbatim. `kMGPipeMaxTextureUnits = 192` and `kMGPipeMaxImageUnits = 192` are minted in
`MGPipeTypes.h` beside `kMGPipeMaxVertexAttribs` (`:378`) and pinned against
`TextureState::MAX_TEXTURE_IMAGE_UNITS` in the one TU that sees both, exactly as `PipeFill.cpp:1032-1038` pins the
attribute count.

#### D-G3 — `ContentHash` per set

XXH64 over the tail entries plus `MGPipeMixShutter` of `Start` and `Count`, computed field-wise over a zero-initialised
staging array (the `VertexInputEmit.h:118-133` shape). It must cover **every** input the record carries — for
`set_shader_images` that includes `InternalFormat` and `Access`, because the format the shader was built against is
live `glBindImageTexture` state and the format-less image bake keys on it (`ImageUnitFormatsStillMatch`,
`Managers.h:2030`). `ImageEmitTest.AnAccessModeChangeAloneStillEmitsTheSet` and
`…AnInternalFormatChangeAloneStillEmitsTheSet` are the unit gates; `FormatlessImageBakeScenario` and
`NonCoreImageFormatScenario` are the behavioural ones; **`photon-v1.3b` desktop retrace is the canary** (G3b).

#### D-G4 — The two image-unit invariants that must survive

1. **The `g_imageUnitHighWaterMark == 0` early-out** (`DirectGLES.cpp:2203`) is what makes every Minecraft draw pay one
   integer test. The client's emitter gets the same shape: high-water 0 → emit nothing, before any hash.
2. **The image sweep's gate is keyed on the FRONTEND sampling-resolution generation and deliberately not on a backend
   re-mint counter** (`DirectGLES.cpp:2195-2201`), because a texture bound *only* to an image unit is re-minted
   **inside** the sweep and a server-side epoch would be bumped after the gate had already declined. The client's
   `NewShaderImages` shutter is already `Mix(Mix(textureContent, textureParams), programImageUnitVersion)`
   (`Tracker.h:311-312`) — all three frontend counters — so the property is preserved by construction. **Say it in the
   emitter's header comment**, because it is the kind of thing a later optimisation deletes.

The bind-format recast (`DirectGLES.cpp:2126-2145`: a `GL_RG32F` bind is `INVALID_VALUE` on 19/26 non-core formats on
Adreno and 25 on both Malis) and the buffer-texture split view (`m_bufferImageSplitViewId`,
`Managers.h:1441`) are **server-side and unchanged**; the record carries the application's format and the server
recasts, which is the `ARCHITECTURE.md:235` assignment (image-bindable storage widen/split → server).

### D-H — Programs: shader CSO, draw/dispatch program, global constants, and the composite resolver

#### D-H1 — `CreateShaderState`'s payload, and what the archive must cover

`ARCHITECTURE.md:260`: the payload is per-stage SPIR-V + the reflection archive (`LinkArtifacts` + `SpirvArtifacts`,
whole structs), **not source**. "Server re-links from source" is explicitly closed: glslang lives entirely on the
client, SPIRV-Cross (`TranspileSpirvToEssl`) entirely on the server, a **file-level** cut; there is no
`MOBILEGL_IPC_PROGRAM` knob and no server-side compile pool (`:608` repeats the non-knob).

`MGPProgramDesc` (192 B, `MGPipeTypes.h:323-335`) fields and their sources:

| field | source |
|---|---|
| `Cso` | the client-minted `ShaderCso` handle (ordinary band, or the composite band for D-H5) |
| `StageMask` | `GetLinkedShaderStages()` — derived from **`GetLinkedShaderSnapshot()`**, never the live attach list (`ProgramObject.h:92-98`, `:121-133`: `glAttachShader`/`glCompileShader` take effect only at the next link and neither moves `m_linkVersion`) |
| `GlobalUboSize` | `GetUBOSize()` |
| `ReservedNumSamplesOffset` | `SpirvArtifacts::reservedNumSamplesOffset` |
| `SpirvStatus`, `NativeFloat64`, `PointSizeDemoted`, `EnableSpirvValidation` | the four `SpirvArtifacts` bytes |
| `Spirv[6]` | one `MGPBlobRef` per module of `SpirvArtifacts::generatedSpirv`, **in the linked-shader-snapshot's order** |
| `Reflection` | the serialised `LinkArtifacts` + `SpirvArtifacts` archive |

The archive **must cover**, verbatim from `ARCHITECTURE.md:261`: the four `ResourceReflection` vectors (each with its
`TypeFacts`), `uniformSamplerOrImageUnitIndex`, `uniformBlockBinding`, `shaderStorageBlockBinding` (**by name**),
`explicitOpaqueUniformBindings`, `xfbVaryings`/`xfbStrides`/`xfbPackedStride`/`xfbNeedsScatteredCapture`,
`computeLocalSize`, the GS/TCS/TES facts, `usesReservedNumSamples`, `uniformOffsets`; and `XfbVarying` carries **two
spellings** (GL name + block instance/member/element).

#### D-H2 — [ruling] The contract package writes the serializer, and the NDK size pin lands in the same phase

**The serializer does not exist.** `ProgramArtifacts.h` has the `VisitFields` tables (`:401-547`, one per type, the
same table serving both directions, `:403-406`) and the `sizeof` trip wires (`:548-575`) and nothing else; every
`VisitFields` comment says *"(and its serializer when one exists)"*.

Decision:

- **New file `MobileGL/MG_State/GLState/ProgramState/ProgramArtifactsCodec.{h,cpp}`**, beside the header, so the
  header's `artifacts-header` include-closure probe (`ProgramArtifacts.h:16-19`) is untouched. Two visitors over the
  existing tables: a writer that appends to a `Vector<Uint8>` and a reader that consumes one, both length-prefixed,
  little-endian, with a **format version word** first and a `MGL_LINKARTIFACTS_SIZE` echo second — so a struct that
  gained a field and a codec that did not is a **mismatch at read time**, not silent truncation.
- `LinkArtifacts::program` (the live `SharedPtr<glslang::TProgram>` at `:165`) is **null for every archived instance by
  construction** and is the one member `VisitFields` deliberately omits (`:471-474`). The codec must not add it; a
  `static_assert` on the visited field count guards that.
- **The libc++/NDK size pin (`ProgramArtifacts.h:563-565`) lands in this phase.** It is inert today
  (`MGL_ARTIFACT_SIZES_LIBCXX_PINNED` undefined), so an Android build has **no trip wire on a 58-member struct**.
  D.5 has the integrator read the four numbers from `ProgramArtifactsTest.SizesArePinnedOnThisToolchain`'s ctest
  properties on an NDK build and define them.
- **`MG_Test/Program/ProgramArtifactsCodecTest.cpp`** (beside `ProgramArtifactsTest.cpp`) round-trips a fully populated
  archive and compares field by field, including both `XfbVarying` spellings and every `TypeFacts` member; and
  `…ATruncatedStreamIsRefusedNotGuessed` and `…AVersionMismatchIsRefused` are its negative controls.

#### D-H3 — [ruling] In monolith the blobs do not travel; the codec is exercised by the verify build

`MGPProgramDesc` is 192 B with **seven** blob refs, and `ARCHITECTURE.md`'s ring caps one record at half its capacity —
so in split `create_shader_state` is one of the two emissions the emitter must split. That is P5's problem.

In monolith the one Blob rule applies (`MGPipeTypes.h:619-631`, ID-8b): **`Blob.Size = 0` means "this record does not
declare its blob"**, which is exactly what a monolith emission is, and the bytes ride beside the record through the
entry point's companion pointer — P3a's `const void* initialBytes` precedent (`PipeApply.h:63-68`). So:

- **push and pull builds**: all seven `MGPBlobRef`s are declared with `Seg = kMGHostSpanSegNone`, `Size = 0`, and
  `MGPipeApplyCreateShaderState(desc, const LinkArtifacts*, const SpirvArtifacts*)` takes the artefacts by pointer.
  **The codec is not called.** Zero serialisation cost on the monolith hot path — which is what keeps D-H's cost out
  of ID-19's budget.
- **verify build only**: the applier serialises, deserialises and field-compares before storing, and a mismatch is
  `Fatal{PipeVerifyDiffer, "program-archive"}`. That makes the codec live code with a gate rather than dead code with
  a unit test, and it is exactly the verify lane's job (`ARCHITECTURE.md:512`).

#### D-H4 — When `create_shader_state` is emitted, and what the tracker must not do

`ARCHITECTURE.md:198` says the async win is emitting from the compile pool's terminal continuation, so the SPIR-V
reaches the server before the first draw that uses it. **That is not P4a's.** In monolith the applier is one function
call away, so the win is unmeasurable, and the emission site matters for a different reason:

> `Tracker.h:242-245`: bit 6's shutter reads `ctx.GetCurrentProgram()` and **deliberately not `GetProgramForDraw()`**,
> because *"the tracker must not force a compile just to answer did the shader move"*.

**Decision: the tracker keeps its shutter; the EMITTER joins.** `create_shader_state` is emitted from the client
emitter at the validate point, from the same `GetProgramForDraw()` / `GetProgramForDispatch()` call the verb is about
to make anyway (join sites J1 at `Core.cpp:609-628`, and `:763` for dispatch) — so no join happens that would not have
happened. Emitting from the compile pool is recorded in `MEASUREMENTS.md` as a **P5 optimisation**, with the reason it
is deferred.

The record is (re-)issued on the **same handle** whenever the program's link version moves, exactly as
`CreateVertexElements` is (`MGPipeHandles.h:44-48`: `Gen` moves only on slot reuse). `BindShaderState` /
`SetDrawProgram` / `SetDispatchProgram` follow it. `set_draw_program` and `set_dispatch_program` are two calls because
the frontend has two joins and two `PipeInputs` slots (`m_programForDraw`, `m_programForDispatch`,
`PipeInputs.h:683-684`).

#### D-H5 — What the server still specialises: do not try to push it

`ARCHITECTURE.md:263` names **eight** extra inputs a backend program depends on beyond the artefacts, and Espryt's
nine-clause rebuild condition (`DirectGLES.cpp:3148-3200`) matches them: the draw-FBO snorm/unorm clamp masks
(`:3152-3153`), the fragColor broadcast count (`:3154`), the storage-block binding signature (`:3155-3156`), the
atomic-counter set, the live image formats (`!ImageUnitFormatsStillMatch()`, `:3164`), the patch parameters
(`:3188-3196`, a bitwise NaN-safe compare); Magma adds the default-FB height for the FragCoord Y-flip and the XFB
layout. `create_shader_state` publishes the **artefacts**; the server specialises at the verb from the state it holds.

**A brief that treated `create_shader_state` as self-contained would produce a per-draw rebuild.** Concretely, after
P4a the nine clauses read: the record's `Serial` (replacing `GetSyncedLinkVersion`/`GetSyncedImageUnitVersion`), the
applier's `DrawFramebuffer` record (replacing the two clamp masks and the broadcast count, both now derived from
`DrawBuffers[8]` and the inline `InternalFormat`s), the archive's `shaderStorageBlockBinding` (replacing
`ComputeShaderStorageBlockBindingSignature`), the applier's `ShaderImages` set (replacing
`ImageUnitFormatsStillMatch`'s live probe), and the P2 patch-state record. **The clause count does not shrink; its
inputs move.** `RelinkStageSetScenario`, `PostLinkAttachScenario`, `ProgramPipelineScenario`, `PointSizeDemotionScenario`
and `SpirvShaderBinaryScenario` are the gates.

Link failure needs no synchronous return (`ARCHITECTURE.md:264`): today it is one `MGLOG_E` plus a bind-program-0 empty
draw, `GL_LINK_STATUS` is never withdrawn, and the synchronous query is answered by the client from `ProgramObject`.
P4a changes none of that.

#### D-H6 — `set_global_constants`

`MGPGlobalConstants{ShaderCso, Version, Pad0, Blob}` (40 B, `MGPipeTypes.h:507-513`), **default uniform block only**
(D6, `ARCHITECTURE.md:102`: `globalUboScratch` is link phase B's CPU array — no GL name, no `BufferObject`), keyed
`(ShaderCso, Version)` and emitted **at most once per program per frame** (`:143`).

- `Version` = `GetUBOContentVersion()` (`ProgramObject.h:740`). **`~0u` is reserved as the backends' "never uploaded"
  sentinel and the wrap skips it** — the client must never emit `Version == ~0u`; `ProgramEmitTest.TheNeverUploadedSentinelIsNeverEmitted`
  pins it.
- The bytes are `MapUBO()`'s image, `GetUBOSize()` long, declared with `Blob.Size = 0` and handed over as a companion
  pointer, D-H3's rule.
- Espryt's **UBO ring is on the do-not-touch list** (`ARCHITECTURE.md:318`) and keeps working exactly as it does at
  `DirectGLES.cpp:3786-3856`: the ring allocation keyed `{contentVersion, ringGeneration, frameSerial, offset}`
  (`Managers.h:2003`), the `glBufferSubData` fallback (`:3833-3852`), the `ByteClass::StageUboGlobal` accounting. What
  changes is where the version and the bytes come from.
- **Named UBOs are not P4a's**: `set_shader_buffers(Uniform)` is P4b (dirty bits 15-17, `Managers.h:717-723` says so).
  `BindCurrentProgramWithResources`' named-UBO block (`DirectGLES.cpp:3857-3910`) is untouched, including its P3a
  buffer clean-probe.

#### D-H7 — `CompositeResolver`

`ARCHITECTURE.md:212`, verbatim, is the whole specification: `GLContext::GetProgramForDraw()` already flattens a
pipeline into one hidden composite `ProgramObject` **entirely in the frontend** (join, signature cache lookup,
`Link(true)`); the tracker takes the `SharedPtr<ProgramObject>` and pushes **one** handle, allocated from the
`ShaderCso` reserved high band; the pipeline cache's eviction releases the slot, bumps `gen` and emits
`delete_shader_state`; **the composite never crosses and the server needs no "resolved draw program" hook**; and the
side benefit is that the blocking `JoinLinkAndSpirv()` leaves the server's draw path.

New file **`MobileGL/MG_Impl/Pipe/CompositeResolver.h`**, header-only inside `#if MOBILEGL_PIPE_PUSH` (the
`ResourceTracker.h` ownership precedent: a new `.cpp` would need the root `CMakeLists.txt`, which is the contract
package's). It:

- keys on `ProgramPipelineObject::ComputeDrawProgramSignature()` (`ProgramPipelineObject.h:97-107`), the existing
  per-graphics-stage `{lifetimeId, GetLinkVersion()}` array — **not** `GetBackendStateVersion()`, which
  `ProgramPipelineObject.h:75-85` records made the SSO conformance loop rebuild the composite (glslang + SPIR-V +
  spirv-opt) **on every draw**;
- allocates from the reserved band. `SlotAllocator.cpp:19-24`'s `SlotIsAllocatable` **refuses the band for kind
  `ShaderCso`**, so the contract package adds `MGPipeSlotAllocator::AllocateComposite(lifetimeId)`, the one entry point
  allowed into `[kMGPipeShaderCsoCompositeSlotBase, kMGPipeShaderCsoSlotLimit)`, with the exhaustion assert
  (`SlotAllocator.cpp:57-60`) mirrored for the band;
- emits `create_shader_state` for the composite exactly like an ordinary program — **the server never learns it is a
  composite**;
- **has two release paths and they must not double-free**: the pipeline cache's LRU eviction
  (`ProgramPipelineObject::GetCachedDrawProgram`, `:149`) and the composite `ProgramObject`'s own destructor (a
  composite is an ordinary `ProgramObject` with its own lifetime id, `Core.cpp:661` `MakeShared<ProgramObject>(0u)`).
  Both go through the same client helper (D-I1) and the second is a **proven no-op**: `MGPipeSlotAllocator::Free` on a
  stale generation returns without touching anything (`SlotAllocator.cpp:117-119`). `CompositeResolverTest.
  EvictionThenDestructionFreesTheSlotExactlyOnce` and its mirror pin both orders.
- **`ProgramPipelineObject` itself has no lifetime id and no wire object** (`:19-170`; the only identity is
  `m_everBound`). It never gets a handle. `MarkProgramPipelineForDeletion` therefore stays `kUnpublishedDestroy`
  (D-K5).

`ProgramPipelineScenario`'s sixteen conformance cases and `MG_Test/Program/ProgramPipelineCompositeTest.cpp` are the
gates; the composite leak case is G8b's.
### D-I — Lifetimes and death paths: backend-neutral from day one

This is the P3a C-1 lesson, and it is the one design item P4a must get right before any package is written.

#### D-I1 — [ruling] Every kind P4a mints has ONE client-side death helper, in one fixed order

`MEASUREMENTS.md:507` records what P3a's final review found: the client-minted `VertexElementsCso` slot had **no
backend-neutral death path**. The slot was returned by a death-ops table the backend installs, Magma installs none, so
under Magma every VAO leaked one slot plus a ~1.3 KB applier record and the process `Fatal`d past 65536 slots. The fix
(`MG_Pipe/PipeMutation.h:127-141`, `MG_Impl/Pipe/PipeFill.cpp:801-849`) was a **client-side** helper taking the
lifetime id, with a three-position ordering argument.

**Decision: all six P4a kinds take that shape, from the first commit, with no intermediate state in which a backend
table is the only path.** One helper per kind, declared in `MG_Pipe/PipeMutation.h` (so `MG_State` sees a declaration
and never the client's tracker — `PipeMutation.h:79-92`, enforced by the closure gate's mutation-header probe), defined
in `MG_Impl/Pipe/PipeFill.cpp`, called from the object's **existing** `#if MOBILEGL_PIPE_PUSH` destructor body:

| kind | destructor | helper | wire call it emits |
|---|---|---|---|
| Texture | `TextureObject.cpp:31-41` (**virtual, on the base**, so 2D/3D/cube/buffer/view all announce once — `TextureObject.h:120-128`) | `MGPipeEmitTextureDestroyAndFree` | `ResourceDestroy` **and** `DeleteSamplerView` **and** `DeleteSamplerState` (the built-in sampler's CSO release, D-E1) |
| Renderbuffer | `RenderbufferObject.cpp:31-41` | `MGPipeEmitRenderbufferDestroyAndFree` | `ResourceDestroy` |
| Framebuffer | `FramebufferObject.cpp:27-37` | `MGPipeEmitFramebufferDestroyAndFree` | **none** (D-I2) |
| SamplerCso | `SamplerObject.cpp:30-40` | `MGPipeEmitSamplerCsoDestroyAndFree` | `DeleteSamplerState` |
| ShaderCso | `ProgramObject.cpp:32-45` (after `CancelLink()`) | `MGPipeEmitShaderCsoDestroyAndFree` | `DeleteShaderState` |
| ShaderCso (composite) | the composite's own `~ProgramObject`, **and** the pipeline cache's LRU eviction | the same helper | `DeleteShaderState`; the second call is a proven no-op (D-H7) |

**The order is fixed and is not a package's choice** (`PipeFill.cpp:836-848`):

1. **emit the wire delete first** — it drops the applier record while the record still exists;
2. **raise `NotifyStateObjectDestroyed(kind, lifetimeId)` second** — it resolves the handle through the allocator, so a
   backend told after the `Free` could no longer find its twin;
3. **free the slot last** — `MGPipeSlots().Free(kind, handle)`, and a double free on a stale generation is a proven
   no-op (`SlotAllocator.cpp:117-119`).

The five existing destructor bodies are **edited, not added** — adding a destructor would resize the pull build's
symbol set and break G1. Each helper returns "did the call go out", and the legacy path runs only when it says false
(the `BufferObject.cpp:49-65` shape).

Each kind also needs the **publication latch** `ResourceTracker.h:331-350` describes: the create is gated at its call
site and the destroy inside the emitter, so the two ask the same question at two different moments, and an object born
while a subsystem bit was clear and destroyed after it was set would otherwise free its slot while the applier's record
stayed `Live` on a slot about to be re-handed-out. **One latch per kind**, plus the self-healing create
(`PipeFill.cpp:648-673`) for the mirror case.

#### D-I2 — [ruling] Framebuffer has a handle but no wire lifetime

`PipeCalls.def` has `ResourceDestroy`, `DeleteRenderState`, `DeleteVertexElements`, `DeleteSamplerState`,
`DeleteSamplerView` and `DeleteShaderState` — and **no framebuffer delete**, because a framebuffer is not a resource
and is not a CSO: it is *state*, and `set_framebuffer_state` is the only call that names one. The catalogue is closed
(`PipeCalls.def:26-29`), so P4a does not invent a row.

**Decision:** the `Framebuffer` kind's handle is minted and freed **entirely client-side** and carries no
create/destroy call. The applier learns of a framebuffer only through `set_framebuffer_state`, keyed by `Fbo`, and
holds **working state**, not an object record. Its death helper therefore does steps 2 and 3 only (notice, then free),
and the client's `MarkFramebufferObjectForDeletion` path (`FramebufferState.cpp:59-72`, which already rebinds any slot
holding the victim to FBO 0) is what makes a dangling `Fbo` unreachable.

A recycled framebuffer handle is distinguished by `Gen`, which is inside `ContentHash` (D-C4), so a recycled handle can
never be suppressed against its predecessor's record. `HandleRecycleScenario`'s framebuffer ABA arm (G8) is the gate,
and it must be an `armExpectsCorruption=true` control — today's case at `:920` explicitly is not
(`:865-868` says *"The `AbaControl` knob does not steer this path"*), so the `MOBILEGL_PIPE_HANDLE_ABA_CONTROL` knob
has to defeat the identity half of P4a's memo keys as well.

#### D-I3 — The three payload-expressed ordering constraints, two of which are P4a's

`ARCHITECTURE.md:210`: *"三条顺序约束由 payload 表达：view 先于存储属主销毁（`ViewOf` + server keep-alive）、FBO
attachment 钉住纹理（surface handle 隐含 keep-alive）、buffer texture 钉住 buffer（`BufferForTexBuffer`，范围实时解析）."*

| constraint | how P4a expresses it |
|---|---|
| a texture view dies before its storage owner | `MGPResourceDesc::ViewOf` names the owner; the applier keeps the owner's record `Live` while any record names it in `ViewOf`, and a `ResourceDestroy` of an owner with live viewers is **not** refused — it drops the owner's record and leaves the server-side keep-alive to the backend twin, which is what `TextureObjectView`'s frontend `SharedPtr<ITextureObject> m_storageOwner` (`TextureObjectView.h:134`) already guarantees in monolith |
| an FBO attachment pins its texture | `MGPSurface::Res` in a live `DrawFramebuffer`/`ReadFramebuffer` record is the implicit keep-alive; in monolith the frontend `SharedPtr` in `FramebufferAttachmentObject` (`FramebufferObject.h:96-97`) is the real one and nothing changes |
| a buffer texture pins its buffer | `MGPResourceDesc::BufferForTexBuffer` + `BufOffset`/`BufSize`, resolved live (`TextureObjectBuffer.h:35-41`, `kMGPipeWholeBuffer`) |

**All three are keep-alives that monolith already has through `SharedPtr`s. P4a's job is to make them
representable, not to enforce them** — enforcement is P5's, when the frontend object's lifetime stops being the
server's. Say so, so nobody writes a refusal that monolith cannot need.

### D-J — What the applier stores

#### D-J1 — Records, bounds, and the three resource vectors

`PipeApply.h:97-113` is the rule and it is unchanged: **a slot at or above its bound is `Fatal{ProtocolCorruption}` —
the same verdict as any other record that would make the server act outside its own storage — and never a resize**;
the tables grow only to the client's own dense high-water mark. P4a adds bounds in the same shape and with the same
argument written out for each, because the records differ in size:

```cpp
inline constexpr Uint32 kMGPipeMaxResourceSlots       = 1u << 20;   // unchanged, PER KIND (D-B2)
inline constexpr Uint32 kMGPipeMaxVertexElementsSlots = 1u << 16;   // unchanged
inline constexpr Uint32 kMGPipeMaxSamplerCsoSlots     = 1u << 16;   // a 100-byte value + a handle
inline constexpr Uint32 kMGPipeMaxSamplerViewSlots    = 1u << 20;   // one per texture (D-F2), so it tracks the texture bound
inline constexpr Uint32 kMGPipeMaxShaderCsoSlots      = kMGPipeShaderCsoSlotLimit;  // 1<<20, and the composite band is inside it
```

New record types, all in `PipeApply.h`, all with `Uint32 Gen`, `Bool Live` and a `Uint64 Serial`:
`MGPipeSamplerCsoRecord{SamplerParameters Params;}`, `MGPipeSamplerViewRecord{MGPSamplerView View;}`,
`MGPipeShaderCsoRecord{MGPProgramDesc Desc; Uint32 GlobalConstantsVersion; Vector<Uint8> GlobalConstants;
Uint64 GlobalConstantsSerial;}`. `MGPipeResourceRecord` gains `MGPTextureParams Params; Uint64 ParamsSerial;
MGPipeHandle ViewCso;` and the per-`(uploadTarget, level)` pending-upload set of D-D5.

The `MGPipeShaderCsoRecord`'s `Vector<Uint8>` is the one allocation P4a adds per program, and it is bounded by
`GlobalUboSize`. It is **not** on the hot path: the emitter only fills it when bit 8 fires
(`ARCHITECTURE.md:143`: at most once per program per frame).

#### D-J2 — The body-level idioms P4a copies verbatim

From `PipeApply.cpp:931-1155`, whose section header is the template — *"what the applier owns here is IDENTITY,
EXTENT AND ORDER — NOT CONTENT"*, and each body resolves the handle against the record, checks the record against its
own declared extent, then moves the record and hands the call on:

- **a create starts the record over rather than editing it** (`:960-967`): a recycled slot's record must not contribute
  one field; `Gen` is taken from the handle and nothing else survives; `Serial` stays 0 because a create is not a
  mutation, so a fresh backend twin starting at 0 agrees without either side publishing anything;
- **`Serial` moves BEFORE the backend is told** (`:578-582`);
- **destroy drops the record whole and keeps the generation** (`:1092-1095`); the *client* frees the slot afterwards;
- **a re-create on the same handle keeps the serial and counts up; a different identity starts over** (`:1200-1204`),
  **and it does not rebind** (`:1223-1226`);
- **a dead handle leaves the previous binding untouched** (`:1263-1278`), and a null handle is legal and means
  "nothing bound";
- **the var-tail window is the bound and entries outside it are not cleared** (`:1303-1324`).

#### D-J3 — Refusal counters, and why they are not assertions

`PipeApply.h:220-229` and `PipeApply.cpp:441-473`: `MOBILEGL_ASSERT` compiles out at INFO — which all three gate builds
and every shipped build are — so *"a no-op nobody can see is a dropped call nobody can see"*. P4a adds
`Uint64 RefusedObjectCalls` (framebuffer / sampler / view / program / texture-params) in the same shape, with the same
`kResourceRefusalNote` wording, and the one legal refusal sequence documented at `PipeApply.cpp:452-462` (teardown →
`MGPipeApplierReleaseObjectRecords()` → `~Object` → death notices naming records already dropped) restated for the new
kinds.

#### D-J4 — Reset semantics, stated as a rule and not as an absence

ID-12's B-C2 ruling, which P4a must restate for its own trackers rather than leaving absence as policy
(`ResourceTracker.h:465-498` is the wording to copy):

- **`MGPipeApplierReset()` is a make-current, not a teardown** (`PipeApply.cpp:601-666`). It clears P4a's **working
  state** — `DrawFramebuffer`, `ReadFramebuffer`, the three unit sets, `DrawProgram`, `DispatchProgram`,
  `BoundShaderCso` — and **advances**, never zeroes, their serials (`PipeApply.cpp:651-665`'s three-way argument:
  carrying over is wrong immediately; restarting at 0 walks back through values already stamped into a twin that
  outlived the switch; advancing is the safe direction).
- It **does not** drop the object records: texture/renderbuffer resources, sampler CSOs, sampler views, shader CSOs.
  Therefore **no P4a tracker needs a re-publication path on `FreshlyPrimed`, and none may have one**: re-emitting
  `create_sampler_state` for a record the applier still holds would move its `Serial` for nothing.
- What **does** reset on `FreshlyPrimed` (`PipeFill.cpp:1570-1585`) is each emitter's **bound-handle latch**, because
  those mirror working state the applier just cleared: the framebuffer emitter's last-emitted hash (through
  `SetHashSuppressor::InvalidateAll()`, which already runs there), the three unit sets' hashes (same), and the program
  emitter's `BoundShaderCso` mirror. The **record half** of every latch stays out of `Reset()`, exactly as
  `VertexInputEmit.h:292-327` keeps `RecordIsPublished`/`NoteRecordDestroyed` out of it.
- The trap `VertexInputEmit.h:293-303` records applies to **every** P4a kind whose slot a backend twin table mints:
  *a slot can exist with no record behind it*, because `BackendSlotTable::GetOrCreate(StatePtr)` mints through
  `MGPipeSlots().Acquire` whether or not the subsystem ever asked the client to emit — **which is exactly what a
  `MOBILEGL_PIPE_PUSH=0x1ff` lane runs**. A `delete_*` on such a handle is a refused call and the resolver asserts and
  counts. Five kinds inherit this; the emitters must ask `RecordIsPublished(handle)` before emitting a delete.

### D-K — Subsystem bits, dirty bits, and the dirty-surface gate

#### D-K1 — Four new subsystem bits, and the phase constant

`MGPipe.h:66-70`: bits are allocated in ROADMAP order and **never reused** — *"an operator's recorded `0x7f` has to
keep meaning what it meant"*. `:86` reserves bits 9..62. P3a took **two** bits rather than one because *"a buffer path
that regressed and a vertex path that regressed are different findings"* (`:80-83`). The same argument gives P4a four:

```cpp
inline constexpr Uint64 kMGPipeSubsystemFramebuffer      = 1ull << 9;   // set_framebuffer_state
inline constexpr Uint64 kMGPipeSubsystemTextureResources = 1ull << 10;  // texture + renderbuffer resource_*, set_texture_params
inline constexpr Uint64 kMGPipeSubsystemSamplers         = 1ull << 11;  // sampler CSO, sampler view, the three unit sets
inline constexpr Uint64 kMGPipeSubsystemPrograms         = 1ull << 12;  // shader CSO, draw/dispatch program, global constants
inline constexpr Uint64 kMGPipeSubsystemsMigratedAtP4a   = 0x1fffull;   // bits 0..12
```

`kMGPipeSubsystemsMigratedAtP2 = 0x7f` and `kMGPipeSubsystemsMigratedAtP3a = 0x1ff` are **not edited** — `0x1ff` is
P4a's T2 arm and its "everything P3a had and nothing of mine" control. The push default in `CMakeLists.txt` /
`ConfigLoader.cpp:257` moves `0x1ff` → `0x1fff`, and every itest lane pinned at `0x1ff` moves with it
(`CMakeLists.txt:949, 1065, 1075, 1149, 1183, 1188, 1219`), because *"pinning it at `0x1ff` after P4a would leave the
Handles arm asserting the P3a shape while the handles it is supposed to be about stayed switched off"*
(`:942-947`'s own argument). The CSO-off lane's mask becomes `0x8000000000001fff`
(`:1070`, `:1080`) — **the mask moves with the phase, the control is bit 63, and the two must not be confused**
(`:1112-1128`).

#### D-K2 — The three dependency refusals

`PipeFill.cpp:1062-1069` and `Managers.cpp:2393-2410` are the precedent: **bit 8 requires bit 7**, and the mirror pair
is fine. P4a has three of the same shape, and every one of them is *diagnosed at bring-up and stopped at first lookup*,
never half-run:

| dependency | why | what happens |
|---|---|---|
| bit 11 (samplers) **requires** bit 10 | every `MGPBoundView::Texture` and `MGPImageView::Res` names a `Texture` handle, and only bit 10 populates the texture slot table; without it every lookup misses and the walk would `continue` without unbinding | one `MGLOG_E` naming both bits, **refuse bit 11**, run the legacy sampler arm |
| bit 9 (framebuffer) **requires** bit 10 | `MGPSurface::Res` names a Texture or Renderbuffer handle | same shape |
| bit 10 (texture resources) **requires** bit 7 | a buffer texture's `BufferForTexBuffer` names a `Buffer` handle, and only bit 7 puts twins in that table (D-D1) | same shape |

The mirror pairs (bit 10 without 11, without 9; bit 7 without 10) are all fine and are stated as such, because an
unreachable branch that says something different is how the reachable one drifts (`Managers.cpp:2377-2381`'s own
words). Bit 12 (programs) depends on nothing: a `ShaderCso` handle names no texture and no buffer.

#### D-K3 — The arm classification, and where the stop lives

`ClassifyPipeSubsystemArm(bitSet, legacyMemosEnabled, legacyArmSurvivesLegacyMemos)` (`Managers.cpp:2358-2362`) and
`StopOnArmlessPipeSubsystem` (`:2352-2356`) are reused unchanged; P4a adds four
`Resolve<Family>SubsystemArm()` functions beside `ResolveResourceSubsystemArm` / `ResolveVertexInputSubsystemArm`, each
stating **for its own family** whether the legacy arm survives `MOBILEGL_PIPE_LEGACY_MEMOS=0`:

| family | legacy arm survives `LEGACY_MEMOS=0`? |
|---|---|
| framebuffer | **no** — the four `g_fboSynced*` arrays and `StampSyncedFBO` are the pre-handle arm |
| texture resources | **no** — the twin's `m_prevTextureInfo` / `m_syncedContentVersion` cheap-gate trio is the pre-handle arm |
| samplers | **no** — `UnitSamplerLookupMemo`'s `WeakPtr` arm and `SamplerPassMemo`'s raw `BackendSamplerObject*` rows are the pre-handle arm |
| programs | **no** — `g_programTwinLookupMemo` (`DirectGLES.cpp:134-135`) is the pre-handle arm |

so all four can reach `NoArm` and all four stop with the named `Fatal{PipeLegacyMemosDisabled, ...}` rather than
skipping green. **The resolution is lazy, at the first use, not at bring-up** — `SlotTables.h`'s long comment explains
why: a forked pre-flight child dying on a signal makes the whole lane SKIP green, *"which is what `ROADMAP.md:7`
forbids"*. `ARCHITECTURE.md:369` keeps `MOBILEGL_PIPE_LEGACY_MEMOS` compiling the pre-handle arm **through P3a/P4a**,
and budgets **+1 day per phase** to maintain it; P4a pays that day.

Also here: **D-C3's `MaxColorAttachments > 8` refusal** rides `ResolveFramebufferSubsystemArm()`.

#### D-K4 — Dirty bits: seven, and the constant that is added rather than edited

`Tracker.h:93-95`: each phase's constant survives as the next phase's A/B control, so none is edited in place. P4a
adds

```cpp
inline constexpr Uint32 kMGPipeDirtyEmittedAtP4a =
    kMGPipeDirtyEmittedAtP3a | MGPipeDirtyBit(MGPipeDirty::NewShader) |
    MGPipeDirtyBit(MGPipeDirty::NewShaderBindings) | MGPipeDirtyBit(MGPipeDirty::NewGlobalConstants) |
    MGPipeDirtyBit(MGPipeDirty::NewFramebuffer) | MGPipeDirtyBit(MGPipeDirty::NewSamplerViews) |
    MGPipeDirtyBit(MGPipeDirty::NewSamplers) | MGPipeDirtyBit(MGPipeDirty::NewShaderImages);
```

and seven `MGPipeSubsystemForDirty` arms. **The shutters are already computed, latched and counted today**
(`Tracker.h:249-262`, `:303-312`) — P4a only emits for them:

| bit | shutter, verbatim from the tree | subsystem |
|---|---|---|
| 6 `NewShader` | `Mix(program->GetLifetimeId(), program->GetLinkVersion())` from **`GetCurrentProgram()`, deliberately not `GetProgramForDraw()`** (`:242-245`) | Programs |
| 7 `NewShaderBindings` | `Mix(Mix(Mix(ImageUnitVersion, BackendStateVersion), BlockBindingVersion), UniformWriteSetVersion)` | Programs |
| 8 `NewGlobalConstants` | `Mix(lifetimeId, GetUBOContentVersion())` | Programs |
| 11 `NewFramebuffer` | `Mix(GetAnyFramebufferAttachmentGeneration(), m_framebufferBind.Observe(GetFramebufferBindingSlot(Draw).GetVersion()))` | Framebuffer |
| 12 `NewSamplerViews` | `Mix(textureContent, GetTextureBindGeneration())` | Samplers |
| 13 `NewSamplers` | `Mix(textureParams, GetSamplingResolutionGeneration())` | Samplers |
| 14 `NewShaderImages` | `Mix(Mix(textureContent, textureParams), program->GetImageUnitVersion())` | Samplers |

Two narrowings P4a **must** make, and both are stated as requirements rather than options:

1. **Bit 11 does not see the READ binding.** Its shutter observes `GetFramebufferBindingSlot(Draw)` only, so a
   `glBindFramebuffer(GL_READ_FRAMEBUFFER, ...)` moves nothing. D-C2 emits per target, so bit 11's shutter gains the
   read slot's version: `Mix(Mix(anyAttachmentGeneration, drawBindVersion), readBindVersion)`. Over-firing is free and
   under-firing is fatal (`Tracker.h:28-35`), and this is an **under-fire** today.
2. **Bit 11 does not see a renderbuffer respecify** (D-D2). That is closed by emitting from the storage entry point,
   not by widening the shutter — the emission is not gated on bit 11.

`FramebufferObject::SetDrawBuffer` versions the *value* being written rather than the index being written to
(`FramebufferObject.cpp:195-199`: it calls `BumpAttachmentVersion(buffer)`). `m_objectVersion` and the aggregate still
move, so bit 11 is safe — **but a narrower P4a shutter built on `m_attachmentVersions` would not be, and P4a must not
build one.** Recorded here so the next narrowing does not walk into it.

#### D-K5 — The dirty-surface gate: the prefix widening and four rows

`gen_pipe_dirty_surface.py:72-77` matches `pGLContext->` + `Add|Set|Mark|Bump|Allocate|Truncate|Record|Notify|Begin|End`.
`UseProgram` begins with `Use` and is therefore invisible; so are `BindVertexArray`,
`BindProgramPipelineObject` and `BindTransformFeedbackObject`. All four move a field P3a or P4a pushes.

**Decision: `MUTATOR_PREFIXES` gains exactly `Use` and `Bind`.** The complete set that widening surfaces, enumerated at
`$BASE` by grep so it cannot surprise the implementer, is four names (`UseProgram` ×2 sites, `BindVertexArray` ×3,
`BindProgramPipelineObject`, `BindTransformFeedbackObject`); `Create*` and `Pop*` are **deliberately not added**, with
the reason written into `DirtySurface.def`'s header: they create or destroy objects rather than move a pushed field,
and each object class's creation and destruction is already answered by its own `Mark*ForDeletion` row plus the
constructor-time `resource_create`. The four rows:

```
X(UseProgram,                  NEW_SHADER)
X(BindVertexArray,             NEW_VERTEX_ELEMENTS)
X(BindProgramPipelineObject,   NEW_SHADER)
X(BindTransformFeedbackObject, kPulledEveryVerb)
```

and the destroy family changes: **`MarkProgramForDeletion` becomes `kExplicitDestroy`** (P4a's `delete_shader_state`
is what closes `DirtySurface.def:113-124`'s recorded hole, *"programs, program pipelines and shaders have no
per-object handle on the wire at all in P2"*), while **`MarkProgramPipelineForDeletion` and `MarkShaderForDeletion`
stay `kUnpublishedDestroy` with an updated reason** — a `ProgramPipelineObject` has no lifetime id and no wire object
(D-H7), and a `ShaderObject` has no lifetime id and never crosses (`ShaderObject.h:24`, dtor `:52`). That is the
deliberate three-way answer `scout-frontend-state.md` risk 10 asks for.

`MGP_DIRTY_SURFACE_UNDECIDED_LIST` (`:332`) stays **empty**; `--self-test` must report more negative controls than
P3a's 21 (one per new row plus one for the widened prefix set).

### D-L — Counters

`ROADMAP.md:7` forbids hot-path instrumentation and `Tracker.h:35-38` forbids timers; every counter below is one
`AddCalls`/`AddBytes` behind the standard `if (PipeStats::Enabled())` predicate (`PipeStats.h:29`), so an off build
pays one predicted branch.

**`CallClass` grows inside the existing `#if MOBILEGL_PIPE_PUSH` block** (`PipeStats.h:105-127`, which ends at
`MapPersistentRoundtrips`) — the reason that block states is G1: growing the enum in a **pull** build resizes the
counter arrays, the name table and `FormatWindowLine`.

| member | short name | meaning |
|---|---|---|
| `FramebufferEmissions` | `fbe` | `set_framebuffer_state` records actually sent (post-suppressor) |
| `SamplerViewEmissions` | `sve` | `set_sampler_views` records sent |
| `SamplerStateEmissions` | `sse` | `bind_sampler_states` records sent |
| `ShaderImageEmissions` | `sie` | `set_shader_images` records sent |
| `ClientTextureUploadEmissions` | `ctu` | `ResourceSubData` texture records sent, the client-side twin of Espryt's `TextureUploadEmissions` |

**`ByteClass` has NO push guard today** (`PipeStats.h:46-77`), so P4a **adds one**: a new
`#if MOBILEGL_PIPE_PUSH` block at the tail of `ByteClass`, immediately before `Count`, carrying

| member | short name | meaning |
|---|---|---|
| `CsoBlobBytes` | `csob-blob` | the bytes of every CSO blob the client declares per frame — vertex-elements blobs, sampler parameter blobs, program archives |

which discharges `MEASUREMENTS.md:465` reading (5) — *"`vtxc` 是 client 数组，**未测**，**留给 P4a 给汇总行加类**"* —
for the vertex-elements blob P3a could not report, and covers P4a's own.

All six names are pinned in `MG_Test/Util/PipeStatsTest.cpp`'s `CounterNamesAreStable` (`:260`) and
`SummaryLineCarriesEveryClassAndGate` (`:137`), and printed by `FormatWindowLine` beside `csom`/`csob`/`mpr`. Espryt's
four existing `TextureUpload*` counters (`Managers.cpp:6360-6395`) are untouched, so a client/server emission-shape
divergence is a **difference of two published numbers** rather than something only a GPU can see — which is the whole
point, since SSIM is blind to the box/rect split.

**Still-unmeasured items P4a inherits and does NOT close**, recorded so nobody thinks they were forgotten
(`MEASUREMENTS.md:525`, `:527`): the per-dirty-bit fire rates (`FireCount`/`WalkCount` exist at `Tracker.h:395-410`
but are not in `FormatWindowLine`), the 24-bucket per-draw payload histogram (teardown-JSON only, and the trace app
never reaches teardown), and **peak RSS** (`retrace_gate.py` takes only `--tree --lib --out -j --only` and has no
`rss`/`maxrss`/`getrusage`). Peak RSS matters more in P4a than it did in P3a, because P4a adds **five** more slot
tables. **Decision: P4a does not instrument the tool** (it is `~/w7`-local and outside the repo's gates) and instead
hedges structurally: unconditional destroy emission (D-I1) plus a **leak test per new kind** (G8b) plus the
`PipeSlotPeek` high-water assertions. The hedge is written into `MEASUREMENTS.md` so the absence is a decision, not an
oversight.

### D-M — Emulations: what stays where, and the split Fatal

`ARCHITECTURE.md:216` is the rule — *"驱动表达不了的变换在 tracker 里 lowering，硬件/驱动强加的变换在 driver 里
lowering"* — and `:221-243` is the per-emulation table. For P4a's families:

| emulation | owner | P4a |
|---|---|---|
| fragColor MRT broadcast (`BroadcastLegacyFragColor`, `Utils.cpp:506`) | **server** | a post-emission ESSL rewrite on `ARCHITECTURE.md:318`'s do-not-touch list, keyed on framebuffer state the server now holds (D-C4). Deleting the workaround and `g_broadcastMemo*` is **P3b/P4b** (`ROADMAP.md:23`) |
| noperspective emulation (`EmulateNoPerspectiveForEssl`) | **server** | driver-capability-armed; the client cannot know the arming |
| fp64 demotion (`DemoteFloat64Pass`) | **client, already** | decided per program at link, recorded as `SpirvArtifacts::nativeFloat64`, and it rides in `MGPProgramDesc::NativeFloat64` |
| texture LOD bias (`EmulateTextureLodBias` + the per-draw `glUniform1f` at `DirectGLES.cpp:3982-4000`) | **server** | the *value* is sampler state and arrives with the sampler CSO; the pass reads `SamplerParameters::lodBias`, and the fallback to the texture's own sampler now reads the texture's `BuiltinSampler` CSO (D-E1) |
| image format bake / widen / split / array remap | **server** | `ARCHITECTURE.md:235`; the record carries the application's format and the server recasts (D-G4) |
| pass-through TCS synthesis | **server** | keyed on `set_patch_state`, already P2 |
| compressed textures / pixel-unpack normalisation | **client, already** | `ARCHITECTURE.md:256`, `:104` |
| `glCopyTexSubImage*` / `glClearTexImage` | **client, already** | pure frontend operations today (a `ReadPixels` into CPU scratch, then a shadow write); the dirty region ships as ordinary sub-data |

**The split Fatal.** `ROADMAP.md:20`'s last clause: *"emulation 在 split 下显式 Fatal 直到 P8"*. In monolith the code
paths keep running exactly as today (the Fatal is a split-only arm), so this costs P4a **a named, greppable call site
per unmigrated emulation and nothing else**:

```cpp
// MG_Pipe/PipeApply.h. In monolith this is a no-op: the emulation still runs, exactly as it
// does today. P5/P8 give it teeth - a split server that reaches one of these has no client
// address space to read and must abort loudly rather than degrade silently (ROADMAP.md:20).
void MGPipeUnmigratedEmulation(const char* name);   // monolith body: (void)name;
```

Call sites P4a plants, each naming the emulation P8 retires:

| site | name |
|---|---|
| `DirectGLES.cpp:~7619` the `glCopyImageSubData` CPU-shadow mirror, under `CanMirrorCopyImageShadow` | `"copy-image-shadow-mirror"` |
| `DirectGLES.cpp:6707` `EnsureGenerateMipmapStorageAllocated` | `"generate-mipmap-storage"` |
| `DirectGLES.cpp:7432` `GenerateThreeChannelFloatMipmapOnCpu` | `"generate-mipmap-cpu-fallback"` |
| `DirectGLES.cpp:9244` `GetTexImageViaShadowConversion` | `"get-tex-image-shadow"` |
| `Managers.cpp:4769` `RequireImageBindableStorage`'s re-dirty of already-uploaded levels | `"texture-remint-pull"` |

`PipeCatalogueTest.EveryUnmigratedEmulationIsNamedOnce` pins the list, and G13b greps the count. The last one is the
head of `ARCHITECTURE.md:299-308`'s **only new stall class**: P4a supplies mitigation 1 (prevention —
`ImageBindableHint` on every create/respecify, D-A4) and names the site; mitigations 2-4 (async pull, bounded
retention, the `ResourceSubDataComplete` terminator) and `TextureRemintPullScenario` are **P9's**
(`ROADMAP.md:26`), and P4a must not build a half-terminator.

### D-N — The byte-identical set (G5)

`ARCHITECTURE.md:318` is the Espryt do-not-touch list and `:321` names the **one** item moved out of it (the sub-rect
upload decision and the stride computation, which is D-D3/D-D6's subject). P3a made the buffer half of that list
literal with a sha gate; P4a extends the same script to its own half.

**P3a's eleven stay, unchanged, with their two shapes (ID-15):** `IsPoolable`, `EnrollIntoPool`, `AcquireFromPool`,
`TrimBufferPool`, `ClearBufferPool`, `ProcessDeferredBufferReleases`, `CreateRingStorage`, `RingAvailable`,
`RingAllocate` and `FlushPendingRangesNow` against the P3a baseline; `FlushPendingRangesFrom` against its **pinned**
`3e298c9a` sha. **P4a must not touch any of the eleven.**

**P4a adds six**, all of which predate the phase and are therefore compared against `$BASE`:

| function | file:line | why it survives verbatim |
|---|---|---|
| `StageBlocksIntoUnpackRing` | `Managers.cpp:4947` | the unpack-PBO staging repack; it is what makes the ring path issue **no** `glPixelStorei`, and the +6 ms/frame Mali cliff lives on the other side of it |
| `UnpackRingAvailable` | `Managers.cpp:3466` | honours `Features.EsprytDisableUnpackRing` and self-heals a stale context generation |
| `UnpackRingAllocate` | `Managers.cpp:3471` | on the hot upload route |
| `RecomputeBackendColorSlots` | `Managers.cpp:7432` | the attachment permutation, three passes plus the forced `~0` version memo on every moved attachment; `:7651-7658` says removing the empty-point detach breaks the invariant |
| `DepthStencilSamplingReadImpl` | `DirectGLES.cpp:8117` | the D24S8 sampling-emulation core (memory `better-clouds-fullmode`), entered from `:8617` and `:8721` |
| `ShouldUseCaveatTextureFormat` | `Utils.cpp:245` | the format handler P4a's `InternalFormat` descriptors feed; a change here silently changes what every texture is allocated as |

**Seventeen functions across three files**, which is a real change to the script: `p3a_untouched_regions.sh`'s
`SOURCE_PATH` is a single file (`:85`). `scripts/p4a_untouched_regions.sh` keeps every other property of its parent
verbatim — the mask-first preprocessor-blind extraction (comments and string/char/raw-string literals replaced by
spaces of the same length, the definition found as the one `<name> (` whose closing paren is followed by `{`, the body
brace-matched in the masked text and **hashed from the ORIGINAL text**, so a comment change inside one of these is a
difference too); **exactly one definition per name, zero or two is exit 2 and never a silent pass**; stdout is always
the sha list so a baseline capture is a plain redirect; both arguments are git refs, so an uncommitted edit is
invisible by design; exit 0 identical / 1 moved, naming the first in the fixed order / 2 could not run — and adds a
per-function source path plus `EXPECTED_FUNCTION_COUNT=17`. `--self-test` must report **four** negative controls:
P3a's two (`ClearBufferPool`, `FlushPendingRangesNow`) plus `RecomputeBackendColorSlots` and
`StageBlocksIntoUnpackRing`.

**A shared template over an accessor interface is REJECTED** (ID-13, `MEASUREMENTS.md:521`): it resizes pull symbols
and breaks G1. If a P4a arm needs a variant of one of the seventeen, it gets its own `#if MOBILEGL_PIPE_PUSH` function
with its own name and its own pinned sha — the `FlushPendingRangesFrom` shape.

### D-O — What must NOT change

Every row names its behavioural gate. A package that has to touch one of these raises it as a finding rather than
doing it.

| must not change | file:line | gate |
|---|---|---|
| P3a's eleven G5 functions | `Managers.cpp` | G5 |
| The three persistent-mapped rings and `PersistentRing`, the buffer pool | `ARCHITECTURE.md:318` | G5, `LargeArenaAdoption`, `StorageBufferRegrow` |
| `m_backendColorSlots`' permutation invariant, including the explicit detach of an empty colour point | `Managers.cpp:7659-7666` | G5, the Iris MRT traces, `TwoAttachmentCompositeFramebufferMatchesIrisComplementaryPass` |
| The **draw-buffer `None` → `GL_NONE`** handling and the "apply draw buffers only for the DRAW target" rule | `Managers.cpp:7544-7560` | `improved-transparency-minecraft-26.3` (memory `oit-improved-transparency-fix`), `FramebufferTest.NamedFramebufferDrawBuffersDoNotModifyDefaultFramebuffer` |
| The **default-FBO bind through the shadow** — a raw bind leaves the shadow claiming the previous user FBO and false-skips its next re-bind | `DirectGLES.cpp:3253-3259`, `:3267-3280`, `Managers.cpp:7096-7105` | `ClearThenReadPixelsScenario`, `OrientationScenario` (memory `readback-state-guards`) |
| `BindCurrentFBO` has **no fast path on the slot version** | `DirectGLES.cpp:3211-3216` | `KHR-GL32.packed_pixels` — and P4a's own G15 block |
| The three scratch FBOs and their driver shadows (`ScratchFBOImpl`), `PackState`/`PixelStoreImpl`, every driver binding shadow | `Managers.cpp:7767-8054`, `Managers.h:1783-1806` | `PixelStoreSweepScenario`, `PackedWordReadbackScenario` |
| `ScopedDefaultUnpackState` and its process-wide shadow | `Managers.cpp:4851-4918` | untouched; D-D3 records the **inherited** hazard that `s_synced` (`:4911`) is written at `:4878`, read at `:4875` and invalidated **nowhere**. P4a neither fixes it (`ROADMAP.md:98`) nor makes it worse: the ring path still issues no `glPixelStorei` and the one outside writer (`:6400`) is still guarded by `!ringStaged` |
| The Adreno disabled-attribute SIGSEGV workaround, the Mali XFB-capture-loss workaround, the SPIRV-Cross session and post-emission ESSL rewrite, the driver POST self-test family | `ARCHITECTURE.md:318` | their own scenarios |
| The alpha-widened / SNORM / UNORM caveat-attachment family, including `SubstituteWidenedClearAlpha` and the forced alpha write mask | `Managers.h:1642-1680`, `DirectGLES.cpp:7917-7963` | `ThreeChannelAttachmentScenario`, `SnormAttachmentScenario`, `FramebufferTest.WidenedDrawBufferIsIdentifiedPerDrawBufferSlotNotPerAttachmentPoint` |
| `SyncToBackend`'s re-entrancy loop until `g_attachmentBackendIdGeneration` is quiescent | `Managers.cpp:7726-7736` | `ImageSizeAfterRespecScenario` |
| `SupportsLayeredImageBinding`'s rule (ask the **backend** target after `MapToBackendTextureTarget`; force `layer` to 0 for a non-layerable target — Adreno took a stray layer index literally) | `DirectGLES.cpp:1992-2013` | `ImageTargetKindScenario`, `LayeredAttachmentShapeScenario` |
| `g_attachmentBackendIdGeneration`, `g_bufferBackendIdGeneration`, `g_backendContextGeneration` | `Managers.cpp:7763`, `:1503`, `:646` | D-B3; they are server-owned `MGGen`s and no client version can replace them |
| `GL_ARB_texture_view`'s double gate (host extension **and** `Features.EsprytEnableTextureView`) and the known `SyncTextureViewToBackend` normalisation break | `BackendObject_DirectGLES.cpp:1177-1203` | `TextureViewScenario` incl. `BetterCloudsCoveragePipeline` |
| `set_pixel_unpack_state` does not exist; `MGPPixelPackState` is PACK only; `GetPixelStoreParameters` is **deliberately absent** from `Coverage.def`'s emitted list | `ARCHITECTURE.md:104`, `Coverage.def:170-181` | `pipe-gates`; **P4a must not add that row** — the field is both halves of `m_pixelStore[2]` and a row would blind both the poison and the verify comparator to the unpack half |
| The eight statically over-approximated `FillPoints.def` rows | `FillPoints.def:30-70` | **all eight stay**; the omission sweep rides the G15 caselist run and no row may be retired on desktop evidence |

### D-P — How the pull build stays inert (G1)

P3a's D-K, restated for P4a's surface. **Every P4a edit to `MG_State`, `MG_Pipe` and `MG_Backend` is inside
`#if MOBILEGL_PIPE_PUSH`.** Concretely:

- the six destructor bodies are **already** `#if MOBILEGL_PIPE_PUSH` (`TextureObject.cpp:31`,
  `RenderbufferObject.cpp:31`, `FramebufferObject.cpp:27`, `SamplerObject.cpp:30`, `ProgramObject.cpp:32`), so P4a
  edits five existing lines and adds no destructor;
- new client sources (`FramebufferEmit.h`, `TextureEmit.h`, `SamplerEmit.h`, `ProgramEmit.h`, `CompositeResolver.h`)
  are **header-only** and are compiled only from push translation units, and the one new `.cpp`
  (`ProgramArtifactsCodec.cpp`) is added to the build inside `if (MOBILEGL_PIPE_PUSH)` in the root `CMakeLists.txt`
  (`:462-468`'s block);
- `PipeStats`' new `CallClass` members go inside the **existing** push block; the new `ByteClass` member goes inside a
  **new** push block (D-L) — without it the pull build's counter arrays, name table and `FormatWindowLine` all resize;
- `MGPTextureParams` 32 → 40 and `MGPFramebufferState::Pad0` → `Target` are header edits to structs no pull code
  instantiates; `PipeCatalogueTest` compiles in every build and pins the sizes, which is where a mistake surfaces;
- `SetBackendResource` is **not deleted** — P3a's `ARCHITECTURE.md:284` deviation stands: under push it is simply never
  called, and the deletion retires with the pull path at P13;
- Espryt's new arms are added beside the legacy ones under `MOBILEGL_PIPE_LEGACY_MEMOS`, never in place of them
  (`ARCHITECTURE.md:369`).

**P4a's admitted-resize set is EMPTY.** A resize means an edit escaped a guard; it is attributed per symbol in the
commit message and fixed, never waved through (ID-3's precedent).

### D-Q — What P4a does **not** do

Stated as loudly as what it does, because three of these look like P4a's from the payload table alone.

| not P4a | who | evidence |
|---|---|---|
| `Blit`, `Clear`, `ReadPixels`, `GetTextureImage`, `ResourceCopyRegion`, `GenerateMipmap` — the transfer verbs and the readbacks | **P3b/P4b** (*"回读 / pack state"*) and **P8** | `ROADMAP.md:23`, `:25`; `ROADMAP.md:20`'s column 3 names none of them |
| the PACK pixel-store push (`set_pixel_pack_state` is P2's row but the readback path still reads `GetPixelStoreParameters` live) | **P3b/P4b** | `ROADMAP.md:23` |
| `set_shader_buffers` (named UBOs, SSBOs, atomic counters) | **P4b** | dirty bits 15-17, `Managers.h:717-723` |
| the ~175-line backend debounce removal (`UnitBindingsSnapshot`, `g_unitTextureSyncList`, `g_fboTextureSyncList`, the unit-bindings epoch derivation) | **P3b/P4b** | `ROADMAP.md:23`; P4a wires the *carrier* (D-G1) |
| the per-storage-owner emission cursor with view/owner index remapping, and `TextureUploadShapeScenario` **as a gate** | **P3b/P4b** | `ROADMAP.md:23`; P4a lands the drain list and builds the scenario as a recorded comparison (D-D4) |
| `ResolvedTextureBindingMemo` → `(shaderCso.slot, viewSetSerial)`, `SamplerPassMemo`, the image sweep, the program registry re-keys | **P3b/P4b** | `ROADMAP.md:23`. `ResolvedDrawBuffers` in that list **is already paid** (P3a, `MEASUREMENTS.md:431`) — do not re-scope it |
| nativising `g_rawDepthFetchSamplerState`, deleting the fragColor re-derivation workaround and `g_broadcastMemo*` | **P3b/P4b** | `ROADMAP.md:23`, `ARCHITECTURE.md:323` |
| the content-addressed sampler-**view** cache (4096) and the vertex-elements cache (1024) | **P7** | `ARCHITECTURE.md:63`, `:65`; D-F2 |
| Magma anything — placeholder textures, `SetupDrawSnapshot`'s probe fields, the baked internal shaders, `ProgramFactory` consuming the archive | **P7** | `ROADMAP.md:24`. **`MG_Backend/DirectVulkan/**` is untouched in P4a** |
| the texture-remint pull protocol (async pull, `MOBILEGL_PIPE_TEXEL_RETAIN_MB`, the `ResourceSubDataComplete` terminator, `TextureRemintPullScenario`) | **P9** | `ARCHITECTURE.md:299-308`, `ROADMAP.md:26`; P4a supplies mitigation 1 and the named Fatal (D-M) |
| emitting `create_shader_state` from the compile pool's terminal continuation | **P5** | `ARCHITECTURE.md:198`; D-H4 |
| the CopyImage shadow mirror moving to the client, `generate_mipmap`'s plan + CPU fallback texels, the index host mirror | **P8** | `ROADMAP.md:25` |
| fixing `create-indirect`, the `g_uploadRing` reset asymmetry, `IntegerBorderColorScenario`'s ASan finding, `ScopedDefaultUnpackState::s_synced`'s missing invalidation, rd12-on-Magma-on-Xiaomi | **`dev`** | `ROADMAP.md:98`, `:100`; ID-21, ID-23 |
---

## C. Packages

**Six packages on seven branches, seven WSL worktrees.** The split is chosen by the code, not by the roadmap's
sentence order: the payloads, the applier, the generated interfaces, the dirty/subsystem plumbing and the
enum-coupled block of `PipeFill.cpp` are **one shared contract** everything else compiles against; the client emitters
divide cleanly into a *framebuffer + resource* half and a *sampler + program* half whose `MG_State` directories do not
overlap; Espryt divides cleanly into the **twins and slot tables** (`Managers.{h,cpp}`, `SlotTables.h`) and the
**draw-path sync and binding walks** (`DirectGLES.cpp`, `Utils.{h,cpp}`); and the gates, scenarios, CI file and
measurement plumbing are cross-cutting tooling that must be able to land before the thing it gates.

**Branch points.** `p4a/contract` from `feat/disaggregated@37da3c3a`. Its single commit `c0` is the contract and is
**tagged**; `p4a/{clientfb, clientsp, esprytobj, esprytdraw, gates}` all branch from the **tag**, so their push builds
compile and link while A finishes. `p4a/wire` continues on from `c0`. **Branch refs are spelled
`refs/heads/feat/disaggregated` in every rebase** — the tag makes `p4a/contract` ambiguous (ID-8).

**Tree creation** (ID-3; the phase is a parameter):

```sh
bash ~/w7/notes/tools/wsl_tree.sh p4a contract    37da3c3a     verify   # first, and it must finish before the rest
bash ~/w7/notes/tools/wsl_tree.sh p4a wire        p4a/contract verify
bash ~/w7/notes/tools/wsl_tree.sh p4a clientfb    p4a/contract verify
bash ~/w7/notes/tools/wsl_tree.sh p4a clientsp    p4a/contract verify
bash ~/w7/notes/tools/wsl_tree.sh p4a esprytobj   p4a/contract
bash ~/w7/notes/tools/wsl_tree.sh p4a esprytdraw  p4a/contract
bash ~/w7/notes/tools/wsl_tree.sh p4a gates       p4a/contract verify
```

Each tree gets the same build directories, flags identical to `~/w7/pipe/build-linux` (the `COMMON` line of
`wsl_p3a_gate.sh`):

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
# benchmark (package F and the integrator only; DriverBench is Linux-desktop only)
cmake -S . -B build-bench  ... same ... -DMOBILEGL_BUILD_BENCHMARK=ON
```

`export CCACHE_BASEDIR=/home/swung/w7` in every shell. **A fresh worktree has `tools/trace_replay/fixtures` as
131/133-byte LFS pointers** and retrace against pointers reports `passed 2 / 79` with `ssim=None` — a **false red that
reads exactly like a total regression** (`MEASUREMENTS.md:413`). `wsl_tree.sh` copies the fixtures and hides them with
`git update-index --assume-unchanged`; a fixture left visible is pulled back to its pointer by a `rebase --abort`.

### C.0 Package A — `p4a/contract` then `p4a/wire`

**Commit `c0` — the contract. Nothing in it may change after the tag without telling the other five packages.**

| action | file | what |
|---|---|---|
| MODIFY | `MG_Pipe/MGPipeTypes.h` | D-A3 `enum MGPipeResourceTarget : Uint8` + the `TextureTarget` mapping table with **no `default:`** and its exhaustiveness `static_assert`; D-A4 the twelve `MGPipeBindBit` enumerators moved here from `ResourceTracker.h`; D-A2 the narrowed `MGPipeResourceRespecifyNeedsAck`; D-C2 `MGPFramebufferState::Pad0` → `Uint8 Target`; D-C3 `kMGPipeMaxColorAttachments = 8` + its `static_assert`; D-E1 `MGPTextureParams` 32 → **40** with `BuiltinSampler` and `SamplerResync`; D-G2 `kMGPipeMaxTextureUnits` / `kMGPipeMaxImageUnits` = 192 |
| MODIFY | `MG_Pipe/PipeFields.def` | `F(Target)` on `MGP_FIELDS_MGPFramebufferState`; `F(BuiltinSampler)`, `F(SamplerResync)` on `MGP_FIELDS_MGPTextureParams`; **new `MGP_FIELDS_SamplerParameters` + `P(SamplerParameters)` in `MGP_VERIFY_PAYLOAD_LIST`** (D-F1's padding trap) |
| MODIFY | `MG_Pipe/MGPipe.h` | D-K1 — bits 9/10/11/12 and `kMGPipeSubsystemsMigratedAtP4a = 0x1fff`; `0x7f` and `0x1ff` untouched |
| MODIFY | `MG_Pipe/MGPipeHandles.h` | nothing but a comment: the composite band's allocator entry point is named (D-H7) |
| MODIFY | `MG_Pipe/PipeApply.{h,cpp}` | D-J1's five new bounds and three new record types; `MGPipeResourceRecord`'s new members; `MGPipeApplierState`'s working-state members; `MGPipeApplierReset` / `MGPipeApplierReleaseObjectRecords` extended per D-J4; `RefusedObjectCalls`; the **eleven new apply entry points with complete signatures and stub bodies** so the other packages link: `…SetFramebufferState`, `…CreateSamplerState`, `…DeleteSamplerState`, `…CreateSamplerView`, `…DeleteSamplerView`, `…SetTextureParams`, `…SetSamplerViews`, `…BindSamplerStates`, `…SetShaderImages`, `…CreateShaderState`, `…BindShaderState`, `…DeleteShaderState`, `…SetDrawProgram`, `…SetDispatchProgram`, `…SetGlobalConstants`; and `void MGPipeUnmigratedEmulation(const char*)` with its no-op body (D-M). **`MGPipeResourceOps` is NOT touched** (D-B1) |
| MODIFY | `MG_Pipe/Coverage.def` | the complete P4a rows in `MGP_COVERAGE_EMITTED_LIST`, each with the `:183-197` paragraph written *for the commit that wires it*; **`GetPixelStoreParameters` stays absent** (`:170-181`) |
| MODIFY | `MG_Pipe/PipeMutation.h` | the six death-helper declarations (D-I1), so `MG_State` sees a declaration and never the client's tracker |
| MODIFY | `MG_Pipe/DirtySurface.def` | D-K5 — the four new rows, `MarkProgramForDeletion` → `kExplicitDestroy`, the updated reasons on the other two, and the header paragraph explaining why `Create*`/`Pop*` are deliberately not in the prefix set |
| MODIFY | `scripts/gen_pipe_dirty_surface.py` | D-K5 — `MUTATOR_PREFIXES` gains `Use` and `Bind`; `--check` keeps failing in both directions; `--self-test` gains one canned control per new row |
| MODIFY | `MG_Impl/Pipe/Tracker.h` | `kMGPipeDirtyEmittedAtP4a`; the seven `MGPipeSubsystemForDirty` arms; bit 11's shutter gains the READ slot's version (D-K4); the `:22-26` and `:57-84` comments corrected to say which bits are P4a's |
| MODIFY | `MG_Impl/Pipe/SetHashSuppressor.h` | append `SetFramebufferState` before `Count`; re-label `SetSamplerViews`/`BindSamplerStates`/`SetShaderImages` as **P4a - wired** and name P3b/P4b as the owner of the backend-debounce deletion (D-G1) |
| MODIFY | `MG_Impl/Pipe/SlotAllocator.{h,cpp}` | `AllocateComposite(lifetimeId)` — the one entry point into the reserved band, with the band's own exhaustion assert (D-H7) |
| MODIFY | `MG_Impl/Pipe/PipeFill.{h,cpp}` | **contract commit only**: `SubsystemForEmitter`'s new arms and the pairing `static_assert`s (**all seven compare against `SubsystemForEmitter`, never against a constant** — `:1016-1023`'s lesson); the validate-point ladder calling five emitters in `ARCHITECTURE.md:196`'s recommended order, **each a stub that emits nothing**; the six death-helper bodies (D-I1); `kMGPipeWiredSubsystems` written as an OR of four per-family constants each **defined in its family's emit header** and initialised to 0 there, so the bit is added by the commit that gives the emitters their bodies (`:1040-1070`'s rule) with no file touched twice; the `EmittedCallSuppliesTheWholeField` arms for P4a's accessors (D-J's note: they land in the **false** arm for the same pointer-storage reason as `GetBoundVertexArray`, `:1094-1115`, and that is decided *there*, deliberately, not by a `Coverage.def` row's presence) |
| CREATE | `MG_State/GLState/ProgramState/ProgramArtifactsCodec.{h,cpp}` | D-H2 — the archive serializer over the existing `VisitFields` tables, versioned and length-prefixed, with the `MGL_LINKARTIFACTS_SIZE` echo |
| MODIFY | `MG_State/GLState/ProgramState/ProgramArtifacts.h` | nothing but the comment that the serializer now exists; **the libc++ pin is the integrator's (D.5)** |
| MODIFY | `scripts/gen_pipe.py` | whatever the payload edits require; regenerate |
| MODIFY | `MG_Pipe/generated/*.inc` | regenerate and commit (all eight) |
| MODIFY | `MG_Util/Metrics/PipeStats.{h,cpp}` | D-L — five `CallClass` members inside the existing push block; **a new `#if MOBILEGL_PIPE_PUSH` block at the tail of `ByteClass`** carrying `CsoBlobBytes`; the name table and `FormatWindowLine` |
| MODIFY | `MG_Test/Util/PipeStatsTest.cpp` | pin the six new names in `CounterNamesAreStable` and `SummaryLineCarriesEveryClassAndGate` |
| MODIFY | `MG_Test/Pipe/PipeCatalogueTest.cpp` | the new sizes (`MGPTextureParams` 40); `ResourceRespecifyAcksOnlyImmutableStorage` gains the `glTexStorage2D` and `glRenderbufferStorage` idioms (D-A2); `EveryUnmigratedEmulationIsNamedOnce` (D-M) |
| CREATE | `MG_State/GLState/ProgramState/…` → `MG_Test/Program/ProgramArtifactsCodecTest.cpp` | D-H2's round-trip and its two negative controls |
| CREATE (stubs) | `MG_Test/Pipe/{FramebufferEmitTest, TextureEmitTest, SamplerEmitTest, ImageEmitTest, ProgramEmitTest, CompositeResolverTest}.cpp` | each a compiling file with one placeholder `TEST`, so the package that owns its contents never edits `MG_Test/Pipe/CMakeLists.txt` |
| MODIFY | `MG_Test/Pipe/CMakeLists.txt`, `MG_Test/Program/CMakeLists.txt` | register the seven new files, `LABELS unit` |
| MODIFY | root `CMakeLists.txt`, `MobileGL/Config.h`, `MobileGL/ConfigLoader.cpp` | the new client sources inside `if (MOBILEGL_PIPE_PUSH)`; the push default `0x1ff` → `0x1fff`; the bit-list comment |

**Ordered steps for `c0`**: payload edits first (build pull; confirm `symbol_report --threshold 0` is clean and every
`MGP_ASSERT_POD` holds) → `PipeFields.def` + generator + regenerate + `git diff --exit-code -- MobileGL/MG_Pipe/generated`
→ `MGPipe.h` / `Config` / `CMakeLists` → `PipeApply.{h,cpp}` with complete signatures and stub bodies →
`Tracker.h` / `SetHashSuppressor.h` / `SlotAllocator` → `PipeFill.{h,cpp}`'s enum-coupled block, stub emitters and the
six death helpers → `DirtySurface.def` + the scanner widening (`--check` and `--self-test` must both be green here) →
`ProgramArtifactsCodec` → `PipeStats` + the four test files. **Then tag `p4a/contract` and tell the other five
packages.**

Message: `[Feat] (Pipe): land the P4a contract - the resource target enum, the framebuffer target byte, the texture params' builtin sampler, the sampler-parameter field table, four subsystem bits, seven dirty arms and the program archive codec`

**Then `p4a/wire` continues** with the work only A can do:

- `w1`: the framebuffer and texture-resource apply entry points in full — `SetFramebufferState` (per-target storage,
  the `Target` validation, `Fbo`-in-hash), the `Desc.Target` branch in `ResourceCreate`/`Respecify`/`SubData`/`Destroy`
  (D-B1), `SetTextureParams`, the sub-data box/level/region validator that replaces `SubDataBoxFault`'s buffer-only
  refusals with a branch on `Target` (`PipeApply.cpp:510-531`), and the pending-upload accumulation of D-D5. Message:
  `[Feat] (Pipe): apply the framebuffer record per bound target and the texture and renderbuffer resource calls into their own slot-indexed records`
- `w2`: the sampler, view and image apply entry points — `CreateSamplerState`/`Delete…`,
  `CreateSamplerView`/`Delete…`, and the three `kVarTail` sets with their `Start + Count` bounds gate. Message:
  `[Feat] (Pipe): apply sampler states, sampler views and the three unit sets into the server's own working state`
- `w3`: the program apply entry points — `CreateShaderState` (with the **verify-only** codec round trip of D-H3),
  `BindShaderState`, `DeleteShaderState`, `SetDrawProgram`, `SetDispatchProgram`, `SetGlobalConstants`. Message:
  `[Feat] (Pipe): apply the shader CSO's artefacts and the default uniform block without ever re-linking on the server`
- `w4`: `MG_Test/Pipe/ResourceEmitTest.cpp` extensions and the applier-side cases of the six new test files — the
  record lifecycle per kind: a create marks the slot live and leaves `Serial` at 0; a respecify replaces the descriptor
  and bumps `Serial`; a destroy clears `Live` and a stale handle's `Gen` compare fails and **counts**; a var-tail
  window outside its bound is `Fatal{ProtocolCorruption}`; entries outside the declared window are not cleared;
  `MGPipeApplierReset` clears working state and advances its serials while object records survive. Message:
  `[Test] (Pipe): pin every P4a record's lifecycle, its bounds gate and what a make-current does and does not clear`

**Verification (`~/w7/p4a-wire`)**

```sh
python3 scripts/gen_pipe.py --check && python3 scripts/gen_pipe.py --self-test
python3 scripts/gen_pipe_dirty_surface.py --check && python3 scripts/gen_pipe_dirty_surface.py --self-test
python3 scripts/check_include_closure.py --mode both --compiler clang++-20 --self-test --require-all
cmake --build build-linux  --parallel 28 && ctest --test-dir build-linux  -L unit --no-tests=error --output-on-failure
python3 scripts/symbol_report.py --before ~/w7/p4a-before-libMobileGL.so --after build-linux/libMobileGL.so \
        --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0
#   expect added 0 / removed 0 / renamed 0 / resized 0 - P4a's admitted set is EMPTY (G1)
cmake --build build-push   --parallel 28 && ctest --test-dir build-push   -L unit --no-tests=error --output-on-failure
cmake --build build-verify --parallel 28 && ctest --test-dir build-verify -L unit --no-tests=error
bash scripts/p3a_untouched_regions.sh 37da3c3a HEAD          # P3a's eleven still unmoved
git diff --exit-code -- MobileGL/MG_Pipe/generated
```

### C.1 Package B — `p4a/clientfb` (framebuffer + texture/renderbuffer resources)

**Files**

| action | file | what |
|---|---|---|
| CREATE | `MG_Impl/Pipe/FramebufferEmit.h` | D-C — the `MGPSurface` builder (pure, per field), the `ReadSurface` resolution, `Complete`, the `Target` discrimination, the `ContentHash`, the suppressor call, and `kMGPipeWiredFramebufferSubsystem` |
| CREATE | `MG_Impl/Pipe/TextureEmit.h` | D-D — the `MGPResourceDesc` builder for every texture target and for renderbuffers, the sticky `BindMask`/`ImageBindableHint`, the drain list, the `MGPSubData` + `MGPSubRegion[]` builder with the level-shadow strides, and `kMGPipeWiredTextureSubsystem` |
| MODIFY | `MG_State/GLState/FramebufferState/**` | `FramebufferObject`'s destructor body (D-I1); no new members |
| MODIFY | `MG_State/GLState/TextureState/**` | `TextureObjectBase`'s virtual destructor body; `resource_create` from the constructor; `resource_respecify` from every storage definition; the drain-list append beside `MarkStorageDirty`/`MarkStorageDirtyRegion`; the emission-time flag clear (D-D5); `set_texture_params` from the thirteen `MGP_NOTE_AGGREGATE(TextureParams)` sites |
| MODIFY | `MG_State/GLState/RenderbufferState/**` | destructor body; `resource_create` from the constructor; **`resource_respecify` from `SetInternalFormat`/`AllocateStorage`/`SetSamples`** (D-D2) |
| MODIFY | `MG_Test/Pipe/{FramebufferEmitTest, TextureEmitTest}.cpp` | contents where A's `w4` did not already cover them |

**Ordered steps**

1. `b1`: `TextureEmit.h` + the `MG_State` texture/renderbuffer emission sites, **with the subsystem bit still absent
   from `kMGPipeWiredTextureSubsystem`**, so every dispatch still behaves exactly as before and the emission is proven
   semantically free before anything is switched over. Message:
   `[Feat] (Pipe, State): mint a {slot, gen} handle for every texture and renderbuffer and publish its create, respecify, parameters and accumulated sub-data as pipe calls`
2. `b2`: `FramebufferEmit.h` + the framebuffer emission at the validate point, per bound target. Message:
   `[Feat] (Pipe): push the bound framebuffers with a resolved read surface, inline attachment formats and a content hash that covers the draw-buffer array`
3. `b3`: flip the two wired-subsystem constants and land the two test files.

**Tests to add**

- `FramebufferEmitTest.TheResolvedReadSurfaceComesFromTheReadFramebuffersOwnReadBuffer` — and its shared-FBO case:
  one object bound to both targets emits one record with `Target = Both` whose `ReadSurface` is that object's.
- `FramebufferEmitTest.ADrawBufferChangeAloneStillMovesTheContentHash` — D-C4's fragColor-broadcast argument.
- `FramebufferEmitTest.ARecycledFramebufferHandleIsNeverSuppressedAgainstItsPredecessor`.
- `FramebufferEmitTest.EveryAttachmentFieldSurvivesTheSurfaceConversion` — **G6**, all 8 colour points plus depth and
  stencil, driven through the texture, the DSA-named, the renderbuffer and the layered entry points.
- `FramebufferEmitTest.AnAttachmentPointAboveTheWireWidthIsRefusedNotTruncated` — D-C3.
- `FramebufferEmitTest.ARestoragedAttachedRenderbufferPublishesItsNewExtent` — D-D2, and it is red before `b1`.
- `TextureEmitTest.EveryTextureTargetMapsToItsOwnResourceTarget` — D-A3's exhaustiveness, walked over every
  `TextureTarget`.
- `TextureEmitTest.EveryBindKindSetsItsBindMaskBit` and `…ABindMaskBitIsStickyAcrossARespecify` — D-A4, on create
  **and** on a following respecify.
- `TextureEmitTest.AnImageBoundTextureCarriesTheImageBindableHintForever`.
- `TextureEmitTest.TheUnionBoxAndTheRegionListDescribeTheSameTexels` — the invariant that makes D-D6's server-side
  choice safe: the rects' union equals the box, and every rect lies inside it.
- `TextureEmitTest.AScatteredUploadCarriesTheLevelShadowsStridesAndNotZero` and
  `…AWholeLevelUploadCarriesZeroStrides` — D-D3.
- `TextureEmitTest.MoreThanKMaxDirtyRectsCollapsesToTheBoxWithRegionCountZero`.
- `TextureEmitTest.AnUploadThroughAViewKeysOnTheStorageOwner` — D-D4.
- `TextureEmitTest.EveryTexturesParamsNameItsBuiltinSamplerCso` and
  `…TwoTexturesWithIdenticalSamplingShareOneBuiltinCso` — D-E1.
- `TextureEmitTest.ADestroyedTextureReleasesItsResourceViewAndBuiltinSamplerSlots` — D-I1's three-call destructor.

**Constraints**: B touches **no** file under `MG_Backend/`, no file under `MG_State/GLState/{SamplerState,
ProgramState}/**` or `Core.{h,cpp}` (C's), and none of A's `MG_Pipe` / `scripts` / `generated` files. It does not edit
`MG_IntegrationTest/**` or `.github/workflows/test.yml` (F's).

**Verification (`~/w7/p4a-clientfb`)**

```sh
python3 scripts/gen_pipe_dirty_surface.py --check && python3 scripts/gen_pipe.py --check
cmake --build build-linux --parallel 28 && python3 scripts/symbol_report.py \
  --before ~/w7/p4a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change
cmake --build build-push  --parallel 28 && ctest --test-dir build-push -L unit --no-tests=error --output-on-failure
ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8
MOBILEGL_PIPE_PUSH=0      ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8   # all-pull arm
MOBILEGL_PIPE_PUSH=0x1ff  ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8   # P4a subsystems off (G12)
cmake --build build-verify --parallel 28 && ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4
MOBILEGL_PIPE_VERIFY=1 python3 ~/w7/retrace_gate.py --tree ~/w7/p4a-clientfb --lib build-verify/libMobileGL.so \
  --out ~/w7/retrace-out/p4a-clientfb -j 4
grep -l 'Fatal{'         ~/w7/retrace-out/p4a-clientfb/*.log   # empty
grep -L 'MGPipe verify:' ~/w7/retrace-out/p4a-clientfb/*.log   # empty: every case armed
```

### C.2 Package C — `p4a/clientsp` (samplers, images, programs, composites)

**Files**

| action | file | what |
|---|---|---|
| CREATE | `MG_Impl/Pipe/SamplerEmit.h` | D-F1's content-addressed sampler CSO cache at capacity 256 with the **canonical zero-initialised copy**; D-F2's per-texture sampler view with its version-first skip; D-F3's client-side sampling resolution; `kMGPipeWiredSamplerSubsystem` |
| CREATE | `MG_Impl/Pipe/ImageEmit.h` | D-G2/D-G4 — the `MGPImageView` builder, the high-water early-out, the frontend-generation gate |
| CREATE | `MG_Impl/Pipe/ProgramEmit.h` | D-H — the `MGPProgramDesc` builder off `GetLinkedShaderSnapshot()`, the companion-pointer artefact hand-off, `set_draw/dispatch_program`, `set_global_constants` with the `~0u` sentinel guard; `kMGPipeWiredProgramSubsystem` |
| CREATE | `MG_Impl/Pipe/CompositeResolver.h` | D-H7 |
| MODIFY | `MG_State/GLState/SamplerState/**` | `SamplerObject`'s destructor body (D-I1) |
| MODIFY | `MG_State/GLState/ProgramState/**` | `ProgramObject`'s destructor body; the composite's release on pipeline-cache eviction |
| MODIFY | `MG_State/GLState/Core.{h,cpp}` | the `GetProgramForDraw`/`GetProgramForDispatch` hook the composite resolver observes — **and nothing else**; `UseProgram` is untouched (bit 6's shutter already sees it, D-K5) |
| MODIFY | `MG_Test/Pipe/{SamplerEmitTest, ImageEmitTest, ProgramEmitTest, CompositeResolverTest}.cpp` | contents |

**Ordered steps**

1. `c1`: `SamplerEmit.h` + `ImageEmit.h` + the `SamplerObject` destructor, wired constant still 0. Message:
   `[Feat] (Pipe): content-address sampler states, mint one sampler view per texture and push the three unit sets behind their own content hashes`
2. `c2`: `ProgramEmit.h` + the `ProgramObject` destructor + `set_global_constants`. Message:
   `[Feat] (Pipe): publish a program's per-stage SPIR-V and reflection archive as a shader CSO and its default uniform block as global constants`
3. `c3`: `CompositeResolver.h` + the band allocation + the two release paths. Message:
   `[Feat] (Pipe): resolve a program pipeline into one shader CSO out of the reserved composite band and release it exactly once`
4. `c4`: flip the two wired-subsystem constants and land the four test files.

**Tests to add**

- `SamplerEmitTest.TwoIdenticalSamplersShareOneCso`, `…ABorderColorFormChangeAloneMintsANewCso`,
  `…PaddingCannotChangeTheHash` (garbage written into the three trailing padding bytes through a byte pointer) — D-F1.
- `SamplerEmitTest.EverySamplerParameterFieldSurvivesTheBlobConversion` — **G6**, all fifteen members.
- `SamplerEmitTest.AViewIsReIssuedOnTheSameHandleWhenItsRestrictionsMove` and
  `…AnUnchangedTextureReIssuesNothing` — D-F2's version-first skip.
- `SamplerEmitTest.OnlyTheProgramResolvedUnitsAreEmitted` and `…AnUnchangedSetEmitsNothing` — D-G1/D-G2.
- `SamplerEmitTest.ARedundantRebindOfTheSameSamplerEmitsNothing` — the MC 26.2 idiom `MGPipeTypes.h:369-373` names.
- `ImageEmitTest.AZeroHighWaterMarkEmitsNothingWithoutHashing` — D-G4 property 1.
- `ImageEmitTest.AnAccessModeChangeAloneStillEmitsTheSet` and `…AnInternalFormatChangeAloneStillEmitsTheSet` — D-G3.
- `ProgramEmitTest.TheStageMaskComesFromTheLinkedSnapshotAndNotTheAttachList` — `ProgramObject.h:92-98`, `:121-133`.
- `ProgramEmitTest.TheNeverUploadedSentinelIsNeverEmitted` — D-H6.
- `ProgramEmitTest.TheEmitterJoinsAndTheTrackerDoesNot` — D-H4: a pending link must not be forced by `Update()`.
- `ProgramEmitTest.AReLinkReIssuesOnTheSameHandle`.
- `CompositeResolverTest.ACompositeIsMintedFromTheReservedBand`,
  `…TwoPipelinesWithTheSameSignatureShareOneComposite`,
  `…EvictionThenDestructionFreesTheSlotExactlyOnce` and its mirror order — D-H7.

**Constraints**: C touches **no** file under `MG_Backend/`, none of B's `MG_State` directories, none of A's files, and
none of F's.

**Verification (`~/w7/p4a-clientsp`)**: identical shape to C.1's, with
`--out ~/w7/retrace-out/p4a-clientsp` and `-R 'SamplerEmit\.|ImageEmit\.|ProgramEmit\.|CompositeResolver\.'` added to
the unit run.

### C.3 Package D — `p4a/esprytobj` (the twins and the slot tables)

**Files**: `MG_Backend/DirectGLES/{Managers.h, Managers.cpp, SlotTables.h}` and `MG_Test/SanityTest.cpp`.
**Nothing else.**

**Ordered steps**

1. `d1`: the five kinds' tables re-keyed onto client-minted handles through
   `BackendSlotTable::GetOrCreate(MGPipeHandle)` / `FindByHandle` / `ReleaseByHandle` (`SlotTables.h:278-382`), closing
   `SlotTables.h:70-77`'s recorded P3+ debt for **Texture, Renderbuffer, Framebuffer, SamplerCso** and **ShaderCso**,
   plus a **new sixth table for `SamplerViewCso`, which has no `TwinRegistry` today**. Both arms compile; nothing uses
   the new one yet. Note the `GetOrCreate(handle)` asymmetry: **forward generations recycle and reset, backward
   generations are REFUSED** (`SlotTables.h:301-321`). Message:
   `[Refactor] (Espryt): key the texture, renderbuffer, framebuffer, sampler and program twins on the client-minted handle instead of the frontend object`
2. `d2`: the texture twin reads the applier — `SyncMipmapsToBackend`'s cheap gate and `m_prevTextureInfo` probe
   re-keyed onto the resource record's `Serial` and `Desc`; the per-level upload loop reading the record's
   pending-upload set and its `MGPSubRegion` strides (`ARCHITECTURE.md:321`, **the one item moved out of the
   do-not-touch list**); `SyncTextureParamsToBackend` and `SyncBuiltinSamplerToBackend` reading `MGPTextureParams` and
   the `BuiltinSampler` CSO record. `StageBlocksIntoUnpackRing`, the two ring helpers and the three staging shapes are
   **byte-identical** (G5). Message:
   `[Refactor] (Espryt): drive texture storage, parameters and uploads from the pushed descriptors and take the upload strides from the record`
3. `d3`: the renderbuffer twin's four-field cache re-keyed onto the record; the `GL_OUT_OF_MEMORY` probe and its
   `RecordError` are unchanged (`Managers.cpp:10673-10683`). Message:
   `[Refactor] (Espryt): allocate renderbuffer storage from the pushed descriptor and keep the deferred out-of-memory report where it is`
4. `d4`: the framebuffer twin — `SyncToBackend` reading `MGPFramebufferState` per target, the draw-buffer array applied
   only for the DRAW target, `SyncReadBufferToBackend` reading `ReadSurface`, the four `g_fboSynced*` arrays replaced
   by one `FramebufferSerial` compare, and the four cross-object masks computed from the inline `InternalFormat`s.
   `RecomputeBackendColorSlots` is **byte-identical** (G5) and `g_attachmentBackendIdGeneration` and the re-entrancy
   loop survive. Message:
   `[Refactor] (Espryt): sync the driver framebuffer from the pushed record and answer the read buffer from the resolved read surface`
5. `d5`: the sampler and program twins — `BackendSamplerObject::SyncToBackend` reading the CSO record's
   `SamplerParameters` (all four border-colour comparands kept); `BackendProgramObjectImpl::SyncToBackend`'s read set
   answered from `MGPProgramDesc` and the archive, with the nine-clause rebuild condition's inputs moved and its clause
   count unchanged (D-H5). Message:
   `[Refactor] (Espryt): take the sampler parameters and the program artefacts from the applier instead of the frontend objects`
6. `d6`: `SanityTest.cpp` — `EverySwitchedOverKindResolvesItsTwinThroughTheHandleArm` (`:3878`) gains the five kinds
   plus `SamplerViewCso`; `EveryReKeyedObjectClassAnnouncesItsOwnDeath` (`:3711`) likewise;
   `ScopedDirectGLESTextureBindings` (`:218-260`, the **second live holder of kind Texture**) keeps working;
   `PixelPackBindingCacheSkipsRedundantBindsAndRestsAtZero` (`:2535`) passes unchanged.

**Must not break**: D-O's table, every row with its named test.

**Verification (`~/w7/p4a-esprytobj`)**

```sh
cmake --build build-linux --parallel 28 && python3 scripts/symbol_report.py \
  --before ~/w7/p4a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change
grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'          # empty (G13)
grep -n 'MG_State' MobileGL/MG_Pipe/PipeApply.h                    # no hit inside the MGPipeResourceOps block
bash scripts/p3a_untouched_regions.sh 37da3c3a HEAD                # P3a's eleven unmoved
bash scripts/p4a_untouched_regions.sh 37da3c3a HEAD                # once package F has landed it; before that, skip and say so
cmake --build build-push --parallel 28
ctest --test-dir build-push -L unit --no-tests=error --output-on-failure
ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8 -R DirectGLES
MOBILEGL_PIPE_PUSH=0     ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8 -R DirectGLES  # legacy
MOBILEGL_PIPE_PUSH=0x1ff ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8 -R DirectGLES  # G12
python3 ~/w7/retrace_gate.py --tree ~/w7/p4a-esprytobj --lib build-push/libMobileGL.so --out ~/w7/retrace-out/p4a-esprytobj -j 4
```

### C.4 Package E — `p4a/esprytdraw` (the draw-path sync and the binding walks)

**Files**: `MG_Backend/DirectGLES/{DirectGLES.cpp, Utils.h, Utils.cpp, MultiDraw.cpp}`. **Nothing else.**

**Ordered steps**

1. `e1`: `SyncCurrentFBO` / `BindCurrentFBO` / `ForceBindCurrentFBO` / `SyncAndBindFramebufferObject` driven from the
   applier's two framebuffer records — the four-part `StampSyncedFBO` memo replaced by one serial compare, the
   "same FBO as draw" skip replaced by the `Target` discrimination, `GetFramebufferBindingSlotChecked`'s five call
   sites reading the record. **`BindCurrentFBO` keeps having no fast path on the slot version** (`:3211-3216`) and the
   **default-FBO bind still goes through the shadow** (`:3253-3259`). Message:
   `[Refactor] (Espryt): resolve the current framebuffers from the pushed records and keep the shadowed default bind and the missing fast path exactly as they are`
2. `e2`: the texture sync work lists — `SyncNeccessaryTextures`' unit list and FBO list rebuilt from the applier's
   `SamplerViews` set and `DrawFramebuffer` record, `UnitBindingsSnapshot`/`PairingsIntact` kept under
   `MOBILEGL_PIPE_LEGACY_MEMOS` (their deletion is P3b/P4b's), `SyncTextureObjectToBackend` taking a handle.
   **`SyncAttachmentObject` gains the parameter sync for READ-only attachments** (D-E3). Message:
   `[Refactor] (Espryt): drive the per-draw texture sync from the pushed sampler-view set and give a read-only attachment its texture parameters`
3. `e3`: the image units — `SyncImageTextureBinding` reading `MGPImageView`, the high-water early-out and the
   frontend-generation gate preserved verbatim (D-G4), the bind-format recast and the buffer split view unchanged.
   Message:
   `[Refactor] (Espryt): bind shader images from the pushed set and leave the format recast and the split view where they are`
4. `e4`: the sampler and program draw paths — `BindCurrentUnitSamplers`' two stacked memos re-derived over the
   applier's per-unit handle array (the 192-row `memcmp` replay proof becomes a serial compare),
   `SamplerPassMemo`'s raw `BackendSamplerObject*` rows becoming handles, `BindCurrentProgramWithResources` reading the
   `ShaderCso` record and `MGPGlobalConstants`, and the **raw-depth-fetch substitution kept on the server** acting on
   the resolved set (D-F3). The UBO ring is untouched. Message:
   `[Refactor] (Espryt): bind samplers and the program's resources from the applier's resolved sets and keep the raw-depth-fetch substitution on the server`
5. `e5`: the five `MGPipeUnmigratedEmulation` call sites (D-M) and the `Utils.{h,cpp}` readers that take a descriptor
   instead of a frontend object. `ShouldUseCaveatTextureFormat` and `DepthStencilSamplingReadImpl` are
   **byte-identical** (G5). Message:
   `[Refactor] (Espryt): name every emulation that cannot survive a split and take the format decisions from the descriptor`

**Must not break**: D-O's table.

**Verification (`~/w7/p4a-esprytdraw`)**: as C.3's, plus the family subset

```sh
ctest --test-dir build-push -L integration-gpu --no-tests=error -j 4 \
  -R 'LayeredAttachmentShape|LayeredTextureReadback|LayeredAttachmentBarrier|ThreeChannelAttachment|SnormAttachment|RenderbufferBlendFormat|DepthStencilReadback|PackedWordReadback|PixelStoreSweep|ClearThenReadPixels|ClearTexImageUndefinedLevelZero|CopyImageLevelRange|CopyImageLayered|CopyImagePacked16|TextureView|BufferTexture|ImageSizeAfterRespec|SampledSetStaleness|ImageTargetKind|ImageLoadStoreSso|NonCoreImageFormat|FormatlessImageBake|ImageFormatQualifier|UnboundImageDescriptor|IntegerBorderColor|FragmentOutputArrayIndex|Orientation|FragCoordOrigin|ProgramPipeline|PostLinkAttach|RelinkStageSet|AsyncCompile|PipelineFailure|SpirvShaderBinary|UniformInitializer|Glsl420Declaration|IoBlockNameCollision|UnlocatedIoBlock|PointSizeDemotion|TessellationDrawMode|GeometryDrawMode'
```

### C.5 Package F — `p4a/gates`

Cross-cutting tooling. F branches from the contract and integrates **last**, but writes the extended
`HandleRecycleScenario` arms and `TextureParamsWithoutASamplerViewScenario` **first**, so their red is proven before D
and E exist — that is the "重键前红" and "落地前必须红" evidence, recorded with its run log.

| action | file | what |
|---|---|---|
| MODIFY | `MG_IntegrationTest/Harness/PipeSlotPeek.{h,cpp}` | `PipeSlotKind` gains `Texture, Renderbuffer, Framebuffer, SamplerCso, SamplerViewCso, ShaderCso`; a peek that cannot look still returns **false, touching nothing**, and a caller that gets false **SKIPs** |
| MODIFY | `MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp` | **G8** — `armExpectsCorruption=true` ABA controls for all six kinds; the existing texture (`:869`) and framebuffer (`:920`) cases become real controls (they are not today, `:865-868`); **G8b** — one `…ReturnTheirSlots` leak case per kind, including a dedicated composite case |
| MODIFY | `MG_Backend/DirectVulkan/Renderer/MagmaPipeArms.h` | **the ONLY DirectVulkan file P4a touches** — `MagmaPipeAbaControlDefeatsIdentity` extended so the ABA knob defeats the identity half of P4a's memo keys on the Magma side too (ID-1's precedent) |
| CREATE | `MG_IntegrationTest/Scenarios/TextureParamsWithoutASamplerViewScenario.cpp` | **G9** — D-E3's four cases |
| CREATE | `MG_IntegrationTest/Scenarios/TextureUploadShapeScenario.cpp` | D-D4 — the per-texture per-frame upload shape (box vs N regions, job count) recorded against a gold standard. **Built here, run as a RECORDED comparison in P4a, promoted to a gate in P3b/P4b** with the Mali frame-time delta published |
| CREATE | `MG_IntegrationTest/Scenarios/ObjectSubsystemControlScenario.cpp` | **G12** — the P4a analogue of `ResourceSubsystemControlScenario`: `0x1fff` vs `0x1ff` asserting the emission counters move and the pixels do not, plus `ASamplerBitWithoutTheTextureBitIsRefusedAndNamed` at `0x9ff` |
| MODIFY | `MG_IntegrationTest/CMakeLists.txt` | register the three new scenarios; **raise every `0x1ff` phase pin to `0x1fff` and the CSO-off pin to `0x8000000000001fff`** (`:949, 1065, 1070, 1075, 1080, 1149, 1183, 1188, 1219`), leaving `:1154` and `:1222`'s `0x7f` alone and adding the new `0x1ff` off-lanes; every environment joined through `mgl_itest_join_environment(... ${MGL_ITEST_COMMON_ENV})` — **ctest `ENVIRONMENT` replaces rather than appends, `;` must be escaped, and the property overrides the job env** (`ARCHITECTURE.md:577`); the SKIP-warning precedent so a configuration that cannot run an arm says so |
| CREATE | `scripts/p4a_untouched_regions.sh` | **G5** — D-N's seventeen functions across three files, `--self-test` with four negative controls |
| CREATE | `scripts/p4a_descriptor_negative_control.sh` | **G7** — two field drops (`MGPSurface::Layered` in `FramebufferEmit.h`, `SamplerParameters::borderColorForm` in `SamplerEmit.h`), each expected to turn its suite red **naming that field**; patched by regex, not by line number, because the headers belong to other packages; **every exit goes through `repair()` (restore, rebuild, re-run), which also runs from the `EXIT` trap**, and a failed repair downgrades the verdict to 2; logs under `<build-dir>/p4a-g7-logs/`; not a CI lane, because it rebuilds the library twice |
| MODIFY | `.github/workflows/test.yml` | `pipe-gates` gains `p4a_untouched_regions.sh $BASELINE HEAD` and its `--self-test`, under the same `feat/disaggregated`-or-dispatch guard the P3a step carries; the three new scenario entries; `BASELINE` → `37da3c3a` |
| CREATE | `~/w7/notes/tools/wsl_p4a_gate.sh` | D.3 — `wsl_p3a_gate.sh` with the delta D.3 lists, **and no other change** |
| MODIFY | `~/w7/notes/tools/p3a_ab.sh` → `p4a_ab.sh`, `p3a_ab_7f.sh` → `p4a_ab_1ff.sh`, `wsl_p3a_bench.sh` → `wsl_p4a_bench.sh`, `ab_reduce3.py` | D.4 — the mask constants only; **every other line verbatim** (reboot-clean, the three-try pin with `check` after each and exit 43, the `mkdir` device-lock mutex with its 900 s timeout and exit 42, the `trap … EXIT` unpin, `--benchmark-repeats 3 --benchmark-tail-frames 200`, the `AB_DONE` terminator, the "do not export `MSYS_NO_PATHCONV` globally" note) |

Messages: `[Test] (Pipe): reproduce the texture, framebuffer, renderbuffer, sampler, view and program handle ABA through public GL and prove the pre-rekey guards are what stops it`;
`[Test] (Espryt): assert a glTexParameter on a texture that only ever was an attachment, an image binding or a copy endpoint reaches the driver`;
`[CI] (Pipe): gate that the unpack ring, the attachment permutation, the depth-stencil sampling core and the format caveat did not move`

**Verification (`~/w7/p4a-gates`)**

```sh
cmake --build build-verify --parallel 28
ctest --test-dir build-verify -R HandleRecycle --no-tests=error --output-on-failure
#   on the contract tree, before D/E land: .Legacy and .AbaControl pass, .Handles SKIPs with
#   "subsystem not implemented on this tree" - a visible skip, never a vanishing test
ctest --test-dir build-push -R 'TextureParamsWithoutASamplerView' --no-tests=error --output-on-failure
#   AReadAttachmentOnlyTexturesDepthStencilModeReachesTheDriver must be RED here (G9)
bash scripts/p4a_untouched_regions.sh --self-test          # rc 0, four negative controls
bash scripts/p4a_descriptor_negative_control.sh build-push # must report "tripped, named Layered" and "tripped, named borderColorForm"
cmake --build build-bench --parallel 28 && ctest --test-dir build-bench -L benchmark --no-tests=error
```

### C.6 What each package may assume of the others

- **B and C assume** A's `MGPipeApply*` signatures, the payload edits (`MGPTextureParams` 40, `MGPFramebufferState::
  Target`, `MGPipeResourceTarget`, `MGPipeBindBit`'s new home, the two unit bounds), the four subsystem bits, the seven
  dirty arms, the `SetHashSuppressor` slot, `AllocateComposite`, the six death-helper declarations and
  `MGPipeUnmigratedEmulation`. They assume **nothing consumes the records**, so the tree is behaviourally unchanged
  and each is landable on its own. **That is what makes B and C the safety net: if an emission is wrong, the verify
  lane says so before any backend has been touched.**
- **D assumes** the same contract plus that **the applier's records are populated**, which is only true after B and C
  land; D is written against the contract and rebased onto them.
- **E assumes** D's twin API in addition.
- **F assumes** all of it, and its two red-before scenarios are written and proven red on the contract tree first.

### C.7 File-ownership table

No two packages edit the same file **after the tag**. The contract is one commit that all five other branches descend
from, so the integrator's rebase never sees an edit on both sides of a file.

**Generated and `.def` interfaces are owned artefacts and are listed as such**, because of the semantic-merge trap
`Coverage.def:484-494` documents from commit `bb2a236d`: the spans work removed `GetPixelStoreParameters` from the
emitted list, so the generated `MGPipeFieldEmitter` no longer had a `SetPixelPackState` enumerator — but
`PipeFill.cpp`'s `SubsystemForEmitter` switch, written against the contract, still named it, and **the push and verify
builds did not compile on the integrated tree**. Green on two branches separately is not green on the merge.

**The rule this imposes on P4a**: *a package that retires a `.def` row must not leave another package referencing the
generated enumerator*, and it is discharged **structurally** — **A owns `Coverage.def` AND the whole of
`PipeFill.{h,cpp}` and `Tracker.h` for the entire phase**, and the only thing B and C add to the wiring is the value of
a per-family constant **defined in their own headers**. A mistake is a compile error at `c0`, not at the merge.

| file / glob | A contract+wire | B clientfb | C clientsp | D esprytobj | E esprytdraw | F gates |
|---|---|---|---|---|---|---|
| `MG_Pipe/{MGPipeTypes.h, MGPipeValueTypes.h, MGPipeHandles.h, MGPipe.h, PipeApply.{h,cpp}, PipeMutation.h}` | **owner** | – | – | – | – | – |
| `MG_Pipe/PipeCalls.def` *(generated interface: opcodes + flag words)* | **owner** (unchanged in P4a; listed so nobody edits it) | – | – | – | – | – |
| `MG_Pipe/{PipeFields.def, Coverage.def, DirtySurface.def, FillPoints.def}` *(generated interfaces)* | **owner** | – | – | – | – | – |
| `MG_Pipe/generated/*.inc` *(all eight, committed; CI diffs them)* | **owner** | – | – | – | – | – |
| `scripts/{gen_pipe.py, gen_pipe_dirty_surface.py}` | **owner** | – | – | – | – | – |
| `MG_Impl/Pipe/{PipeFill.{h,cpp}, Tracker.h, SetHashSuppressor.h, SlotAllocator.{h,cpp}, CsoCache.h, ResourceTracker.h, VertexInputEmit.h}` | **owner** | – | – | – | – | – |
| `MG_Impl/Pipe/{FramebufferEmit.h, TextureEmit.h}` | – | **owner** | – | – | – | – |
| `MG_Impl/Pipe/{SamplerEmit.h, ImageEmit.h, ProgramEmit.h, CompositeResolver.h}` | – | – | **owner** | – | – | – |
| `MG_State/GLState/{FramebufferState, TextureState, RenderbufferState}/**` | – | **owner** | – | – | – | – |
| `MG_State/GLState/{SamplerState, ProgramState}/**`, `MG_State/GLState/Core.{h,cpp}` | **owner (contract commit only, for `ProgramArtifactsCodec`)** | – | **owner (after the tag)** | – | – | – |
| `MG_State/GLState/BufferState/**`, `VertexArrayState/**` | – | – | – | – | – | – (**unchanged in P4a**) |
| `MG_Backend/DirectGLES/{Managers.h, Managers.cpp, SlotTables.h}` | – | – | – | **owner** | – | – |
| `MG_Backend/DirectGLES/{DirectGLES.cpp, Utils.h, Utils.cpp, MultiDraw.cpp}` | – | – | – | – | **owner** | – |
| `MG_Backend/DirectGLES/**` (everything else, incl. `BackendObject_DirectGLES.cpp`) | – | – | – | **owner** | – | – |
| `MG_Backend/DirectVulkan/**` | – | – | – | – | – | **owner of `Renderer/MagmaPipeArms.h` ONLY**; everything else untouched |
| `MG_Backend/MGPipe/PipeInputs.h` | **owner** (unchanged in P4a; listed so nobody adds a `SharedPtr` member — ID-18) | – | – | – | – | – |
| `MG_Util/Metrics/PipeStats.{h,cpp}`, `MG_Test/Util/PipeStatsTest.cpp` | **owner** | – | – | – | – | – |
| root `CMakeLists.txt`, `MobileGL/Config.h`, `MobileGL/ConfigLoader.cpp` | **owner** | – | – | – | – | – |
| `MG_Test/Pipe/CMakeLists.txt`, `MG_Test/Program/CMakeLists.txt`, and the seven stubs' creation | **owner** | – | – | – | – | – |
| `MG_Test/Pipe/{FramebufferEmitTest, TextureEmitTest}.cpp` contents | **owner** (`w4`) | **owner** (`b3`, disjoint cases) | – | – | – | – |
| `MG_Test/Pipe/{SamplerEmitTest, ImageEmitTest, ProgramEmitTest, CompositeResolverTest}.cpp` contents | **owner** (`w4`) | – | **owner** (`c4`, disjoint cases) | – | – | – |
| `MG_Test/Pipe/{PipeCatalogueTest, ResourceEmitTest}.cpp` | **owner** | – | – | – | – | – |
| `MG_Test/Program/{ProgramArtifactsCodecTest, ProgramArtifactsTest}.cpp` | **owner** | – | – | – | – | – |
| `MG_Test/SanityTest.cpp` | – | – | – | **owner** | – | – |
| `MG_Test/{Framebuffer, Texture, State}/**` | – | – | – | – | – | – (**granted per request, see below**) |
| `MG_Impl/GLImpl/**` | – | – | – | – | – | – (**granted per request, see below**) |
| `MG_IntegrationTest/**`, `.github/workflows/test.yml`, `tools/**`, `android-plugin/**`, `MG_Benchmark/**` | – | – | – | – | – | **owner** |
| `docs/Disaggregated/*.md` | – | – | – | – | – | – (integrator only) |
| everything else | nobody | nobody | nobody | nobody | nobody | nobody |

**Explicit grants, because C.7 gives these files to nobody by default and P4a needs them** (ID-10's precedent, which
had to be granted mid-phase because the brief had not):

| file | granted to | exactly what, and nothing else |
|---|---|---|
| `MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.cpp` | **B** | nothing today — the renderbuffer respecify emission hangs off `RenderbufferObject`'s own mutators (D-D2). **If an implementer finds a storage path that does not go through them, the grant is one call site per path and it is recorded as a deviation.** |
| `MG_Impl/GLImpl/Texture/GL_Texture.cpp` | **B** | same rule: emissions hang off `TextureObjectBase`/`MipmapStorage` mutators. The one anticipated exception is the generated-mip storage grow at `:528-539`, which calls `AllocateStorage` + `MarkStorageDirty(false)` + `TruncateMipmapLevels` + `BumpContentVersion()` **from the GLImpl side**; B is granted that one respecify emission |
| `MG_Impl/GLImpl/Program/GL_Program.cpp` | **C** | nothing today — `create_shader_state` is emitted from the validate point (D-H4). Same deviation rule |
| `MG_Test/Framebuffer/FramebufferTest.cpp` | **B** | additions only, and only if a payload edit breaks a case |
| `MG_Test/Texture/{TextureTest, TextureViewTest}.cpp` | **B** | additions only. **ID-8's collision warning applies**: P3a's wave-1/wave-2 both appended at the same point in `TextureTest.cpp` and collided; resolve by **union**, keep every test name from both sides, never choose a side |
| `MG_Test/State/ObjectLifetimeIdTest.cpp` | **B** | extend `ProbeLifetimeIdAcrossAddressReuse` to `TextureObject2D`, `FramebufferObject`, `SamplerObject` and `ProgramObject` — it covers only Buffer/Renderbuffer/VertexArray today (`:34-36`) and all four now have ids. **The cheapest test win in the phase** |
| `MG_Test/Buffer/BufferTest.cpp` | **D** | **ID-14's shape, applied forward**: any fixture that scopes one backend op table must scope every table the push build installs at bring-up. P4a installs no new table (D-B1), so this is a **read-and-confirm**, not an edit — but if D adds a registration, the fixture is granted to it |

The one file two packages both *touch conceptually* is each `*EmitTest.cpp`: A's `w4` writes the applier-side cases,
B/C's last commit writes the emitter-side cases. **They are disjoint `TEST` bodies in one file and A lands first**, so
the rebase is an append; a collision is resolved by union, never by choosing a side.
---

## D. Integration order, and what the integrator runs

### D.0 Preconditions

- The §A baseline captures (`~/w7/p4a-before-libMobileGL.so`, `~/w7/p4a-before-ctest-names.txt`,
  `~/w7/p4a-before-untouched-p3a.sha`, `~/w7/p4a-before-dirty-surface.txt`, `~/w7/p4a-before-syncrenderstate.sha`),
  taken in `~/w7/pipe` at `37da3c3a` **before any package lands**.
- `~/w7/pipe` clean at `37da3c3a`; fixture binaries hidden the way `wsl_tree.sh` does it
  (`git update-index --assume-unchanged` on `tools/trace_replay/fixtures`) — **a fixture left visible gets pulled back
  to its LFS pointer by a `rebase --abort`, and retrace against pointers is a false red that reads like a total
  regression** (`MEASUREMENTS.md:413`).
- `~/w7/retrace_gate.py` present (flags: `--tree --lib --out -j --only`; **no `--ssim`, no `--backend`**; `--only` is a
  **regex**).
- Both devices reachable and **not** locked by the GL4.6 Wave-7 campaign
  (`ls .../8ab6b3cb-.../scratchpad/w7/locks/`).
- A worktree created for a package must have its submodules initialised and `3rdparty/glslang/External` copied, or the
  build fails in a way that looks like a code error.
- **`$BASE` is restated in `INTEGRATOR-DECISIONS.md` the moment `dev` is merged or a version bump lands on the branch**
  (ID-1, ID-8d). Every package that has not yet branched takes the new ref; every package that has re-opens the files
  it edits.

### D.1 Order: **contract → wire → clientfb → clientsp → esprytobj → esprytdraw → gates**

Reason, and it is ID-2's order generalised to six packages:

- **contract** first because everything else compiles against it, and because its `PipeFill.cpp` / `Tracker.h` block is
  what makes the generated-enum coupling a compile error at `c0` rather than at a merge (C.7).
- **wire** second: the applier bodies must exist before anything emits into them. Wire is behaviour-neutral on its own
  (nothing emits until a client package flips its wired-subsystem constant), so it is always-green.
- **clientfb** and **clientsp** third and fourth, in either order, because **they are the only packages that can land
  without changing behaviour**: with the wired constants still 0 the fields keep being pulled and the tree behaves
  exactly as before. If an emission is wrong, the verify lane says so before any backend has been touched — P3a's `b2`
  logic, applied to a phase whose backend half is the risky one.
- **esprytobj** fifth: the twins and the tables. It is the commit family that switches the path over.
- **esprytdraw** sixth: the draw-path walks, against twins whose API is already integrated.
- **gates** last, so its lanes assert against the finished tree — except the two red-before scenarios, whose red is
  recorded on the contract tree at D.2.

**Every backend package gets a real-path verification round on the INTEGRATED tree before its final review.** This is
not optional and it is not a formality: P3a's espryt-v3 round *"found and fixed three seam defects"* — a vertex buffer
resolved by GL binding point instead of attribute index, a stale descriptor consulted for the shadow on a full
re-upload, and the shadow base not published for a lazily twinned store — none of which any package's own tree could
see (ID-8e). For P4a that means: after `clientsp` integrates, **esprytobj** runs its full verification list on
`~/w7/pipe`'s tree, not on its own; after `esprytobj` integrates, **esprytdraw** does the same. Then each gets a
focused re-review.

Per slug: rebase `p4a/<slug>` onto `refs/heads/feat/disaggregated` in its own worktree, `git merge --ff-only` in
`~/w7/pipe`, then **after every merge**:

```sh
cmake --build build-linux --parallel 28 && ctest --test-dir build-linux -L unit --no-tests=error --output-on-failure
cmake --build build-push  --parallel 28   # push AND verify, both, every time - that is where bb2a236d surfaced
cmake --build build-verify --parallel 28
python3 scripts/gen_pipe.py --check && python3 scripts/gen_pipe.py --self-test
python3 scripts/gen_pipe_dirty_surface.py --check && python3 scripts/gen_pipe_dirty_surface.py --self-test
python3 scripts/check_include_closure.py --mode both --compiler clang++-20 --self-test --require-all
python3 scripts/symbol_report.py --before ~/w7/p4a-before-libMobileGL.so --after build-linux/libMobileGL.so \
        --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0 --markdown ~/w7/p4a-symbol-<slug>.md
#   0 added / 0 removed / 0 renamed / 0 RESIZED after every merge. P4a's admitted set is EMPTY (G1).
grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'                        # empty
bash scripts/p3a_untouched_regions.sh 37da3c3a HEAD                              # P3a's eleven, after EVERY merge
bash scripts/p4a_untouched_regions.sh 37da3c3a HEAD                              # from `gates` onward
ctest --test-dir build-linux -N | grep -E "^[[:space:]]*Test[[:space:]]+#[0-9]+:" \
  | sed -E "s/^ *Test +#[0-9]+: //" | LC_ALL=C sort | LC_ALL=C comm -23 ~/w7/p4a-before-ctest-names.txt -   # empty
```

**Any symbol resize outside an admitted set is a defect, attributed per symbol in the commit message, never waved
through** (ID-3). P4a's admitted set is empty; a resize means an edit escaped a `#if MOBILEGL_PIPE_PUSH` guard (D-P).

**A final whole-diff review of `37da3c3a..HEAD` runs after every package has integrated and before the docs commit.**
P3a's found two cross-package seam defects that four passing package reviews had not (`MEASUREMENTS.md:507`): the
client-minted CSO slot with no backend-neutral death path, and a function whose two preprocessor arms broke the
byte-identity gate's own extractor. Its rework lands as the last P4a commits on a `p4a/final` branch cut from the
integrated head, and the device A/B and the bench are recorded against the **pre-rework** APKs when the rework is
lifecycle/seam correction rather than a cost change — with the docs saying so (ID-15's precedent).

### D.2 On the contract tree, before `clientfb`: record the "red before" evidence

```sh
cd ~/w7/p4a-gates && cmake --build build-verify --parallel 28 && cmake --build build-push --parallel 28
ctest --test-dir build-verify -R HandleRecycle --no-tests=error --output-on-failure | tee ~/w7/p4a-handlerecycle-before.log
#   for each of the six new kinds: .AbaControl PASSES (it asserts the corrupted contents),
#   .Legacy PASSES, .Handles SKIPs with a stated reason - a visible skip, never a vanishing test
ctest --test-dir build-push -R 'TextureParamsWithoutASamplerView' --no-tests=error --output-on-failure \
  | tee ~/w7/p4a-texparams-before.log
#   AReadAttachmentOnlyTexturesDepthStencilModeReachesTheDriver must be RED; the other three GREEN
```

Both logs are the artefacts `ROADMAP.md:20` asks for — the first for "重键前红" per kind, the second for the mandatory
scenario's "落地前必须红". Keep them; they go into `MEASUREMENTS.md`.

### D.3 After every package has landed: the five-part gate on `~/w7/pipe`

`~/w7/notes/tools/wsl_p4a_gate.sh` is `wsl_p3a_gate.sh` with **these changes and no others**:

- `p3a`→`p4a` in every path and in the `P3A_GATE_DONE` terminator (→ `P4A_GATE_DONE`).
- `BASE=44c2b5cf` → **`BASE=37da3c3a`**.
- **The fail-fast rule is kept verbatim**: the three builds are made first and a non-zero rc prints
  `BUILD FAILED - gate aborted (no stale-binary verdicts)` and exits 1. *Results from a stale binary must never look
  like a verdict.*
- `FAMILY` is replaced by P4a's framebuffer/texture/program family regex (C.4's list).
- `NAMED` gains `photon-v1.3b` (G3b) and keeps the `# retrace_gate.py --only is a regex` comment.
- The G5 line becomes **two** lines: `bash scripts/p3a_untouched_regions.sh $BASE HEAD` (P3a's eleven still true) and
  `bash scripts/p4a_untouched_regions.sh $BASE HEAD` (P4a's six), each with its own rc echo; the part-5 self-tests
  likewise become two.
- Part 5's `ctest -R 'RenderStateSpans\.|Residual|VertexInputEmit\.|ResourceEmit\.'` gains
  `|FramebufferEmit\.|TextureEmit\.|SamplerEmit\.|ImageEmit\.|ProgramEmit\.|CompositeResolver\.` (G6).
- Part 3's five `integration-gpu` arms become **six**: default (`0x1fff`), `MOBILEGL_PIPE_PUSH=0`,
  `MOBILEGL_PIPE_PUSH=0x1ff` (**P4a's subsystems off**), `MOBILEGL_PIPE_PUSH=0x7f` (P2's boundary, kept as the
  second-order control), `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1`, and the family subset.
- Part 3's negative controls become **three**: `g7_negative_control.sh`, `p3a_vertex_input_negative_control.sh` and
  `p4a_descriptor_negative_control.sh`.
- `CsoContentAddressing|ResourceSubsystemControl` becomes
  `CsoContentAddressing|ResourceSubsystemControl|ObjectSubsystemControl` (G12), and the dependency-refusal arm is run
  separately: `MOBILEGL_PIPE_PUSH=0x9ff ctest -R 'ObjectSubsystemControl'`.
- Part 3 gains `ctest -R 'TextureParamsWithoutASamplerView'` (G9) and `-R 'TextureUploadShape'` (recorded, not gated).
- ID-4's ctest-name extractor and the arming-line greps are kept **verbatim**.
- `GLIBC_TUNABLES=glibc.malloc.tcache_count=0` is exported for the `integration-verify` and push `integration-gpu`
  runs (ID-18/ID-21: it is what makes an exit-order UAF reproduce natively instead of only in CI's allocator layout).

**Part 1 — interface purity.**
```sh
python3 scripts/check_include_closure.py --mode both --compiler clang++-20 --self-test --require-all   # gate A
nm --undefined-only build-linux/libMobileGL.so | grep -E 'MG_State::GLState::|glslang'                 # gate B (n/a in monolith; recorded)
grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'                                              # gate C: empty
grep -n 'MG_State' MobileGL/MG_Pipe/PipeApply.h                                                        # no hit in the ops block (G13)
grep -rc 'MGPipeUnmigratedEmulation' MobileGL/MG_Backend/DirectGLES/                                   # == the pinned count (G13b)
ctest --test-dir build-verify -R HandleRecycle --no-tests=error --output-on-failure                    # G8, G8b
python3 scripts/symbol_report.py --before ~/w7/p4a-before-libMobileGL.so --after build-linux/libMobileGL.so \
        --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0                              # G1
bash scripts/p3a_untouched_regions.sh 37da3c3a HEAD && bash scripts/p4a_untouched_regions.sh 37da3c3a HEAD  # G5
```

**Part 2 — the semantic shadow comparison (the decisive one).**
```sh
GLIBC_TUNABLES=glibc.malloc.tcache_count=0 ctest --test-dir build-verify -L unit --no-tests=error --output-on-failure
GLIBC_TUNABLES=glibc.malloc.tcache_count=0 ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4 --output-on-failure
MOBILEGL_PIPE_VERIFY=1 python3 ~/w7/retrace_gate.py --tree ~/w7/pipe \
  --lib ~/w7/pipe/build-verify/libMobileGL.so --out ~/w7/retrace-out/p4a-verify -j 4
grep -L 'MGPipe verify:' ~/w7/retrace-out/p4a-verify/*.log        # empty: every case armed
grep -l 'Fatal{'         ~/w7/retrace-out/p4a-verify/*.log        # empty
```
A `Fatal{UnmigratedPipeInput, "F@V"}` is a missing `FillPoints.def` row — add the row, **never a sticky row**
(`Coverage.def:144-151`: none of the version/generation accessors is sticky and `gen_pipe.py` refuses a name that is
not an accessor), regenerate, rerun. A `Fatal{PipeVerifyDiffer, …, where=read}` is a real push/pull divergence and
names its field; it is fixed, never silenced. A `Fatal{PipeVerifyDiffer, "program-archive"}` is D-H3's codec round trip
failing. A `Fatal{ProtocolCorruption}` is a malformed record — a `Start + Count` outside 192, a texture sub-data box
outside its level, a `create_sampler_view` naming a texture with no record, or a slot at or above its bound.

**Part 3 — behavioural A/B.** As above, plus the three retrace sweeps (verify / push / named).

**Part 4 — performance. RECORDED, NOT GATED (rule (a)).** D.4.

**Part 5 — coverage, poison, handle discipline.**
```sh
python3 scripts/gen_pipe.py --check && python3 scripts/gen_pipe.py --self-test                 # 0 UNMAPPED
python3 scripts/gen_pipe_dirty_surface.py --check && python3 scripts/gen_pipe_dirty_surface.py --self-test
ctest --test-dir build-push -R 'RenderStateSpans\.|Residual|VertexInputEmit\.|ResourceEmit\.|FramebufferEmit\.|TextureEmit\.|SamplerEmit\.|ImageEmit\.|ProgramEmit\.|CompositeResolver\.' \
  --no-tests=error --output-on-failure
bash scripts/p3a_untouched_regions.sh --self-test && bash scripts/p4a_untouched_regions.sh --self-test
ctest --test-dir build-verify -R 'PoisonOmitted\.|VerifyCorrupted\.' --no-tests=error          # the verify controls
```

**CTS (G15), off the critical path, scheduled at the `dev` merge.** Two obligations, and the first is the one
`ROADMAP.md:20` names by hand:

```sh
# (a) the two named blocks, BOTH devices, whole - packed_pixels runs whole because the roadmap
#     calls it a handle-reuse stress test, not a sample
tools/cts/run.sh --device 35d0befa          --caselist 'KHR-GL46.direct_state_access.framebuffers*'
tools/cts/run.sh --device 35d0befa          --caselist 'KHR-GL46.*packed_pixels*'
tools/cts/run.sh --device 3B159D009VZ00000  --caselist 'KHR-GL46.direct_state_access.framebuffers*'
tools/cts/run.sh --device 3B159D009VZ00000  --caselist 'KHR-GL46.*packed_pixels*'
# (b) the full gl44to46 caselist (~56,271), both devices, at the dev merge, per-backend conformance
#     within 0.5 pp of the $BASE reading; the MOBILEGL_PIPE_POISON_OMIT sweep rides the same run
```

Report shape per the house rule: rows = GL version/extension, columns = status counts, rate = Pass/(Pass+Fail),
**NS not in the denominator**.

**CI.** `gh workflow run test.yml --ref feat/disaggregated`, then once green
`gh workflow run test.yml --ref feat/disaggregated -f baseline_sha=37da3c3a` for `monolith-symbol-report`, and once
more with `-f baseline_sha=087685d1` for the four-resize reading of G1. Record all three run URLs. Expected green:
`pipe-gates`, `include-graph-check`, `flatc-check`, `test`, `integration`, `benchmark`, `build-linux-verify`,
`integration-verify`, `retrace`, `retrace-verify`, `monolith-symbol-report`. **The exit evidence is the local WSL run
(`~/w7/pipe`), not CI; CI green is required before the `dev` merge, not before the phase verdict.**
**Two CI reds that are not code signals** (ID-16, ID-22): artifact-download `digest-mismatch` /
"Artifact download failed after 5 retries" on `retrace`/`retrace verify` matrix entries, and the
"Delete intermediate Linux retrace artifacts" step dying on a GitHub HTTP 504 after every test passed. Only a real
retrace red **after a successful download** matters.

### D.4 The measurement runs P4a owes — recorded, not gated

Rule (a): **every number below is published; none of them can stop the phase.** A regression is written into
`MEASUREMENTS.md` with its suspected cause and carried into the next phase's planning. **The one non-negotiable is
that the number is actually collected** — an unmeasured regression is not a recorded one.

#### D.4.1 The baseline of record

**`MEASUREMENTS.md` §20 (`:436-503`), not §10.** §10's absolutes and its +8-18% deltas are **-O0** readings and carry
an erratum (ID-17, `ROADMAP.md:18`, `:57`). The numbers P4a is measured against:

- Device p50, `--benchmark-no-finish`: P2's Release boundary **+6-12%** on both devices and both backends; P3a added
  **~0** on 26.3 and sodium but **+17-19 points on rd12** (Espryt +30%, Magma +27% total).
- **MC 26.3 p99 on Adreno: pull 25.457 → P3a 26.322 ms (+3.4%)**, still inside the 21-26 ms band of the adoption
  baseline at `MEASUREMENTS.md:87` (p99 163→21 ms, 40→115 fps, ~400 MB).
- DriverBench `mc_vanilla_draw` ns/draw at `c20e2f2b`: **T1** (push `0x1ff` − pull) = Espryt **+1048** (+20.5%) /
  Magma **+611** (+3.6%); **T2** (`0x7f` − pull) = +349 / +189; **P3a itself = +699 / +422**. Blend-toggle
  +5.2% / +2.8%; pass switch ≈ 0; CSO content addressing ≈ 0.

**The caveat that must travel above the numbers, not below them** (`MEASUREMENTS.md:440`): `CallClass::AccessorCalls`
is a set of **static tallies at ~10 hot entry points** (`PipeStats.cpp:16-100`, *"那份站点清单本身就是契约"*), so a
change that merely **relocates** reads scores a lower `acc/draw` without removing work. Lead with the **memo gates'
hit/miss pairs** and the **CPU-time series**; quote `acc/draw` only alongside a re-audit of the tally constants.

#### D.4.2 The paired two-device A/B — three arms

Protocol per device, unchanged from §20 and driven by `p4a_ab.sh` + `p4a_ab_1ff.sh`:

- **reboot-clean**: `adb -s $SER reboot`, wait for `sys.boot_completed`, poll for root (24 × 5 s), sleep 20, wake +
  unlock, uninstall the package, then **pin up to 3 times with a `check` after each and exit 43 if none verified**.
- **CPU/GPU pinned** via `tools/device_bench/pin_device.sh <serial> pin|unpin|check` — **not** `bench.sh`'s
  `pin_freqs()`, which is MediaTek-legacy-only and **writes into nothing and exits 0 on both campaign devices**.
  `check` is three-valued: **0 PINNED**, **1 DRIFT** (*the dangerous state — a run overlapping it is not comparable,
  discard and repeat it*), **2 UNPINNED**. Big 1.96 GHz / little 1.55 GHz, GPU maxed, 40 °C entry gate, `check` before
  and after every run, downclock afterwards.
- **serially from one tree**: `run_android_retrace_local.py` shares one `.trace-work/android-retrace-result` root per
  tree and `rmtree`s it per invocation, so the two devices **must not** be driven concurrently from one worktree.
- **device lock** ≤ 25 min per acquisition, `owner` file written, `rm -rf` on the way out; a lock held > 60 min is not
  broken — report "blocked by device lock".
- `MSYS_NO_PATHCONV=1` on every invocation; **do not export it globally in the script** — the runner passes `/c/...`
  paths to Windows python and relies on MSYS converting them.
- **Release APKs only** (ID-17). `wsl_build_trace_apks.sh` prints the lib size and the `MGPipe` string counts;
  **Release `.text` ≈ 9.4 MB, and a 21.8 MB `.text` is an -O0 build that must not pass unnoticed.**
- **ColorOS traps** on `3B159D009VZ00000`: the first install of a not-yet-installed package blocks on
  `com.oplus.appdetail InstallGuideActivity` — `topResumedActivity` shows it, `mCurrentFocus` does **not**, so grepping
  for it in `mCurrentFocus` never matches and not tapping it hangs the harness to timeout; **tapping during a streamed
  install drops the device off adb for seconds and voids all three repeats**. A foreign-signed APK must be uninstalled
  first.
- **Xiaomi thermal pin**: it cannot reboot+pin immediately after ~32 runs (big cores pin at 1689600, or the GPU reads
  1050 MHz) — wait for `cpuss-0-0` < 42 °C. **A crash resets the GPU pwrlevel range to 2..5**, so every row after a
  crash carries a DRIFT verdict on both arms.
- Git Bash cannot create directories on `//wsl.localhost`; outputs go to a Windows-local scratchpad and `copy_ab.sh`
  moves them into `~/w7/notes/p4a/ab/<serial>/`.

**Three arms, and the middle one is what isolates P4a**:

| arm | how | what it is |
|---|---|---|
| T0 | the **pull** APK | the baseline |
| **T2** | the **same push APK** with `--env MOBILEGL_PIPE_PUSH=0x1ff` | **P3a's boundary — everything before P4a** |
| T1 | the **push** APK at its default mask `0x1fff` | the whole boundary |

**P4a's own cost is T1 − T2**, per case, per backend, per device. That is exactly ID-17's supplementary-arm shape with
`0x7f` replaced by `0x1ff`; `p4a_ab_1ff.sh` is `p3a_ab_7f.sh` with the mask constant and the output root changed and
**nothing else**, and it runs `arm=push × mode=nofinish` only, in its own reboot-clean session.

Cases, matching `ROADMAP.md:20`'s families rather than P3a's buffer-shaped set:

```
improved-transparency-minecraft-26.3                   # the OIT draw-buffer / read-buffer family
minecraft-1.21.4-rd12-odinlite-in-world                # the fixture P3a cost +30%/+27% on
minecraft-1.21.4-fabric-sodium-in-world                # the per-call overhead canary
minecraft-1.21.4-fabric-iris-complementary-reimagined-in-world   # heavy MRT + attachment relocation
minecraft-1.21.4-fabric-rei-in-world                   # GUI/atlas: the union-box vs scatter-rect shape
```

```sh
LOCKS=.../8ab6b3cb-.../scratchpad/w7/locks
ANDROID_SERIAL=$SER MSYS_NO_PATHCONV=1 python3 tools/trace_replay/run_android_retrace_local.py \
  --case "$c" --backend "$b" --benchmark --benchmark-no-finish --benchmark-repeats 3 --benchmark-tail-frames 200 \
  --env MOBILEGL_PIPE_STATS=1 --env MOBILEGL_PIPE_STATS_PERIOD=120
```

Then, host-side, reduce `frameCpuTimesMs[]` over the trailing 200 frames to **p50 and p99** per arm and publish
`T1 − T0`, `T2 − T0` and `T1 − T2` per (device, case, backend). `--benchmark-no-finish` is the **primary** arm because
P4a's question is CPU cost; the finish-ON arm is the sanity check that GPU time did not move.

Read out of the same runs: the six memo gate hit/miss pairs (`ers etl eub mfp mpm mdt`) from the **last complete
window** of `grep 'MGPipe stats:' /sdcard/MG/latest.log`, the `stage-texture` / `stage-ubo-global` byte classes, and
**P4a's six new counters** (`fbe sve sse sie ctu csob-blob`, G11) plus Espryt's four `TextureUpload*`.

**Trap**: `MOBILEGL_PIPE_STATS_FILE` never produces a file on device — the JSON is written from `PipeStats::Shutdown()`
which runs from `MobileGL::Destroy`, and the trace app never reaches that teardown. Only the periodic log lines exist,
and a run shorter than one period yields nothing; hence `MOBILEGL_PIPE_STATS_PERIOD=120`, or lower for a short fixture.

**Excluded from the table, with the reason printed beside the gap**: `create-indirect` (pre-existing `dev` red on both
devices, `ROADMAP.md:100`), `create-instancing` (a 2-frame fixture, not a benchmark), rd12 on Magma on the Xiaomi
(ID-23's pre-existing abort), and any row whose `pin-after.txt` says DRIFT that could not be repeated.

#### D.4.3 The headline numbers

Two, both published against §20's table:

1. **MC 26.3 p99 on Adreno**, both backends, all three arms, against `MEASUREMENTS.md:87`'s adoption baseline and
   against P3a's 25.457 → 26.322 ms.
2. **The GUI/atlas texture-upload shape**: `TextureUploadBoxEmissions` / `…RectEmissions` / `…Jobs` on the `rei`
   fixture, client-side and server-side, pull vs push. **SSIM is blind to this and the +6 ms/frame Mali cliff lives
   here** (`ARCHITECTURE.md:249`), so it is the one number that must be read even when every gate is green.

#### D.4.4 DriverBench T1/T2/T3

```sh
cd ~/w7/pipe && cmake --build build-bench --parallel 28
bash ~/w7/notes/tools/wsl_p4a_bench.sh     # on an idle box: no gate, no builds running
```

`wsl_p4a_bench.sh` is `wsl_p3a_bench.sh` with one arm added: for each of `espryt`/`magma`, the existing
`-pull`, `-push` (`0x1fff`), `-push0` (`=0`), `-nocso` (`=0x8000000000001fff`) and **`-push1ff`
(`MOBILEGL_PIPE_PUSH=0x1ff`)**, plus `-push7f` kept as the second-order control. `REPEATS=5`,
`DRIVERBENCH_FRAMES=240`, cases `mc_vanilla_draw mc_state_toggle mc_pass_switch`, lavapipe/llvmpipe ICDs,
`EGL_PLATFORM=surfaceless`.

- **T1** = `ns_per_op(push, 0x1fff) − ns_per_op(pull)` — the whole boundary per draw.
- **T2** = `ns_per_op(push, 0x1ff) − ns_per_op(pull)` — everything before P4a.
- **T3 = T1 − T2** — **exactly what P4a added and what it removed.**
- `mc_state_toggle` is published beside them as the non-P4a control (a blend toggle touches no framebuffer record, no
  texture and no sampler set, so it must not move).

`DriverBench` `dlopen`s exactly one EGL provider through `DRIVERBENCH_EGL_LIB` with **no `LD_LIBRARY_PATH`
shadowing**, and pins the vendor json / ICD explicitly — a bare `libEGL.so.1` on a glvnd system resolves to
Mesa/llvmpipe, *a software rasteriser silently replacing the GPU under a benchmark*. The gate builds set
`-DMOBILEGL_BUILD_BENCHMARK=OFF`, so this runs in `build-bench`. **No ceiling is pinned and none is enforced**
(A.1) — the numbers are published so the optimisation phase can pin one.

#### D.4.5 The Track H unit-cost census and the 39-day checkpoint

`ROADMAP.md:56`'s criterion is *"Track H 单位成本不超出估计的 50%"*, measured in calendar days. Record the elapsed days
**per package**, the diff sizes, and the memos actually retired against `ARCHITECTURE.md:365`'s census. **P2 paid all
11 direct deletions plus 2 of the 7 re-keys; P3a paid the last direct deletion (`ConvertedVertexStreamKey`'s
`sourcePin`) plus 4 re-keys including `ResolvedDrawBuffers`** (`MEASUREMENTS.md:425`, `:429-431`). **P4a pays the
`StampSyncedFBO` 四元组 → `ContentHash` + server-private `attachmentRemintEpoch` re-key** — the fifth of the seven —
and leaves `ResolvedTextureBindingMemo`, `SamplerPassMemo` and `SetupDrawSnapshot` to P3b/P4b and P7. Also record the
**39-day trip wire** (`ROADMAP.md:67`): if P4a exceeded it, `ROADMAP.md:70` says to run the `inproc` falsification
numbers before the next phase, and `ROADMAP.md:66`'s action applies. P3a's comparison point is
**1 calendar day for four packages** (`MEASUREMENTS.md:417`).

#### D.4.6 What is published even though it is not asked for

- **`CsoBlobBytes` per frame** on the five A/B cases — vertex-elements blobs (P3a's, `MEASUREMENTS.md:465`'s open item)
  plus P4a's sampler and program archives, so P13's trim decision starts from data.
- **The four suppressors' hit rates** — `fbe`/`sve`/`sse`/`sie` against the corresponding dirty bits' `FireCount`, so
  the next phase knows whether the ~175-line backend debounce it is about to delete has anything left to do.
- **The per-dirty-bit fire rates for bits 6, 7, 8, 11, 12, 13 and 14 before and after wiring**, so P3b/P4b's narrowing
  starts from data the way P3a's did. `FireCount`/`WalkCount` exist (`Tracker.h:395-410`) but are not in
  `FormatWindowLine`; the integrator reads them from the unit harness rather than from a device window, and says so.
- **Peak RSS is NOT measured** and the hedge is stated (D-L): `retrace_gate.py` has no `rss`/`maxrss`/`getrusage`
  (`MEASUREMENTS.md:527`), P4a adds five slot tables, and the substitutes are the unconditional destroy emission, the
  six leak cases (G8b) and the `PipeSlotPeek` high-water assertions.

### D.5 Docs, and the merge to `dev` (integrator, one commit)

`scripts/check_doc_citations.py docs/Disaggregated/*.md` must stay clean — every `file:line` a doc edit adds has to be
true. The scout corrections in this brief's preamble are **not** doc bugs (the scouts mis-cited, the docs are right)
except where noted. **The exact doc lines to touch:**

| doc line | edit |
|---|---|
| `README.md:3` | status → P4a landed, with the hash; next phase → P3b/P4b (and P5, which also depends on P4a) |
| `ROADMAP.md:20` | ✅, with the real numbers: the fifteen calls wired, the six kinds re-keyed, the memos retired, the two red-before logs, the CTS block readings, and T3 |
| `ROADMAP.md:23` | restate what P4a left for P3b/P4b (D-Q's table), so the next brief does not re-scope `ResolvedDrawBuffers` (already paid) or the suppressor slots (now wired) |
| `ROADMAP.md:67` | the 39-day checkpoint's actual elapsed days, per package |
| `ROADMAP.md:85` (open question 2) | the texture-remint pull rate as P4a's corpus measured it: how often `RequireImageBindableStorage` re-dirtied an already-uploaded level across the 79-case retrace and the five A/B fixtures. This is the number that decides whether `MOBILEGL_PIPE_TEXEL_RETAIN_MB` stays 0 by default |
| `ROADMAP.md:91` (open question 8) | whether one reflection archive can serve three consumers — P4a answers the Espryt half and says so |
| `ROADMAP.md:100` (open question 17) | the `create-indirect` state at the phase exit, honestly; **and fix this line's own citation**: it attributes the no-drive-by rule to `ROADMAP.md:7`, which does not contain it — the standing statement is `:98` |
| `ARCHITECTURE.md:63` | **[deviation] D-F2**: the sampler-**view** CSO is identity-addressed per texture object for Espryt; the content-addressed 4096-entry cache is P7's. The sampler-**state** capacity 256 is landed as written |
| `ARCHITECTURE.md:100` | note that P4a **closed** the read-attachment-only parameter gap deliberately, with the scenario that proved it red first |
| `ARCHITECTURE.md:134` | **[deviation] D-E1**: `MGPTextureParams` is 40 bytes and names the texture's built-in sampler CSO; the row's list of texture-object parameters is otherwise unchanged |
| `ARCHITECTURE.md:136` | `MGPFramebufferState` carries a `Target` byte and is emitted per bound target; `Complete` is the backend-free `CheckCompleteness()` answer (D-C3) |
| `ARCHITECTURE.md:212` | `CompositeResolver` landed; name its file and its two release paths |
| `ARCHITECTURE.md:249-254` | the texture path as it landed: the drain list is P4a's, the per-storage-owner cursor with index remapping is P3b/P4b's, and `TextureUploadShapeScenario` was **built** in P4a and is **gated** in P3b/P4b |
| `ARCHITECTURE.md:261` | the archive serializer exists; name `ProgramArtifactsCodec.{h,cpp}` and the fact that monolith does not call it outside verify |
| `ARCHITECTURE.md:294-295` | `kNeedsAck`'s predicate is narrowed to the buffer target so `glTexStorage*` cannot ack (D-A2) |
| `ARCHITECTURE.md:318`/`:321` | P4a's six byte-identical rows are named beside P3a's eleven |
| `ARCHITECTURE.md:323` | Espryt's `g_rawDepthFetchSamplerState` is **still** a frontend `SamplerObject` inside `MG_Backend`; its nativisation is P3b/P4b's (`ROADMAP.md:23`), and it is the largest surviving `MG_State`-type usage the P13 A gate will meet |
| `ARCHITECTURE.md:369` | `MOBILEGL_PIPE_LEGACY_MEMOS`'s P4a arm is named, and the +1 day it cost is recorded |
| `ARCHITECTURE.md:598` | the push default is `0x1fff`; bits 9-12 are listed with their families and their three dependency refusals |
| `MEASUREMENTS.md` | a new numbered section with: D.4.2's three-arm two-device table (with D.4.1's `acc/draw` caveat **above** it), D.4.3's two headline numbers, T1/T2/T3 and the `mc_state_toggle` control, the six new counters' readings, the four suppressors' hit rates, the per-dirty-bit fire rates, D.2's two red-before logs, the Track H census row, the CTS block results, **the peak-RSS hedge stated as a decision**, and the fact that P4a **declined to pin a DriverBench ceiling** with its reason (A.1) |
| `MEASUREMENTS.md:465` | reading (5) is discharged: the CSO blob byte class exists and here are its numbers |
| `MEASUREMENTS.md:519` | the `g_uploadRing` reset asymmetry is still an open `dev` follow-up; P4a adds `ScopedDefaultUnpackState::s_synced`'s missing invalidation (D-O) beside it, as a second `dev`-side item P4a deliberately preserved |

**One chore that is not a doc edit and must not be forgotten**: `ProgramArtifacts.h:563-565`'s libc++/NDK size pin.
Build the Android target, read the four numbers from `ProgramArtifactsTest.SizesArePinnedOnThisToolchain`'s ctest
properties, define `MGL_ARTIFACT_SIZES_LIBCXX_PINNED` and the four macros, and rebuild. **Until that lands, an Android
build has no trip wire on a 58-member struct that P4a's codec walks.**

Then `[Merge]` to `dev` per the milestone flow, and schedule G15's full caselist on both devices **off the critical
path**. Commit messages are single-line `[Type] (Scope): description`; **never** a `Co-Authored-By` or any other
attribution line.

---

## E. Risks and mitigations

| risk | mitigation |
|---|---|
| **The `Coverage.def` ↔ `SubsystemForEmitter` semantic-merge trap fires again** (commit `bb2a236d`: an emitted row removed on one branch, its generated enumerator still named on another, and the push and verify builds did not compile on the integrated tree). P4a has **five** consumer packages instead of three, so the surface is larger. | Discharged **structurally and more strongly than P3a did it**: A owns `Coverage.def`, `Tracker.h` **and the whole of `PipeFill.{h,cpp}` for the entire phase**; B and C contribute only the value of a per-family constant defined in their own headers (C.0, C.7). No file is touched twice by any two packages. On top of that, **every merge in D.1 builds push AND verify**, which is the only place the trap surfaces, and `gen_pipe.py --self-test` refuses an emitted row naming a call that does not exist. |
| **A payload edit (`MGPTextureParams` 32→40, `MGPFramebufferState::Pad0`→`Target`) breaks something no test covers** — a wire `static_assert`, a `PipeFields.def` row, a generated table. | Both are **build breaks at `c0`**, and `c0` is the first thing built. `gen_pipe.py` asserts that every field list names exactly its struct's direct members **excluding `Pad\d*`** (`:306`), in both modes and hence in `pipe-gates`; `git diff --exit-code -- MobileGL/MG_Pipe/generated` catches an unregenerated table; `PipeCatalogueTest` pins the sizes. The `MGP_VERIFY_PAYLOAD_LIST` row is the one a compiler cannot catch — hence its explicit line in C.0's table. |
| **A stale generation is adopted.** `BackendSlotTable::GetOrCreate(handle)` recycles on a **forward** generation and refuses a **backward** one (`SlotTables.h:301-321`); five new kinds now go through it, and P3a's contract review found exactly this bug (M2) before it shipped. | The asymmetry is written into C.3's `d1` step verbatim. `SanityTest`'s `ARecycledSlotIsANewHandleAndTheStaleOneResolvesToNothing` (`:3316`) and `TwoTablesOfTheSameKindShareOneSlotAndKeepTheirOwnTwin` (`:3491`) are extended to every new kind, and `HandleRecycleScenario`'s six ABA arms (G8) are the behavioural net. The `MOBILEGL_PIPE_HANDLE_ABA_CONTROL` knob must **defeat the identity half of P4a's memo keys on both backends** (`MagmaPipeArms.h`, F's one DirectVulkan file), or the control is green forever and means nothing (`ARCHITECTURE.md:599`). |
| **A client-minted kind has no backend-neutral death path** — P3a's C-1: the `VertexElementsCso` slot was returned by a death-ops table Magma does not install, so every VAO leaked a slot and a ~1.3 KB record and the process `Fatal`d past 65536 slots (`MEASUREMENTS.md:507`). **P4a mints six kinds.** | D-I1 makes the client-side helper the **only** death path for every kind, from the first commit, with the three-step order fixed and the double free proven a no-op. G8b adds a leak test per kind, including a dedicated composite case (whose slot has **two** independent release paths). `PipeSlotPeek` grows six members and a peek that cannot look **SKIPs**. Peak RSS is not measurable (D-L), which is exactly why the leak tests are not optional. |
| **The composite's slot is freed twice or never** — it is released by the pipeline cache's LRU eviction *and* by the composite `ProgramObject`'s destructor. | One helper, `Free` on a stale generation is a proven no-op (`SlotAllocator.cpp:117-119`), and `CompositeResolverTest.EvictionThenDestructionFreesTheSlotExactlyOnce` plus its mirror order pin both directions. The band's exhaustion assert (`SlotAllocator.cpp:57-60` mirrored) turns a leak into a named `Fatal` rather than silent slot theft from ordinary programs. |
| **Static-destruction order: a new static holds a frontend `SharedPtr` and `exit()` runs a frontend destructor into a torn-down pipe** (ID-18: `exit` → `~PipeInputs` → `~VertexArrayObject` → `~BufferObject` → `MGPipeEmitResourceDestroyAndFree` → `FindByLifetimeId` on a hash table `~MGPipeSlotAllocator` had already freed). P4a adds **four new client singletons** and six new death paths. | The rule is absolute and is stated in the tree's own comments as *"every MGPipe process singleton"*: `MGPipeSamplerEmitterInstance()`, `MGPipeImageEmitterInstance()`, `MGPipeProgramEmitterInstance()`, `MGPipeCompositeResolverInstance()` and any other new singleton are **heap-constructed and intentionally leaked at exit**, the `MGPipeVertexInputEmitterInstance()` shape (`VertexInputEmit.h:428-440`). **No new static may hold a frontend `SharedPtr`** — and `PipeInputs.h` is A's precisely so nobody adds one there (C.7). Proof recipe, run in D.3's parts 2 and 3: `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`, which is what makes the UAF reproduce natively instead of only in CI's allocator layout. |
| **A stub applier produces false greens** — P3a's espryt v1 ran 202/461 red on the default arm, *all* attributable to the stub applier, and the attribution had to be argued rather than measured (ID-8). | D.1's order makes it structural: `wire` lands the applier bodies **before** any client package, so no package after `contract` ever runs against stubs. A package that must run before its consumer states the expected failing set **by name** and re-states it unchanged after the rework, exactly as espryt v2 did (*"the identical failing set before and after"*). |
| **`ContentHash` suppression swallows an input the server derives from the record** — most dangerously the fragColor broadcast count, which is a **shader-compilation input** read from the frontend before the FBO sync precisely so a program relinks in the same draw (`DirectGLES.cpp:3076-3078`, `:3154`). | The hash covers **every** field including `DrawBuffers[8]` and `Fbo` (D-C4), computed field-wise over a zero-initialised staging copy so no padding enters it; `FramebufferEmitTest.ADrawBufferChangeAloneStillMovesTheContentHash` is the unit gate; `FragmentOutputArrayIndexScenario` and the seventeen Iris MRT traces are the behavioural one; and `ARCHITECTURE.md:196`'s lazy-specialisation-at-the-verb rule is what makes deriving the count at the verb legal. |
| **The sampler CSO's hash reads uninitialised padding** and mints a fresh CSO per call, or the verify comparator false-differs on it. `SamplerParameters` is 100 bytes with **3 bytes of trailing padding** and has **no field table and no verify-list row today**. | D-F1's three parts: the field table + verify-list row in the contract commit; hashing and confirming over a **zero-initialised canonical copy**, field-wise; and `SamplerEmitTest.PaddingCannotChangeTheHash`, which writes garbage into the padding through a byte pointer. Without the first, G4 is a coin flip; without the second, the 256-entry cache has a hit rate of zero and nobody notices because the pixels are right. |
| **A per-verb emission costs what P3a's did.** ID-19: P3a added **+699 / +422 ns/draw** and rd12 moved +30% / +27%. P4a adds a framebuffer record, three 192-entry sets and a program record per verb. | Every new emission goes through a **version-first skip before it hashes anything** (`CsoCache.h:14-25`'s shape, not a hash-every-verb shape): the framebuffer emitter latches the dirty bit, the three sets latch their suppressor slots, the sampler view latches `(paramsVersion, shapeVersion)`, the image set has the high-water zero early-out, and `set_global_constants` fires at most once per program per frame. D.4.4 publishes T3 so the cost is a number rather than an argument, and A.1 explains why no ceiling is pinned. |
| **-O0 APKs.** P2's device A/B was measured on a Debug-flavour build: 43.2 MB APK, 21.8 MB `.text`, replaying 26.3 on the Oppo at **42 ms/frame CPU where Release takes 9.7** (ID-17). | `wsl_build_trace_apks.sh` prints the lib size and the string counts for each arm, and **Release `.text` ≈ 9.4 MB** is the number to check. D.4.2 says Release-only in as many words, and §20 — not §10 — is the baseline of record. |
| **Thermal pins drift and a row is scored against the wrong arm.** | `pin_device.sh check` is three-valued and runs **before and after every run**; a **DRIFT** row is discarded and repeated, never averaged. The Xiaomi cannot reboot+pin after ~32 runs until `cpuss-0-0` < 42 °C, and **a crash resets the GPU pwrlevel range to 2..5**, so every row after a crash carries DRIFT on both arms (ID-23). rd12-on-Magma-on-Xiaomi aborts on **every** arm including pull and is excluded with its reason printed beside the gap. |
| **The mandatory scenario is green for the wrong reason** — `ROADMAP.md:20` says 落地前必须红, and three of its four cases are green today. | Exactly one case is the red-before artefact (`AReadAttachmentOnlyTexturesDepthStencilModeReachesTheDriver`, D-E3), its D.2 log is kept, and the other three are declared **regression nets around the change** rather than evidence. The gate is not "the scenario passes"; it is "this named case was red on the contract tree and is green on the finished one". |
| **`Complete` is emitted as the GL-visible status** and the client ends up reading the backend's probed format-capability cache — the coupling P4a exists to remove. | D-C3 rules it to `CheckCompleteness()`, amends the payload comment so a later phase cannot assume the stronger answer, and leaves `glCheckFramebufferStatus` answering from the frontend exactly as it does today. `FramebufferTest`'s completeness cases are the net. |
| **The texture drain list loses texels on a backend bail** — Espryt's upload loop has early-outs that today leave the frontend dirty flag set. | D-D5's three steps: the client clears only on an **accepted** record; the applier accumulates a pending-upload set that is server-side and survives any bail; Espryt clears the applier's set only where it actually uploads. Verify's **retain mode** compares the emitted `(UnionBox, RegionCount, Regions[])` against the retained pre-clear set, which is the only way a consume-and-clear group can be checked at all (`ARCHITECTURE.md:512`). |
| **The upload shape regresses invisibly.** SSIM is completely blind to box-vs-rect, and the Mali cliff is **+6 ms/frame** for ~100 one-rect jobs against one union box. | The four `TextureUpload*` counters are published from **both** sides (client and server) so a divergence is a difference of two numbers; `TextureUploadShapeScenario` records the per-texture per-frame shape against a gold standard in P4a (gated in P3b/P4b); and D.4.3 makes the `rei` fixture's shape a headline number that must be read even when every gate is green. |
| **The subsystem A/B is dead** — a bit that changes nothing, or a combination that half-runs. | D-K2's three dependency refusals are modelled on the bit-8-requires-bit-7 refusal that already ships (`Managers.cpp:2393-2410`), each logs one ERROR naming both bits and runs the legacy arm, and `ObjectSubsystemControlScenario` asserts both that the emissions move and that the pixels do not, plus `ASamplerBitWithoutTheTextureBitIsRefusedAndNamed` at `0x9ff`. All four families can reach `NoArm` and all four **stop** rather than skipping green (D-K3), which is what `ROADMAP.md:7` requires. |
| **The dirty-surface gate goes red the moment the prefix set widens**, or the widening surfaces mutators nobody has an answer for. | The complete set the widening surfaces was enumerated at `$BASE` by grep and is **four names** (D-K5); `Create*` and `Pop*` are deliberately excluded with the reason written into the `.def` header; `--self-test` gains one canned control per new row; and A lands the widening and the rows in the same commit, so the gate is green at `c0`. |
| **`packed_pixels` is a handle-reuse stress test and it finds what the desktop corpus cannot.** `BindCurrentFBO` deliberately has **no fast path on the slot version** because `KHR-GL32.packed_pixels` read out of the previous subtest's framebuffer (`DirectGLES.cpp:3211-3216`). | That absence is in D-O's must-not-change table with the CTS block named beside it, the block runs **whole** on both devices (G15), and it runs off the critical path so it cannot be traded away for schedule. |
| **The 39-day trip wire fires.** | `ROADMAP.md:67` and `:70`: run the `inproc` falsification numbers, then decide. The desktop half of every correctness gate can NO-GO the phase without touching a device, so the schedule's long pole (two shared devices under a `mkdir` lock, ~40 lock holds of ~10 min for D.4.2's three arms and five cases) sits **after** the verdict, not before it. Budget three days for the device work and hold ≤ 25 min per acquisition. **`esprytdraw` and `esprytobj` are the other long pole**; they get an extra rework round in the budget and a verification round each on the integrated tree (D.1). |
| **A gate goes green for the wrong reason** — `ROADMAP.md:7`'s **每个门必须能因它存在的理由变红**. | Every P4a gate ships its negative control: G5 has `p4a_untouched_regions.sh --self-test` with four controls; G6 has G7's two scripted field drops; G8 has the six `.AbaControl` arms and D.2's log; G8b's leak cases fail on a leak the pixels cannot show; G9 is defined by a case that was red; G10 has the scanner's canned controls; G11's counters would read zero if never incremented, which the scenarios assert against; G12 has `ObjectSubsystemControlScenario` and the refusal arm; G13b has a pinned count; G4 has the verify lane's `MOBILEGL_PIPE_VERIFY_CORRUPT` and `MOBILEGL_PIPE_POISON_OMIT` controls, whose steps pass **when ctest fails**. |
| **A test that drives a backend helper directly aborts on the per-verb poison** because no verb filled the block. | `MG_Test/ScopedPipeVerb.h` exists for exactly this; any new test declares its verb the same way. The poison is never weakened and **no test name changes** (G14). |
| **A fixture scopes one op table and the other one routes into the pipe** — ID-14: `BufferTest.cpp`'s `ScopedBackendOps` scoped only `BufferBackendOps`, so under a push build 26 of 86 dispatch cases routed into the pipe. *"这是合并缝的典型形态——两个分支各自绿、合起来红."* | P4a installs **no new backend op table** (D-B1), so the failure mode is structurally absent; the file is granted to D as a **read-and-confirm** rather than an edit (C.7), and if a package does add a registration it inherits the two-scope shape. |
| **A worktree builds against a stale `.cxx`, a half-initialised submodule, or LFS-pointer fixtures**, and a green looks like a verdict. | `wsl_p4a_gate.sh` keeps the fail-fast rule verbatim: three builds first, any non-zero rc prints `BUILD FAILED - gate aborted (no stale-binary verdicts)` and exits 1. Tree creation initialises submodules and copies `3rdparty/glslang/External`; fixtures are hidden with `git update-index --assume-unchanged`, because a `rebase --abort` on a visible fixture pulls it back to its pointer and retrace then reports `passed 2 / 79` with `ssim=None` — a false red that reads exactly like a total regression. |
| **`retrace_gate.py --only` is given a comma list and silently matches nothing**, which is what happened to P3a's named sweep (ID-15). | It is a **regex**. Every occurrence in this brief and in `wsl_p4a_gate.sh` is spelled `'a\|b\|c'`, and the gate script carries the comment. The named sweep's case count is echoed and must be six. |
| **Performance regresses and someone stops the phase for it.** | Rule (a) is explicit and is transcribed into `MEASUREMENTS.md` (D.5): **performance is recorded against the pull baseline, never a blocking gate; correctness gates stay.** A regression is written down with its suspected cause and carried into the next phase. The one thing that is *not* negotiable is that the number is actually collected — an unmeasured regression is not a recorded one. |

---

## F. Integrator amendments during the build (2026-09-08; the rulings are in INTEGRATOR-DECISIONS.md)

| section | amendment | ruling |
|---|---|---|
| D-K2 | a FOURTH dependency row: **bit 10 (textures) requires bit 11 (samplers)** - `MGPTextureParams::BuiltinSampler` is a SamplerCso handle that only bit 11 mints; the "bit 10 without 11 is fine" sentence is withdrawn. Server-side in D's texture-family resolver; F pins the 0x5ff-shaped arm | ID-14/ID-15 |
| A / G9 | the D10 red-before is not observable through public GL (readback emulation sets `GL_DEPTH_STENCIL_TEXTURE_MODE` itself; `IsDrawSyncClean` pushes params on first sample); G9 is a white-box assertion on the applier's params record + Espryt's applied state | ID-19 |
| D-C2 | records are PER FRAMEBUFFER OBJECT keyed by handle, plus two bound handles; `MGPipeFramebufferTarget::Named = 3` describes a framebuffer without moving a binding; every DSA entry point that hands a framebuffer to the server by name is preceded by a Named record | ID-19 |
| D-D5 | step 1 clears a level's client dirty flags on the applier's ACCEPTANCE (Bool return), never on dispatch | ID-18 |
| D-D1 | a resource_respecify whose storage-defining fields equal the stored descriptor is a metadata update (BindMask / ImageBindableHint): no ack, no pending-upload clear | ID-18 |
| D-D3 | `MGPSubData::Target` = low byte MGPipeResourceTarget, high byte TextureUploadTarget (c0c helpers) | ID-12 |
| D-E1 | `DepthStencilMode` 0 = DEPTH_COMPONENT, 1 = STENCIL_INDEX; the built-in sampler handle is acquired from C's content-addressed cache with a reference count that pins it against LRU eviction | ID-12/ID-14/ID-17 |
| D-C1 | `MGPSurface::Pad0` -> `Uint16 TextureTarget` (0xFFFF on non-texture points) so the four cross-object masks read the record | ID-12 |
