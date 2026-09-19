---

## 6. 数据面

### 6.1 段（segment）布局

| 段 | 拥有者 | 默认大小 | 内容 |
|---|---|---|---|
| `SEG_CMD` | client（server 只读） | 8 MiB，2 的幂，64B 对齐 | `RingControl`(4KiB) + POD 记录 + ≤4KiB 内联负载 |
| `SEG_STAGE` | client（server 只读） | 32 MiB → 上限由实测定，**不是默认 256 MiB** | bulk 字节：buffer sub-data、纹理区域、UBO scratch、client 顶点/索引/indirect 数组、persistent-map 脏块 |
| `SEG_REPLY` | **server**（client 只读） | 8 MiB，4KiB slot | readback 像素、buffer writeback |
| `SEG_EVENT` | **server**（client 只读） | 256 KiB SPSC ring | `EvQueryResult`/`EvGpuWritten`/`EvGlError`/`EvLogLine`/`EvDefaultFramebufferInfo`… |
| `SEG_SHADOW[n]` | client（server 只读） | 每对象，P4.5+，≥256KiB shadow | 零拷贝 buffer/texture shadow |
| `SEG_ADOPT[n]` | **server**（client RW） | 每 buffer，P7，≥16MiB adopted store | 应用直写 GPU 内存 |

创建：Android `ASharedMemory_create`（API 26，`android/sharedmem.h:78`；libc 的 `memfd_create` wrapper 是 API 30，`sys/mman.h:196`）；桌面 Linux `syscall(SYS_memfd_create, …)`；macOS `shm_open`+`shm_unlink`；Windows `CreateFileMappingW`(`Local\`)。

**传递：POSIX `SCM_RIGHTS`，在第一个 transport commit 里实现**（asio 无 cmsg API → 在 `socket.native_handle()` 上裸 `sendmsg`/`recvmsg`，约 80 行）。`Feat/CS-Delta-IPC` 把它推迟到"P6"（`LocalSocketTransport.h:16-20`，`PollOffer` 里 `out->fd = -1` 硬编码于 `:296`），结果它的数据面在唯一重要的平台上**一个字节都过不去**。

**SEG_SHADOW 块的退休规则（本轮新增）**：§6.4 的 64KiB 块发送水位只解决"覆盖一个**活着的** shadow"；它没说怎么**释放**一个 shadow。`glDeleteBuffers` 或 `glBufferData` 重定义会释放/重分配 `SEG_SHADOW` 的 arena 块，而携带 `{segId, offset, size}` 指向该块的记录可能还没被 apply——server 于是读到另一个对象的字节。规则：释放的块进入 pending 链表，只有当 `appliedSeq`（对被借入 GPU 时间线的 slot 是 `retiredSeq`）越过最后一条引用它的记录之后才归还 arena，而不是在对象析构时立即归还。

### 6.2 RingControl：watermark 是一条共享 cache line，**且带双向 doorbell**

```cpp
// MobileGL/MG_Remote/Transport/Ring.h
struct alignas(4096) RingControl {
    // ---- SEG_CMD 游标 ----
    alignas(64) std::atomic<Uint64> cmdHead;              // producer：累计写入字节
    alignas(64) std::atomic<Uint64> cmdAppliedTail;       // consumer：已解码并拷出的字节
                std::atomic<Uint64> cmdRetiredTail;       // consumer：被借入 GPU 时间线的 slot 已释放
    // ---- SEG_STAGE 游标（独立三元组；上一版遗漏）----
    alignas(64) std::atomic<Uint64> stageHead;
    alignas(64) std::atomic<Uint64> stageAppliedTail;
                std::atomic<Uint64> stageRetiredTail;
    // ---- 序号 / 帧水位 ----
    alignas(64) std::atomic<Uint64> appliedSeq;           // 已 apply 的记录序号
                std::atomic<Uint64> submittedSeq;         // 已提交给驱动
                std::atomic<Uint64> retiredSeq;           // GPU 已完成
                std::atomic<Uint64> completedFrameSerial;
                std::atomic<Uint64> presentAckSerial;
    // ---- doorbell / 代 ----
    alignas(64) std::atomic<Uint32> serverEpoch;          // context 丢失 / server 重启时 ++
                std::atomic<Uint32> ringGeneration;       // 硬 drain 后 ++，作废缓存 offset
                std::atomic<Uint32> consumerParked;       // server 睡了，producer 要敲门
                std::atomic<Uint32> producerParked;       // client 睡了，server 要敲门（本轮新增）
                std::atomic<Uint32> eventRingFull;        // SEG_EVENT 满，server 已停止 apply
                std::atomic<Uint32> eventDropped;         // 被丢弃的 EvLogLine 计数
};
```

**三个 seq 水位严格区分**（混为一谈是经典错误）：`appliedSeq` 释放 `cmdAppliedTail`/`stageAppliedTail`；`submittedSeq` 释放 staging；`retiredSeq`/`completedFrameSerial` 释放 `*RetiredTail` 与 `SEG_ADOPT` 复用。

**两个 tail 是必须的**：`Ops_ResidentSubData` 把字节拷进 `pendingResidentWrites`（`Managers.cpp:1158-1166`），P7 之后 server 会**借用** ring slot 而不是再拷一次——那种 slot 只能在 `completedFrameSerial` 之后回收。单 tail 会在 P7 落地当天变成保守回收。

**SEG_STAGE 必须有自己的游标三元组**：§7.2 把"`SEG_STAGE` 余量 < 1/4"列为 Publish 触发器，而第二个 ring 的占用率无法从第一个 ring 的游标算出；且 stage slot 的退休条件（`retiredSeq`）与 cmd 记录（`appliedSeq`）不同。

#### 6.2a 双向 doorbell（本轮新增，修 "client 只能自旋" 的缺陷）

- **client → server**：consumer 自旋 ~200µs → 置 `consumerParked=1` → 在控制 socket 上阻塞读 1 字节；producer 在 release-store `cmdHead` 之后，仅当 `consumerParked` 时写 1 字节（字节码 `0x01 = 'ring advanced'`）。
- **server → client**（上一版缺失）：client 在**任何**等待里（present credit、`kNeedsAck` 阻塞请求、ring/stage 满的升级等待）先自旋 `MOBILEGL_IPC_SPIN_US`（默认 50µs），再置 `producerParked=1`，然后在同一个 socket 的反向流上阻塞读；server 在 release-store 任何 watermark 之后，仅当 `producerParked` 时写 1 字节（字节码 `0x02 = 'watermark advanced'`）。

没有这一条，上一版的每一处 client 等待都退化成跨进程自旋一条共享 cache line：present-credit 等待最长一整帧（60Hz 下 16.6ms），在手机上就是一颗大核满频空转，与 GPU 和游戏 JVM 抢核；§6.5 的"有界 50ms 等待"就是 50ms 自旋。而 MobileGL 全库没有任何亲和性控制（`grep -rn 'sched_setaffinity\|cpu_set_t' MobileGL/` 零命中），无法把它赶到小核上。

`spawn` 模式用 socketpair 的两个方向做 doorbell；`inproc` 模式用一对 `std::condition_variable`（同一套 `producerParked`/`consumerParked` 语义）。**零 futex/eventfd/named-event 平台代码**（asio 已 vendored，`3rdparty/asio/include` 已在主 target 的 include path 上，`CMakeLists.txt:483`）。

### 6.3 记录格式

```cpp
// MobileGL/MG_Remote/Protocol/RecordKinds.h
struct RecHeader { Uint16 kind; Uint16 flags; Uint32 size; };   // 8 B，size 含 header，8 字节倍数
enum RecFlags : Uint16 { kNone=0, kNeedsAck=1<<0, kHasBlob=1<<1, kPad=1<<2, kBorrowSlot=1<<3, kVarTail=1<<4 };
struct BlobRef  { Uint32 seg; Uint32 pad; Uint64 offset; Uint64 size; };  // 24 B
```
**没有 per-record 序号字段**：seq 就是记录序数（producer `m_emitSeq++`，consumer `m_applySeq++`），省 8B/记录并消除一整类失步。

X-macro 单一真相源：
```cpp
// MobileGL/MG_Remote/Protocol/Records.def
#define MGL_REC_LIST(X)                                       \
    X(BindBuffer,      RecBindBuffer,       24)               \
    X(DrawArrays,      RecDrawArrays,       32)               \
    X(DrawElements,    RecDrawElements,     56)               \
    X(BufferSubData,   RecBufferSubData,    64)               \
    X(BufferMap,       RecBufferMap,        40)               \
    X(BufferUnmap,     RecBufferUnmap,      24)               \
    X(RenderStateBlob, RecRenderStateBlob,  40)               \
    X(XfbAccounting,   RecXfbAccounting,    56)               \
    X(GenerateMipmapLevels, RecGenerateMipmapLevels, 32)      \
    X(RenderbufferStorage,  RecRenderbufferStorage,  40)      \
    /* … ~95 项 … */
#define MGL_REC_SIZE_CHECK(name, T, sz) \
    static_assert(sizeof(MobileGL::Wire::T) == (sz), #name " record size drift");
MGL_REC_LIST(MGL_REC_SIZE_CHECK)
```
**每种一条 `static_assert`** ——修掉正是 `Feat/CS-Delta-IPC` 中过一次的 bug 类（`b50f3348`："旧的 off-by-one 让 applier 误读 TexImage 之后的每一条 state delta"），而它那条只断言 union 首成员的 assert（`ServerCore.cpp:31-33`）永远抓不到中间插入。

**运行期边界纪律（本轮新增）**：`SEG_CMD` 是对端并发写入的区域，编译期 `static_assert` 管不到运行期损坏。同一个 X-macro 额外生成 applier 分发前的前置条件：
```cpp
#define MGL_REC_BOUNDS_CHECK(name, T, sz)                                     \
    case RecKind::name:                                                       \
        if (h.size < (sz) || h.size > remainingRingBytes || (h.size & 7u))    \
            return Fatal(FatalCode::ProtocolCorruption, #name);               \
        break;
```
`kVarTail` 记录额外校验 `定长前缀 + 尾巴自描述长度 == h.size`。违反一律 `Fatal{ProtocolCorruption}`，绝不进入未定义行为。

变长记录（`RecVaoConfig`、`RecTexSubImage` 的 rect 列表、`RecProgramLinkOp`、`RecMultiDrawArgs`）：`kVarTail` + 定长前缀 + 自描述长度的内联尾巴。

### 6.4 WAR 危害与字节稳定性

**Phase 1-4 规则：GL 调用时刻把字节拷进 ring slot。** slot 从写入到 `stageAppliedTail` 越过它为止不可变，client 拿不回它 → **危害按构造消除**。代价是一次 memcpy，而 `Ops_ResidentSubData`（`Managers.cpp:1165`）和 `StageBlocksIntoUnpackRing` 在 monolith 里已经在付同样的钱。

**Phase 4.5 规则（shadow-in-shm，零拷贝）：** ≥256KiB 的 shadow 分配在 client 拥有的 `SEG_SHADOW` 里——`PipeResource` 的 `MapAlignedAllocator`（`PipeResource.h:33-60`，无状态、25 行、64B 对齐）增加一个 shm arena（保留 `MIN_MAP_BUFFER_ALIGNMENT=64` 契约，`PipeResource.h:28`），`MipmapStorage` 的 level vector 同理。`RecBufferSubData` 于是只带 `{segId, offset, size}`，**client 侧零拷贝**。
WAR 用 **per-shadow 64KiB 块发送水位**：若应用写入某块而该块最后一次发送尚未 `appliedSeq` 覆盖，这次写走 `SEG_STAGE`。有界、局部、压力下自动退化成 Phase-1 行为。这套块水位同时是 §5.10 精确版 persistent-map 推送的脏位来源。

**该改动必须整段 `#if MOBILEGL_BUILD_DISAGGREGATED` 包裹**：`PipeResource` 与 `MipmapStorage` 住在 `MG_State`，不在 `MG_Remote`，而改一个容器的 allocator 就改了类型；不包裹的话 §12/D8 的 `nm`/`.text` 门会在 P4.5 变红。写法是"分配器特化：option OFF 时逐字折叠成今天的 `MapAlignedAllocator`"。

#### 拷贝账（更正版，MC pan 一帧约 9MB section mesh + ~1MB UBO scratch）

上一版这张表把 monolith 和 split 两侧都数少了。逐条核对：

- monolith 的 `glBufferSubData` → shadow store 是 **2 次**：(1) app→shadow（`BufferObject::UploadSubData` 的 `Memcpy`），(2) shadow→目的地（`FlushPendingRangesNow`：`Memcpy(dst, bufferObject.MappedData()+start, size)` 进 invalidating map，`Managers.cpp:914`；或 `Memcpy(g_uploadRing.store.mappedPtr+ringOffset, ..., size)` 进 upload ring，`Managers.cpp:922`）。
- split P1-4 是 **4 次**：app→client shadow (1)、client shadow→`SEG_STAGE` (2)、applier replay mutator ⇒ `SEG_STAGE`→**replica** shadow (3)、server 的 `FlushPendingRangesNow` ⇒ replica shadow→upload ring (4)。
- P4.5 只去掉 (2)，剩 **3 次**。它去不掉 (3)，因为 `SEG_SHADOW` 是 client 拥有 / server 只读，而 replica 的 `BufferObject` 拥有自己的 `PipeResource` 分配。

| 路径 | monolith | P1-4 | P4.5 | P4.5+replica-adopt（可选，见下） |
|---|---|---|---|---|
| `glBufferSubData` → shadow store | 2 | 4 | 3 | **2** |
| `glBufferSubData` → adopted store（P7） | 2 | — | — | 2 |
| `glMapBufferRange(WRITE)`+unmap | 3 | 5 | 4 | 3 |
| persistent coherent map 推送（§5.10 保守版） | 0 | 2/发射点 | 1/发射点(精确块) | 1/发射点 |
| `glTexSubImage` | 2 | 3 | 2 | 2 |
| 全局 UBO / draw | 1 | 2 | 2 | 1 |
| adopted ≥16MiB（P7 T1/T0） | 0 | — | — | 0 |

**目标选择（必须在 P4.5 之前拍板）**：
- **方案 A（默认，保守）**：接受 3 次，写进文档。P4.5 的价值是消掉 client 侧那次拷贝与那份重复内存。
- **方案 B（激进，需额外设计）**：给 replica 的 `PipeResource` 增加**第三种模式** `AdoptedClientShadow`——`Bytes()` 返回 server 映射的 client `SEG_SHADOW`（只读），applier 的 `UploadSubData` 退化成一次 range 记账 + change-serial bump，只剩 server 的 ring 拷贝。这保持了 mutator replay 的全部副作用（包括 `IsBufferDrawClean` 比较的 change serial），只是不搬字节。风险：replica 的 shadow 变成只读会让任何 server 侧写（`WritebackFromBackend`、生成 mip、CopyImage 镜像）需要就地 copy-on-write 升级回普通 shadow。**先按方案 A 实现并测量，方案 B 作为 P6 的候选优化项，由 Tracy 计数器决定是否值得。**

无论选哪个，`TracyPlot` 字节计数器必须**装在 wire 两侧**（client 的 emit 字节 + server 的 apply 字节 + server 的 ring/staging 字节），P4.5 的验收看**总量**，不是只看 client 一侧的数字。

### 6.5 Ring 分配与背压

逐字移植 `PersistentRing`（`Managers.cpp:657-727`、`RingAllocateSlow` `:1891-1970`、`RingOnPresent` `:1975-2016`）：单调 head/tail、2 的幂掩码、frame mark。分配失败升级：**扩容(翻倍) → 对最老未 retire 批次有界等待（默认 50ms，走 §6.2a 的 producerParked doorbell，不是自旋） → 硬 `Drain` 请求 + `ringGeneration` bump**。generation bump 上线，防止后续记录引用被回收的 offset；硬 drain 之后按 §5.6a 重发未 apply 的纹理记录。

`SEG_CMD` 与 `SEG_STAGE` 各自独立跑这套升级（各有自己的游标三元组）。

### 6.6 纹理

- **Unpack PBO 完全在 client 解析**（`GL_Texture.cpp:1719,1765,1887,1976,2457,2604,2722,4458,6176` 读 `pixelUnpackBufferObject->MappedData() + (SizeT)pixels`，再由 `ProcessTexturePixelsDataUnpack` 紧密重排）。**没有任何纹理像素以 PBO 引用形式过线，server 永远不需要 `GL_PIXEL_UNPACK_BUFFER` 状态。`PixelStoreBlob` 只用于 PACK 方向。**
- **压缩纹理永不到达任何 backend**（前端在 `glTexImage` 时把压缩 internalformat 解析成非压缩后备，`GL_Texture.cpp:298-306`；`grep -i compress MG_Backend/DirectGLES/*.cpp` 只命中一条注释）。逐字节 `m_compressedData` blob 仅供 `glGetCompressedTexImage`，纯 client 侧，不过线。
- **`glCopyTexSubImage*` 与 `glClearTexImage` 整体留在 client（推翻上一版的 P4 项）。** 已确认这两个入口今天就是**纯前端操作**：`CopyTexSubImage{1,2,3}D_State`（`GL_Texture.cpp:3955,3979`）调 `CopyReadFramebufferIntoMipmapRegion`（`:1044-1097`），它借一次 backend `ReadPixels` 进 CPU scratch（`:1079`）、逐行 memcpy 进 mipmap shadow（`:1089-1094`）、`MarkStorageDirty(...,true)`（`:1095`）。拆分后它恰好是**一次阻塞 ReadPixels round trip**，产生的脏区按普通纹理 delta 下发——正确，且不需要任何新命令。上一版提议"整体移到 server + `EvTexWriteback`"是错的：那个事件在 §7.4 的列表里根本不存在（只有 `EvBufferWriteback`），它仍然要付一次 round trip（client shadow 必须为 `glGetTexImage` 保持最新），还多出一个 `GLFunctionsTable` 里没有对应项的命令。`glClearTexImage`（`GL_Texture.cpp:985-1006`）同形。
- **per-level `serverAuthoritative` 位**只保留给两处**字节确实在 backend 里写进 shadow** 的场景：生成 mip 的 CPU 路径（`DirectGLES.cpp:6270-6271,6861` 的 `AllocateStorage` + 直写 `MapMipmapData`）与 `MirrorCopyImageIntoDestinationShadow`（`:7144`，`glCopyImageSubData` 的目的地镜像）。client 在发射对应命令时对受影响 level 置位。`CopyTextureImageToClientOrPBO_State` 查它：**清 → 本地 shadow 回答，零 round trip**（应用自己上传的 level 全走这条）；**置 → 一次 round trip**。

### 6.7 回读

| 路径 | monolith | 拆分后 |
|---|---|---|
| `glReadPixels` → 客户内存 | 阻塞 | 一次 round trip，像素放 `SEG_REPLY` slot；per-row 循环留在 server 内 |
| `glReadPixels` → pack PBO | **也阻塞**（`DirectGLES.cpp:9189-9205` 把整个 PBO map 回来写 shadow） | **fire-and-forget** + client 侧对该 PBO 置 `MarkGpuWritten`（§5.6b），代价推迟到之后的 map/read。**严格优于 monolith** |
| `glGetTexImage`/`glGetTextureImage` | DirectGLES 从 client shadow 回答 | DirectGLES **零 round trip**（除 `serverAuthoritative` level）；DirectVulkan 一次 |
| `glGetBufferSubData` / `glMapBuffer(READ)` on gpuWritePending | 阻塞（`glFinish()`，`Managers.cpp:1246`） | 一次，由 client 侧 pending 集合触发（§5.6b），被 `EvGpuWritten{ranges}` 收窄 |
| XFB capture writeback | `glEndTransformFeedback` 里无条件无限 `ClientWaitSync`（`GL_Drawing.cpp:1326-1337`） | **不等**，client 对 capture target 置 `MarkGpuWritten`，首次读时付；`FixupGsStripCaptureOrder` 移到 server |
| `glCopyTexSubImage*` | 内含一次同步 ReadPixels | 一次 round trip（保持前端实现不变） |

### 6.8 persistent map 与 ≥16MiB 采纳

三档，由**运行时 POST 探针**选择（遵循本项目"后端限制一律探针判定、绝不硬编码驱动名"的既定规则）：

- **T2 — 拒绝（P1-6 默认，永久正确回退）**：`AcquirePersistentMap` 返回 `nullptr`。**此档下 §5.10 的 client 侧推送是强制的**，否则应用的 coherent persistent 写会丢。
- **T1 — server 导出自己的映射（P7 主攻）**：server 照常铸造 coherent map（`Managers.cpp:988-1058` / `VkBufferManager.cpp:515-563`），经 `VK_KHR_external_memory_fd` / `AHardwareBuffer_sendHandleToUnixSocket`（API 26，`hardware_buffer.h:521`）/ `VK_KHR_external_memory_win32` / `GL_EXT_memory_object_fd` 导出，client `mmap` 后调 `PipeResource::AdoptPersistentMap(base)`。**每 store 生命周期一次 round trip。** 采纳成功后 §5.10 的推送对该 buffer 自动停止（`SyncPersistentMappedRange` 的 `IsGpuResident()` 早退），与 monolith 一致。
- **T0 — server 导入 client 分配**：client 分配 `AHardwareBuffer`/dma-buf，server 以 `GL_EXT_external_buffer`+`glBufferStorageExternalEXT` 或 `VK_EXT_external_memory_host` 导入。理想但可用性未知。

**`MOBILEGL_COHERENT_AS_FLUSH` 在拆分模式下照常生效**（推翻上一版的禁令，理由见 §5.10 结尾）：有了 client 侧推送，被改写出来的 coherent map 与应用原生请求的 coherent map 走同一条正确路径，两个 Create/Flywheel fixture 才能在 split 与 monolith 下做同路径对比。

### 6.9 program artifacts

- **P1-4**：`RecProgramLinkOp{handle, shaderSources[], bindAttribLocations[], bindFragDataLocations[], xfbVaryings[], xfbMode, separable, reflectionDigest}` — server 重新 link。只需 5 个 schema 字段，**且分歧不可能静默**（两半跑同一二进制里的同一段代码）。源码可得：`ProgramObject::GetLinkedShaderSnapshot()`（`ProgramObject.h:157`）刻意持有 linked shader 的 `SharedPtr`（注释在 `:1716`），所以 `glDeleteShader` 之后源码仍在。
- **`reflectionDigest` 必须覆盖 backend 实际读的全集**：xxHash over
  `(uniformName, location, type, typeFacts, samplerOrImageUnitIndex)` 全表 + `maxUniformLocation` + `(blockName, blockBinding, blockSize)` 全表 + `shaderStorageBlockBindingOverrides` + `PointSizeDemoted` + `GetLinkedShaderStages` + `xfbVaryings/xfbStrides/xfbPackedStride/xfbBufferMode` + **`GetGeneratedSpirv()` 各 module 的 xxHash**。不匹配 → `Fatal{ReflectionDivergence}`。
  （理由：本项目自己的二分历史记录过"glslang 反射/生成顺序是真载重，桌面字节一致是语料受限的假绿"。）
- **P5**：`RecProgramPublish{handle, stages[], spirvBlobs[], reflectionBlobRef}`，reflection 用 **`Visit()` 式归档**：
```cpp
// MobileGL/MG_State/GLState/ProgramState/ProgramArtifactsArchive.h
template <class Ar> void Visit(Ar& ar, LinkArtifacts& a) { ar(a.writtenUniformLocationBits, /*…全字段…*/); }
static_assert(sizeof(LinkArtifacts) == MGL_LINKARTIFACTS_SIZE,
              "新字段请加进 Visit() 并 bump MGL_LINKARTIFACTS_SIZE");
```
  一份字段表服务两个方向 + `sizeof` 绊线。**序列化整个结构体**（而非 backend 当前读的 ~40 字段），这样 backend 新增一次 read 永不需要改协议。
  安装入口：`ProgramObject::InstallPublishedLink(LinkArtifacts&&, SpirvArtifacts&&, linkVersion, imageUnitVersion, backendStateVersion)`，绕过 `m_pendingLink`/`m_pendingSpirv`，**server 因此不需要 compile pool**。
- `relink` 路径保留为常驻 oracle 与 A/B 对照（`MOBILEGL_IPC_PROGRAM=publish|relink`）。
- 全局 UBO scratch 相反：小、每次 `glUniform*` 变、有版本 → 走 `SEG_STAGE`，键 `(programHandle, uboContentVersion)`，复现 monolith 的"每 program 每帧至多一次"（`DirectGLES.cpp:3369-3392`）。

### 6.10 应用指针（四类，范围全部可算）

| 类 | 范围 | 站点 |
|---|---|---|
| client 顶点数组（仅 DrawArrays 族） | `(first+count-1)*stride + elementSize` | `Managers.cpp:2560`、`VulkanRenderer.cpp:3737` |
| client 索引数组 | `count * indexSize` | `DirectGLES.cpp:4436`、`VulkanRenderer.cpp:4081` |
| client indirect / parameter 块 | `stride*(drawcount-1)+cmdSize` | `DirectGLES.cpp:276`、`DirectVulkan.cpp:303` |
| `MultiDraw*` 参数数组、`ClearBuffer*` value | `drawcount*4`、16B | `DirectVulkan.cpp:963-1057` |

唯一无界的是**索引 draw 下的 client 顶点数组**：索引扫描（`TryComputeMaxIndexFromHostBytes`，`VulkanRenderer.cpp:3406-3470`）必须在 **client** 侧跑，只有 client 同时持有两个数组。实现于 `MG_Remote/Client/ClientArrayBounds.cpp`，两个 backend 共用。

**陈旧索引危害（本轮新增）**：monolith 在每次这类扫描之前都调 `indexBuffer->SyncGpuWrites()`（`DirectGLES.cpp:4413`、`MultiDraw.cpp:499`、`VulkanRenderer.cpp:3431,4159`），因为 EBO 可能刚被 compute shader 或 XFB 写过。client 侧扫的是 client shadow，若不做同样的强制回读，算出的 `maxIndex` 来自陈旧字节，顶点数组会被少拷 → 几何缺失/花屏，或越界读应用数组。同样的暴露面还有 primitive-restart 重写（`DirectGLES.cpp:4412-4414`）与 `*IndirectCount` 的 parameter buffer 读（`DirectGLES.cpp:4666-4693,4768-4793`）。

**规则**：`ClientArrayBounds`、restart 重写、indirect-count 读者在触碰 shadow 之前，必须走 §5.6b 的 pending 检查（Publish + 等 `appliedSeq` + 排空事件），即 monolith 里 `SyncGpuWrites()` 所在的**同一个位置**。P2 增加 `ClientArrayAfterComputeWriteScenario` 作为门。

draw 记录里 `indicesAreClient` 由"是否绑定了 element array buffer"决定（`DirectGLES.cpp:4423` vs `:4425-4442`），在 binding 所在的一侧判定。

---

## 7. 控制面

### 7.1 FlatBuffers 用法

**一份 schema `MobileGL/MG_Remote/Protocol/protocol.fbs`，两种用法：**
- **热路径 → FlatBuffers `struct`**（flatc 保证定长布局、无 vtable、无偏移间接、无需 verifier walk，只需边界检查），直接放进 ring：`[RecHeader | struct | 可选变长尾]`。`DrawArrays` = 8+24 = 32B（对比 table-per-command 的 ~60B 与一次 vtable 遍历）。这正是 `Feat/CS-Delta-IPC` 自己的 plan 第 55 行要求而实现没做的事。
- **罕见/变长/需演进 → FlatBuffers `table`**，走 CTRL socket。

```fbs
namespace MobileGL.Wire;

// ---------- 热路径 struct（进 ring）----------
struct WireHandle  { kind:ubyte; p0:ubyte; p1:ubyte; p2:ubyte; glName:uint; lifetimeId:ulong; }
struct BlobRef     { seg:uint; pad:uint; offset:ulong; size:ulong; }
struct RecBindBuffer     { target:uint; index:uint; h:WireHandle; }
struct RecDrawArrays     { mode:uint; first:int; count:int; instances:int; baseInstance:uint; pad:uint; }
struct RecDrawElements   { mode:uint; count:int; type:uint; flags:uint; indices:ulong; blob:BlobRef; }
struct RecBufferSubData  { h:WireHandle; offset:ulong; size:ulong; blob:BlobRef; }
struct RecBufferMap      { h:WireHandle; rangeStart:ulong; rangeEnd:ulong; access:uint; pad:uint; }
struct RecBufferUnmap    { h:WireHandle; }
struct RecTexSubImage    { h:WireHandle; target:uint; level:uint; box:[uint:6]; rectCount:uint;
                           pad:uint; blob:BlobRef; }        // rects 在变长尾
struct RecGenerateMipmapLevels { h:WireHandle; target:uint; requiredLevelCount:uint;
                                 bytesPerTexel:uint; shrinkingAxes:uint; }
struct RecRenderbufferStorage  { h:WireHandle; internalFormat:uint; width:int; height:int;
                                 samples:int; pad:uint; }
struct RecXfbAccounting  { pausedPrims:ulong; inputPrims:ulong; prims:ulong;
                           capturedVerts:ulong; geomDraws:uint; accountedDraws:uint; }
struct RecRenderStateBlob{ version:ushort; pipelineVersion:ushort; pad:uint; blob:BlobRef; }
struct RecPresent        { frameSerial:ulong; swapInterval:int; pad:uint; }
struct RecSetResolvedDrawProgram { h:WireHandle; }
// … 共约 95 个

// ---------- 控制面 table（走 socket）----------
table SegmentRef { id:uint; kind:ubyte; sizeBytes:ulong; name:string; }
table Hello   { abiMajor:uint; abiMinor:uint; buildFingerprint:string; backendType:uint;
                pid:uint; configBlob:[ubyte]; }
table Welcome { abiMajor:uint; abiMinor:uint; serverPid:uint;
                cmdRing:SegmentRef; stageRing:SegmentRef; replyPool:SegmentRef; eventRing:SegmentRef; }
table CapsSnapshot { dynamicParameters:[ubyte];        // DynamicBackendParameters 逐字节
                     rendererInfo:[ubyte]; formatCaps:[ubyte]; extensions:[string];
                     apiVersion:string;
                     maxComputeWorkGroupCount:[int:3]; maxComputeWorkGroupSize:[int:3];
                     tableSlotMask:ulong;              // 远端实际注册了哪些 GLFunctionsTable 槽
                     prefersCpuXfbPrimitiveAccounting:bool; }
table DefaultFramebufferInfo { width:int; height:int; colorFormat:uint; depthFormat:uint; stencilFormat:uint; }
table SurfaceOp    { seq:ulong; kind:ubyte; display:ulong; surface:ulong; windowKind:ubyte;
                     nativeToken:ulong; width:int; height:int; swapInterval:int; }
table SurfaceReply { seq:ulong; ok:bool; eglMajor:int; eglMinor:int; defaultFb:DefaultFramebufferInfo; }
table ProgramReflection { /* Visit() 归档的结构化镜像，P5 */ }
table ResyncRequest { serverEpoch:uint; }  table ResyncDone {}
table AuxRequest   { seq:ulong; kind:ubyte; payload:[ubyte]; }   // 外来线程 sync/query
table Fatal   { code:uint; message:string; }
table LogLine { level:ubyte; text:string; }
union CtrlMsg { Hello, Welcome, CapsSnapshot, SurfaceOp, SurfaceReply,
                ProgramReflection, ResyncRequest, ResyncDone, AuxRequest, Fatal, LogLine }
table CtrlEnvelope { msg:CtrlMsg; }
root_type CtrlEnvelope;
```

`protocol_generated.h` **提交进仓库**，由 `scripts/gen_protocol.py` 重新生成（镜像 `tools/trace_replay/CMakeLists.txt:52-69` 驱动 `glproc.py` 的做法）；CI 加 `flatc-check` 步骤重新生成并 `git diff --exit-code`。

**codegen 绝不进默认构建图（本轮加强）**：`Feat/CS-Delta-IPC:MobileGL/Protocol/CMakeLists.txt:22-38` 在 `MOBILEGL_FLATC_EXECUTABLE` 未设时 `add_subdirectory(3rdparty/flatbuffers)` 并开 `FLATBUFFERS_BUILD_FLATC ON`——这正是它自称要修的 NDK 陷阱（交叉编译造出 arm64 `flatc` 然后在 host 上执行）。**本计划不复用这一段**：`gen_protocol.py` 是纯开发者/CI 目标，默认构建图里没有 `flatc`，`MOBILEGL_FLATC_EXECUTABLE` 只服务 CI 的 `flatc-check`。FlatBuffers 运行时是 header-only，只需要 `3rdparty/flatbuffers/include` 在 include path 上（P4 用 `nm` 复核 `libMobileGL.so` 链接行没有新增库，不靠断言）。

### 7.2 帧封装与 flush 策略

CTRL socket 封帧：`[u32 'MGLF'][u32 len][payload]`，64MiB 上限，**读时校验**（`Feat/CS-Delta-IPC` 的 `Feed()` 永远返回 OK，坏 magic 变成静默永久挂起，`Framing.h:41-45`；`StartRead` 直接按 wire 长度分配无上限检查，`LocalSocketTransport.cpp:232-236`）。接收缓冲不足时**返回所需大小并保留消息**（上一版的 transport 会失败且不弹出消息，把流永久卡死）。

#### Publish 触发器（重写，删掉 64KiB 阈值）

上一版设 "records ≥ 64KiB" 为主触发器。按 §6.3 的记录尺寸，64KiB ≈ 1200-2700 条记录，即**一整帧**（计划自己把 MC 帧估为 1000-4000 draw）。那意味着 server 在 client 发完整帧之前无法开始工作——这不是异步，是一个整帧的流水线气泡，且在 present credit 之上再加一整帧延迟。它还在 P2.5 跑之前就先把 P2.5 的假设否掉了（inproc 的全部意义就是让 `PrepareForDraw` 与 GL 线程重叠，帧粒度 publish 保证零重叠）。而 `SEG_CMD` 是 SPSC ring，"publish" 只是一次 `cmdHead` 的 release store，唯一值得摊销的是门铃写。

**新规则**：
- **每条记录（或每 8-16 条，用来摊销 store）release-store `cmdHead`**；仅当 `consumerParked` 时敲门铃。
- 显式门铃点：`Present`、任何 `kNeedsAck` 阻塞请求、`eglMakeCurrent`、`glFlush`（**刷出 outbox，不等待**）。
- **`SEG_STAGE` 余量 < 1/4** 时敲门铃（用 `stageHead - stageAppliedTail`）。
- **轮询类入口点也是门铃点（本轮新增，修 livelock）**：`glClientWaitSync`（任意 timeout）、`glGetSynciv(GL_SYNC_STATUS)`、`glGetQueryObject*(GL_QUERY_RESULT_AVAILABLE | GL_QUERY_RESULT_NO_WAIT)`。
  理由：GL 的标准惯用法是 `glFenceSync(); while (glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, 0) == GL_TIMEOUT_EXPIRED) {}` 与 `while (!avail) glGetQueryObjectuiv(id, GL_QUERY_RESULT_AVAILABLE, &avail);`。循环里没有别的 GL 调用，若这些入口不 publish，`RecFenceSync` 就永远躺在 ring 里，server 看不到，watermark 不动，循环永久自旋——这是挂死，不是变慢。仓库自己在意这件事：`DirectVulkan.cpp:1158-1160` 写明 "GL_SYNC_FLUSH_COMMANDS_BIT: flush regardless of timeout, so a zero-timeout poll loop makes progress across calls"，而 MG_Impl 无条件把 flags 透传给 backend（`GL_Sync.cpp:96`）。
  **携带 `GL_SYNC_FLUSH_COMMANDS_BIT` 的调用无条件 publish**（spec 要求 flush）。
- **饥饿升级**：同一个 handle 连续 N 次（默认 64，`MOBILEGL_IPC_POLL_ESCALATE`）本地回答 `TIMEOUT_EXPIRED` / "未就绪" 而 watermark 毫无移动时，升级成一次阻塞 round trip，这样一个已经卡住的 server 不会把 client 自旋成死循环。

**`glFinish` 保持纯 no-op**（`Definitions.cpp:111-112`）——应用唯一的强制停顿手段在 monolith 里免费，拆分后也必须免费。

### 7.3 序号与 credit

seq = 记录序数。**两个互相独立的窗口，绝不是 per-batch 锁步**（`Feat/CS-Delta-IPC` 在 apply 循环里同步发 ack，`ServerCore.cpp:421-429`，是最差的节奏；而且它的 credit 算成 `baseSeq + items.size()`，只有 `baseSeq==0` 时才对）：

- **字节 credit**：`SEG_CMD` 与 `SEG_STAGE` 各自的占用，升级路径见 §6.5。
- **Present credit**：`eglSwapBuffers` 在 `presentsSent - presentAckSerial >= MOBILEGL_IPC_PRESENT_CREDIT`（**默认 1**，见 §9）时阻塞。

server 端**不发 credit 消息**：它对 `RingControl` 做 release store，consumer 每 64 条记录更新一次 `appliedSeq`，并在 `producerParked` 时敲反向门铃。

### 7.4 事件回传通道

`SEG_EVENT` 是 server→client 的 SPSC POD ring：`EvQueryResult{handle, available, value}`、`EvFenceSignaled{handle}`、`EvGpuWritten{handle, rangeCount, ranges[]}`、`EvBufferWriteback{handle, offset, BlobRef}`、`EvReadbackDone{seq, BlobRef}`、`EvGlError{code}`、`EvDefaultFramebufferInfo`、`EvCompileEnvInvalidate`、`EvLogLine{level,len,text}`。

#### 排空点（补齐）

client 在下列位置排空：`glGetError`、`glGetQueryObject*`、`glClientWaitSync`、`glGetSynciv`、`eglSwapBuffers`、**`glMapBuffer` / `glMapBufferRange` / `glGetBufferSubData` / `glGetNamedBufferSubData` / `glCopyBufferSubData`**（§5.6b 要求），以及**每一次等待循环的每一轮**（present credit、`kNeedsAck`、ring/stage 满）。最后一条是必须的，见下。

#### 溢出策略（本轮新增，修一个双向死锁）

上一版没说 `SEG_EVENT` 满了怎么办，也没要求 client 在**等待中**排空。具体死锁：client 卡在 `eglSwapBuffers` 等 present credit；server 的 apply 线程一边 apply 一边产 `EvLogLine` 与 `EvGpuWritten`；`SEG_EVENT` 满；apply 线程阻塞在生产上；`presentAckSerial` 永不前进；client 永不离开 `eglSwapBuffers`，因而永不排空。两边都死。

**策略**：
1. client **必须**在每个等待循环内排空 `SEG_EVENT`，不只是在入口点边界。
2. `EvLogLine` 是**有损**的：覆盖最旧，并累加 `RingControl.eventDropped`（client 在排空时把丢失条数打进日志）。丢一条日志绝不允许卡住渲染。
3. 语义承载事件（`EvGpuWritten`、`EvReadbackDone`、`EvFenceSignaled`、`EvBufferWriteback`、`EvGlError`、`EvDefaultFramebufferInfo`、`EvCompileEnvInvalidate`）**无损**：ring 装不下时 server 置 `RingControl.eventRingFull=1` 并**停止 apply**（停在一条记录的边界上，不是记录中间），敲反向门铃；client 排空后清标志并敲正向门铃。状态因此永远可恢复。
4. 故障注入测试：在 client 被 credit 阻塞时灌满 `SEG_EVENT`，与 P8 的 SIGKILL 测试并列。

server 侧的 `MGLOG` 与延迟诊断按流顺序 replay 进 client 日志流——复用已存在的 `DeferredLogLine`/`ApplyDeferredDiagnostics` 机制（`JobNode.h:26-58,149-158`）。

---

## 8. Roundtrip 清单

### 不可避免的阻塞点

| # | 站点 | 频率 | 为什么 |
|---|---|---|---|
| 1 | 握手 `Hello`/`Welcome` + 段 fd 传递 | 一次 | — |
| 2 | `InitializeEGLDisplay`（写 `major`/`minor`） | 一次 | 出参 |
| 3 | `CreateEGL{Window,Pbuffer}Surface` / `Resize` | 罕见 | 返回 `Bool`；回复顺带 `DefaultFramebufferInfo` |
| 4 | 首次 `MakeEGLCurrent` + `InitCapabilities` → `CapsSnapshot` | 每 surface 一次 | caps 只在那一刻才存在（`BackendObject.cpp:341-347`） |
| 5 | `glReadPixels` → 客户内存 | 罕见（CTS 热） | GL 要求返回时字节已就位 |
| 6 | `glCopyTexSubImage*` / `glClearTexImage`（内含 ReadPixels） | 罕见 | 前端实现本来就借一次 ReadPixels |
| 7 | `glGetTexImage`/`glGetTextureImage`（DirectVulkan；DirectGLES 仅 `serverAuthoritative` level） | 罕见 | — |
| 8 | `glGetBufferSubData` / `glMapBuffer(READ)` on client-pending | 罕见 | monolith 里本来就阻塞；client pending 集合触发 |
| 9 | client 顶点数组的索引扫描 / restart 重写 / indirect-count 读（当 EBO 在 pending 集合里） | 罕见 | monolith 在同一位置调 `SyncGpuWrites()` |
| 10 | `glClientWaitSync(timeout>0)` 超出 watermark | 每帧级 | 应用请求的等待 |
| 11 | `glGetQueryObject*(GL_QUERY_RESULT)` 未完成；`glBeginConditionalRender` | 罕见 | `GL_Query.cpp:300`、`:705-706`（后者注释明说"by WAITING even for the _NO_WAIT modes"） |
| 12 | 轮询饥饿升级（连续 N 次无进展） | 极罕见 | 防死锁保险 |
| 13 | **分配类入口点的错误 ack**（`glRenderbufferStorage*`、部分 `glTexImage*`/`glTexStorage*`/`glCopyTexImage*`、`glBufferStorage`） | 罕见 | OOM 探测惯用法（§5.6c） |
| 14 | `AcquirePersistentMap`（仅 P7 T1） | 每 store 一次 | 返回映射 |
| 15 | ring/stage 耗尽、present credit | 节奏 | 非语义 |

### 变成异步或本地的

- 全部 20 个 draw、9 个 clear、blit/copy、`GenerateMipmap`、dispatch、barrier、image bind、7 个 XFB 跨度标记、`PatchParameteri`、`ShaderStorageBlockBinding`（权威状态已在 client，`GL_Program.cpp:3391`）、所有 buffer/texture/program/VAO/FBO delta、`Present`。
- **`glGetError` 永远本地**（`GL_Getter.cpp:2811-2817`；`Core.cpp:48-49` 的不变式）。
- **`glFinish`/`glFlush` 保持免费**。
- **89 个 caps 站点全部本地**（45 `GetDynamicParameters` + 8 `GetRendererInfo` + 4 `GetFormatCapabilities` + 3 `GetBackendType` + `IsTimerQuerySupported` + `PrefersCpuXfbPrimitiveAccounting` + `BeginOcclusionQuery!=nullptr`）。
- **`glGetIntegeri_v` 全部本地**；`glDispatchCompute` 的三次 per-dispatch 校验查询（`GL_Drawing.cpp:719`）改读 `CompileEnv::maxComputeWorkGroupCount`（`CompileEnv.h:52-54`）。
- **`GetInteger64i_v`、`GetProgramiv` 删除**。
- **`FenceSync`、`Begin{TimeElapsed,Occlusion,XfbPrimitives}Query`、`QueryCounterTimestamp` → client 铸造 handle**，fire-and-forget（前端本来就铸造应用可见的名字：`GL_Sync.cpp:61`、`GL_Query.cpp:54`）。
- **`GetSyncStatus`、`ClientWaitSync(0)`、`IsQueryResultAvailable`、`GetQueryResult64(wait=false)` → 先 publish（§7.2），再从水位一次 acquire load 回答**。miss 返回 `GL_UNSIGNALED` / "未就绪"，两处契约明确允许（`BackendObject.h:210-214`、`:236-241`；`GL_Query.cpp:302-311` 已遵守：读 0、**不缓存**、保留 backend handle）。
- **`glReadPixels` 进 PBO → fire-and-forget**（配 client 侧 `MarkGpuWritten`），比 monolith 更好。
- **`glEndTransformFeedback` 的无限 fence 等待取消**（配 client 侧对 capture target 置 `MarkGpuWritten`）。

**稳态帧：零 round trip**（对不使用 conditional render / 阻塞式 query / 分配类调用的帧而言；见 §15 P3 的门措辞修正）。

### fence 完成度必须来自真 fence，不是 present 水位（本轮新增）

上一版让 `retiredSeq`/`completedFrameSerial` 兜底 fence 语义。但在 DirectGLES 上这两个水位**只在 `Present()` 里前进**（`DirectGLES.cpp:10626-10643` 在 `eglSwapBuffers` 之后轮询 4 深 fence ring），或在 `WaitForFrameSerialCompleted`（`:10583-10607`）里。帧中创建的 fence 于是要等到**下一次 present 退休**才报 signalled，即 fence 完成度退化成帧计数推断。`DirectVulkan.cpp:1120-1128` 恰恰写明这是被修掉的 bug：完成度必须"track the GPU itself rather than the frame-count inference; MC 1.21.5's fence-paced ring buffers depend on this to recycle their space instead of growing without bound"，而项目记忆 `magma-mc1215-fence-oom` 记录了它曾导致 native-heap OOM kill。

**规则**：`RecFenceSync` 在 server 侧转成一次**真实的 backend `FenceSync()`**；server 用自己已有的逐 fence 轮询（DirectGLES 有 `WaitForFrameSerialCompleted` 的 fence 选择逻辑 `:10586-10600` 可复用；DirectVulkan 有 `IsSubmitIndexComplete`）在**非 present 时刻**也推进，并发 `EvFenceSignaled{handle}`。client 的本地快路径读的是"由真实逐 fence 退休导出的 handle 水位"，不是 present 水位。

### 三个应先独立落到 `dev` 的 monolith 修复（可二分、monolith 自身受益）
1. `glEndTransformFeedback` 的无条件无限 `ClientWaitSync`（`GL_Drawing.cpp:1326-1337`）→ 用既有 `MarkGpuWritten`/`SyncGpuWrites` 推迟到首次读。
2. `glDispatchCompute` 三次 `GetIntegeri_v` → `CompileEnv`。
3. 删除 `GetInteger64i_v`/`GetProgramiv` 两个死表项及两个 backend 的实现。

---

## 9. Present 与帧节奏

`eglSwapBuffers` → `EGLImpl::SwapBuffers`（`EGLImpl.cpp:162-183`）→ `BackendObject::SwapEGLBuffers`（`BackendObject.cpp:369-398`，其线程归属校验全部对 client 镜像的 EGL 状态求值，**不需要回复**）→ 发 `RecPresent{frameSerial, swapInterval}` → publish + 敲门铃 → 返回，除非 `presentsSent - presentAckSerial >= MOBILEGL_IPC_PRESENT_CREDIT`。

**`Present` 与应用的 `eglSwapBuffers` 严格 1:1，绝不批量。** Magma 侧四次 `OnFrameBoundary()` 缓存老化、`TryDrainFrameTransients` 和全部四次 `BeginFrame` 只在 `Present` 内发生（`VulkanRenderer.cpp:12765-12904`）；Espryt 侧三个 ring 与 `TrimBufferPool` 在那里 retire（`DirectGLES.cpp:10646-10649`）。批量会饿死这些排空。

### 9.1 延迟是叠加的：credit 默认改为 1

上一版设 credit=2 并论证它"镜像系统已有预算"，因此"不引入新的停顿类别"。停顿**类别**确实不新，但**延迟会叠加**，而上一版没有把它加起来：

- server 自己的 `Present` 在返回之前就已经等了 2-3 帧：`VulkanRenderer::Present` 末尾调 `FrameContext::WaitAndAcquireNextImage`，其第一条语句是 `vkWaitForFences(device, 1, &frame.imageInFlightFence, VK_TRUE, timeout)`（`FrameContext.cpp:288-290`）。`presentAckSerial` 因此只能在那次等待完成后才前进。
- 一个被允许领先 2 个 present 的 client，叠在一个自身已领先 GPU 2-3 帧的 server 上 = **端到端 4-5 帧**，60Hz 下 66-83ms，对第一人称游戏不可接受。
- 现有的验收门都看不见它：SSIM 是帧内容比较，`bench.sh` 量的是 FPS，都不是 input-to-photon。

**规则**：`MOBILEGL_IPC_PRESENT_CREDIT` **默认 1**（可配 1-4）。文档里写明叠加公式：`端到端 ≈ client credit + server FIF + 驱动深度`。P3 与 P9 的验收增加**输入延迟测量**：用已有的 `GetGpuTimestampNs` 与 trace-replay `--benchmark` 的逐帧 JSON 构建 "记录发射时刻 → present 完成时刻" 直方图；只有当实测吞吐收益能抵掉实测延迟代价时才调高 credit。

参考基线：`MagmaFramesInFlight = 3` 钳到 `[2, maxImageCount]`（`VulkanRendererConfig.h:14-19`、`VulkanRenderer.cpp:3051-3058`），Espryt 深度 4 的 fence ring 刻意高于驱动的 2-3（`DirectGLES.cpp:10071-10074`）。

### 9.2 swap interval 与 Magma

Swap interval 搭 `RecPresent` 过去。注意 Magma 从不注册 `SetSwapInterval`（`BackendObject_DirectVulkan.cpp:698` 只注册 `Present`）且偏好 `MAILBOX`/`IMMEDIATE`（`SwapchainObject.h:74-79`），因此 **IPC credit 成为 Magma 唯一的显式限帧器** —— 记录在案，P6/P9 在设备上测量输入延迟与帧节奏；若 Magma 需要，把"注册 `SetSwapInterval` 并映射到 FIFO"作为**独立的 `dev` 变更**，不让两套机制同时管节奏。

### 9.3 无 present 循环下的水位饥饿

`retiredTail` 的回收依赖 server 发布准确的 `completedFrameSerial`。DirectVulkan 有 `TryDrainFrameTransients`/`RefreshCompletedSubmits` 可以在非 present 时刻推进，**DirectGLES 没有对应物**：`g_completedFrameSerial` 只在 `Present()` 里（`DirectGLES.cpp:10626-10643`）和 `WaitForFrameSerialCompleted`（`:10583-10607`，且要求存在覆盖目标 serial 的活 fence，slot 被回收时返回 false）前进。在无 present 的负载里——`tools/cts` 的 `run_cts_local.py`、回读循环、从不 swap 的 `MG_IntegrationTest` 场景——一个 fence 都不会被插入，`retiredTail` 永不前进，`SEG_STAGE` 填满，§6.5 的升级路径在每个用例上都跑到硬 drain。那会把一次 CTS run 变成一连串 50ms 等待加整体 drain，并可能被误读成一致性回归。

**规则**：给 DirectGLES 的 server 加**非 present fence tick**——距上次 `Present` 超过阈值（默认 8ms）或每 N 条已 apply 记录（默认 4096）时，插入一个 `glFenceSync` 并轮询 fence ring，复用 `g_frameFenceRing` 机制。同时把 ring 占用率与升级次数打进 Tracy 计数器（P0 交付），让"水位饿死"表现为一个指标而不是一次无法解释的停顿。P2 增加一个无 present 的 split 用例。
