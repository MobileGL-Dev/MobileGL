# P8-E：CopyImageSubData 由 server 的 staged store 跟随；死闩清理

> 2026-09-29，分支 `p8/e`（基线 `8d3e2317`）。简报 `~/w7/notes/p8/BRIEF-E.md`；证据 `~/w7/notes/p8/e-evidence/`。只在 Espryt；Magma 无代码改动，作对照。

## 1. 结论

- 审计的推断「split 下 copy 后 `glGetTexImage` 读到陈旧影子」不成立：split 的 `glGetTexImage` 不进 `DirectGLES::GetTexImage`，也不读任何影子。
- split 路径：client `EmitGetTexImage`（`MG_Remote/Client/TextureReadbackEmit.inc:158`）→ server `OnGetTextureImage`（`MG_Remote/Server/PipeApplier.cpp:443`）→ `ReadTextureImageWire`（`DirectGLES/WireTextureReadback.inc:422`）→ `ReadTextureLevelTight`（`DirectGLES/WireTextureReadback.inc:217`），只读 GPU（R32UI 字腿或 FBO）。
- 真实缺口：驱动不能 attach 的格式（Adreno：RGB16、16 位 SNORM）FBO 不完整（`DirectGLES/WireTextureReadback.inc:269`）→ `kStatusError`（`PipeApplier.cpp:462`）→ client `Fatal{ReplyError, "ReadPixels"}`（`EmitTables.cpp:903`）。与有没有 copy 无关，只上传过的层也结束会话。
- 主机 llvmpipe 能 attach 这些格式：不加旋钮时 split / spawn / tcp 与 monolith 全绿，所以要旋钮才能在主机上走到这条路（§4 R1）。
- 修法全在 server，不搬到 client：驱动拒绝时由 staged store 回答（store 路由），store 跟随 copy 做同一次 texel 块搬移（`FollowCopy`）。选「正确字节」，理由见 §2。
- monolith 同一缺陷（影子不随 copy 更新）记给 dev（§6）。

## 2. 设计

| 部件 | 位置 | 内容 |
|---|---|---|
| store 路由 | `DirectGLES/WireTextureReadback.inc:352` | 驱动拒绝回读时，store 仅在「整层覆盖、尺寸一致、纹理从未被驱动写过」时回答；转换同 `GetTexImageViaShadowConversion`（打包格式按字原样，其余解成宽 RGBA 逐行转） |
| 具名拒绝 / 武装标记 | `DirectGLES/WireTextureReadback.inc:411`，`:416` | 拒绝：`MGLOG_E_ONCE` `texture-readback-store-declined`，回复 error，client Fatal。回答：`MGLOG_W_ONCE` `texture-readback-from-store` |
| 驱动写标记 | `MG_Remote/Server/StagedTextureStore.h:479-514` | 按纹理、粘性；只有 `Drop` / `DropAll` 清，`ResetLevels` 不清（附件在重定义后照样写） |
| 标记来源 | `DirectGLES/Managers.cpp:10991` | `NoteDriverSideTextureWriteByHandle` 同时标 store：附件、可写 image、`CopyTexSubImage`、生成 mip 都经过它 |
| store 跟随 copy | `StagedTextureStore.h:519`，`DirectGLES/Managers.cpp:11014`，调用点 `DirectGLES/DirectGLES.cpp:13670-13688` | 两端同一内部格式（`DirectGLES/Managers.cpp:11036`）、单 upload target（非 cube / 1D array）、源整层覆盖且未被驱动写、目标整层覆盖或 copy 覆盖整层 → store 做同一搬移；否则给目标标驱动写 |
| copy 失败 | `DirectGLES/DirectGLES.cpp:13763` | 驱动拒绝 copy 时 store 已搬，补标驱动写 |
| 跟随后的 client 上传 | `DirectGLES/Managers.cpp:3197`，`StagedTextureStore.h:573` | client run 是 client 影子（没见过 copy），只落 record 声明写过的盒（regions，无 region 时 union box）；放不下则照常收养并标驱动写；`AdoptRun` 兜底同样标（`StagedTextureStore.h:353`） |
| 测试旋钮 | `DirectGLES/WireTextureReadback.inc:328` | `MGITEST_ESPRYT_REFUSE_TEXTURE_READBACK_EXTENT=<w>x<h>`，server 读；该尺寸颜色层的 FBO 腿视为驱动拒绝，R32UI 字腿不动 |

不选「标 GPU 脏」：

- Espryt 的 `GpuDirty` 语义是「需要从 store 重传」（`DirectGLES/Managers.cpp:8550`），标了会把 store 旧字节传回去盖掉 GPU 上的 copy。
- 跟随后其他整层读 store 的路径也拿到对的字节：重铸回读被拒时的 W2-b 重放（`DirectGLES/Managers.cpp:6978`）、整层重传。
- 跟随不了的形状（跨格式、renderbuffer 源、被驱动写过的源、cube / 1D array、视图）降为具名拒绝，不给旧字节。

旋钮按尺寸定界：

- tcp 的 server 是整条 lane 共用的 `TcpServer.Start` fixture，旋钮只能放进它的 ENVIRONMENT（`MG_IntegrationTest/CMakeLists.txt:3268`）。
- 场景只有自身环境里有旋钮时才建 13x7 的纹理，否则 12x6（`CopyImageStoreReadbackScenario.cpp:66`），所以 lane 级旋钮只碰 StoreRead 条目。
- 实测：tcp 上无旋钮条目的 server 日志没有 `texture-readback-from-store`，StoreRead 条目有。

## 3. 场景与登记

`CopyImageStoreReadbackScenario`（`MG_IntegrationTest/Scenarios/CopyImageStoreReadbackScenario.cpp:251-337`）；格式 RGB16、RGBA16_SNORM、RGB16_SNORM、RGBA16，RGB9_E5 为正对照（R32UI 字腿）。

| 例 | 内容 |
|---|---|
| `AnUploadedLevelReadsBackExactly` | 上传后读回（对照） |
| `ACopiedWindowReadsBackOverTheUpload` | 可变纹理，5x3 窗口 copy 后读目标与源 |
| `ACopiedWindowReadsBackFromAnImmutableLevel` | 同上，`glTexStorage2D` |
| `APartialUploadAfterTheCopyKeepsTheCopiedWindow` | copy 后在同层别处 `glTexSubImage2D` |
| `ARenderbufferSourceLandsInTheTexture` | RGBA16 renderbuffer → 纹理（handle 臂，证明 `+RENDERBUFFER` 闩已死） |

| 条目 | 臂 | 登记 |
|---|---|---|
| `DirectGLES.*` / `DirectVulkan.*` | monolith | 整体发现 |
| `DirectGLES.{Split,Spawn,Tcp}.*` | 三臂，无旋钮 | `mgl_itest_register_split_arms`（`MG_IntegrationTest/CMakeLists.txt:3252`） |
| `DirectGLES.{Split,Spawn,Tcp}.StoreRead.*` | 三臂，旋钮 13x7，前 4 例 | `MG_IntegrationTest/CMakeLists.txt:3270-3296`，带 `integration-gpu` |
| `DirectVulkan.{Split,Spawn,Tcp}.CopyStore.*` | Magma 门控三臂 | `MG_IntegrationTest/CMakeLists.txt:3304`；另有 tier 2 信息层 |

- 三臂对称，`spawn_lane_parity.py` 不需例外行（`StoreRead.` 尾不被 Espryt 的 `CASE` 解析，`scripts/ci/spawn_lane_parity.py:39`）。
- store 单元用例 6 个：`MG_Test/Wire/StagedTextureStoreTest.cpp:330-475`。

## 4. red-once

| # | 变异 | 载体 | 红 |
|---|---|---|---|
| R1 | 基线代码 + 旋钮（无 store 路由） | StoreRead 12 条 | 12/12 `Fatal{ReplyError, "ReadPixels"}`，只上传的对照也死 |
| M0 | server 跳过 copy | 无旋钮 Espryt 三臂 15 条 | 12 红（4 例 × 3 臂），读到 copy 前字节；上传对照绿 |
| M1 | store 既不跟随也不标记 | StoreRead | 9/12 红，读到 copy 前字节（简报的「去掉即陈旧字节」） |
| M2 | 不跟随但标记 | StoreRead | 9/12 红，`Fatal{ReplyError}`（具名拒绝） |
| M3 | 关掉盒合并与 `AdoptRun` 兜底 | StoreRead 部分上传例 + 单元 | 3 条读到旧 copy 窗口 + `AWholeRunOverAFollowedCopyMarksTheTextureDriverWritten` 红 |
| M4 | store 路由忽略驱动写标记 | renderbuffer 例 + 旋钮 12x6（手动） | 旧字节；未变异时是 `texture-readback-store-declined` |
| U1 | `FollowCopy` 信任被驱动写过的源 | `ACopyTheStoreCannotVouchForIsNotFollowed` | 红 |
| U2 | `ResetLevels` 清驱动写标记 | `TheDriverWrittenMarkOutlivesAResetAndDiesWithTheKey` | 红 |
| U3 | `AdoptClientBoxes` 落整条 run | `AClientRunOverAFollowedCopyLandsOnlyItsBoxes` | 红 |
| MV | Magma 跳过 copy | Magma 35 条 | 28 红（4 例 × 7 条目），上传对照绿 |
| MONO | monolith `GetTexImage` 强制走影子（临时探针，不提交） | `DirectGLES.CopyImageStoreReadbackScenario.*` | 4 个 copy 例 × 4 个 16 位格式读到 copy 前字节；RGB9_E5 与上传对照绿 |

每次变异后按字节复制还原、`touch`、`cmp` 一致；日志在证据目录（`redonce-*.log`、`red1-head-knob-all.log`、`monolith-probe-shadow.log`）。

## 5. 死闩清理（无行为变化）

- `CopyImageSubData+RENDERBUFFER`：原在 `SyncRenderbufferObjectToBackend` 内，条件是 transport≠monolith；唯一调用点只在 monolith 臂（split 臂按句柄处理，`DirectGLES/DirectGLES.cpp:13461-13482`），删去并改写注释（`DirectGLES/DirectGLES.cpp:13437-13444`）。
- 镜像函数里过时的「client 侧镜像留给 P8」「由两个 Fatal 兜底」注释改写（`DirectGLES/DirectGLES.cpp:13566-13579`）。
- Fatal 普查：abort 点 79 → 78（`DirectGLES.cpp` 15 → 14），族 45 不变；基线用 `fatal_census.py --write-baseline` 重写。

## 6. monolith 缺陷（ID-P8-3，记给 dev，不在此修）

- `MirrorCopyImageIntoDestinationShadow` 只对 `HasRedundantPackedEncoding`（仅 RGB9_E5）调用（`DirectGLES/DirectGLES.cpp:13773`）；其他格式 copy 后 CPU 影子不变。
- 驱动 attach 不了目标格式时 `GetTexImage` 退到影子（`DirectGLES/DirectGLES.cpp:16071`）→ 返回 copy 前字节。设备（Adreno：RGB16 / 16 位 SNORM / 无 norm16 时的 RGBA16）可达。
- red-once 见 §4 MONO；修法建议：镜像对所有非压缩同格式 copy 都做，或像 split 一样按纹理记「驱动写过」后拒绝。

## 7. 未做 / 发现未修

- `copy-image-shadow-mirror` 的 `MGPipeUnmigratedEmulation` 调用在 transport 下不可达（`DirectGLES/DirectGLES.cpp:13580` 先返回），本包未删；P8-SE 已删调用与名（`DirectGLES/DirectGLES.cpp:13582-13585`、`PipeCatalogueTest.cpp:1448-1450`，[`SE.md`](SE.md)）。
- `MGPipeTypes.h:1535` 的 P5b 历史注释仍提 `+RENDERBUFFER` 拒绝；是 wire 头注释，未动。
- 跨格式 copy 不跟随（store 存规范影子，16 位打包、RGB10 / RGB12 的规范表示不是 GL 位），降为具名拒绝。
- 具名拒绝仍是会话 Fatal（client 的 `RequireReadbackReplyComplete`）；改成 `GL_INVALID_OPERATION` 需要协议变更，不在本包。
- 深度格式经 `BlitDepthTexture2D` 的 copy 由 store 按原样搬移；若该仿真非逐位，store 与 GPU 可能有精度差，只影响驱动拒绝深度回读的情形。
- 未上设备验证（简报：只有 S 包用手机）。

- tcp 的 lane 级旋钮今后会碰到任何读回 13x7x1 颜色层的 tcp 条目；现有场景无 13x7 纹理（grep），现有 357 个 tcp 条目在它下面全过。
- 设备上走 store 路由时，`ReadTextureLevelTight` 先打一行 `MGLOG_E_ONCE` `framebuffer status=`（`DirectGLES/WireTextureReadback.inc:269`），随后才是成功的 `texture-readback-from-store`；日志里会有一行 ERROR 在正常路径上。

## 8. 门

`gate.sh ~/w7/p8-e p8e`，头 `00d6c248`，dirty 0（`~/w7/notes/p8/e-evidence/gate-p8e.log`）。

| 项 | 读数 |
|---|---|
| G1 | `added=0 removed=0`；`.text` a5ba13 与基线相同 |
| Fatal 普查 | 78 abort 点 / 20 文件 / 45 族 / 0 未标记（79 → 78，§5） |
| link ratchet | 172 不变 |
| parity errors | 0 |
| protocol pin / wire-declines | revision 5 / 53 行 53 处 0 未记 |
| hygiene | 12/12 OK |
| unit | 2720 / 2720 |
| integration-split / spawn / tcp | 436 / 355 / 357，全过 |
| magma split / spawn / tcp / full-split | 256 / 233 / 202 / 641，全过 |
| integration-gpu[monolith] / [inproc] | 2725 / 2725 |
| retrace（`-L retrace`，236） | 230 过；6 红 = 已知 `iterationrp-in-world` llvmpipe JIT 崩溃（monolith 两后端 + split / spawn 两后端）；split 76、spawn 76 条全过（`retrace-p8e.log`） |
