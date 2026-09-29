# W1：读进 pack buffer 的回读不再等回复

> 2026-09-29。协议与行为见 `MobileGL/MG_Remote/CONTRACT-P9.md` §1、§4；裁定 `INTEGRATOR-DECISIONS-P9.md` ID-P9-1..4。本页只记为什么这样做、门与红一次的证据。

## 选了哪条路

交接 §3 W1 给了两条：(a) client 只标"GPU 写过"，数据留在 server，CPU 读时惰性取回；(b) server 读完主动发 `OnBufferWriteback`。取 (a)：

| 点 | (a) 标记 + 惰性取回 | (b) server 主动推 |
|---|---|---|
| client 在读的那一刻 | 不等 | 不等 |
| 此后 CPU 读 | 一次 `resource_readback` 往返（既有路径） | 要知道"推送还在路上"并等它，另一套在飞状态 |
| 从不读的 PBO（只作 UNPACK 源或丢弃） | 零字节过线 | 整块字节白推 |
| 新机制 | 无：`GpuWritePending` 第 4 行 P5 就留好了，只是没接线 | 新的排序与等待 |

server 侧不用驱动的 PBO 读，而是读紧排像素后走 applier 的 buffer 写入口（契约 §1 server）：Espryt 的 buffer 有两份存储（GL buffer 与 R-11 暂存影子），驱动写只到前者，后续从暂存影子排空的范围会把旧字节盖回；Magma 的 `ReadPixels` 本来就没有 pack 路径。走写入口两后端同形、零新后端代码。

## 顺带补上的洞

UNPACK 源读取（`glTexSubImage*` 等以 `GL_PIXEL_UNPACK_BUFFER` 为源，`GL_Texture.cpp` 8 处）直接读 client 影子、从不调 `SyncGpuWrites`。monolith 里 PBO 读完就回写影子所以无事；split 下读进 pack buffer 后立刻当 UNPACK 源上传（GPU→GPU 的常见写法）会上传旧字节。现在 split 下先同步。同一个洞以前对"着色器写过的 SSBO 当 UNPACK 源"也存在，一并关掉（monolith 不动）。

## 门与红一次

| 门 | 绿 | 红一次（怎么变红、哪条断言） |
|---|---|---|
| `PackBufferReadbackScenario`（6 条对等用例 × 单进程/Espryt split·spawn·tcp/Magma split·spawn·tcp） | 全绿 | 去掉 unpack 同步：`APackBufferReadFeedsAnUploadFromTheSameBuffer` 在 split 臂红（见下） |
| 计数门 `APackBufferReadPostsNoReplyAndMarksTheBuffer`（split 臂） | 回复槽记录增量 0，标记增量 = 读次数 | `MOBILEGL_IPC_PBO_READBACK_SYNC=1`：两条计数断言都红（见下） |
| F1 `RemoteClientControls.APackReadOwedWhenTheDeviceIsLostNeverReadsBackAsTheOldBytes` | 以 `Fatal{ReadbackDeclined, "buffer-writeback"}` 结束 | 去掉 `AwaitBufferWriteback` 的 device-lost 分支：abort 名为 `UnimplementedWritebackWait`，期望的族名找不到 → 红。（设 `MOBILEGL_IPC_PBO_READBACK_SYNC=1` 跑它**不红**：单元子进程不走 `ConfigLoader`，环境旋钮不生效；用例里的 `packReads != 1 → exit 151` 只是前提守卫。） |
| 既有 `BandedReadbackScenario` / `PixelStoreSweepScenario`（含 PBO 用例） | 全绿 | — |
| G1 | 增 0 减 0，`.text` 0xa52203 | — |
| verify 构建（`MOBILEGL_PIPE_VERIFY`）unit / integration-verify / integration-verify-split | 2626 / 1202 / 1136 全绿 | — |
| retrace（主机，39 例 × 两后端 × monolith/split/spawn，236 项） | 230 过；`iris-iterationrp-in-world` 6 项在**所有臂含 monolith**上挂于宿主 Mesa llvmpipe 的 JIT（`LLVM ERROR: Cannot select: intrinsic %llvm.x86.vcvtps2ph.256`），环境问题，与本包无关 | — |

红一次的原始输出：`~/w7/logs/p9/redonce.log`（WSL），摘录：

```
RED (a) MOBILEGL_IPC_PBO_READBACK_SYNC=1, DirectGLES.Split + DirectVulkan.Split.Pbo:
  PackBufferReadbackScenario.cpp:440  reply-slot records: 65, expected 0
  PackBufferReadbackScenario.cpp:444  pack-buffer marks:   0, expected 5
RED (b2) device-lost arm removed:
  MGPipe: Fatal{UnimplementedWritebackWait} - ... buffer 1 still has an outstanding GPU write ...
RED (c) unpack sync removed, DirectGLES.{Split,Spawn,Tcp} + DirectVulkan.{Split,Spawn,Tcp}.Pbo:
  493 texels of the uploaded copy differ from the pattern, first at (0,0)   (6/6 arms)
```

两处临时改动从备份拷回、`git diff --stat` 只剩本包的改动、重编通过后再跑门。
