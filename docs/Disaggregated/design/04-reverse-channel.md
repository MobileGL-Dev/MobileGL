# 反向通道（原 ARCHITECTURE §8）

> 设计细节。设计要点与全部章节的索引见 [`../ARCHITECTURE.md`](../ARCHITECTURE.md)。章节编号沿用原 `ARCHITECTURE.md`，代码注释里的 `ARCHITECTURE.md §N` 按编号在这里找到。

## 8. 反向通道

### 8.1 `MGPipeCallbacks`（`MG_Pipe/MGPipeCallbacks.h`）

九个具名回调 + 一个正向终止符（`ResourceSubDataComplete`），取代后端直接 poke 前端对象的 95 个调用点（具名化是有意偏离 D8）。monolith 下直调，split 下是 `SEG_EVENT` 上的记录。

| 回调 | 作用 |
|---|---|
| `OnGlError` | 延迟的 GL 错误；**必须对命令流有序** |
| `OnGpuWritten` | GPU 写过的 buffer 范围（收窄 client 保守自建的 pending 集） |
| `OnBufferWriteback` | PBO 回读、XFB 捕获结果；必须与 epoch bump 有序 |
| `OnTextureWriteback` | CPU 回退生成 mip 的纹素 |
| `OnTexturePullRequest` | §8.4 |
| `OnMipLevelsGenerated` | 只带形状 |
| `OnSurfaceChanged` | 默认帧缓冲的格式与尺寸（server 拥有显示时 client 靠它得知窗口尺寸） |
| `OnCapsInvalidated` | 取代 `InvalidateCompileEnv` |
| `OnLog` | ≤WARN 有损，≥ERROR 无损 + 速率限制 |

后端凭空造的前端对象（Magma 占位纹理、swapchain 默认 FB 占位）改为 server 原生；pull 构建里的 `m_backend` 类成员真删会动 `sizeof`（G1），随 P13 退役（D-K）。

### 8.2 有序性是正确性要求

每次 writeback 之后的 epoch bump 必须在任何后续读该 handle 的命令之前被 server 应用。**反向通道需要与正向通道相同的有序保证。**

### 8.3 错误、ack 与日志

- **唯一允许同步 ack 的入口是 `glBufferStorage`**：`ResourceRespecify` 的 `kNeedsAck` 由逐记录谓词收窄到"不可变 **buffer**"（D-A2），纹理 OOM 在 monolith 里本来就推迟到 sync 时刻。respecify 带逐 level 作用域与"只换元数据"的形式，不误丢已接受的待上传。
- 其余错误一律晚到，走有序的 `OnGlError`。

### 8.4 唯一的新停顿类：server 发起的纹理重铸拉取（D-B6）

server 不保留纹素；image-bindable 重铸、整格式再生、view 源重铸会要求重发 level。缓解：`ImageBindableHint` 预防主因；拉取异步（阻塞 apply 线程，不阻塞应用线程）；保留 LRU 默认关（`MOBILEGL_PIPE_TEXEL_RETAIN_MB=0`）；显式终止符可带零个 region。真实语料上发生率可忽略（780 个统计窗口 2 次）；split 下这条路径今天仍是具名 `Fatal{UnmigratedEmulation, "texture-remint-pull"}`，归 P9。

### 8.5 XFB scatter 留在 server

补丁循环需要"捕获前的字节"，而拆分后权威影子归 server（`StagedShadow`），所以 scatter 在 server 原地跑完；回程只有一条 `OnBufferWriteback`，载补好的整段，按 `SEG_EVENT` 容量的四分之一切片（`MGPipeBufferWritebackSliceBytes`），最后一片落地即蕴含之前每一片。孤儿目标（`HasDefinedContent == 0`）零填充后照常散射。`OnXfbScatterReady` 已删除（回调 10 → 9）。落地论证见 [`notes/p34b/README.md`](../notes/p34b/README.md)（espryt D1）。
