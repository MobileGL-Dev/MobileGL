# MGPipe 路线图

> 状态：**P0、P0.5、P1、P2、P3a、P4a、P5、P5b 已收官**（2026-09-16，代码头 `82683d4a`）。当前头、逐门数字、开放项与下一步见 **[`CURRENT_STAGE_PROGRESS.md`](CURRENT_STAGE_PROGRESS.md)**；设计见 `ARCHITECTURE.md`；逐阶段实测见 `MEASUREMENTS.md`。**下一个是 P6 spawn transport**。

## 通用纪律（每个 commit）

- 默认 ALL target 必须完整构建；禁止提交热路径插桩（CI grep 门）。
- **每个门必须能因它存在的理由变红**：每个门带阴性对照并真跑过一次红（R-16）；公共 GL 看不见的改白盒断言。
- 正确性门是硬门；**性能对着 pull 臂只记录、不作阻塞门**（用户 2026-09-08：push 比 pull 多约 10% 逐线程 CPU 已被接受；专门的优化阶段排在路线图推完之后）。唯一不可谈判的是数字必须真的采到。
- 每阶段出口跑一次五部分门（`ARCHITECTURE.md` §13.2）；性能判据是**逐线程 CPU 时间**；设备对比走 reboot-clean + 同热窗口配对 A/B，只用 Redmi `2f7cbe2e`（`devices/pin-verification-2026-09-07.md`）；Windows 机器不是正确性门。
- G1：pull 构建的符号集与 `.text` 字节对基线恒等（每阶段 0/0/0/0）；G2：pull 与 push 的 ctest 名集合相同；G14：测试名只增不删；G5：保护区字节一致。
- **拆分不借机顺手修 `dev` 的 bug**：句柄化过程中发现的 `dev` 侧缺陷记成独立条目、独立 PR（开放问题 15、17）。
- 过程（用户 2026-09-16，ID-66）：不过度验证；轮次间不做对抗性审查，每个 P 阶段收官由 Codex 审一次，发现进入下一阶段首轮；只读 / 非核心任务派给 Codex；实现方与审查方错开模型族。

两条跑道分开：**monolith 跑道** P0 → P0.5 → P1 → P2 → P3a → P4a → P3b/P4b → P7 → P8 → P13，每段可独立交付、可随时中止；**IPC 跑道** P5 → P5b → P6 → P9 → P10 → P11 → P12。目标顺序：先完整的 separate-thread rendering（P5b，已达成），再 separate-process transport（P6）。

## 阶段

| 阶段 | 状态 | 落地什么 | 验收门 / 证据 |
|---|---|---|---|
| **P0** 卫生、度量、门、骨架 | ✅ | 边界计数器；`PipeCalls.def` 完整目录 + payload POD + 生成器 G1–G7 + CI `pipe-gates`；`gen_pipe_dirty_surface.py`；`check_doc_citations.py`；`MOBILEGL_PIPE_*` 开关；`MG_Remote/{Protocol,Transport}` 骨架 + `protocol.fbs` + `flatc-check` + `MG_Test/Wire`；三个严格 no-op 收益；spike A / B；retrace `--env` 透传 | 单元 / 集成 / trace 逐名不变；wire 层测试绿；两台设备字节 / 调用基线在案；spike 结论。`MEASUREMENTS.md` §1 |
| **P0.5** 值头与制品头抽取 | ✅ `5d99ee43` | `MG_Pipe/MGPipeValueTypes.h`；`ProgramArtifacts.h`；`Visit()` 归档 + `sizeof` 绊线；CI `-H` include 闭包断言（`scripts/check_include_closure.py`）+ `scripts/symbol_report.py` | 测试名零删除、`.text` 不变、符号 0 增 / 0 删 / 42 重命名 |
| **P1** `PipeInputs` 替换与 verify harness | ✅ | `MG_Backend/MGPipe/PipeInputs.h`（63 字段）；277 处箭头 + 58 行非箭头逐条转换；逐 verb 类填充点；逐 verb 世代 poison；G4 影子比对器 + verify CI 模式；push-on-mutation 三字段 | pull `nm` 不变；单元 1485×3；`integration-gpu` 878；`integration-verify` 818 零 Fatal；79 retrace verify 79/79 armed 零分歧。§2 |
| **P2** 渲染状态 CSO + 第一片 Track H + 残余值块 | ✅ `738b289d` | Tracker（dirty 位、5 个聚合世代、抑制器）；dirty-surface 成门；`MGPipeRenderStateSpans` chunk 表 + G7；`CsoCache`（64）；`create/bind_render_state` + `set_dynamic_state`（`SyncRenderState` 一行不动）；pixel pack / patch / attrib defaults；`ResidualValueBlock` 棘轮 1248 → 8；Espryt slot 表 + Magma vertex input 重键（11 条 memo 删除 + 2 条重键）；`MOBILEGL_PIPE_LEGACY_MEMOS`；三个 capability 真存储 | G1 4 处认定 resize、`.text` +160 B；G2 名差 0；单元 1566×3；`integration-gpu` 916 三臂；retrace 79/79 push + verify；对照 44/44。**GO/NO-GO（2026-09-08）：继续**。§3 |
| **P3a** handle wave 1（Espryt）：buffer、VAO | ✅ `fde5fda3` | 九条 `resource_*` + 五条 vertex-input 调用接线到句柄形 `MGPipeResourceOps`；第七张 Espryt slot 表；`ResourceRespecify` 的 `kNeedsAck` 谓词；VAO twin 六条身份 memo 在句柄臂退役、四条重键；`mpr` 定义；位 7/8，push 默认 `0x1ff` | 五部分门全绿：G1 0/0/0/0 `.text +0`；G5 十一函数；单元 1619×3；`integration-gpu` 958 五臂；verify 842；retrace 79/79 ×2。实际 1 天。§4 |
| **P4a** handle wave 2（Espryt）：FBO / 纹理 / sampler / program | ✅ `8c458cd5` | 十四条族调用接线（framebuffer state、sampler state / view、texture params、shader images、shader state、draw / dispatch program、global constants）；纹理 / renderbuffer 复用 `resource_*`（目录不加行）；六种 kind 按 `{slot, gen}` 重键；`CompositeResolver`；`Named = 3` framebuffer 记录；消费者门 + 客户端依赖表；位 9–12，push 默认 `0x1fff`；emulation 在 split 下具名 Fatal | 全门：G1 0/0/0/0；G5 17 区；G2 2902；单元 1785×3；`integration-gpu` 1117 七臂；verify 920；retrace 79/79；八族拒绝普查 0；`PipeApplyPeek` 白盒对照。实际 1 天。§5 |
| **P5** 传输 + inproc applier + 发射表 | ✅ `ff2994d9..37fc4fdb` | 同一 codec 上的 `InProcessTransport`；四种 build flavour；发射表三类 / caps mirror / reply mailbox；apply-thread context 终身持有；lockstep verb barrier；tight ReadPixels；persistent-map 块推送；G8 字段归属生成器；split 测试 / CI / APK 车道；八包（c0 契约、w1 codec、s1 session、p1 归属、b1 persistent map、t1 车道、c1 路由、v1 server）+ j0 / x2 / v1-r3 收尾 | joint：OpenRA inproc 2/2 SSIM 1.0；`integration-split` 21/21；E1 14/14 红；G1 0/0/0/0。收尾头全门在 E3(a) 停止（设计性 skip 被判红），Part 2/4 未到达；收官审查 1 blocker / 10 major / 2 minor → P5b r1/r2。§6 |
| **P5b** `inproc` verb migration | ✅ `37fc4fdb..82683d4a` | class-C 普查决定顺序；d1 19 / i1 7 / t2 6 / f1 11 槽迁移；sync 五槽；具名 blit；GLES mip descriptor；r1 / r2；收官审查三项修复。发射表 A=2 / B=54 / C=15 | 主机全门 `348d22a4` complete；`integration-split` 107/107；合并普查零回退、79 trace 72/6/1；**Redmi 正确性 8/8 + 四臂 A/B、barrier tax 首测**。§7 |
| **P6** spawn transport | 下一个 | `SocketTransport`（socketpair + fork/execve，envp 剔除 + 强制 monolith 双保险）；`ServerMain`；`MOBILEGL_IPC_SERVER_PATH` + `dladdr` 兜底；有界重试握手；EOF 即时退出；device-lost latch；替换 P5b 的三处 inproc 依赖 | P5b 的完整渲染路径在 `spawn` 下绿；进程树只多一个子进程；`HeadlessGL` fork 预检无孤儿；OpenRA 在 Adreno 830 上 split SSIM ≥ 0.99 |
| **P3b / P4b** 深化（Espryt） | 待排 | 按存储属主键控的发射游标与 view 索引重映射；`g_fboTextureSyncList`；`ResolvedTextureBindingMemo` / `SamplerPassMemo` / image sweep / program registry 重键；XFB scatter 搬到 client；删 fragColor 重推导 workaround 与 `g_broadcastMemo*`；raw-depth-fetch sampler 原生化；回读 / pack state；P4a 记下的 R-3 / R-4 / R-5 / R-7 / R-10 / R-11；`ProgramArtifacts.h` 的 NDK 尺寸钉 | 纹理 / program 场景；CTS `texture_*` / `shader_image_*` / `packed_pixels` 在 0.5 pp 内；每一个 Iris trace；`TextureUploadShapeScenario` 升级成门 |
| **P7** DirectVulkan（Magma）全量迁移 | 待排 | 其余 10 个子系统：`SetupDrawSnapshot` 探测字段塌成 dirty mask；占位纹理原生化；具名 UBO host payload（D-B8）；内部 shader 烘焙；`VertexInputStateFactory` 内容寻址 CSO；D18 容器纪律保留 | 集成 + trace 在 Magma 的 push 与 split 下全绿；verify 零分歧；`nm -D libMobileGLServer.so \| grep glslang` 为空；CTS 0.5 pp 内。**再基线检查点：中点完成子系统 < 40% 立即重定基线** |
| **P8** emulation 下放 + 索引宿主镜像 + 协议广度 | 待排 | `MG_Impl/Pipe/HostResolve.cpp`（client 数组范围、最大索引扫描、`*IndirectCount` 解析，逐站点 reconcile）；`Server/IndexHostMirror`；CopyImage 镜像搬到 client；viewport-array 回放验证；`generate_mipmap` 计划 + CPU 回退纹素；大 blob carrier（开放问题 11）；无 present fence tick；`kCapDriverOrderedXfbCapture` | `'^DirectGLES\.Split\.'` 与 `'^DirectGLES\.'` 逐名相同；trace split 双后端 SSIM ≥ 0.99 含两个 `coherent_as_flush` fixture；`ClientArrayAfterComputeWriteScenario`；`create-indirect` 上 `roundtrips-per-frame` 读零；`index-mirror-bytes` 逐用例发布 |
| **P9** 反向通道 | 待排 | `SEG_REPLY` 异步 slot 池；PBO 回读 fire-and-forget；`OnGpuWritten` 收窄；`OnBufferWriteback` 批处理 + epoch 排序；`OnXfbScatterReady`；`OnTextureWriteback`；`OnMipLevelsGenerated`；纹理拉取四条缓解 + 终止符；`OnGlError` 有序；`OnSurfaceChanged`；`OnLog` 分级；`SEG_EVENT` 溢出策略；class-C 的 readback 尾 | 回读 / XFB 场景在 split 下绿；`TextureRemintPullScenario`（含无解用例）；拉取计数逐 trace 发布；两条故障注入 |
| **P10** sync / query / present 节奏 | 待排 | client 铸造 query handle；轮询入口成门铃点 + `MOBILEGL_IPC_POLL_ESCALATE`；fence 完成度来自逐 fence 退休；非 present fence tick；`present` 1:1；credit 默认 1；roundtrip 计数器与输入延迟直方图；class-C 的 query / swap-interval 尾 | query / XFB / `AsyncCompile` 场景在 split 下绿；trace 上 draw / state / upload 路径 roundtrip 读零；零 timeout 轮询有界退出；配对 A/B 记录 |
| **P11** persistent map 与 ≥16 MiB 采纳 | 待排 | POST 探针档位选择（T0 主攻，Adreno 可选 T1，T2 回退）；`SEG_ADOPT` 生命周期绑 `completedFrameSerial` | `LargeArenaAdoptionScenario` 在所选档下绿；26.3 与两个 Create fixture SSIM ≥ 0.99；Adreno 830 上 p99 帧时与峰值 RSS 对采纳基线（163→21 ms / 40→115 fps / ~400 MB）回归不超过 10% |
| **P12** Android 生产窗口路径 | 待排 | `android:process=":mgl"` Service 收 Java `Surface`；server 生命周期绑 Activity；FCL env 与 plugin APK 开关表接线 | Minecraft 经 FCL 在 spawn 模式下于 Adreno 830 双后端入世界；杀 server 产生干净 device-lost latch |
| **P13** 退役 pull 路径 | 待排 | 删 `SnapshotFromGLContext()` 非 verify 分支、`MGB_CTX`、`MOBILEGL_PIPE_PUSH`、`MOBILEGL_PIPE_LEGACY_MEMOS`；保留 `MOBILEGL_PIPE_VERIFY`；MGPipe recorder；删 `set_residual_value_state`；在计数器活着的情况下重调幸存缓存容量 | `static_assert(sizeof(ResidualValueBlock) == 0)`；三道纯度门在非 verify 构建上转绿；recorder 金标建立；monolith 逐线程 CPU 不差于 P0 基线 |

CTS 周转单独计价（`gl44to46` 约 56,271 例）：逐阶段只跑该阶段可能影响的具名块；完整 caselist 只在架构边界与合并 `dev` 之前跑，放 CI 不放关键路径。

## 里程碑

- **P2 出口（2026-09-08）：GO/NO-GO 判定继续**——五部分门全绿，逐线程 CPU 代价约 +10% 被接受；tracker 绝对 ns 上限降为记录项。
- **P3a / P4a 出口（2026-09-08）**：各 1 天，远低于 27 / 39 天的再基线绊线，未触发重定基线。
- **P5 出口（2026-09-16）**：缩减路径首个 IPC 帧；最终全门在 E3(a) 停止；红米四臂的 split 均停在索引 draw，barrier tax 未测（归 P5b）。
- **P5b 出口（2026-09-16，已达成）**：主机全门 `348d22a4` complete；79 trace 72/6/1；设备源头 `82683d4a`（APK `p5bcodex2`）Redmi 正确性 8/8——四条 A/B trace 双后端首次在设备上 inproc 渲染；barrier tax 首测 split−push 逐线程 CPU p50 +5.9% – +18.2%。
- 仍是方向、不据此伪造新日历：全功能 split（P8 之后）、纯度门在非 verify 构建上转绿（P13）。

再基线检查点仅剩一条：**P7 中点完成子系统 < 40% 立即重定基线**（P3a 的检查点发现不了 Magma 特有的超期）。

## P5 / P5b 出口记录的债务

| 债务 | 去向 / 当前口径 |
|---|---|
| P5 的 27 个普通 inproc wrong-answer（历史计数） | 22（14 layered + 3 packed depth/stencil + 5 framebuffer recycle）→ P4b/P7 texture readback；3 → P7 query；1 inspection → P6；1 FBO/RBO delete 用例在 r1 定向验证中通过，完整像素因果未隔离。最终计数以 `MEASUREMENTS.md` §7.2 的同名比对为准 |
| `rsp` residual inputs | STRICT_ERRORS reduced lane 19 abort / 2 skip；`rsp=35` 只是有 stamp 读点的下界，无 verb stamp 的 sticky / non-verb forward 仍可绕过计数；P7/P8 补逐字段归类，其余按字段的 P3b/P4b/P7 退役 |
| `SEG_REPLY` 2 MiB 单槽 payload cap | 更大 readback 需要 P6+ chunking 或专用 carrier（ID-47） |
| `GetCaps` 的两个 blobref | 目前不骑 record；一旦运输，必须有 server→client carrier rule，不能套 `SEG_STAGE` |
| PACK-PBO readback | P5 具名拒绝；真实形状是 server 写 buffer resource、client `MarkGpuWritten`，P6（ID-57） |
| ABI fingerprint | 不混 segment sizes；改变 8/32/16 MiB + 256 KiB ledger 时仍欠这项 |
| 默认 32 MiB staging | 目标负载单次 128 MiB 上传装不进默认 stage；普查 / Redmi 显式 256 MiB profile，默认不改；分块 / 专用 carrier 归 P8（开放问题 11） |
| P5b 的 inproc 依赖 | 具名 blit 的 scoped client binding + barrier；mip descriptor 的 registry 身份查询依赖 barrier-held object；FBO death 的 inproc mailbox。P6 必须替换 |
| 未迁移与仿真路径 | 15 个 class-C 槽（query / sync 尾 / `GetTexImage` / `SetSwapInterval`）→ P9 / P10；client vertex arrays、multi-draw client indices、RGB 三通道 CPU mip、renderbuffer copy endpoint、未绑定 named clear、`texture-remint-pull` 仿真保留具名拒绝 → P8 / P9；Magma split compute / image 路径 82 个错答 → P7 |
| E2 wire 内容控制 | draw-drop 是 OpenRA 的有效控制（758 draws，SSIM 0.000036）；clear-drop 被全屏 overdraw 掩盖，不能作控制 |
| max record bytes | 实测 reduced / OpenRA `maxrec=784 B`，默认 cap 4 MiB；只代表所测负载，后续索引 / indirect 尾仍须记录 |
| 树外脚本 | 普查跑器、`wsl_p5_gate.sh`、Redmi 定频行都在 `~/w7/notes/`，git merge 不会传播 |
| 临时 CI trigger | 合入 dev 前删除 `test.yml` / `apk.yml` 的 `feat/disaggregated` TEMPORARY trigger |

## 开放问题

P0 已回答的不再列出（spike A 的域、spike B 的分档、`posix_spawn` 不可用、OOM 探测惯用法、`GetInteger64i_v`/`GetProgramiv` 退役、D21 与 `RenderbufferObject` lifetime id、动态 accessor 基线）。

1. **client 侧 dirty 走查的真实每 draw CPU 代价。** 已答（P2–P4a）：推送没有在拉取基线之下净减少；Release 下 P2 边界 +6–12%，P3a 在 VAO 切换密的 rd12 上再加 +17–19 pt，P4a 在 Espryt 上 +3.4–5.7 pt（`MEASUREMENTS.md` §3–§5）。用户 2026-09-08 接受，性能自此只记录。
2. **真实语料上纹理重铸拉取的发生率。** 已答（P4a）：可忽略，保留 LRU 维持默认 0——79 例 × 两后端 780 个统计窗口里共 2 次（`iris-photon`、`iris-derivative` 各 1，只在 DirectGLES）。但 P5b 下这条路径是具名 Fatal，正是这两条 trace 加 `create-indirect` GLES 的首阻塞（P9）。
3. **spike B 的 `untrusted_app` 域复核。** 两台设备的分档在 `shell` 域测得；从应用进程再跑一次 `extmem_probe`（spike A 的 exec 钩子已可用）。P11 前做。
4. **渲染状态的 wire 粒度。** 已答（P2）：16 个边界 / 15 个 chunk，7 pipeline 396 B + 8 dynamic 772 B；CSO LRU 64 暂定，P13 重调。
5. **无存储的 capability。** 已答（P2）：`FramebufferSrgb`、`DepthClamp`、`TextureCubeMapSeamless` 三个都补了真存储，`sizeof(RenderStateParameters)` 仍 1168。
6. **具名 UBO host payload 的形状（D-B8）。** Magma 在 26.3 世界每帧重打包 331 KB 具名 UBO 字节，Espryt 为 0。要么冻结第二变长尾形状，要么走备选（Magma 直接描述符绑定常驻 `VkBuffer` range，独立 `dev` PR）。P7。
7. **`MG_Util` 的切割缝。** server 需要 SPIRV-Cross / ESSL 转译缓存 / 格式处理器 / POST 探针，client 需要 glslang 与反射层；`MG_Util` 内部是否有干净的 Transpile-vs-Reflect 缝未审计。P7。
8. **一份反射归档能否服务三个消费者。** Espryt 那一半已答（P4a）：能且不需要复制。Magma 的两个消费者（`DirectVulkan.cpp` 为 `glGetProgramResource*` 重跑反射）未答。P7。
9. **viewport-array 回放能否塞进一次 `draw_vbo`**：各遍之间观察到的状态是否与今天一致未验证。P8。
10. **`ResidentSubData` 的不对称。** P3a 原样保留：`SubDataResident` 是 `kOptional`，Magma 未补实现、`kCapResidentSubData` 未接线。P7 / 独立 `dev` PR。
11. **`SEG_STAGE` 的上限。** 目标负载已实测单次 128 MiB 上传，超过默认 32 MiB；普查与 Redmi 统一显式 256 MiB，默认不变，分块路径未实现。P8 需要 MC in-world / Create 的占用分布与更大 blob 的 carrier 设计。
12. **P13 之后 split-only 渲染 bug 的 server 侧第二意见。** verify 构建 + recorder 只覆盖推送内容，不覆盖后端对它的解释。
13. **烘焙后的内部 shader 能否在没有活 `ProgramObject` 的情况下表达 uniform location 与 UBO 布局。** 未做原型。P7。
14. **推送模型改变哪些按拉取模式调过的缓存命中率。** 幸存者容量在 P13 重调。一条线索：设备上 26.3 Espryt 的 `sve` ≈ draw 数（每 draw 重发一次 sampler-view 集合），桌面只有 ~0.07/draw；列入 P3b/P4b 优化清单。
15. **monolith 的 `*IndirectCount` 不调 `SyncGpuWrites()` 是不是潜在缺口。** 独立 `dev` 问题，拆分不得借机顺手修。
16. **索引宿主镜像的实际内存占用。** MC / Sodium / Iris 语料里 element-array buffer 总量未测；若显著超 64 MiB，退化路径的频率与代价必须实测。P8。
17. **`create-indirect` fixture 在 Adreno 830 上的失败**是 `dev@81b17c0b` 就有的（基线 APK 复现），不是本分支造成；`rd12` + Magma 在两台 Adreno 830 上的 `scudo::reportMapError` 崩溃同样是 `dev` 侧。两者排除在设备 A/B 之外、留在桌面语料里；P8 要在 `create-indirect` 上断言 `roundtrips-per-frame == 0`，所以 `dev` 的修复在别人的关键路径上。
