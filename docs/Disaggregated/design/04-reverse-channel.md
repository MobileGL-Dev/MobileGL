# 反向通道（原 ARCHITECTURE §8）

> 设计细节。设计要点与全部章节的索引见 [`../ARCHITECTURE.md`](../ARCHITECTURE.md)。章节编号沿用原 `ARCHITECTURE.md`，代码注释里的 `ARCHITECTURE.md §N` 按编号在这里找到。

## 8. 反向通道

### 8.1 `MGPipeCallbacks`（`MG_Pipe/MGPipeCallbacks.h`）

八个具名回调 + 一个正向终止符（`ResourceSubDataComplete`），取代后端直接 poke 前端对象的 95 个调用点（具名化是有意偏离 D8）。monolith 下直调，split 下是 `SEG_EVENT` 上的记录。

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

`OnLog` 已删除（P9 W3，回调 9 → 8）：零生产者、零消费者、无 `EventKind`。server 日志走控制面 `LogLine`（带 `level`），由 `MG_Remote/Transport/LogForward.h` 的队列 + 发送线程转发：≤WARN 有损（队列满即丢、计数、流内 `LogForward{Dropped}` 标出缺口），≥ERROR 无损 + 限速（超速排队延迟；超出错误字节上限时合并成一条 `LogForward{Coalesced}`），FATAL 有界等待写出；日志线程永不等 socket。

后端凭空造的前端对象（Magma 占位纹理、swapchain 默认 FB 占位）改为 server 原生；pull 构建里的 `m_backend` 类成员真删会动 `sizeof`（G1），随 P13 退役（D-K）。

### 8.2 有序性是正确性要求

每次 writeback 之后的 epoch bump 必须在任何后续读该 handle 的命令之前被 server 应用。**反向通道需要与正向通道相同的有序保证。**

### 8.3 错误、ack 与日志

- **唯一允许同步 ack 的入口是 `glBufferStorage`**：`ResourceRespecify` 的 `kNeedsAck` 由逐记录谓词收窄到"不可变 **buffer**"（D-A2），纹理 OOM 在 monolith 里本来就推迟到 sync 时刻。respecify 带逐 level 作用域与"只换元数据"的形式，不误丢已接受的待上传。
- 其余错误一律晚到，走有序的 `OnGlError`。

### 8.4 纹理重铸：字节全在 server 手里，没有拉取（P9 W2 撤销原「唯一的新停顿类」D-B6）

原设计假设 server 不保留纹素、重铸要把 level 从 client 拉回来。P5c 之后 server 有 `StagedTextureStore`，GPU 写过的层本来就在 GPU 图像里——**两个后端的重铸都不需要 client 的任何字节**：

| | split 下重铸的字节来源 | monolith |
|---|---|---|
| Espryt | `RequireImageBindableStorageByHandle`：驱动上确实存在的层逐层读回旧 GPU 纹理，pending 盒用 store 合并；驱动没有的层（最后一次 sync 之后才定义）不读，store 覆盖就重放 store，否则跳过；驱动拒读而 store 覆盖时退回 store；已 immutable 且无需加宽的原样保留 | 前端臂重放 client 影子 |
| Magma | 重建带 `STORAGE` 的 VkImage，`PreserveTextureContentsOnRecreate` GPU→GPU 拷贝 | 同左 |

- 只改读 store 不够：GPU 写过的层 store 里是旧字节或没有字节；语料里 Espryt 的两次重铸（`iris-photon`、`iris-derivative`）都是 store 无字节的渲染目标。
- 「整格式再生」两臂都只上传 pending 层、不回读也不拉取；「view 源重铸」只剩源需要加宽，走同一条 ByHandle 路径。
- 删除：`OnTexturePullRequest`（回调 9 → 8）、`texture-remint-pull` 标记。op 50 `ResourceSubDataComplete` 只追加不删，留作退役行（解码拒收）。`MOBILEGL_PIPE_TEXEL_RETAIN_MB` 已无消费者，但在 pull 构建里，随 P13 删（G1）。
- `ImageBindableHint` 仍是预防：hint 在第一次 sync 前到达就直接按 image-bindable 分配，省掉一次 server 回读 + 重传。
- 已知 dev 缺陷（非 P9）：monolith Espryt 的前端臂重放 client 影子，覆盖 GPU 写过的纹素。
- 普查、覆盖矩阵、门与 red-once：[`notes/p9/W2-REMINT.md`](../notes/p9/W2-REMINT.md)。

### 8.5 XFB scatter 留在 server

补丁循环需要"捕获前的字节"，而拆分后权威影子归 server（`StagedShadow`），所以 scatter 在 server 原地跑完；回程只有一条 `OnBufferWriteback`，载补好的整段，按 `SEG_EVENT` 容量的四分之一切片（`MGPipeBufferWritebackSliceBytes`），最后一片落地即蕴含之前每一片。孤儿目标（`HasDefinedContent == 0`）零填充后照常散射。`OnXfbScatterReady` 已删除（回调 10 → 9）。落地论证见 [`notes/p34b/README.md`](../notes/p34b/README.md)（espryt D1）。
