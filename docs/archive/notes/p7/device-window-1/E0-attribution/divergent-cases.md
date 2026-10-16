# E0 · 分歧 case 名单（本窗口最重要的产出）

E0a：39 例 × {monolith, inproc} × DirectVulkan × `--use-pbuffer` × 1 遍，APK `p7w1-9be62cbc`（`apk.sha256`），reboot-clean，`svc power stayon usb`。两臂各 39/39 回放完成；两臂所有有结果的 case 都逐位确定（`ssim vs first = 1.0`，E0b 的 3 遍确定性在 E1 上补做了 OpenRA 一遍——同样逐位相同）。

## 结论一句话

**inproc 对 monolith 的分歧 = 1 个像素分歧 case + 6 个进程死亡 case；36 个分母 case 里其余 29 个与 monolith 同读数**（`ssim vs golden` 之差 < 0.0005，都在阈值内）。

## 名单

| case | monolith | inproc（ARMED） | inproc（lockstep，E1） | 判定 | 归属 |
|---|---|---|---|---|---|
| `OpenRA` | **1.000000**（mismatch 0） | **0.976493989**（14658 px） | **0.988997893** | 像素分歧，**逐位确定**，两种模式都分歧、幅度随模式变 | **caps generation 0 的家族发射丢失**：client 日志第 6 行 `the server does not consume MGPipe subsystem 0x400 (TextureResources) ... callMask=0x0, caps generation 0` 出现在 `CapsMirror generation 1 adopted` **之前**——一条纹理族 verb 在首个 CapsSnapshot 之前被判「server 不消费，走 legacy pull」，server 从没收到那条记录；diff 图只有特定 sprite / 建筑错、背景全对。lockstep 丢得少（client 逐 verb 等待，snapshot 更早到）所以 0.989 > 0.976。**两个历史观测值 0.976494 / 0.988998 都是 OpenRA**，分别是 ARMED 与 lockstep。→ wave 2 **F1**（`MG_Remote/Client/CapsMirror.cpp:137-151` 在 placeholder 上作答） |
| `improved-transparency-minecraft-26.3` | pass | **SIGSEGV** | SIGSEGV | GL 线程 `__memcpy_aarch64_nt` 从 `libtrace_replay_runner.so` 写进映射缓冲，`SEGV_ACCERR`、页对齐地址 | **与后端无关**（E4a：DirectGLES inproc 同样 SEGV）、**与 persistent-map push 无关**（E4b：`MOBILEGL_IPC_PERSISTENT_BLOCK_KB=0` 仍 SEGV）→ wave 2 **X1** |
| `minecraft-1.21.1-neoforge-create-instancing-in-world` | pass | **SIGSEGV** | SIGSEGV | 同上形状 | 同上 → X1 |
| `minecraft-1.21.1-neoforge-create-indirect-in-world` | 0.000005（monolith 就红） | SIGSEGV | — | 同上形状 | ID-P7-4 已排除；崩溃形状同 X1，修好后照常记录 |
| `minecraft-1.21.4-fabric-iris-iterationt-in-world` | pass | **`Fatal{UnmigratedVerb, "Magma:mipmap-shader-format-or-shape"}`**（apply 线程） | 同 | 具名拒绝，确定 | → wave 2 **B**（`WireFramebuffer.inc` 的 wire mip 路径；lavapipe 上绿，Adreno 缺某格式的 BLIT 特性） |
| `minecraft-1.21.4-fabric-iris-iterationt-nodsa-in-world` | pass | 同上 | — | 同上 | → B |
| `minecraft-1.21.4-fabric-iris-bsl-esc-menu-854` | pass | **SIGABRT** `Scudo ERROR: internal map failure (Out of memory)`（apply 线程） | **SIGSEGV** | 内存上限：inproc 把两个角色的 shadow 装在一个进程里 | 记录；在 spawn（两进程）臂上复测（窗口 1b）后再定归属——若 spawn 过则是 inproc 的地址空间账（P8/P11 的题材），若 spawn 也 OOM 则是 Magma 的 staging 账 |
| `minecraft-1.21.11-main-menu` | 0.164984 | 0.164984 | — | 两臂逐字相同 | trace 自身的 UBO 对齐伪差，非分离缺陷（`exclusions.md`） |
| `minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world` | 0.008865 | 0.008865 | — | 两臂逐字相同 | Adreno 拒绝 photon 管线，monolith 级（`exclusions.md`） |

其余 29 个分母 case：两臂 `ssim vs golden` 之差均 < 0.0005（iris 光影 case 两臂都在 0.996–0.9995，非光影 case 两臂都 ≥ 0.9999），逐位确定。完整数据：`E0a-monolith-summary.{txt,json}`、`E0a-inproc-summary.{txt,json}`、`../E1-lockstep/E1-summary.{txt,json}`。

## E2（credit 1 vs 3）与 E0b（3 遍确定性）

按 runbook §5 的判别，E1「不变」时 E2 不必跑：run-ahead 与 credit 整条线出局（OpenRA 在 lockstep 下也分歧，且崩溃 case 在 lockstep 下也崩）。E0b 的 3 遍确定性只在 OpenRA 上有意义，E1 已给出第二遍逐位相同（lockstep 臂内）；ARMED 下的三遍在 F1 修复后的复测里一起做。

## E3（caps 比对）

已在 OpenRA 上做：inproc 臂 `answering a PLACEHOLDER` 0 条、`does not consume MGPipe subsystem` **1 条**（0x400，generation 0）、`CapsMirror generation 1..5 adopted`（callMask 0x3fff00000460 → 0x3fff00000470）、`run-ahead ARMED` 1 条；monolith 臂无 wire 行。`GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT` 行两臂都没打（该日志行不在这条 trace 的路径上）。判别落在「能力协商」：与 runbook 表的预测一致。

## 与出口门 3 的关系

分母 36（`exclusions.md`）。今天 inproc 对 monolith：**29 同、1 像素分歧（F1）、5 死（X1 ×2 + B ×2 + bsl 内存）**，另 1（create-indirect）在分母外但同为 X1 形状。wave 2 的 F1 / X1 / B 三个包各自落地后按 `CONTRACT-P7.md` §7.2 复测（3 遍 + monolith 同会话对照）。
