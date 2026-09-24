# 开放问题全文（原 `ROADMAP.md`「开放问题」，2026-09-24 移入）

> [`ROADMAP.md`](../ROADMAP.md) 只留每题一行的状态；这里是每一题的完整陈述、已答部分的出处与未答部分的判据。编号稳定，引用写 "开放问题 N"。

P0 已回答的不再列出（spike A 的域、spike B 的分档、`posix_spawn` 不可用、OOM 探测惯用法、`GetInteger64i_v`/`GetProgramiv` 退役、D21 与 `RenderbufferObject` lifetime id、动态 accessor 基线）。

1. **client 侧 dirty 走查的真实每 draw CPU 代价。** 已答（P2–P4a）：推送没有在拉取基线之下净减少；Release 下 P2 边界 +6–12%，P3a 在 VAO 切换密的 rd12 上再加 +17–19 pt，P4a 在 Espryt 上 +3.4–5.7 pt（`MEASUREMENTS.md` §3–§5）。用户 2026-09-08 接受，性能自此只记录。
2. **真实语料上纹理重铸拉取的发生率。** 已答（P4a）：可忽略，保留 LRU 维持默认 0——79 例 × 两后端 780 个统计窗口里共 2 次（`iris-photon`、`iris-derivative` 各 1，只在 DirectGLES）。但 P5b 下这条路径是具名 Fatal，正是这两条 trace 加 `create-indirect` GLES 的首阻塞（P9）。
3. **spike B 的 `untrusted_app` 域复核。** 两台设备的分档在 `shell` 域测得；从应用进程再跑一次 `extmem_probe`（spike A 的 exec 钩子已可用）。P11 前做。**2026-09-22 降级**：只对同机臂的 T0 / T1 有意义，stream 数据面不需要。
4. **渲染状态的 wire 粒度。** 已答（P2）：16 个边界 / 15 个 chunk，7 pipeline 396 B + 8 dynamic 772 B；CSO LRU 64 暂定，P13 重调。
5. **无存储的 capability。** 已答（P2）：`FramebufferSrgb`、`DepthClamp`、`TextureCubeMapSeamless` 三个都补了真存储，`sizeof(RenderStateParameters)` 仍 1168。
6. **具名 UBO host payload 的形状（D-B8）。** Magma 在 26.3 世界每帧重打包 331 KB 具名 UBO 字节，Espryt 为 0。要么冻结第二变长尾形状，要么走备选（Magma 直接描述符绑定常驻 `VkBuffer` range，独立 `dev` PR）。P7。
7. **`MG_Util` 的切割缝。** server 需要 SPIRV-Cross / ESSL 转译缓存 / 格式处理器 / POST 探针，client 需要 glslang 与反射层；`MG_Util` 内部是否有干净的 Transpile-vs-Reflect 缝未审计。P7。
8. **一份反射归档能否服务三个消费者。** Espryt 那一半已答（P4a）：能且不需要复制。Magma 的第一个消费者已由 P5f fm 答：`MagmaProgramSource` 全部从 wire `ProgramArchive` 取，无前端对象、无复制。第二个（storage block 表）今天是**否**且变差了：split 下 `MagmaProgramSource::EnsureStorageBlocks` **每 draw** 重跑 SPIRV-Reflect（monolith 臂 `DirectVulkan.cpp:177-200` 至少有持久缓存）；答案是往 `LinkArtifacts` 加有序 `storageBlocks`（数据已在 `blockReflection` 里），两个消费者都改读它。**已答（P7 wave 2-C，`dce9953d..cf7ca59f`）**：`storageBlocks` 入归档、两个消费者改读、每 draw SPIRV-Reflect 4→0；`g_programResourceCaches` 的 monolith 半边归 P13（G1）。
9. **viewport-array 回放能否塞进一次 `draw_vbo`**：各遍之间观察到的状态是否与今天一致未验证。P8。
10. **`ResidentSubData` 的不对称。** 已答一半（2026-09-22 核）：**Magma 的实现已补**（`VkBufferManager.cpp:123` monolith 表、`:148` wire 表，`fff9d639f`）。**仍未答：`kCapResidentSubData` 从未被 server 发布**——`MG_Backend/Init.cpp:192-219` 是全树唯一的 `SetCapabilityBits` 生产调用点，只 OR 了 XFB、三个 query 位与 run-ahead 位；于是 `PipeFill.cpp:848` 在 split 下恒 false，**两个后端在拆分模式下都退回 `BufferObject.cpp:711-716` 的就地 memcpy**，Magma 的实现在 split 里是死代码。**已答（P7 wave 2-C，ID-P7-20）**：`InitServerRoleCommon` 按 server 自己的 wire 资源表发布 `kCapResidentSubData`，镜像收到即验收；白盒「发出的是 opcode 49」在 P11 采纳档落地前不可达（§5.4 债）。
11. **`SEG_STAGE` 的上限。** 内容侧已答（2026-09-20）：**分块已实现**——buffer 范围走查（`1e7c372e`）与纹理整宽 slab（`9469d48e`）按 `MGPipeStageChunkBytes()`（默认 32 MiB 的 1/4 = 8 MiB）切成多条 `resource_subdata`，所以单次 128 MiB 上传不再需要显式 `MOBILEGL_IPC_STAGE_MB=256`；单片仍超 arena 或该 record 类型未接入分片时仍是 `Fatal{RingOverrun, "SEG_STAGE"}` 兜底。**仍未答（2026-09-22 收窄）**：a6 §6 实测没走查的只剩 **program archive**（唯一无界 blob）与 **`draw_vbo` 的 `NumDraws*12` range 尾**；宿主索引 span 无 producer（划掉），`set_global_constants` 有走查。二者的分片是 P6.5 sl 与 P8 共用的同一件事，stream 发送窗口对着它们定尺。MC in-world / Create 的占用分布仍未量（门 8 已给出两条负载的字节/帧与记录/帧，逐 blob 分布只在 integration 车道取到）。
12. **P13 之后 split-only 渲染 bug 的 server 侧第二意见。** verify 构建 + recorder 只覆盖推送内容，不覆盖后端对它的解释。
13. **烘焙后的内部 shader 能否在没有活 `ProgramObject` 的情况下表达 uniform location 与 UBO 布局。** 颜色 blit 已答（P5f fv：`WireColorBlit.{vert,frag}` 用 push constant，`WireColorBlitSpirv.h` 签进树）；窄化到深度 mip 程序（今天在 split 下被编译掉、其 split 路径是具名 Fatal）与直通 TCS（`ProgramFactory.cpp:4060-4262`，第三个未盘点的内部 shader 生成器）。**已答（P7 wave 2-B / B2）**：深度 mip 烘焙 (A)(D)（`WireDepthMipmap.*`，push constant，`MOBILEGL_BAKED_INTERNAL_SHADERS` 新鲜度门表驱动）与 (B′)（disaggregated 构建的 monolith 臂改用烘焙模块）落地；multisample resolve 第三套烘焙 shader 随 B2；直通 TCS 仍未盘点。
14. **推送模型改变哪些按拉取模式调过的缓存命中率。** 幸存者容量在 P13 重调。一条线索：设备上 26.3 Espryt 的 `sve` ≈ draw 数（每 draw 重发一次 sampler-view 集合），桌面只有 ~0.07/draw；列入 P3b/P4b 优化清单。
15. **monolith 的 `*IndirectCount` 不调 `SyncGpuWrites()` 是不是潜在缺口。** 独立 `dev` 问题，拆分不得借机顺手修。
16. **索引宿主镜像的实际内存占用。** MC / Sodium / Iris 语料里 element-array buffer 总量未测；若显著超 64 MiB，退化路径的频率与代价必须实测。P8。
17. **`create-indirect` fixture 在 Adreno 830 上的失败**是 `dev@81b17c0b` 就有的（基线 APK 复现），不是本分支造成；`rd12` + Magma 在两台 Adreno 830 上的 `scudo::reportMapError` 崩溃同样是 `dev` 侧。两者排除在设备 A/B 之外、留在桌面语料里；P8 要在 `create-indirect` 上断言 `roundtrips-per-frame == 0`，所以 `dev` 的修复在别人的关键路径上。
18. **split 两侧的负载平衡。** P5e 解锁出来的新问题：VD32（~3550 draws/帧）下 apply 线程 **11-12 ms**、client **15.7 ms**，**client 是瓶颈且打满 ~94%**。把工作从 client 挪到 apply 会直接降瓶颈——这在 lockstep 下毫无意义（两侧串行相加），**run-ahead 才使它成为一个杠杆**。供选：编码侧的 emitter、tracker 走查、持久映射的脏页追踪（`change_protection` 在侧写里占 1.3%）。需先重新侧写：等待消失后尾部各项的分母都变了。排在 P5e 之后；无主，2026-09-22 建议挂在路线图推完后的优化阶段，P6.5 的 2×2 矩阵车道顺带重新侧写
19. **链路带宽预算与压缩。** TCP 终局下 `SEG_STAGE` 字节/帧对的是 Wi-Fi / USB（量级 30–100 MB/s），不是内存；`ARCHITECTURE.md` §14 的 ~1 MB/帧预算在 60 fps 是 60 MB/s，已贴着 Wi-Fi 上限。是否对 stage 载荷做 LZ4 类压缩、纹理上传是否按内容哈希在 server 侧去重，要等门 8 的分布出来再定。P6.5 契约写下第一行之前必答。
20. **大单次上传的停顿。** 128 MiB 一次上传在 50 MB/s 上是 2.5 s；内容侧分块解决的是"装不装得下"，不是"要等多久"。跨机臂上要么接受并记录，要么给上传做异步化（与 P9 的语义一起想）。不假装解决。
21. **配对 / 认证的形式。** **已裁定（ID-P7-3，2026-09-22）：令牌足够，不做 TLS。** 威胁模型是「任何人都能连的远程代码路径」而不是窃听：令牌常量时间比较、≥16 字节、无令牌只 loopback、数据面以 `Welcome.dataNonce` 绑定到已认证的控制连接、fork 前认证。TLS 作为 P12 的开放项保留。
22. **client 平台范围。** 默认先 Linux + Android（现有 CI 能验），Windows client 第二批（`ShmSegmentWin32` 在；`pipe:` 具名拒绝作废、TCP 替代），macOS 仍不拆分。这决定 `ARCHITECTURE.md` §15.3 "Windows 机器不是正确性门"要不要改。
23. **半开连接的检测。** Wi-Fi 掉线不产生挂断事件；dl 的闩取自描述符挂断，TCP 上需要 keepalive / 心跳超时并**由传输层报告为挂断**——这与"绝不取自 apply 超时"不矛盾，写清楚免得重吵。P6.5 ct。
24. **E1 对照的重定义（ID-122）无主。** 建议并入 P6.5 的门设计：S8 的好路径断言（`SessionFaultCount() == 0`）已是它想表达的形状，缺的是一个能被证伪的负控。
