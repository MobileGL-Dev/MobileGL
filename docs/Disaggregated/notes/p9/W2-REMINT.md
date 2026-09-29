# P9 W2：`texture-remint-pull`——重铸不需要拉取

> 2026-09-29，分支 `p9/w2`（基线 `ad705a73`）。交接：[`HANDOFF-P9.md`](HANDOFF-P9.md) §3 W2 / W4。
> 裁定（集成方）：采纳 A——不做拉取协议，删 `OnTexturePullRequest`；monolith Espryt 的缺陷不在 P9 修（§6 单列）；W2-b 只退回 store；W4 不做（§5 只记测量）。

## 1. 结论

两个后端在 split 下重铸都只用 server 自己的字节，client 从不参与：

| 后端 | split | monolith |
|---|---|---|
| Espryt | `RequireImageBindableStorageByHandle`（`MobileGL/MG_Backend/DirectGLES/Managers.cpp:6289`，`c1bf7e15` 落地）：驱动上存在的层逐层读回旧 GPU 纹理（`ReadTextureLevelTight`），pending 盒用 `StagedTextureStore` 合并后整层重放；已 immutable 且无需加宽的原样保留（keep） | 前端臂 `RequireImageBindableStorage`（`Managers.cpp:6459`）重放 client 影子 |
| Magma | 重建带 `STORAGE` 的 VkImage，`PreserveTextureContentsOnRecreate`（`MobileGL/MG_Backend/DirectVulkan/Renderer/VkTextureManager.cpp:289`）GPU→GPU；wire 臂 `SyncWireTextureShape` 的 `preserve`（`:2254`） | 同一套（`SyncTextureResource`，`:3313`） |

- **只读 store 不够**：store 只有 client 上传过的字节；GPU 写过的层（渲染目标、clear、生成的 mip）在 store 里是旧的或没有。普查里 Espryt 的两次重铸都是 store 无字节的渲染目标（§4）。
- `Fatal{UnmigratedEmulation, "texture-remint-pull"}` 的两处标记都在前端臂里，transport 下不可达（P7 与本次普查都是 0 次），随本包删除。

## 2. 改了什么

| 包 | 内容 |
|---|---|
| W2-a | 回读前问驱动这一层在不在、尺寸对不对（`NativeTextureLevelHasExtent`，`DirectGLES/WireTextureReadback.inc:290`，`glGetTexLevelParameteriv`）。不在：store 覆盖就重放 store，否则跳过（GL 定义为未定义内容）。修的是：在最后一次 sync 之后才定义（空数据，或空数据 + 局部上传）的层，旧代码去读它 → FBO 不完整 → `Fatal{ResourceUnavailable, "image-promotion-readback"}`，合法 GL 让会话死掉 |
| W2-b | 驱动拒绝回读（设备上 SNORM / 16 位 norm / 无 `EXT_color_buffer_float` 的浮点不可渲染）而 store 覆盖该层 → 重放 store（= monolith 的答案），`MGLOG_W_ONCE` 带 `remint-readback-fallback`；store 也不覆盖才保留原 Fatal。测试旋钮 `MGITEST_ESPRYT_FORCE_REMINT_READBACK_FAILURE`（server 读） |
| W2-c | 删两处 `texture-remint-pull` 标记（transport 专用的提前标记块 + 循环内标记）；`PipeCatalogue.EveryUnmigratedEmulationIsNamedOnce` 5 → 4；「remint stall class」注释改写 |
| W2-d | `TextureRemintPulls`（`trp=`）在 server 路径也计：ByHandle 重铸已有存储时 +1（keep 不计），名字不改 |
| W2-e | 删 `OnTexturePullRequest`，`kMGPipeCallbackCount` 9 → 8；op 50 `ResourceSubDataComplete` 留作退役行（解码仍拒收），不改 `protocol.fbs` |
| W2-f/g | `TextureRemintPullScenario`：14 例 × 两后端 × {monolith, Split, Spawn, Tcp}；另有 `DirectGLES.{Split,Spawn}.RemintFallback.` 4 例（W2-b 旋钮，tcp 不带，理由同 `MAGMA_SERVER_ENV_KNOB_NO_TCP`） |

没改：Magma 无功能改动；`MOBILEGL_PIPE_TEXEL_RETAIN_MB`（`Config.h:400-403`）已无消费者但不在 PUSH 守卫内，删它动 pull 的 `Features` / `ConfigLoader`（G1），留到 P13。

## 3. 覆盖矩阵（场景 14 例）

内容 → 采样 draw（让后端持有存储）→（按例）GPU 写 → `glBindImageTexture` → compute `imageLoad` 拷到一张「sync 之前就 image 绑定」的 RGBA8 → FBO 回读，比 GL 正确答案。

| 例 | 内容来源 | store | 驱动上的层 | Espryt mono / pull | Espryt split / spawn / tcp | Magma 全部臂 |
|---|---|---|---|---|---|---|
| A 可变，上传 | 上传 | 有，= GPU | 有 | ✓ | ✓ | ✓ |
| B 可变，上传 → GPU clear | GPU | 旧 | 有 | **✗ 旧影子** | ✓ | ✓ |
| C immutable RGBA8，上传 → clear | GPU | 旧 | 有 | **✗** | ✓（keep） | ✓ |
| D 空定义 → clear | GPU | 无 | 有 | **✗ 零** | ✓ | ✓ |
| E clear → 未 sync 的局部上传 | GPU + 盒 | 盒 | 有 | **✗** | ✓（合并） | ✓ |
| F `glGenerateMipmap` 的 level 1 | GPU | 无 | 有 | **✗ 零** | ✓ | ✓ |
| F2 上传的 level 1 → clear | GPU | 旧 | 有 | **✗** | ✓ | ✓ |
| G sync 后空定义 level 1，绑 level 0 | — | 无 | **无** | ✓ | ✓（W2-a 前 Fatal） | ✓ |
| G2 sync 后以更大尺寸空定义 level 0 | — | 无 | 旧尺寸 | 不 Fatal | 不 Fatal | 不 Fatal |
| G3 G + 局部上传，绑 level 1 | 上传 | 整层 | **无** | ✓ | ✓（W2-a 前 Fatal） | ✓ |
| H 加宽 rg8 immutable，上传 → clear | GPU | 旧 | 有 | **✗** | ✓（promote） | ✓ |
| H2 / I / J 加宽 rg8 / RGBA8_SNORM / RGBA16_SNORM，仅上传 | 上传 | 有 | 有 | ✓ | ✓ | ✓ |

「谁都没有字节」只剩 G / G3，而那里 GL 不要求字节（G）或字节就在 store（G3）。Espryt 重铸代价：一次 `glFinish` + 逐层回读 + 整层重传，主机 0.3–6.5 ms / 次；GPU→GPU 是优化，只记录。

## 4. 普查（主机 lavapipe/llvmpipe，77 行 × {monolith, inproc} = 154 次，`MOBILEGL_PIPE_STATS=1`）

口径同 P7 E1（CI 旋钮、每条单独 `ctest`、`-j 2`）；154 次 rc 全 0、`Fatal{` 0 行。只有下表非零，其余 71 行全零：

| case | 后端 | monolith 重铸已有存储（`trp`） | split | mono / split SSIM |
|---|---|---|---|---|
| `iris-photon-v1.3b` | GLES | 1，RGBA16F 1 层，GPU ≠ 影子 | promote 1，回读 165,888 B，store 无字节，0.3 ms | 0.998774617 / 同（纹理随后被整张覆盖） |
| `iris-derivative` | GLES | 1，RGBA16F 1 层，GPU ≠ 影子 | promote 1，回读 3,279,360 B，store 无字节，6.5 ms | **0.996196552 / 0.996376198**（§6） |
| `create-indirect` | GLES | 0（首次分配，9 层） | 0（首次分配） | 0.999961275 / 同 |
| `iris-photon-v1.3b` / `iris-derivative` / `iris-iterationrp` | Vulkan | STORAGE 升级 1 / 1 / 4 | 1 / 1 / 5 | 全 `preserve`（GPU→GPU） |

与 P4a 的「780 个统计窗口 2 次」同两条 trace。测量用的是临时插桩（未提交），跑器与原始数据在 `~/w7/notes/p9/w2census/`。

## 5. W4：反向通道事件量（inproc，server 侧逐事件计，同一次普查）

| 量 | 读数 |
|---|---|
| `OnBufferWriteback` | 只有 `create-indirect`：两后端各 6 片、7,180 B，最大单片 6,656 B（上限 SEG_EVENT / 4 = 64 KiB） |
| `OnGpuWritten` | 只有 `create-indirect` × Vulkan：7,093 条（每条 1 个 whole-buffer range，几乎全在第一帧）；Espryt 同 trace 0 条 |
| `OnGlError` / 阻塞（ring 满）/ 丢弃 | 0 / 0 / 0 |

Magma 的 7,093 条是冗余的：transport 臂每次 draw / dispatch 对每个可写 SSBO / texel buffer / XFB 目标发一条 whole-buffer `OnGpuWritten`（`VkBufferManager.cpp` 的 `MarkWireBufferGpuWritten`），而 client 早已按同一组绑定自标保守集（`MG_Remote/Client/GpuWritePending.h`）；Espryt 的同名生产者 P5e fb 已在 transport 下删掉。**裁定：没有阻塞、没有丢弃 = 没有实测需求，不做。** 以后要做，改动是「transport 下只保留 server 簿记、不发事件」。今天没有逐 EventKind 的计数器，`eventDropped` 也不进 stats 行。

## 6. dev 缺陷：monolith Espryt 重铸重放过期影子（不在 P9 修，单独立项）

> **2026-09-29 已修**：dev `568090f0`（重铸前先把驱动上的各层读回影子，immutable 且无需加宽的分配直接保留），P11 合并进本线（`notes/p11/INTEGRATOR-DECISIONS-P11.md` ID-P11-6）；`TextureRemintPullScenario` 对 monolith DirectGLES 的 7 例 skip 已撤，iris-derivative 的 monolith SSIM 0.996196817 → 0.996376498，与 split 各臂相同。下文是当时的记录。

- **一句话原因**：前端臂 `RequireImageBindableStorage`（`Managers.cpp:6459`）把每个已定义层标脏、从 client `MipmapStorage` 重放进新 carrier，而影子从没见过 GPU 写的内容（渲染目标、clear、`glGenerateMipmap` 的层、`imageStore`）；immutable RGBA8 也走这条（monolith 没有 split 的 keep）。
- **复现**：本场景的 B、C、D、E、F、F2、H 在 `DirectGLES.TextureRemintPullScenario.*`（monolith）和 pull 构建（`build-linux`）逐例同错（B：读到上传的图案而不是 clear 的绿；D、F：读到零）。Magma 两臂全对。场景对 monolith DirectGLES 这 7 例按名 skip，理由指向本节。
- **真实证据**：`iris-derivative` × DirectGLES，P7 普查 §3 #6 那个「未归因」的 ~26.9k px 系统差。monolith 在该 trace 重铸一张 854×480 RGBA16F（GPU ≠ 影子）；把前端臂改成重放 GPU 字节（临时实验）后 monolith 的 `actual.png` 与 split 逐字节相同（sha `ebbe8e9d…`），SSIM 0.996196552 → 0.996376198（更近 golden）。`iris-photon-v1.3b` 同样重铸一次，但纹理随后被整张覆盖，无可见差。
- **建议修法**：前端臂在重放前，对 `hadBackendStorage` 且无 pending 的层从旧驱动纹理读回（Espryt 已有同一把工具 `ReadTextureLevelTight`，目前只编进 `MOBILEGL_BUILD_DISAGGREGATED`）写进影子再重放；或更干净地学 Magma：`glCopyImageSubData` / 着色器拷贝 GPU→GPU，影子不参与。
- **G1**：函数主体是 pull 构建共用代码；任何修法都会动 pull 的 `.text`（`ReadTextureLevelTight` 还得搬出 disaggregated 守卫）。按规矩在 dev 上单独修，不在 P9。

## 7. 未决 / 观察

- **keep 路径的一个排序缺口（非重铸问题）**：C 在 Espryt split 上不加 barrier 时 30 次里 3 次读到 clear 之前的纹素（加 `glMemoryBarrier(GL_ALL_BARRIER_BITS)` 后 0 / 30），失败与通过两种运行里 server 的每个同步决定完全一样（临时日志：keep、无 regen、无上传）。GL 不要求 framebuffer 写之后、imageLoad 之前加 barrier；promote 路径因 `glFinish` 回读被串行化，所以只有 keep 暴露。怀疑 llvmpipe 的 compute image 读没有等待挂起的光栅场景，未归因。场景在 dispatch 前加了 barrier（注释写明），这件事单独记。
- **设备格式风险**：Adreno 830 上 SNORM 等格式的回读是否被拒没测（本包不碰设备）；W2-b 让这种情况退回 store，只有「GPU 写过 + 不可读 + store 无字节」才 Fatal。
- 多重采样：`ReadTextureLevelTight` 对 MS 直接失败；ES 没有 MS image 单元，只有「MS + 需加宽 + 已有存储」才进 promote。理论可达，没见过。

## 8. 门与 red-once（R-16）

| 门 | 红的方式（临时改动，已还原，真跑过） | 结果 |
|---|---|---|
| G：`ALevelDefinedWithoutDataAfterTheLastSyncDoesNotEndTheSession` | 去掉 W2-a 检查 | Split / Spawn / Tcp 3 / 3 `Subprocess aborted` |
| G3：`APartialUploadIntoALevelDefinedAfterTheLastSyncReachesTheImage` | W2-a 与 W2-b 一起去掉（G3 被两者各自兜住） | 3 / 3 aborted（G 同） |
| `RemintFallback.*`（W2-b） | 退路改回 Fatal | 8 / 8 aborted |
| `trp=`（W2-d） | 删 server 路径的 +1 | inproc 上 A、H 红，C（keep）保持绿 |
| Espryt 读 GPU 是承重的 | 不改代码，整组 split 场景带 `MGITEST_ESPRYT_FORCE_REMINT_READBACK_FAILURE=1`（= 只读 store） | B、E、F2、H 读到旧字节，D、F aborted（store 无字节）；C（keep）、A、G、G2、G3、H2、I、J 绿 |
| Magma 的 GPU→GPU 保留 | 两臂 `preserve` 恒 false | 4 臂 × 12 例红（G2、G3 除外） |
| 回调数、名单 | `static_assert` + `PipeCatalogue.ReverseChannelHasTenCallbacks`（断言 8）、`EveryUnmigratedEmulationIsNamedOnce`（断言 4） | 编译期 / 名单钉 |
