# P8-MF：dev 的 Adreno 830 部分冲刷修复镜像到 push 梯子

> 2026-09-30，分支 `p8/mf`（基线 `37174242` = `p8/main` 合入 dev `f973008c`）。简报 `~/w7/notes/p8/BRIEF-MF.md`；全文 `~/w7/notes/p8/mf-report.md`；证据（设备归档、探针日志、脚本、APK 哈希）`~/w7/notes/p8/mf-evidence/`（树外）。只动 Espryt：Magma 没有这条梯子，create-instancing × Magma 本来就是 0.999983725（MD）。

## 1. 改动

| 提交 | 位置 | 内容 |
|---|---|---|
| `ac9ab078` | `MobileGL/MG_Backend/DirectGLES/Managers.cpp:1520` | `FlushPendingRangesFrom` 的 ring 档每次 `glCopyBufferSubData` 后盖 `ringCopyRetireSerial = CurrentFrameSerial() + 1`（同 pull 臂 `Managers.cpp:1808`） |
| `ac9ab078` | `MobileGL/MG_Backend/DirectGLES/Managers.cpp:1496` | 逐 range 读 `ringCopyRetireSerial > CompletedFrameSerial()`（同 pull 臂 `:1785`），传给 `InvalidateFlushAccessFor` |
| `ac9ab078` | `MobileGL/MG_Backend/DirectGLES/Managers.h:1222` | 纯函数 `InvalidateFlushAccessFor` 新参数 `ringCopyInFlight`：部分范围且拷贝未退役 → 0（走 ring，GPU 序排在拷贝后）；整 buffer 孤立映射豁免 |
| `ac9ab078` | `MobileGL/MG_Test/Buffer/SplitBufferTest.cpp:865` | `EsprytFlushLadder.APartialRangeDoesNotOvertakeAQueuedRingCopy`（pull 构建同名 skip 桩 `:886`）；原 5 例补 `false` |
| `9c24a14d` | `scripts/p3a_untouched_regions.sh`、`scripts/p4a_untouched_regions.sh` | G5 第十行 `FlushPendingRangesNow` 也改为钉住行；两条梯子一起重钉在审过的修后正文：Now `c40cae66…`、From `33bc4cb8…`，提交 `ac9ab078`，裁定 ID-P8-14 |

- pull 臂 `FlushPendingRangesNow` 未动：`37174242` 的正文 = `37da3c3a` 正文 + 恰好 `f973008c` 的那一块（dev 自己的正文另缺 feat 既有的 PipeStats 块，所以 `f973008c` 处哈希是 `c85443ce…`，不是钉值）。
- 不盖 `DrainResidentWritesNow` 的 ring 拷贝：目标是收养的不可变 store，永不走范围失效映射（dev 同样不盖）。

## 2. 设备（红米 2f7cbe2e，create-instancing × Espryt，SSIM）

| APK（lib sha256 前 8） | monolith ×3 | spawn ×3（pbuffer） |
|---|---|---|
| 基线 `37174242`（`5ed6d6dc`） | 0.870389049 ×3 | 0.999984504 ×3 |
| 修复 `9c24a14d`（`34d826c7`） | **0.999984504 ×3** | 0.999984504 ×3 |
| red-once：守卫关（`7938ff89`） | 0.870389049 ×3 | — |

回归（Espryt monolith ×1，基线 → 修复）：sodium 0.999993736 → 0.999993736；common-mods 0.999869638 → 0.999869638；improved-transparency-26.3 0.999683991 → 0.999683991。

## 3. 为什么 spawn 不受影响：同样撞上，但拷贝在映射前已提交

探针 APK（守卫关 = 基线行为，`FlushPendingRangesFrom` 每个 range 一行日志，未提交；`04e09442`），monolith / spawn 各 1 次；主机 llvmpipe 三臂同一探针数字完全一致。

| 量 | monolith | spawn（server） |
|---|---|---|
| SSIM | 0.870389049 | 0.999984504 |
| 冲刷的 range / store | 3340 / 11 | 12956 / 26 |
| ring 拷贝 | 3337 | 3337 |
| **部分失效映射且该 store 仍有未退役 ring 拷贝** | **2**（store 38、63，`[3518480,+217120)`、`[3279760,+209360)`） | **2**（同偏移、同大小的两个 arena） |
| 整 buffer 孤立映射 | 1 | 9617（158 MB；24 B–307 KB 的小 store，monolith 下它们不进这条梯子） |
| 该 arena 上次 ring 拷贝与部分映射之间，其他 store 的 range | 4，其中整 buffer 映射 0 | 32 / 34，其中整 buffer 映射 28 / 30 |

- server 走的是同一条梯子：客户端的 `glCopyBufferSubData` 是 CPU 拷贝（`MobileGL/MG_State/GLState/BufferState/BufferObject.cpp:809-831`），以 `resource_subdata` 到 server，`Ops_H_SubData` 排队（`MobileGL/MG_Backend/DirectGLES/Managers.cpp:2469`），draw 时 `EnsureBufferResourceForHandle` 冲刷（`Managers.cpp:4247`）。
- T0 只收养了 16 MiB 持久暂存 buffer（server 日志 `T0 imported store {slot=1, gen=1} - 16777216 bytes`），3.8 MiB arena 不是 T0，不走 `Managers.cpp:2412` 的 resident 分支。
- 机理探针（守卫关 + 每次 ring 拷贝后 `glFlush`，未提交；`754c5d64`）：monolith 0.999984504。拷贝只在部分映射到来时仍未提交才会丢。
- 结论：spawn 以同样的 tier 组合撞上同样两处，但 server 在拷贝与映射之间多做了 28–30 次其他 store 的整 buffer 孤立映射，拷贝在映射前已被提交（推断：具体是哪一步触发提交未单独验证）。spawn 是碰巧躲过；修复后 spawn 不变（0.999984504 ×3）。

## 4. red-once

| 门 | 变异 | 载体 | 红 |
|---|---|---|---|
| 设备修复 | `Managers.h:1222` 守卫改 `false && …`（盖戳保留），APK `7938ff89` | create-instancing × Espryt × monolith ×3 | 0.870389049 ×3 |
| 新单测（守卫） | 同上，主机 | `EsprytFlushLadder.APartialRangeDoesNotOvertakeAQueuedRingCopy` | `SplitBufferTest.cpp:868`：得 6（WRITE\|INVALIDATE_RANGE），期望 0 |
| 新单测（整 buffer 豁免） | 守卫去掉 `!wholeBuffer &&` | 同上 | `SplitBufferTest.cpp:871`：得 0，期望 10（WRITE\|INVALIDATE_BUFFER） |
| G5 重钉 | 提交 `ac9ab078` 对旧脚本（`HEAD:` 版本） | `p3a`/`p4a` `37da3c3a HEAD` | 两脚本 rc=1：首个移动 `FlushPendingRangesNow`，另移动 `FlushPendingRangesFrom` |
| G5 From 钉值 | 新脚本对基线提交 | `p3a`/`p4a` `37da3c3a 37174242` | rc=1：首个移动 `FlushPendingRangesFrom`，并报 "IS A PINNED ROW (ID-P8-14)" |

- 每个探针都以拷回 + 比对 + `touch` 恢复（`restored identical: True`、`git diff` 干净）。
- G5 `--self-test`：p3a 两条梯子各有 pin-precedence 控制与一 token 变异控制，全过；p4a 10 个负控制全触发且点名。

## 5. 门（`gate.sh ~/w7/p8-mf p8mf`，头 `9c24a14d`）

| 项 | 结果 |
|---|---|
| G1 | 符号增 0 减 0（clang 22.1.6）；`.text` `0xa5ba43`，与本树基线构建相同 |
| fatal census / link ratchet / parity / 协议修订 | 78 / 172 不变 / 0 / 5 |
| wire-declines | 58 行，59 站点 |
| 卫生 | 14/14 OK（含 `g5_p3a`、`g5_p4a`） |
| unit | 2722 / 2722（+1 新单测） |
| split / spawn / tcp | 738 / 652 / 664 全过 |
| Magma split / spawn / tcp / full-split | 271 / 248 / 216 / 671 全过 |
| gpu monolith / inproc | 两臂各 3695 / 3699，同 4 例：`DirectGLES.Tcp.UnlocatedIoBlocks.*` 3 例 + 其 fixture `TcpServer.Start.P8aUnlocatedIoBlocks`（负载下）；单独重跑两臂各 31/31 过 |
| retrace（`ctest -L retrace`，monolith + .SPLIT + .SPAWN，两后端） | 230 / 236，失败只有已知 6 例 iterationrp（主机 llvmpipe JIT） |

## 6. 未做 / 遗留

| 项 | 原因 |
|---|---|
| server 对小 store 反复整 buffer 孤立映射（create-instancing 一次重放 9617 次 / 158 MB，monolith 1 次） | 性能项，不是本包；它恰好让 spawn 躲过本缺陷 |
| 其他 GPU 写（SSBO、xfb）后接部分失效映射是否同样丢 | 未观察到；守卫只覆盖 ring 拷贝（同 MD） |
| 两臂之间的注释仍写 pull 臂冻结于 `5cb826b0`、From 钉于 `3e298c9a`（`MobileGL/MG_Backend/DirectGLES/Managers.cpp:1376`、`:1535`、`:1553`；`MobileGL/MG_Test/Buffer/SplitBufferTest.cpp:877`） | ID-41 起就过时，ID-P8-14 后 pull 行也改为钉住；都在哈希正文之外，留给下次改这段的人 |
