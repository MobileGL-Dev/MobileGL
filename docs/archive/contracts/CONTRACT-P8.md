# CONTRACT-P8：server 侧仿真缺口与 split 覆盖

> P8（2026-09-29 起，重定界见 `docs/Disaggregated/notes/p8/PLAN-P8.md`）落地后的规则。每条写明：规则、代码落点、守住它的用例。包说明见 `docs/Disaggregated/notes/p8/{A,B,C,D,E,F,SE,SV,MF,S}.md`，裁定见 `INTEGRATOR-DECISIONS-P8.md`。

## 1. Espryt 生成 mip（B、SE）

- server 按记录的生成窗口 [BASE+1, min(MAX+1, 存储层数, 链长)) 生成，与 Magma 同口径：`GenerateMipmapWindowByRecord`（`DirectGLES.cpp:11759`）；两条 blit 链都走这个窗口。
- RGB16F / RGB32F：先试驱动原生生成；驱动拒绝时 server 从**自己的**基级做 2×2 box，永不读 client 影子。
- 基级读不回来时，暂存 store 只在「整级、extent 相符、未被驱动写过」时可代替；否则具名拒绝 `generate-mipmap-store-declined`（`DirectGLES.cpp:13134`），生成的层保持原样。driver-written 标在本次生成自己打标之前读取。
- 用例：`GenerateMipmapServerScenario`（两后端 × monolith / Split / Spawn / Tcp；Espryt 强制 CPU 臂 `.CpuMip.`、`.CpuMipRefused.` 只上 Split / Spawn）。

## 2. Espryt server 暂存副本与 GPU 写（C）

- shader 写过的 buffer（SSBO / 原子计数、可写 buffer image、XFB）在 server 孪生体上打 `serverGpuWritten` 标。
- server 的 CPU 读者（restart 重写、multi-draw rebase、`*IndirectCount`、CPU 取用的 indirect 字段、XFB scatter、fp64 收窄）一律经 `SplitHostBytesForCpuRead`（`Managers.cpp:3613`）：有标或有排队常驻写时先刷新再读；T0 store 先落写再 `glFinish`。
- 用例：`GpuWrittenDrawInputScenario`（Espryt Split / Spawn / Tcp 门控）。

## 3. Magma wire 臂的 indirect（D、SV）

- `glDraw*Indirect` / `glMultiDraw*Indirect[Count]` 从 wire store 直接录成 `vkCmdDraw*Indirect[Count]`（`DrawWireIndirectNative`，`WireDraw.inc:627`）；CPU 展开只留给设备发不出的 COUNT 形态（与 monolith 同条件）。
- `glDispatchComputeIndirect` 同样原生（`WireDraw.inc:844`），无 CPU 路径。
- 屏障规则：store 被 shader 写过（`MarkWireBufferGpuWritten`）后，首个 indirect 读之前录一次 MEMORY_WRITE → INDIRECT_COMMAND_READ。
- 用例：`WireIndirectDrawScenario`、`WireIndirectDispatchScenario`（计数经 `WireIndirectPeek`：原生可发时 CPU 展开 = 0）。

## 4. 驱动拒读的纹理回读（E）

- split 的 `glGetTexImage` 只读 GPU。驱动拒绝时由 server 暂存 store 作答，条件同 §1（整级、extent 相符、未被驱动写过），否则具名拒绝 `texture-readback-store-declined`（`DirectGLES/WireTextureReadback.inc:411`）。
- store 跟随同格式、单上传目标、源可信的 `glCopyImageSubData`（`FollowCopyImageInStagedStore`，`Managers.cpp:11032`）；跟不了的形状打粘性 driver-written 标。

## 5. 分类与拒绝（F、SV）

- 我们的 client 发不出的形状一律 `ProtocolCorruption`：multi-draw `CLIENT_INDICES` / `INSTANCED`（`MultiDrawShapeLatch`，`PipeApplier.cpp:779`）、Magma `client-vertex-array`。
- Magma 生成 mip 的形状 / 格式缺口是具名拒绝：`MipmapShaderFormatOrShape`、`MipmapDepthStencilAspect`（`WireFramebuffer.inc:1401`、`:1465`），判定在长链之前；3D 原生 blit 用 1 层、深度走 z 偏移。

## 6. 默认帧缓冲的 surface 发布（SE）

- push 构建里，只要 `OnSurfaceChanged` 回调在，Espryt 就发布默认帧缓冲的格式与尺寸，不再以本进程的前端占位为条件（`DirectGLES.cpp:16239`）；spawn / tcp 的 client 由此拿到 server surface 的真实深度模板格式。pull 构建不变。

## 7. Espryt flush 阶梯（MF，dev `f973008c`）

- 部分范围的失效映射不得越过同一 store 上 GPU 还没跑的环形拷贝：两条阶梯（push `FlushPendingRangesFrom`、pull `FlushPendingRangesNow`）都以 `ringCopyRetireSerial` 守住（`Managers.cpp:1496`、`:1785`）；整 buffer 映射除外。
- G5 两个脚本把两条阶梯的评审后正文钉在 ID-P8-14。

## 8. 覆盖与门（A、SE）

- 归一化前缀后，每个 monolith 用例要么登上 split 门控臂，要么在 `scripts/data/split_coverage_exemptions.txt` 里有原因类（mechanism / single-backend / no-records / pending-fix）：`scripts/ci/split_coverage.py`。
- `MGPipeUnmigratedEmulation` 的名单与调用点一一对应：`scripts/ci/unmigrated_emulation_sites.py`。
- server 读的旋钮车道：要么独立 tcp supervisor，要么只上 split + spawn，要么证明对其他用例惰性。

## 不变量

- G1：pull 构建符号不变（整阶段 0 / 0）；pull 的 `.text` 只随 dev 修复变化。
- 控制协议修订号不变（5）；线上格式不变。
