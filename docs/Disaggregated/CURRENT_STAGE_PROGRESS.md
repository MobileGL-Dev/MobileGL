# 当前阶段进度

分支 `feat/disaggregated`；代码头 `82683d4a`（2026-09-16），其后只有文档提交。本文随每次落地更新。ID-1..75 的逐条裁定长文在 git 历史（`ef35ea0c` 之前版本的本文件）。

## 1. 阶段状态

| 阶段 | 状态 | 范围 / 证据 |
|---|---|---|
| P0 / P0.5 / P1 / P2 / P3a / P4a（monolith 跑道） | 已落地 | `ROADMAP.md` 阶段表；`MEASUREMENTS.md` §1–§5 |
| **P5** 首个 IPC 帧（reduced path，lockstep inproc） | 已收官 | `ff2994d9..37fc4fdb`；`MEASUREMENTS.md` §6 |
| **P5b** inproc 下的 verb 迁移（Minecraft 优先） | **已收官（2026-09-16）** | `37fc4fdb..82683d4a`；`MEASUREMENTS.md` §7 |
| P6 spawn transport | 未开始 | 设备出口已完成，可以开始 |

## 2. 当前头实测

| 门 | 结果 |
|---|---|
| 构建 | pull / push / verify / split 四个 flavour 全部通过 |
| G1（pull 符号恒等） | `.text +0`，27814 符号 0 增 / 0 删 / 0 resize / 0 重命名 |
| 发射表分区（`EmitTables.cpp` 的 `static_assert`） | A（本地回答）2 / B（发射）54 / C（具名拒绝）15，共 71 槽 |
| unit | pull / push / verify 各 1817；split 2138（含 `7cb29d46` 后 `UserIndexSpan` 3/3） |
| `integration-split`（inproc） | **107/107**（P5 收官时 22） |
| `integration-gpu` | push 1148/1148；split 单体传输 1267/1267 |
| G5（p3a / p4a 保护区） | 双绿 |
| 主机收尾全门（`348d22a4`） | complete=true，exit 0：31 步 rc=0；retrace-push 79/79、retrace-verify 79/79（钉住库续跑合并）；OpenRA 2/2 |
| 集成普查（inproc 车道，`348d22a4`） | 1267 selected = 811 passed / 203 skipped / 62 aborted / 191 failed；对 c0b 基线零回退 |
| 79 trace 普查（inproc，stage 256 MiB，`348d22a4`） | **72 passed / 6 aborted / 1 failed** |
| Redmi 正确性（`82683d4a`，APK `p5bcodex2`，stage 256 MiB） | **8/8**：四条 A/B trace × 双后端首次在设备上 inproc 渲染 |
| Redmi 四臂 A/B | 24/32 组 200 帧尾验证全绿；barrier tax（split−push 逐线程 CPU p50）+5.9% – +18.2%；iris-bsl 8 组为 fixture 123 帧上限（pull 同失败），补充表另记 |

## 3. P5b 落地内容

| 包 | 迁移的槽 / 内容 | 效果 |
|---|---|---|
| c0b | 25 个已测量 class-C 槽的 wire 记录、`WireVerbSink` 分派、具名 Fatal stub、`CONTRACT-P5B.md` | 四包可并行 |
| f1 | `ClearBuffer{iv,uiv,fv,fi}`、`ClearNamedFramebuffer*`、`CopyTexImage2D`、`CopyTexSubImage2D`、`GenerateMipmap` | 34 个车道中止归零 |
| i1 | `BindImageTexture`、`DispatchCompute`、`CopyImageSubData`、`MemoryBarrier`、`ShaderStorageBlockBinding` + 两个伴随槽 | 235 个车道条目解锁 |
| t2 | 四个 stream-output span 行、XFB 对象绑定、`PatchParameteri`、`kCapBackendOwnsXfbCapture` | 138 个车道条目解锁 |
| d1 | 十九个索引 / 实例 / multi-draw / indirect 槽下沉到 `draw_vbo` | Minecraft 主阻塞解除 |
| sync | `FenceSync`、`ClientWaitSync`、`GetSyncStatus`、`WaitSync`、`DeleteSync` 上 apply 线程；client 铸造 Fence 句柄，wire 只过 `{slot,gen}` | 顺带修掉 `MGL_BACKEND_SLOT_PTR_LOCAL` 对 split 恒返回 nullptr |
| blit / mip | `BlitNamedFramebuffer` 经作用域化 client-shadow 绑定下沉；生成 mip 存储按推送描述符验证 | 31 + 7 个 trace 阻塞解除 |
| r1 / r2 | P5 收官审查 13 项（coherent-map 服务器侧绕过、FBO 死亡在 client 线程、`PACK_SWAP_BYTES`、`RingOverrun` 等待、CI 对照九项） | — |
| 收官审查修复 | 索引 span 上界（`Count × IndexSize` 必须装进声明的 run）、fence wait 预算、`Fatal{ReplyStatusInvalid, "ReadPixels"}` | P5b 审查 0 blocker |

## 4. 真实负载（inproc，独立 apply 线程）

- `DrawElements` 不再是阻塞：d1 普查时 77 个 Minecraft 后端用例中 28 个渲染通过（SSIM ≥ 0.99995）。
- blit + mip 之后：`improved-transparency-minecraft-26.3` DirectGLES SSIM 1.0 / DirectVulkan 0.999914；`minecraft-1.21.4-fabric-iris-bsl-in-world` DirectVulkan 0.997324、DirectGLES 0.997496；OpenRA 双后端 1.0。
- 79 trace 普查 7 条未过项的首阻塞：`rd12` GLES `Fatal{InitialBytesNotCarried,"resource_respecify"}`、VK `Fatal{BarrierTimeout,"Present"}`；`iris-photon` / `iris-derivative` / `create-indirect` GLES 均 `Fatal{UnmigratedEmulation,"texture-remint-pull"}`；`iris-bsl-esc-menu-854` GLES 同 rd12 GLES；`create-indirect` VK 在 llvmpipe 上内存膨胀（>60 GiB RSS，两次复现，守护杀死）。
- Redmi 出口（2026-09-16）：正确性 8/8；barrier tax 首测，逐例见 `MEASUREMENTS.md` §7.4。

## 5. 开放项（按优先级）

| 项 | 证据 / 去向 |
|---|---|
| `SEG_STAGE` 默认 32 MiB 装不下目标负载的单次 128 MiB 上传；**决定 = 默认不改**，普查与 Redmi 显式 `MOBILEGL_IPC_STAGE_MB=256`；分块 / 专用 carrier 留 P8（`ROADMAP.md` 开放问题 11） | `p5b-results/blit-codex-v1.md`；`MEASUREMENTS.md` §7.2 |
| Magma（DirectVulkan）split compute/image 路径：89 个错答中 82 个 | `p5b-results/i1-v1.md` |
| rd12 GLES `InitialBytesNotCarried/resource_respecify`、rd12 VK `BarrierTimeout/Present`、`iris-bsl-esc-menu-854` GLES、三条 `texture-remint-pull` 仿真槽、`create-indirect` VK 内存膨胀 | 79 trace 普查 `counts.json` / `trace-transitions.json` |
| RGB 三通道 CPU mip 回退仍是具名 Fatal | `p5b-results/mip-codex-v1.md` |
| P5b 的 inproc 依赖：具名 blit 的 scoped client binding + barrier、mip descriptor 的 barrier-held registry 查询、FBO death 的 inproc mailbox | P6 必须替换（`ROADMAP.md` 债务表） |
| 27 个 P5 inproc 错答：22 纹理读回走 client-shadow 回退、3 query、1 inspection、1 FBO/RBO 删除后生命期 | P4b / P7 / P6 债 |
| `rsp` 残余输入；`SEG_REPLY` 2 MiB 单槽上限；GetCaps 两个 blobref 的载体；PACK-PBO 读回真实形式 | P3b/P4b、P6+ |
| 15 个 class-C 槽（query / sync 尾 / `GetTexImage` / `SetSwapInterval` 等）无负载命中，仍具名拒绝 | P9 / P10 |
| Redmi 定频行只在树外 `~/w7/notes/p2/devices/pin_device.sh`；钉频口径 2026-09-16 起 1100 MHz（原 1050） | `devices/pin-verification-2026-09-07.md` |
| `test.yml` / `apk.yml` 的 `feat/disaggregated` 触发器是临时的，合入 dev 前必须移除 | — |

## 6. 下一步

1. **P6 spawn transport**：`SocketTransport` + `ServerMain` + 握手 / 退出语义；替换 P5b 的三处 inproc 依赖（上表）。
2. 剩余首阻塞一轮（Magma compute/image、rd12、RGB mip、`texture-remint-pull` 仿真槽）。
3. P6 出口门：P5b 的完整渲染路径在 `spawn` 下绿；OpenRA 在 Adreno 830 上 split SSIM ≥ 0.99。

## 7. 记录位置

| 内容 | 位置 |
|---|---|
| P5 brief / 契约 / 前言 | `~/w7/notes/p5/BRIEF-P5.md`、`MobileGL/MG_Remote/CONTRACT-P5.md`、`~/w7/notes/p5/PACKAGE-PREAMBLE.md` |
| P5 包报告与审查 | `~/w7/notes/p5/p5-results/`（`joint-v1.md` 全门记录、`ab-v1.md` 设备 A/B、`p5-close-codex-review.md`） |
| P5b brief / 契约 / 报告 | `~/w7/notes/p5b/BRIEF-P5B.md`、`MobileGL/MG_Remote/CONTRACT-P5B.md`、`~/w7/notes/p5b/p5b-results/` |
| P5b 收官主机门 | `~/w7/p5b-final-host-348d22a4/`（`reconciliation-20260916.md`、`summary-counts.{json,md}`） |
| P5b 合并普查 | `~/w7/p5b-final-census-348d22a4/`（+ `-resume1/2/3`）、`p5b-results/joint-codex-v1.md` census 块 |
| P5b 设备证据 | `~/w7/notes/p5b/apk/p5bcodex2/`（APK + proof）；`MobileGL/.trace-work/p5b-redmi/p5bcodex2/2f7cbe2e/`（correctness 8/8、`ab-tables.md` 与 bsl 补充表） |
| class-C 普查 | `~/w7/notes/p6/census-classC.md`；基线 `~/w7/p5b-c0b-census-logs/results.json` |
| 门日志 | `~/w7/p5-joint-gate.log`、`~/w7/p5b-quickgate.log`、`~/w7/p5-joint-evidence/` |
| 脚本 | `~/w7/notes/tools/`（`wsl_p5_gate.sh`、`p5_ab_redmi.sh`、`p5b_codex_redmi.sh`、`wsl_build_p5_apks.sh`、`p6_census_*.{sh,py}`、`p5b-c0b-census.sh`） |

## 8. 仍在生效的裁定（ID 索引；过程性裁定与 ID-1..39 见 git 历史）

| ID | 裁定 |
|---|---|
| 41 | G5 第十一行 `FlushPendingRangesFrom` 始终对固定 pin 比较；re-pin 必须同时改两份脚本 |
| 42 | persistent-map 成员性期望 `live == (arm == emulated)` |
| 43 | 带 CALL 旗标的 ring 头会被弹出，不当 wrap filler |
| 47 | `SEG_REPLY` 16 MiB / 8 × 2 MiB slot；超限在 client 侧具名拒绝；ABI fingerprint 尚未混入段尺寸 |
| 49 | ReadPixels 以紧凑形式过线：server 用中性 pack 读进回复，client 按应用 pack 散布 |
| 50/52 | 有传输时 `liveHostBase()` 不得回落到 client 的 `MappedData()` |
| 53 | 每个 `DirectGLES.Split.*` 条目一个私有日志；对照只从被选条目的私有日志取 Fatal |
| 54 | forwarder 每 (dpy, draw, read, ctx) tuple 只 bind 一次；client 的 release 不解绑 apply 线程的上下文 |
| 55 | R-17：33 行生成路由 + 4 escape，`PipeCatalogueTest` 钉 37；`&MGPipeApply*` 地址取用点同样在扫描内 |
| 57 | split 下绑定 PACK PBO 的 ReadPixels 具名拒绝；真实形式（server 写 buffer resource + client `MarkGpuWritten`）留 P6+ |
| 62 | 对照必须把"选中条目被跳过"判为失败 |
| 65 | 27 个 inproc 错答归 P4b/P7 债；普查只记录不设门 |
| 66 | 过程规则：不过度验证、轮次间不审查、每阶段收官由 Codex 审一次、只读 / 非核心任务派给 Codex |
| 67 | 相同 tuple 的重复 make-current 不重发 caps；不同 tuple 重发且 client 立即采纳 |
| 68 | class-C 普查决定迁移顺序（所有 Minecraft trace 首阻塞 = `DrawElements` → Minecraft 优先） |
| 76 | P5b 出口 = 主机门 `348d22a4` complete + 79 trace 普查 + Redmi 正确性 8/8 + 四臂 A/B |
| 77 | Redmi 钉频期望 1050 → 1100 MHz（2026-09-16）；iris-bsl 8 组判为 fixture 123 帧上限，补充表不进 200 帧尾门表 |
