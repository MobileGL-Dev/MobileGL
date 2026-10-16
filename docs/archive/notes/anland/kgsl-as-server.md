# 把 kgsl 看成 server：A / B 两条路线的另一种读法（2026-09-30）

> 接 [`README.md`](README.md) 的"对照：容器里现在的 Mesa"。只读推演，没做实验；Anland 事实链到审计版本（6.x `cc169180`、5.x `9ab13eb1`）。

## 结论

A / B 的区别不在"渲染器放在哪"，而在**缓冲归谁**：

- **A**：server 分配缓冲、画好，把句柄交给合成器。
- **B**：窗口把自己的缓冲交给 server，server 直接画进去。

把内核 kgsl 当 server，Anland 两代现有路径正好各占一条：6.x + Mesa 是 A，5.x 私有协议是 B。

## 对照

| | 6.x + Mesa | 5.x 私有协议 | MobileGL A | MobileGL B |
|---|---|---|---|---|
| client | 应用进程里的完整用户态驱动（freedreno / turnip） | 合成器进程（如 KWin）里的 Mesa | 应用进程里的 GL 前端 | 同 A |
| server | 内核 kgsl | 内核 kgsl | Android 用户态进程（厂商驱动，往下仍是 kgsl / kbase） | 同 A |
| 命令通道 | ioctl + 共享 GPU 内存 | 同左 | MGPipe（共享内存环 + 门铃） | 同 A |
| 缓冲归谁 | kgsl 分配 → dma-buf 给 client → `wl_buffer` → Anland 导入（伪造 AHB） | 窗口缓冲队列分配 → `data[0]` 以 dma-buf 交给 kgsl 渲染 → 回 sync_file → Android 端 `queueBuffer` | server 用 gralloc 分配 AHB → Anland | 窗口 Surface 交给 server |
| 形状 | A | B | A | B |

5.x 出处：[protocol.h:7–49](https://github.com/SuperTurtleDev/anland/blob/9ab13eb146bc9f3dd3cc44cabc268659d9eb0a46/common/protocol.h#L7-L49)、[native_consumer.c:144–172](https://github.com/SuperTurtleDev/anland/blob/9ab13eb146bc9f3dd3cc44cabc268659d9eb0a46/consumers/anland_v5/android_consumer/app/src/main/jni/native_consumer.c#L144-L172)、[:955–1003](https://github.com/SuperTurtleDev/anland/blob/9ab13eb146bc9f3dd3cc44cabc268659d9eb0a46/consumers/anland_v5/android_consumer/app/src/main/jni/native_consumer.c#L955-L1003)；6.x 伪造 AHB 的起因：[awl_ahb.cpp:10–14](https://github.com/SuperTurtleDev/anland/blob/cc1691805fe6aca6851b6fcccb6a1e33019026c1/services/waylandbridge/awl_ahb.cpp#L10-L14)。

## 读出来的三点

1. **B 不是新东西。** Anland 5.x 就是 B，只是 server 在内核。6.x 放弃它是为了让原生合成器不改就能接入，不是因为做不通。

2. **server 在内核时两条路都只需要传 fd；挪到用户态就多一层。**
   - 内核对象（dma-buf fd、sync_file）任何进程都认。
   - server 是用户态进程时：A 要传完整 AHB（gralloc 元数据 + dma-buf），B 要传缓冲队列的生产端（Surface）。
   - 与现状正好相反：kgsl 句柄通用但缺 gralloc 元数据，所以 Anland 只能伪造；MobileGL server 的 AHB 本来就带元数据，Anland 只要肯收完整句柄。
   - B 也可以照 5.x 逐缓冲交接（Anland `dequeueBuffer` → 整个 AHB 交 server → 回 sync_file → Anland `queueBuffer`）；对 MobileGL 交 Surface 更省，直接复用 swapchain。

3. **拆分层级不同 = virtio-gpu 的 native context 对 virgl / venus。**

   | | Mesa / kgsl（拆在硬件命令层） | MobileGL（拆在 GL 状态层） |
   |---|---|---|
   | client 要懂什么 | 这块 GPU 的硬件命令 | 只懂 GL |
   | 能覆盖的 GPU | 有开源用户态驱动的（现在只有 Adreno） | 有厂商驱动的（含 Mali） |
   | 额外开销 | 几乎没有 | 状态序列化 + IPC |

## 对 MobileGL 的启示

- **server 必须天然多 client**：kgsl 同时服务任意多进程、每进程独立 GPU 上下文——对应 README 的 C2（今天每 supervisor 一个会话）。
- **fence 导出（A2）没有原理障碍**：厂商驱动导出 SYNC_FD，最终也是向 kgsl / kbase 要一个 sync_file，fence 仍是内核对象。
