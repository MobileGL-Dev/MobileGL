# MGPipe 路线图

> 状态：P0 已落地（`feat/disaggregated@458ccde1`）。设计见 `ARCHITECTURE.md`，实测见 `MEASUREMENTS.md`。天数是各阶段所含子系统行的求和（低端 / 高端），总计 **267–337 人天**（不含 CTS 周转）；两个工程师、P7 与 P5/P6/P8 并行约 7–9 个月，真正的约束是两台设备的争用。

## 通用纪律（每个 commit）

默认 ALL target 必须完整构建；禁止提交热路径插桩（CI grep 门）；**每个门必须能因它存在的理由变红**；Windows 机器不是正确性门；设备对比走 reboot-clean + 同热窗口配对 A/B，CPU 定频按项目协议；每阶段出口跑一次五部分门；每阶段性能判据是**逐线程 CPU 时间**。

两条跑道：**monolith 跑道** P0 → P0.5 → P1 → P2 → P3a → P4a → P3b/P4b → P7 → P8 → P13，每段可独立交付、可随时中止且 monolith 严格好于起点；**IPC 跑道** P5 → P6 → P9 → P10 → P11 → P12。

## 阶段

| 阶段 | 天 | 落地什么 | 验收门 | 依赖 |
|---|---|---|---|---|
| **P0** 卫生、度量、门、骨架 | 9–11 | ✅ 边界计数器（字节 / 动态 accessor / 六个 memo 门 / 上传形状）；`PipeCalls.def` 完整目录 + payload POD + 七个生成器 + CI `pipe-gates`；`gen_pipe_dirty_surface.py`；`check_doc_citations.py`；八个 `MOBILEGL_PIPE_*` 开关；`MG_Remote/{Protocol,Transport}` 骨架（`SCM_RIGHTS` 第一优先、双 tail 双三元组的 `RingControl`、双向 doorbell、校验型 `Framing`、`ShmSegment`、`InProcessTransport`）+ `protocol.fbs` + `flatc-check` + `MG_Test/Wire` 五个套件；三个严格 no-op 收益（`GetInteger64i_v`/`GetProgramiv` 退役、`RenderbufferObject::GetLifetimeId()`、D21 XFB 计数槽重键）；compute 限制进 `DynamicBackendParameters`；spike A、spike B；retrace 通道 `--env` 透传 | ✅ 单元/集成/40 trace 逐名不变；wire 层测试（fd 传递、doorbell、ring、封帧、inproc）绿；两台设备的字节/调用基线在案；spike A/B 出结论；citation lint 绿 | — |
| **P0.5** 值头与制品头抽取 | 6–9 | `MG_Pipe/MGPipeValueTypes.h`（`RenderStateParameters`、`SamplerParameters`、`PixelStoreParameters`、`VertexAttribute`… 不 include `MG_State/GLState`）；`MG_State/GLState/ProgramState/ProgramArtifacts.h`（五个反射类型，不 include `ShaderObject.h`/`SpvcSession.h`，更新 7 个 includer）；`Visit()` 归档 + `sizeof` 绊线；CI `-H` include 闭包断言 | 全套测试逐名不变（纯搬移）；两条闭包断言绿且人为加回一个 `MG_State` include 能变红；`nm`/`.text` 变化可逐符号归因 | P0；**P1 与 P7 的硬前置** |
| **P1** `PipeInputs` 替换与 verify harness | 10–13 | `MG_Backend/MGPipe/PipeInputs.h`（Espryt 32 / Magma 55 访问器）；`sed` 293 处 + 58 行非箭头清单逐条转换（显式交付物）；逐 verb 类填充点（G5 表，~93 个边界站点）；逐 verb 世代 poison；G4 影子比对器 + 第三种 CI 模式；20 处 `SyncPersistentMappedRange` + 6 处 `SyncGpuWrites` 的逐站点归属表 | pull 构建 `nm --defined-only` 不变、`.text` 差异逐行归因（空守卫/三元重写推迟到 P2）；40 trace + 全部集成测试在 `MOBILEGL_PIPE_VERIFY=1` 下零分歧；故意损坏一个快照字段能让 verify 变红；故意在 `glGenerateMipmap` 的填充表漏一个字段能在**那条 verb** 上触发 poison Fatal | P0.5 |
| **P2** 渲染状态 CSO + 第一片 Track H + 残余值块 | 18–26 | `MG_Impl/Pipe/Tracker`（dirty 位、5 个聚合世代、抑制器骨架）；`gen_pipe_dirty_surface.py` 首轮映射成门；`MGPipeRenderStateSpans` + G7 setter 一致性测试；`CsoCache`（64 项，键 = pipeline 子集）；`create/bind_render_state` + `set_dynamic_state`（Espryt `SyncRenderState` 一行不动；Magma `ComputePipelineStateHash`/`GetOrCreatePipeline`/`ApplyDynamicDrawStateTail` 改从 CSO 与动态 payload 取）；`set_pixel_pack_state`、`set_patch_state`、`set_vertex_attrib_defaults`；`set_residual_value_state` + `ResidualValueBlock` 绊线；**第一片 Track H**：Espryt 0b（`SlotAllocator` + 6 个 registry → slot 数组 + 删 `TwinLookupMemo`×3/`OwnerEquals`/`g_fbSlotCache`/GC）与 Magma 子系统 4（`VertexInputStateFactory`/`VaoDrawMemo` 重键，删前端 VAO 里的后端裸指针）；`MOBILEGL_PIPE_LEGACY_MEMOS`；补 `FramebufferSrgb`/`DepthClamp` 存储 | 集成 × 2 后端 × {pull, push} 逐名相同；40 trace push 下 SSIM ≥ 0.99 双后端；verify 零分歧；`HandleRecycleScenario` 绿且重键前红；G7 测试绿且拿掉一个字段能红；两台设备配对逐线程 CPU p50/p99 不差且 tracker 绝对 ns 在上限内；Blaze3D blend-toggle 微基准；CSO 内容寻址关闭的负面对照 | P1 |
| **P3a** handle wave 1（Espryt）：buffer、VAO | 18–23 | 7 个 `BufferBackendOps` → `resource_*`、`buffer_subdata_resident`（可 null）、`resource_flush_range`、`resource_readback`、`map_persistent`（不碰实现）；pool 与延迟释放原样搬；vertex elements 三件（两个视图都带）；`set_vertex_buffers`（`baseInstance` 显式字段）；`set_index_buffer`；Adreno SIGSEGV workaround 保留 | 全套门；buffer/VAO 族场景（`LargeArenaAdoption`、`StorageBufferRegrow` 发布 `map-persistent-roundtrips`、`VertexAttribBinding`、`MultiDraw`、`PrimitiveRestart`…）；Create/rd12/26.3/sodium trace；MC 26.3 在 Adreno 上 p99 不变。**再基线检查点 1：超过 27 天必须重定基线** | P2 |
| **P4a** handle wave 2（Espryt）：FBO / 纹理 / sampler / program 身份与描述符 | 26–34 | `set_framebuffer_state`（解析后的 `ReadSurface`、内联格式、`ContentHash`、`{0,1}`）；sampler CSO（含 `borderColorForm`）；sampler view + `set_texture_params`；`set_sampler_views`/`bind_sampler_states`/`set_shader_images`；shader CSO（SPIR-V + 归档）；`set_draw/dispatch_program`；`set_global_constants`；`CompositeResolver`；纹理/renderbuffer 的 `resource_*`。emulation 在 split 下显式 Fatal 直到 P8 | 全套门；framebuffer/纹理/program 族场景；**新增"只作 attachment / image 单元 / CopyImage 端点的纹理其 `glTexParameter` 生效"场景（落地前必须红）**；两台设备 `KHR-GL46.direct_state_access.framebuffers*` 与整个 `packed_pixels` 块（~3300 例，句柄复用压力测试）。**再基线检查点 1b：超过 39 天** | P3a |
| **P5** 传输 + inproc applier + 发射表 | 12 | `MG_Remote/Client` 发射表；`Server/PipeApplier`、`ServerLoop`（`mgl-srv-io` + `mgl-srv-apply`）；`Init.cpp` 单一 hook 装 `BackendObject_Remote`；`MGPCaps` 快照；阻塞 `read_pixels`；client 侧保守 `MarkGpuWritten`；**client 侧块粒度 persistent-map 推送**；`InProcessTransport` 走与 spawn 相同的 G3 编解码；trace-replay `SPLIT` 后缀 + `-DTRACE_TRANSPORT=`；`MOBILEGL_TRANSPORT` 解析 | `DirectGLES.Split.*(ClearThenReadPixels|Triangle)` 在 `inproc` 下绿；OpenRA trace split SSIM ≥ 0.99；`PersistentCoherentMapScenario` 绿；两个角色峰值 RSS 在案；`persistent-map-push` 出数；未迁移字段读 = `Fatal{UnmigratedPipeInput}`。**第 99 天：首个 IPC 帧（缩减路径）** | P4a |
| **P6** spawn transport | 5 | `SocketTransport`（socketpair + fork/execve，envp 剔除 + 强制 monolith 双保险）；`ServerMain`；`MOBILEGL_IPC_SERVER_PATH` + `dladdr` 兜底；有界重试握手；EOF 即时退出；device-lost latch | P5 全部测试在 `spawn` 下绿；进程树只多一个子进程；`HeadlessGL` fork 预检无孤儿；OpenRA 在 Adreno 830 上 split SSIM ≥ 0.99。**第 104 天：首个跨进程帧** | P5 |
| **P3b / P4b** 深化（Espryt） | 29–38 | memo 重键（`ResolvedDrawBuffers`、`ResolvedTextureBindingMemo`、`SamplerPassMemo`、image sweep、program registry…）；server 删 `g_unitTextureSyncList`/`g_fboTextureSyncList`/`DirectGLES.cpp` 的 ~115 行 unit-bindings epoch 推导，**同时**在 Tracker 落地集合 hash 抑制器；dirty 归属反转（按存储属主键控的发射游标）；`MGPSubRegion` 跨步描述符改造；XFB scatter 搬到 client；删 fragColor 重推导 workaround 与 `g_broadcastMemo*`；raw-depth-fetch sampler 原生化；回读 / pack state | ~25 个纹理场景、21 个 program 场景 + `MG_Test/ShaderTranspiler`；两台设备 `KHR-GL46.texture_*`/`internalformat.texture2d.*`/`shader_image_*`/`packed_pixels` 在 pull 基线 0.5 pp 内；每一个 Iris trace；**`TextureUploadShapeScenario`**（形状金标，Mali 帧时增量必须发布）；view/owner 发射游标别名场景；verify 保留模式下 subdata 形状逐项相等；XFB 场景 + `capture_special_interleaved_test` | P4a |
| **P7** DirectVulkan（Magma）全量迁移 | 80–104 | §5.5 其余 10 个子系统（子系统 1、4 已在 P2）：`SetupDrawSnapshot` 探测字段塌成 dirty mask；占位纹理原生化（~120 行删除）；具名 UBO host payload（D-B8，`kCapNeedsHostUboBytes`）；blit/depth-mipmap 内部 shader 烘焙 + 新鲜度测试；`VertexInputStateFactory` 裸指针写回删除；D18 容器纪律原样保留 | 集成 + 40 trace 在 Magma 的 push 与 split 下全绿；verify 零分歧；**`nm -D libMobileGLServer.so | grep glslang` 为空**；Iris trace 上 `stage-ubo-named` 逐帧字节发布；两台设备 CTS 0.5 pp 内。**再基线检查点 2：中点（第 40–52 工作日）完成子系统 < 40% 立即重定基线** | P0.5、P2；可与 P5/P6/P8 并行 |
| **P8** emulation 下放 + 索引宿主镜像 + 协议广度 | 12–16 | `MG_Impl/Pipe/HostResolve.cpp`（client 数组范围、最大索引扫描、`*IndirectCount` 解析，各带逐站点 reconcile）；`MGHostSpan` split 填法；`Server/IndexHostMirror`；CopyImage 镜像搬到 client；`draw_vbo` 收编 multi-draw 族（分档仍在 server）；viewport-array 回放验证；`generate_mipmap` 计划 + CPU 回退纹素；G3 分块路径；无 present fence tick + 无 present split 用例；`kCapDriverOrderedXfbCapture` | `'^DirectGLES\.Split\.'` 与 `'^DirectGLES\.'` 逐名相同（DirectVulkan 同）；40 trace split 双后端 SSIM ≥ 0.99 含两个 `coherent_as_flush` Create fixture；`ClientArrayAfterComputeWriteScenario` 绿（去掉等待必须见几何缺失）；`create-indirect` 上 `roundtrips-per-frame` 读零；`index-mirror-bytes`/`index-bytes-shipped` 逐用例发布。**第 145 天：全功能 split** | P6、P3b/P4b |
| **P9** 反向通道 | 10 | `SEG_REPLY` slot 池；阻塞 `read_pixels`；PBO 回读 fire-and-forget；`OnGpuWritten` 收窄；`OnBufferWriteback` 按操作级批处理 + epoch 排序；`OnXfbScatterReady` + client scatter；`OnTextureWriteback`；`OnMipLevelsGenerated`；纹理拉取四条缓解 + 终止符；`OnGlError` 有序 + `glBufferStorage` 的 ack；`OnCapsInvalidated`；`OnSurfaceChanged`；`OnLog` 分级 + 速率限制；`SEG_EVENT` 溢出策略 | 回读/XFB 场景在 split 下绿；`TextureRemintPullScenario` 绿且含无解用例（终止符前表现为 apply 线程挂死/超时）；拉取计数逐 trace 发布；故障注入：credit 阻塞时灌满 `SEG_EVENT`、日志洪泛下注入 link 失败 | P8 |
| **P10** sync / query / present 节奏 | 6 | client 铸造 sync/query handle；轮询入口成门铃点 + `MOBILEGL_IPC_POLL_ESCALATE`；fence 完成度来自真的逐 fence 退休；DirectGLES 非 present fence tick；`present` 1:1；credit 默认 1 + 叠加公式；roundtrip 计数器与输入延迟直方图；三个独立 `dev` monolith 修复（`glEndTransformFeedback` 无限 `ClientWaitSync` → 推迟到首次读；`glDispatchCompute` 三次 `GetIntegeri_v` 校验 → 读 `CompileEnv`；D21 已落地） | query/XFB/`AsyncCompile` 场景在 split 下绿；40 个用例上 draw/state/upload 路径 roundtrip 读零，条件渲染与阻塞 query 次数逐用例发布；零 timeout 轮询在有界时间退出；`bench.sh` 配对 A/B：两侧都关采纳时 split 帧时在 monolith 10% 内，输入延迟 p50/p99 在案 | P9 |
| **P11** persistent map 与 ≥16 MiB 采纳 | 8 | POST 探针档位选择（T0 主攻，Adreno 可选 T1，T2 回退）；`SEG_ADOPT` 生命周期绑 `completedFrameSerial`；`MOBILEGL_IPC_ADOPT_TIER` 负面对照 | `LargeArenaAdoptionScenario` 在所选档下绿；26.3 与两个 Create fixture SSIM ≥ 0.99；`StorageBufferRegrowScenario` 发布 `map-persistent-roundtrips`；Adreno 830 上 p99 帧时与峰值 RSS 对 monolith 采纳基线（163→21 ms / 40→115 fps / ~400 MB）**回归不超过 10%**；若 T2 成为某设备的永久答案，其实测代价写进文档 | P10、spike B（已答） |
| **P12** Android 生产窗口路径 | 10 | `android:process=":mgl"` Service 收 Java `Surface` → `ANativeWindow_fromSurface`；server 生命周期绑 Activity；FCL 用户 env 与 plugin APK V2 开关表接线 | Minecraft 经 FCL 在 spawn 模式下于 Adreno 830 双后端入世界；配对 reboot-clean bench + 输入延迟直方图；杀 server 产生干净 device-lost latch；SIGKILL 故障注入 | P11 |
| **P13** 退役 pull 路径 | 8–12 | 删 `SnapshotFromGLContext()` 非 verify 分支、`MGB_CTX`、`MOBILEGL_PIPE_PUSH`、`MOBILEGL_PIPE_LEGACY_MEMOS`；保留 `MOBILEGL_PIPE_VERIFY`；MGPipe recorder 金标模式；删 `set_residual_value_state`；`MG_Backend` 的 `MG_State` include 收缩到 `MGPipeValueTypes.h`；在计数器活着的情况下重调幸存缓存容量（`VaoDrawMemo` 2048、`SetupDrawSnapshot` 4、pipeline memo 8、`syncedTextureMemo` 8）并变成带 env 覆盖的调优参数；最终符号/尺寸/CPU 报告 | `static_assert(sizeof(ResidualValueBlock) == 0)` 编译通过；三道纯度门在非 verify 构建上转绿；verify 构建仍零分歧；recorder 金标在 40 trace 上建立；全套门（集成 × 2 后端 × {monolith, split}、单元、40 trace、两台设备 CTS 在 `81b17c0b` 基线 0.5 pp 内）；**monolith 逐线程 CPU 在两台设备 p50/p99 上不差于 P0 基线** | P7、P8、P12 |

累计（低端）：P0 9 → P0.5 15 → P1 25 → P2 43 → P3a 61 → P4a 87 → P5 99 → P6 104 → P3b/P4b 133 → P8 145 → P9 155 → P10 161 → P11 169 → P12 179 → P13 187；P7 另 80–104，单跑道累计 267。

**CTS 周转单独计价**：`gl44to46` 约 56,271 例。逐阶段只跑该阶段可能影响的具名块（P4a `packed_pixels`、P3b/P4b `texture_*`/`shader_image_*`、P9 `transform_feedback*`）；完整 caselist 只在五个架构边界（P0.5、P3a、P4a、P3b/P4b、P13）与每次合并 `dev` 之前跑，放 CI 不放关键路径。若周转仍主导排期，加宽估时而不是削弱门。

## 里程碑

- **第 25 天（P1 出口）**：verify harness 逐 draw 逐字段证明"推送等价于拉取"。零产品风险，**不是** GO/NO-GO。
- **第 43 天（P2 出口）：GO/NO-GO**。
- 第 99 天：首个 `inproc` IPC 帧（缩减路径）；第 104 天：首个跨进程帧；第 145 天：全功能 split；第 187 / 267 天：三道纯度门转绿。

## 第 43 天 GO/NO-GO 清单

手上必须有：

- [ ] P1 交付的逐 draw 逐字段语义等价证明（40 trace + 全部集成测试零分歧）
- [ ] 两个后端上都已推送的渲染状态，`SyncRenderState` 693 行一行未动
- [ ] 两片 Track H 的实测单位成本（Espryt 0b、Magma 子系统 4）
- [ ] 两台设备（Adreno 830 `35d0befa`、Mali `3B159D009VZ00000`）reboot-clean 配对的逐线程 CPU 时间增量，p50 与 p99
- [ ] tracker 每 draw 的**绝对 ns**（上限从设备基线定：稳态每 draw 6.5–9.3 次 accessor + memo 探测，见 `MEASUREMENTS.md`）
- [ ] Blaze3D blend-toggle 微基准（enable/draw/disable/draw，MC batch 速率）
- [ ] 负面对照：关掉 CSO 内容寻址重跑，把"推送更慢"与"CSO 设计更慢"分开

判据与出口：

- **继续**：两台设备 p50 与 p99 逐线程 CPU 增量都不为负；tracker 绝对 ns 在上限内；Track H 单位成本不超出估计的 50%。按两条跑道推进。
- **收缩为 headless 工装用途或重新评估**：任一判据落空。**不回滚**：P0/P0.5/P1/P2 的产物（句柄基建与重键、两个头文件抽取、计数器、verify harness、渲染状态 CSO）全是自洽的 monolith 交付物，留在 `dev`；MGPipe 收缩为 `MG_Test` mock 后端 → MGPipe recorder（给 trace_replay 一种记录已解析状态的录制格式）+ `inproc` 渲染线程实验；IPC 跑道搁置到出现新判据。
- 沉没成本：P0 与 P0.5 无论走哪条路都要花（后者本身是 monolith 净收益）；真正只为 MGPipe 押上的是 P1 + P2 ≈ 28–39 天，NO-GO 分支下仍留下上述产物。

## 再基线检查点

| 触发 | 动作 |
|---|---|
| P3a > 27 天 | "窄句柄化"的前提错了，P4a 开始前重定基线 |
| P4a > 39 天 | 同上 |
| P7 中点（第 40–52 工作日）完成子系统 < 40% | 立即重定基线（P3a 的检查点发现不了 Magma 特有的超期） |

任一触发，先跑 `inproc` 的证伪数字再决定是否继续。

## 仍然开放的问题

P0 已回答的不再列出（spike A 的域、spike B 的分档、`posix_spawn` 不可用、OOM 探测惯用法、`GetInteger64i_v`/`GetProgramiv` 退役、D21 与 `RenderbufferObject` lifetime id、动态 accessor 基线）。

1. **client 侧 dirty 走查的真实每 draw CPU 代价。** 拉取基线已实测为每 draw 6.5–9.3 次 accessor + memo 探测；推送要在这个数字下净减少。P2 的头号数字，逐线程 CPU + 绝对 ns，两台设备。
2. **真实语料上纹理重铸拉取的发生率。** `ImageBindableHint` 预防主因，但整格式再生在普通 `glTexImage` 格式变更上就触发。若 MC/Iris fixture 上非平凡，保留 LRU 从默认 0 升为强制并拿真预算。
3. **spike B 的 `untrusted_app` 域复核。** 两台设备的分档在 `shell` 域测得；T0 的 AHB socket 交接是每个与 SurfaceFlinger 共享 buffer 的应用都在走的路径，风险在 memfd/opaque-fd 腿上。从应用进程再跑一次 `extmem_probe`（spike A 的 exec 钩子已可用）。
4. **渲染状态的 wire 粒度。** chunk 划分定下来后，CSO LRU 容量（暂定 64）与 `set_dynamic_state` 的 chunk 粒度由计数器定。
5. **`FramebufferSrgb` / `DepthClamp` 的拍板。** 事实已清（无存储、`glEnable` 静默吞掉、六个读点恒 false、41 个 fixture 无一开启）；建议在 chunk 表冻结前补真存储并把 `FramebufferSrgb` 划进 pipeline 半边。由计划所有者拍板，**拍板前不冻结 chunk 表**。
6. **具名 UBO host payload 的形状（D-B8）。** 第一个数字已有：Magma 在 26.3 世界每帧重打包 331 KB 具名 UBO 字节，Espryt 为 0。要么冻结现在的第二变长尾形状，要么走备选（Magma 直接描述符绑定常驻 `VkBuffer` range，独立 `dev` PR + Iris 性能门）。
7. **`MG_Util` 的切割缝。** server 需要 SPIRV-Cross pass 流水线、ESSL 转译缓存、格式处理器、POST 探针；client 需要 glslang phase A/B 与反射层。P0.5 解决了 `ProgramObject.h` 一处，`MG_Util` 内部是否有干净的 Transpile-vs-Reflect 缝未审计。
8. **一份反射归档能否服务三个消费者**（Espryt 读前端表、Magma 跑 SPIRV-Reflect、`DirectVulkan.cpp` 为 `glGetProgramResource*` 又反射一遍）。
9. **viewport-array 回放能否塞进一次 `draw_vbo`**：`EndViewportRoutingPasses` 会 `InvalidateSyncedRenderState`，各遍之间观察到的状态是否与今天一致未验证。
10. **`ResidentSubData` 的不对称怎么收口。** null 项保住今天的行为；给 Magma 补真实现是行为变更，独立 `dev` PR。
11. **`SEG_STAGE` 的上限。** 六类新字节需要 P8 之后用 MC in-world 与 Create 两类 fixture 的 `stage-*` 计数器给 p99 占用；G3 分块路径需要设计与测试。
12. **P13 之后 split-only 渲染 bug 的 server 侧第二意见。** verify 构建 + recorder 只覆盖推送内容，不覆盖后端对它的解释。
13. **烘焙后的内部 shader 能否在没有活 `ProgramObject` 的情况下表达 uniform location 与 UBO 布局。** 未做原型。
14. **推送模型改变哪些按拉取模式调过的缓存命中率。** 幸存者容量在 P13 重调。
15. **monolith 的 `*IndirectCount` 不调 `SyncGpuWrites()` 是不是潜在缺口**（compute 写的 indirect buffer）。独立 `dev` 问题，拆分不得借机顺手修。
16. **索引宿主镜像的实际内存占用。** MC/Sodium/Iris 语料里 element-array buffer 总量未测；若显著超 64 MiB，退化路径的频率与代价必须实测。
17. **create-indirect fixture 在 Adreno 830 上的失败**是 `dev@81b17c0b` 就有的（基线 APK 复现），不是本分支造成；它是 P3a/P8 验收清单里的用例，需要先在 `dev` 上修。
