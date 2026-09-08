# 实测记录（P0、P1、P2）

> 每张表都写明设备、提交与命令，以便复现。设备：`35d0befa` = Xiaomi 24129PN74C，Adreno 830，Android 16；`3B159D009VZ00000` = Oppo PLG110，Mali，Android 16（ColorOS）。设备运行日期 2026-09-05。设备锁协议照旧。

## 1. Spike A — 从应用自身进程 exec 第二个原生可执行文件

问题：Android 上能否把 server 以 `lib*.so` 打进 APK，并从应用自己的 `untrusted_app` 域 `fork`+`execve` 它（`adb run-as` 跑在别的域，证明不了）。

| 设备 | 结果 |
|---|---|
| Adreno 830 | **OK**。父进程 `u:r:untrusted_app:s0:c173,c257,c512,c768` `fork`+`execve` `<nativeLibraryDir>/libMobileGLServer.so` → 子进程 pid 31348，exit 0；子进程 SELinux `u:r:untrusted_app:s0:c173,c257,c512,c768`（同域同 category）；marker 文件、stdout 捕获、报告全在；`execErrno=0`；窗口内**零 avc denial** |
| Mali | **OK**，同形：子进程 pid 28433，exit 0，`execErrno=0`，`u:r:untrusted_app:s0:c94,c257,c512,c768`，零 avc denial |

主机侧已证的三条（随 `8a239177`）：AGP 会把改名成 `lib*.so` 的 `add_executable` 打进 `lib/arm64-v8a/`，前提是把 `RUNTIME_OUTPUT_DIRECTORY` 重定向到 `CMAKE_LIBRARY_OUTPUT_DIRECTORY`；`posix_spawn` 在 minSdk 26 不可用（bionic API 28 起），出货臂是 `fork`+`execve`；应用进程 stdout/stderr 是 `/dev/null`，子进程用 marker 文件证明自己活过。

- 代码：`tools/spikes/server_stub/main.cpp`（stub：打印并写 marker 自己的 pid/uid/SELinux 上下文）、`android-plugin/app/src/trace/cpp/spawn_spike.cpp`（`RunSpawnSpike`）、`CMakeLists.txt:784-808`（`MOBILEGL_BUILD_SERVER_SPIKE`）。
- APK：`p0-spike-a-android/trace-debug-spike-on.apk`（在 `30d7595b` 构建，与 `7ef7c7e5` 源码相同）。
- ColorOS 陷阱：首次 `adb install` 一个未安装的包会卡在 `com.oplus.appdetail InstallGuideActivity` 确认页，直到点"继续安装"（1272×2772 面板上 `input tap 353 2349`）；同签名重装静默通过。另一台设备上一个外来签名的 trace APK（versionCode 26080769）会让 `install -r` 报 `INSTALL_FAILED_UPDATE_INCOMPATIBLE`，需先卸载。
- 42-device.sh 的 env 透传 A/B 腿在该 ROM 上跑不了（`run-as sh -c 'cat > files/…'` 被拒）；透传由下面的 stats 基线端到端证明（`--env MOBILEGL_PIPE_STATS=1` 必须在 `mobilegl.log` 里产生 `MGPipe stats` 行）。

## 2. Spike B — 跨进程外部内存分档

问题：`AcquirePersistentMap` 背后的内存能否共享给另一个进程并在那里映射，两个后端各走哪条路。探针 `tools/spikes/extmem_probe/`（`39f982e6` 源码，arm64，`adb shell` = `u:r:shell:s0` 域），4 MiB payload，64 KiB 同判决。每一行都取一次真 GPU 访问（`vkCmdCopyBuffer` + `vkCmdFillBuffer` + host-read barrier）并两侧字节校验才算 OK。

| 路线 | Adreno 830 | Mali |
|---|---|---|
| T1-opaque-fd（server 导出 `VkDeviceMemory` fd，client 裸 `mmap` + 导入） | **OK** 完整往返含 GPU 访问（`/dmabuf:system`，dedicatedOnly=1） | UNSUPPORTED（`vkCreateBuffer(external)=VK_ERROR_INVALID_EXTERNAL_HANDLE`，advertisedExportable=0） |
| T1-dma-buf | UNSUPPORTED（`VK_EXT_external_memory_dma_buf` 缺） | UNSUPPORTED |
| T1-gles-memobj-fd（`GL_EXT_memory_object_fd` 导入导出的 fd） | **FAIL**：导入 + `glBufferStorageMemEXT` 接受（`GL_NO_ERROR`）但每次 `glMapBufferRange` → `GL_INVALID_OPERATION`（persistent 与 plain 都是）；`GL_DEVICE_UUID` 不可读 | UNSUPPORTED（扩展字符串缺，入口点可解析） |
| **T0-ahb-blob-transfer**（client 分配 `AHardwareBuffer` BLOB → socket 交接 → server Vulkan 导入 + GL 导入） | **OK** 全链：cpu-lock、vk-import+map、GPU copy/fill、GL map persistent+coherent、写回 client 全部字节校验 | **OK** 全链，判决相同（glPersistentCoherent=1，gpuRan=1） |
| T3-external-memory-host（`VK_EXT_external_memory_host`） | UNSUPPORTED（扩展缺） | PARTIAL：导入 + map 往返，但 **GPU 写对宿主映射不可见**（只读档） |
| T3-memfd-cross-process / client-memfd-server-import | UNSUPPORTED | OK / PARTIAL（同样的 GPU 只读 caveat） |

**P11 的分档决定**：唯一在两台设备、两个后端上都是完整读写的档是 **T0**——client 分配 `AHardwareBuffer` BLOB，server 以 `VK_ANDROID_external_memory_android_hardware_buffer`（Magma）或 `EGL_ANDROID_get_native_client_buffer` + `glBufferStorageExternalEXT`（Espryt）导入，两侧 persistent+coherent 映射。Adreno 另有 T1（Vulkan 路径）；Mali 无任何 server 导出路线，host-pointer 导入只读。Caveat：运行域是 `shell` 不是 `untrusted_app`；AHB 的 socket 交接是每个与 SurfaceFlinger 共享 buffer 的应用都在走的路径，域风险在 memfd/opaque-fd 腿上。

复现：

```sh
ANDROID_NDK=$HOME/android-sdk/ndk/27.3.13750724 tools/spikes/extmem_probe/build_android.sh /tmp/extmem-build
S=<serial>; adb -s $S push /tmp/extmem-build/extmem_probe /data/local/tmp/extmem_probe \
  && adb -s $S shell "chmod 755 /data/local/tmp/extmem_probe && /data/local/tmp/extmem_probe; echo EXIT=\$?" | tee out-$S.txt
```

判决语义（OK / PARTIAL / FAIL / UNSUPPORTED）与逐腿 trace 格式见 `tools/spikes/extmem_probe/README.md`。主机构建（lavapipe）用来证明探针本身报得对：T1/T3 在 lavapipe 上全 OK；T1-gles 在 llvmpipe 上 `GL_OUT_OF_MEMORY` 是 Mesa interop 缺口，不是探针缺陷。

## 3. 边界计数器基线（双设备、双后端、四条 trace）

`MOBILEGL_PIPE_STATS=1` 经 retrace 通道的 `--env` 透传；trace APK 从 `7ef7c7e5` 构建，spike OFF。取每次运行的**最后一个完整 120 帧窗口**。accessor/draw 与 memo 门数字是软件确定的（同一 trace 在两台设备上完全相同：它们数的是代码路径不是硬件），只有墙钟/CPU 时间随设备变。

| trace（窗口内帧数） | 后端 | draws/f | **acc/draw** | buf B/f | tex B/f（发射 box/rect） | ubo-global B/f | **ubo-named B/f** | memo 门（hit/miss） |
|---|---|---|---|---|---|---|---|---|
| `minecraft-1.21.4-in-world`（360） | Espryt | 91.6 | **9.28** | 13.5 K | **635 K**（185 box / 0 rect） | 16.7 K | 0 | ers 9257/2577，etl 10538/1296，eub 10720/1114 |
| `minecraft-1.21.4-in-world`（360） | Magma | 91.6 | **8.56** | 13.5 K | 39.9 K（97 box / 89 rect） | 16.7 K | 0 | mfp 0/10994，mpm 9240/1754，mdt 9120/1874 |
| `minecraft-1.21.4-fabric-iris-bsl-in-world`（120，memo 冷） | Espryt | 23.2 | 21.04 | 32.6 K | 8.8 K | 1.8 K | 0 | ers 1958/1843，etl 722/3079 |
| `minecraft-1.21.4-fabric-iris-bsl-in-world`（120，memo 冷） | Magma | 23.2 | 11.26 | 313 K | 256 K | 1.8 K | 0（vtxc 1.7 K） | mpm 1890/895，mdt 1573/1212 |
| `improved-transparency-minecraft-26.3`（1200） | Espryt | 1320 | **8.44** | 333 K | 0 | 0 | 0 | ers 156925/2791，etl 148606/11110，eub 148246/11470 |
| `improved-transparency-minecraft-26.3`（1200） | Magma | 1320 | **6.53** | 173 K | 0 | 0 | **331 K** | mfp 21360/137036，mpm 134421/2615，mdt 156611/1785 |
| `minecraft-1.21.1-neoforge-create-indirect-in-world` | 两者 | — | — | — | — | — | — | 两台设备都失败（§5），且不足 120 帧 |

门缩写：ers = `EsprytRenderState`，etl = `EsprytTextureSyncList`，eub = `EsprytUnitBindingsEpoch`，mfp = `MagmaDrawFastPath`，mpm = `MagmaPipelineMemo`，mdt = `MagmaDynamicTail`（`MobileGL/MG_Util/Metrics/PipeStats.h:46-122`）。`accessor-calls` 是约 10 个热入口的静态计数，是每 draw accessor 数的**下界**（站点清单 `MobileGL/MG_Util/Metrics/PipeStats.cpp:16-100`）。

对设计的读法：

- **真机稳态动态 accessor 成本是每 draw 6.5–9.3 次**（预测区间 10–25 的下沿；llvmpipe 的 15.5/20.7 是 memo 冷的）。推送要打败的是 ~8 次 accessor + memo 探测，不是 124/169 的静态调用点数。GO/NO-GO 的 tracker 绝对 ns 上限从这里定。
- **`stage-ubo-named`（D-B8）**：Magma 在 26.3 世界每帧重打包 **331 KB** 具名 UBO 字节，Espryt 直接绑定为 0——host payload 决定的第一个真数字。
- **union box vs region list**：vanilla 世界同样 185 次发射，Espryt 的整 box 路径移动 **635 K** 纹素字节/帧，Magma 的 rect 路径 **40 K**，16×——"server 选上传形状"这一条的量化依据（Mali 侧的 +6 ms/frame 作业数悬崖在另一个方向）。

复现（一台设备一次；两台必须**串行**，见 §4）：

```sh
ANDROID_SERIAL=<serial> MSYS_NO_PATHCONV=1 \
python3 tools/trace_replay/run_android_retrace_local.py \
  --case minecraft-1.21.4-in-world --backend DirectGLES \
  --env MOBILEGL_PIPE_STATS=1 --env MOBILEGL_PIPE_STATS_PERIOD=120
# 数字在结果目录的 mobilegl.log 里，grep 'MGPipe stats:'，取最后一个完整窗口
```

## 4. 桌面数据点与语料事实

- **llvmpipe / lavapipe 动态 accessor**（`GuiBatchScenario`，14 帧 / 26 draw，memo 冷）：Espryt 20.65 / Magma 15.54 次/draw——落在预测区间内，且因场景太短偏高；真机稳态数字见 §3。
- **dirty-surface 面**（`python3 scripts/gen_pipe_dirty_surface.py --summary`，本树）：`MG_Impl/GLImpl` 41 个文件，926 次 mutator 调用，73 个不同 mutator（`RecordError` 一项就占 836 次）；92 次（36 个即时发布点、7 个 mutator，绝大多数是 `RecordError`）位于同函数内也到达后端的入口，其余 834 次由紧随的 verb 发布。映射表是 73 条目的问题。
- **读点覆盖**（`python3 scripts/gen_pipe.py`）：71 条调用（11 screen / 60 context）、63 个 verify payload、61 个 `PipeInputs` 字段；477 行后端读点清单 → 299 调用、5 client 自答、6 反向通道、167 结构性句柄、**0 UNMAPPED**。
- **OOM 探测惯用法**：41 个 trace fixture 中 0 例——全部语料只有 9 次 `glRenderbufferStorage` 调用散在 5 个 fixture，无一在其后 3 个调用内跟 `glGetError`；语料里真实的成功性检查是 `glCheckFramebufferStatus`。→ `glRenderbufferStorage*` 不 ack。
- **`FramebufferSrgb` / `DepthClamp`**：`FramebufferSrgb` 的六个后端读点全部消费一个编译期常量 `false`，`DepthClamp` 零读点；两者的 `glEnable` 落到 `RenderState.cpp` 的 `default:` 分支既不存储也不报 `GL_INVALID_ENUM`；41 个 fixture 无一开启任一项（补真存储不会改动任何既有 fixture 的输出）。
- **`GetIntegeri_v` 族**：Espryt 实现里是 `GetIntegeri_v` 的 9 个分支 + `GetInteger64i_v` 的 2 个（不是"15 个 case"）；`GL_COMPUTE_WORK_GROUP_SIZE` 由 `GL_Program.cpp` 用 `ProgramObject::GetComputeLocalSize` 纯前端回答。
- **payload 尺寸**（`MG_Pipe/MGPipeTypes.h` 的 `static_assert`，arm64 与 x86-64 一致）：`MGPDrawInfo` **56**、`MGHostSpan` 32、`MGPBindRenderState` **12**、`MGPResourceDesc` 88、`MGPFramebufferState` 304、`MGPProgramDesc` 192、`MGPSubData` 72、`MGPPixelPackState` 28、`ResidualValueBlock` **1248**（其中 `RenderStateParameters` 1168）。`SEG_CMD` 按 56 B 固定头定尺：MC 帧 1000–4000 draw 时每帧 56–224 KiB 头字节。
- **persistent map 采纳的既有基线**（`dev`，MC 26.3，Adreno）：≥16 MiB 可变 store 定义时采纳为 coherent persistent map 后 p99 163→21 ms、稳态 40→115 fps、省 ~400 MB。P11 的回归上限对着它。
- **Mali 上传作业数悬崖**（Espryt 代码注释记录的既有实测）：~100 个精灵 rect 对一个 union box 是 +6 ms/frame。

## 5. Harness 事实与陷阱

1. trace app 从不到达 `MobileGL::DestroyImpl`，所以 `MOBILEGL_PIPE_STATS_FILE` 的 JSON 转储在设备上永远不会写——只有 `mobilegl.log` 里的周期汇总行；短于一个周期的 trace 什么都不产出。`MOBILEGL_PIPE_STATS_PERIOD`（`458ccde1`）为此而加：需要数字的运行把它设到足够小。
2. `run_android_retrace_local.py` 每棵树共用一个 `.trace-work/android-retrace-result` 根并在每次调用时 `rmtree`，所以两台设备必须从一棵树**串行**跑。
3. `--env` 值里嵌入的 `/data/...` 会被 runner 的 bash.exe 做 MSYS 路径转换（`MSYS2_ARG_CONV_EXCL="/data/*"` 只覆盖开头匹配）——用 `MSYS_NO_PATHCONV=1` 跑。
4. `coherent_as_flush` 管线完好：`--ez coherent_as_flush true` → `trace_replay_core.cpp` 的 `setenv`，独立于 `--env` 透传。
5. **`minecraft-1.21.1-neoforge-create-indirect-in-world` 在两台设备上都失败**（Adreno 830：Espryt ~4.5 分钟后黑帧，Magma 纹理上传提交时 `VK_ERROR_DEVICE_LOST`；Mali：SSIM 0.85 / 0.45）。Adreno 830 上用 `dev@81b17c0b` 基线 APK 复现，**是基线就有的问题，不是本分支造成**；它是 P3a/P8 验收清单里的用例，需先在 `dev` 修。

---

# P1 实测（`feat/disaggregated`，lavapipe / llvmpipe）

## 6. 规模：站点、访问器、填充点

| 量 | 值 | 出处 |
|---|---|---|
| 后端 `pGLContext->` 箭头站点 | 277（Espryt 113、Magma 164） | 计划写 293，出自过期的 vendored 清单 |
| 非箭头行 | 58（Espryt 9、Magma 49，其中 43 条是 Magma 的逐 verb `MOBILEGL_ASSERT`） | 与计划一致 |
| `PipeInputs` 字段 | 63 | 计划写 61；`GetBoundTransformFeedbackLifetimeId`、`HasOpenTransformFeedbackSpan` 是 D21 之后新增的读点 |
| 后端调用的不同访问器 | 62（Espryt 32、Magma 56） | `GetBoundTransformFeedbackName` 已无人读，留作已标注的死行 |
| 填充点 | 83 条 `MGP_FILL`，覆盖 69 个 verb、9 个类 | `MG_Pipe/FillPoints.def` |
| `SyncPersistentMappedRange` / `SyncGpuWrites` | 20 / 6 | 与计划一致 |

## 7. verify 通道发现的两类真问题

**缺填充行（9 处）。** 逐 verb poison 在 79 例 retrace 与 818 条 integration-verify 上抓出三组：`kReadback` 缺 `IsTransformFeedbackActive`/`IsTransformFeedbackPaused`（深度/模板回读仿真的 `ScopedEmulationDrawState` 会暂停在飞的捕获）、`kTextureOp` 与 `kDispatch` 缺 `IsCapabilityEnabled`、`kBlitOrCopy`/`kTextureOp` 缺着色器 blit 用到的 viewport 与顶点/缓冲绑定。其中 8 行是静态过近似（代码路径可达但通道未跑到），过近似会让那对 (类, 字段) 的 poison 永久失效，**P2 收紧填充表时先复查这 8 行**。

**verb 内后端改前端（3 个字段）。** Magma 在自己的 draw 里写前端对象（为未绑定采样器合成回退纹理、材质化排队清除、覆写采样器 filter），把边界已经拷走的值挪了位，8 条 DirectVulkan 用例与 2 条 trace 因此报 `Fatal{PipeVerifyDiffer, "GetSamplingResolutionGeneration@Draw*", where=read}`。**选定的解法是 push-on-mutation**：前端计数器移动时用 `MGP_NOTE_MUTATION(Field)`（`MG_Pipe/PipeMutation.h`）刷新推送块里的那一个字段（只刷值不刷戳记），"推送块在每次读取时都等于活上下文"这条不变式因此字面成立，也正是 P2 tracker 需要的形状。

三个被这样处理的字段与它们的钩子：

| 字段 | 钩子 |
|---|---|
| `GetSamplingResolutionGeneration` | `TextureState::BumpSamplingResolutionGeneration()` |
| `GetTextureBindGeneration` | `TextureState::BumpTextureBindGeneration()` 与 `NoteUnitTouched()` 的 `bindingChanged` 分支 |
| `GetMaxTouchedTextureUnit` | `NoteUnitTouched()` 的高水位分支 |

钩子挂在**计数器**上而不是四十个写入点上：后端写前端的全部路径（`SamplerObject` 的 8 个 setter、`ITextureObject` 的 6 个、纹理单元绑定路径）都经由这三个计数器，`BufferObject`/`ProgramObject`/`VertexArrayObject` 的后端写入不移动任何推送字段（它们是句柄类读点）。

## 8. P1 验收（合入后在 `~/w7/pipe` 实测）

| 门 | 结果 |
|---|---|
| pull 构建符号与 `.text` | 0 增 / 0 删 / 0 改尺寸 / 0 重命名，`.text` 字节不变 |
| `MG_Backend` 里的 `pGLContext` | 0（唯一保留处是开关头文件的 pull 分支） |
| 单元（pull / push / verify） | 1485 全绿 × 3 |
| `integration-gpu`（pull） | 878 全绿 |
| `integration-verify` | 818 全绿，零 `Fatal{` |
| 79 例 retrace（`MOBILEGL_PIPE_VERIFY=1`） | 79/79 通过，79/79 带 armed 摘要，零 `Fatal{` |
| 两个阴性对照 | 4 条专用条目全绿（篡改字段变红、抽掉一个填充戳记在那条 verb 上变红） |
| 测试名 | 0 删除，+29 |

**verify 构建的代价**：`integration-verify` 818 条在 4 路并行下约与 `integration-gpu` 同量级；79 例 retrace 在 4 路下约 20 分钟。

---

# P2 实测（`feat/disaggregated@738b289d`）

> **口径变更（用户 2026-09-08）**：push 比 pull 多约 10% 逐线程 CPU 可接受；**性能自此对着 pull 臂记录，不作阻塞门**——pull 臂的数字就是此后的基准线，第 43 天的 tracker 绝对 ns 上限降为记录项；路线图先推完（下一步 P3a），专门的优化阶段排在其后（或首个 IPC 帧之后）。所以本部分把"门"与"记录"分开写：§9 是门，§10–§14 是记录与遗留判定。

## 9. P2 验收门（合并点实测；一个在飞的修复落地后复核）

| 门 | 结果 |
|---|---|
| G1 pull 构建符号 | 0 增 / 0 删 / 0 重命名；**4 处 resize**，全部事先认定：`RenderState::{RenderState, SetCapability, IsCapabilityEnabled}` 与 `_GLOBAL__sub_I_DirectGLES.cpp`（`g_syncedRenderStateParameters` 的静态初始化器）；`.text` **+160 B** |
| G5 `SyncRenderState` 一行未动 | `DirectGLES.cpp` 里整个 `namespace RenderStateImpl` 的 sha 与 P2 起点**逐字节相同** |
| G2 pull 与 push 逐名相同 | `ctest -N` 名集合**差 0** |
| G14 测试名只增不删 | **0 删除，+119**（+115 的 P2 包 + ABA 世代覆盖的 4 条单元） |
| 单元 | **1566** 全绿 × {pull, push, verify}（1562 + ABA 世代覆盖的 4 条 `MagmaPipeIdentity` 单元） |
| `integration-gpu` | **916/916** pull、**916/916** push、**916/916** `MOBILEGL_PIPE_PUSH=0`（全 pull 臂） |
| 渲染状态敏感子集 | **72/72** |
| `integration-verify` | **828** 条，零 `Fatal{` |
| 79 例 retrace | push 下 **79/79**；`MOBILEGL_PIPE_VERIFY=1` 下 **79/79 全部 armed，零分歧** |
| G7 setter 一致性的负面对照 | 按设计变红并**点名 `SetColorMask`**（把一个成员从 pipeline 半边降到 dynamic 半边，划分仍完整、仍能编译） |
| CSO 内容寻址对照 | `CsoContentAddressing` **6/6** |
| verify 构建的三组对照 | `PoisonOmitted`、`VerifyCorrupted`、`HandleRecycle` 合计 **44/44** |

## 10. 设备配对 A/B：逐线程 CPU（D.4.2）

协议：reboot-clean、同热窗口、按项目协议定频（大核 1.96 / 小核 1.55 GHz、GPU 拉满、40 °C 门），两臂背靠背同一会话，一次一台设备（`run_android_retrace_local.py` 每棵树共用一个结果根，见 §5.2），`--benchmark-no-finish` 为主臂（P2 问的是 CPU）。数字取 `benchmark.json` 的 `frameCpuTimesMs[]` 尾 200 帧，主机侧算 p50/p99。`minecraft-1.21.1-neoforge-create-indirect-in-world` 不在设备 A/B 里（§5.5，基线就坏）。

每格取 runner 自己的 "best of 3"（三次重复里平均墙钟帧时间最低的那次；`run_android_retrace_local.py` 的约定），p50 按设备的中位数规则、p99 为 nearest-rank，都在同一尾窗口上算；`tools/device_bench/pin_device.sh check` 在每次运行前后各跑一次，判定记在最后一列（P = 三个节点都在钉住的频率上；D = 大核被热管理钳在 1689600 kHz，脚本钉的是 1958400——用例前后两臂同一状态才可比）。APK 两臂出自 `55d2af9b`（P2 集成后、ABA 对照补丁前；差异只在负面对照臂）。

小米 24129PN74C（`35d0befa`，Adreno 830，会话 reboot-clean 一次、整段钉频），单位 ms/帧：

| trace | 后端 | finish | pull p50 | push p50 | Δ p50 | pull p99 | push p99 | Δ p99 | 钉频 pull / push |
|---|---|---|---|---|---|---|---|---|---|
| `minecraft-1.21.4-in-world` | DirectGLES | 关 | 7.392 | 8.187 | **+10.8%** | 11.086 | 13.192 | +19.0% | P/P |
| `minecraft-1.21.4-in-world` | DirectGLES | 开 | 7.433 | 8.383 | **+12.8%** | 10.969 | 11.789 | +7.5% | P/P |
| `minecraft-1.21.4-in-world` | DirectVulkan | 关 | 5.230 | 5.849 | **+11.8%** | 6.772 | 7.396 | +9.2% | P/P |
| `minecraft-1.21.4-in-world` | DirectVulkan | 开 | 5.237 | 5.842 | **+11.6%** | 6.767 | 7.380 | +9.1% | P/P |
| `improved-transparency-minecraft-26.3` | DirectGLES | 关 | 37.263 | 42.436 | **+13.9%** | 54.965 | 60.409 | +9.9% | P/P |
| `improved-transparency-minecraft-26.3` | DirectGLES | 开 | 37.222 | 42.422 | **+14.0%** | 54.879 | 60.401 | +10.1% | P/P |
| `improved-transparency-minecraft-26.3` | DirectVulkan | 关 | 61.549 | 68.118 | **+10.7%** | 79.403 | 88.111 | +11.0% | D/D（热钳，两臂同） |
| `improved-transparency-minecraft-26.3` | DirectVulkan | 开 | 61.446 | 68.252 | **+11.1%** | 79.244 | 88.357 | +11.5% | D/D |
| `minecraft-1.21.4-fabric-iris-bsl-in-world` | DirectGLES | 关 | 4.911 | 5.353 | **+9.0%** | 2314.8 | 2315.7 | 编译主导 | P/P |
| `minecraft-1.21.4-fabric-iris-bsl-in-world` | DirectGLES | 开 | 4.972 | 5.370 | **+8.0%** | 2312.5 | 2322.2 | 编译主导 | P/P |
| `minecraft-1.21.4-fabric-iris-bsl-in-world` | DirectVulkan | 关 | 4.158 | 4.525 | **+8.8%** | 1604.5 | 1624.2 | 编译主导 | P/P |
| `minecraft-1.21.4-fabric-iris-bsl-in-world` | DirectVulkan | 开 | 4.158 | 4.583 | **+10.2%** | 1614.0 | 1646.9 | 编译主导 | P/P |
| `minecraft-1.21.4-startup` | DirectGLES | 关 | 0.941 | 1.068 | +13.5% | 1180.4 | 1291.2 | 加载主导 | D/D |
| `minecraft-1.21.4-startup` | DirectGLES | 开 | 0.822 | 0.841 | +2.3% | 1271.6 | 1284.3 | 加载主导 | D/D |
| `minecraft-1.21.4-startup` | DirectVulkan | 关 | 0.382 | 0.415 | +8.6% | 1463.0 | 1487.1 | 加载主导 | D/D |
| `minecraft-1.21.4-startup` | DirectVulkan | 开 | 0.368 | 0.422 | +14.7% | 1485.4 | 1459.7 | 加载主导 | D/D |

读法：**推送没有在拉取基线之下净减少**（那是开放问题 1 原本的期望），而是在四个 trace、两个后端上稳定多花 **8–14% 逐线程 CPU**（p50），p99 同向；finish 开与关两臂几乎一致，说明多出来的是客户端 CPU 而不是 GPU 时间。三点读数纪律：bsl 用例的 p99 两臂都由着色器编译主导（约 1.6–2.3 s），startup 用例只有 59 帧尾窗且 p99 是加载，两者的 p99 都不承载这个问题；26.3 的 DirectVulkan 两臂都在热钳下跑（大核 1689600 kHz），绝对值偏高但两臂同状态，相对差有效；`acc/draw` 两臂相同（accessor 计数还是 tracker 填充时的调用，P2 没改它的定义）。

Oppo PLG110（`3B159D009VZ00000`，Mali，会话 reboot-clean 一次、整段钉频，32 次运行前后判定全部 PINNED），单位 ms/帧：

| trace | 后端 | finish | pull p50 | push p50 | Δ p50 | pull p99 | push p99 | Δ p99 | 备注 |
|---|---|---|---|---|---|---|---|---|---|
| `minecraft-1.21.4-in-world` | DirectGLES | 关 | 8.297 | 9.803 | **+18.2%** | 10.446 | 11.817 | +13.1% | |
| `minecraft-1.21.4-in-world` | DirectGLES | 开 | 8.366 | 9.700 | **+15.9%** | 10.774 | 11.935 | +10.8% | |
| `minecraft-1.21.4-in-world` | DirectVulkan | 关 | 5.341 | 5.941 | **+11.2%** | 6.785 | 7.287 | +7.4% | |
| `minecraft-1.21.4-in-world` | DirectVulkan | 开 | 5.349 | 5.931 | **+10.9%** | 6.765 | 7.376 | +9.0% | |
| `improved-transparency-minecraft-26.3` | DirectGLES | 关 | 41.252 | 45.022 | **+9.1%** | 79.054 | 91.729 | +16.0% | |
| `improved-transparency-minecraft-26.3` | DirectGLES | 开 | 41.062 | 44.227 | **+7.7%** | 82.357 | 85.087 | +3.3% | push 臂第 1 次重复被 harness 误杀（见下），best of 2 |
| `improved-transparency-minecraft-26.3` | DirectVulkan | 关 | 54.478 | 60.108 | **+10.3%** | 91.733 | 111.789 | +21.9% | |
| `improved-transparency-minecraft-26.3` | DirectVulkan | 开 | 50.657 | 59.829 | +18.1% | 68.068 | 100.402 | +47.5% | pull 臂这次 best-of-3 明显快于同臂 nofinish（50.7 对 54.5），差值被放大；以 nofinish 行为准 |
| `minecraft-1.21.4-fabric-iris-bsl-in-world` | DirectGLES | 关 | 6.615 | 7.185 | **+8.6%** | 2491.9 | 2268.9 | 编译主导 | |
| `minecraft-1.21.4-fabric-iris-bsl-in-world` | DirectGLES | 开 | 6.543 | 7.065 | **+8.0%** | 2170.5 | 3387.9 | 编译主导 | |
| `minecraft-1.21.4-fabric-iris-bsl-in-world` | DirectVulkan | 关 | 4.087 | 4.452 | **+8.9%** | 1264.6 | 1295.4 | 编译主导 | |
| `minecraft-1.21.4-fabric-iris-bsl-in-world` | DirectVulkan | 开 | 4.058 | 4.464 | **+10.0%** | 1271.9 | 1295.3 | 编译主导 | |
| `minecraft-1.21.4-startup` | DirectGLES | 关 | 1.471 | 1.740 | +18.3% | 966.9 | 956.3 | 加载主导 | |
| `minecraft-1.21.4-startup` | DirectGLES | 开 | 1.473 | 1.641 | +11.4% | 959.0 | 946.2 | 加载主导 | |
| `minecraft-1.21.4-startup` | DirectVulkan | 关 | 0.416 | 0.460 | +10.6% | 1129.1 | 1124.7 | 加载主导 | |
| `minecraft-1.21.4-startup` | DirectVulkan | 开 | 0.419 | 0.471 | +12.4% | 1131.2 | 1124.0 | 加载主导 | |

两机读法：Mali 上 Espryt 的相对代价比 Adreno 高一档（in-world +16–18% 对 +11–13%；26.3 +8–9% 对 +14%），Magma 两机一致（+10–11%）；p99 在 26.3 上同向放大（Mali Magma +22%），这是 P2 tracker 在 draw 最密的用例上的尾部代价，随 P3a–P4a 的句柄化收缩，按口径记录。计数器（`acc/draw`、六个 memo 门、`resid=`、`csom`/`csob`）两机逐字相同——它们数的是代码路径不是硬件（§3 的结论再次成立）。

**harness 误杀（记录，dev 侧跟进）**：Oppo `improved-transparency-minecraft-26.3` DirectGLES push/finish 的第 1 次重复在第 960 帧被 `android-plugin/trace-replay-ci.sh` 强停（events 日志 `am_kill … due to from pid <adb shell>`，oom adj 0，`logcat -b crash` 无崩溃）：那个脚本的等待循环对 `pidof` 单次采样失败即判定"应用已退出"并 force-stop，一次 adb 抖动就丢一次重复。数字用剩下两次重复的 best-of；修法是连续 N 次采样失败才判退出。

## 11. tracker 的绝对 ns：上限与 T1/T2（D.4.3）

**上限在跑之前钉死**（`ARCHITECTURE.md` §13.2 第 4 条要绝对阈值，因为相对噪声阈值会平凡通过）。推导：拉取基线是每 draw **6.5–9.3 次 accessor 调用加 memo 探测**（§3 的表：MC 1.21.4 in-world Espryt 9.28 / Magma 8.56 @ 91.6 draws/帧；improved-transparency 26.3 Espryt 8.44 / Magma 6.53 @ 1320 draws/帧）。按"一次未内联的 accessor 调用加一次 load"计价——6–10 周期，1.96 GHz 上约 4 ns——再加六次 memo 探测各约 2 ns，得到推送必须不差于的 **约 44 ns/draw**。因此：

> **T1 ≤ 45 ns/draw（Adreno 830 `35d0befa`）、≤ 60 ns/draw（Mali `3B159D009VZ00000`）**。若开跑前的校准（pull 库的一次 DriverBench，`mc_vanilla_draw` 的 `ns_per_op` 减去 `native` 对照）显示这两台设备上的单次 accessor 价格与估计不同，则用**实测**价格按同一算术重推上限——但仍在测 push 臂之前钉死，不在之后。

两个差值都要公布，分母是 `mc_vanilla_draw` 的 5495 draws/帧：**T1** = `ns_per_op(push, 默认位图) − ns_per_op(pull)`，即整个边界的每 draw 代价；**T2** = `ns_per_op(push, MOBILEGL_PIPE_PUSH=0) − ns_per_op(pull)`，即 P1 的残余填充本身，于是 **T1 − T2** 恰好隔离出 P2 加了什么、删了什么。`DriverBench` 只在 Linux 桌面构建，且它不链接 MobileGL——`dlopen` 一个 provider，所以同一个二进制同时量原生驱动与两个后端；这对 P2 够用，因为绝对 ns 是客户端 CPU 问题，而"两台设备"的要求落在 §10 的逐线程 CPU 上。

## 12. Blaze3D blend-toggle 与 CSO 内容寻址负面对照（D.4.4 / D.4.5）

**微基准用例已在树里并有存活门**：`mc_state_toggle` 就是 `glEnable(GL_BLEND); glBlendFuncSeparate; glDrawElements; glDisable(GL_BLEND); glDrawElements` × 46，按 vanilla 帧的真实速率（每帧 46 对开关、28 次 `glBlendFuncSeparate`）。`DriverBenchStateToggle` 这条 ctest 用 `PASS_REGULAR_EXPRESSION` 钉住该用例自己的 CSV 行、并把每帧 ops 列钉在 **46**，所以用例被改名、被删、改了每帧 ops 或干脆没打印，它都会红；`DriverBench` 遇到不认识的用例名现在返回 rc 2 并列出现有用例，而不是打个表头就 rc 0。

**负面对照的开关是行为位 63**（`kMGPipeBehaviourNoCsoContentAddressing`，`MobileGL/MG_Pipe/MGPipe.h:83`）：它关掉 map 探测与句柄复用，**不关 CSO 记录**——否则量的是另一回事。开关不会烂掉，因为 `CsoContentAddressingScenario` 是常开的 ctest，钉住的数值契约是：一帧 8 对开关 = 16 draw；内容寻址臂 `csom ≤ 4` **且** `csom < csob`；位 63 臂 `csom == csob`；两臂都 `csob ≥ 16`；两帧连续开关的回读全绿且逐字节相同（要 `MOBILEGL_PIPE_STATS_PERIOD=1` 才读得到）。

对照的意义是把"**推送更慢**"与"**CSO 设计更慢**"分开：若 T1 越界而对照不越界，代价在 tracker；两个都越界，代价在线上形状。

**实测（桌面 llvmpipe / lavapipe，`~/w7/notes/tools/wsl_p2_bench.sh`：`DriverBench` Release 构建，240 帧，每臂 5 次重复取中位数；pull 库 = `build-linux`，push 库 = `build-push` 默认位图 `0x7f`）**，`ns_per_op`：

| 臂 | `mc_vanilla_draw`（ns/draw） | `mc_state_toggle`（ns/开关对） | `mc_pass_switch`（ns/pass） |
|---|---|---|---|
| native（裸驱动对照） | 4771 | 22968 | 437759 |
| Espryt pull | 5121 | 23753 | 443300 |
| Espryt push | 5443 | 24869 | 446017 |
| Espryt push，`MOBILEGL_PIPE_PUSH=0` | 5671 | 24584 | 443807 |
| Espryt push，位 63（无 CSO 内容寻址） | 5374 | 24630 | 435454 |
| Magma pull | 16900 | 32705 | 438406 |
| Magma push | 17245 | 33857 | 446067 |
| Magma push，`MOBILEGL_PIPE_PUSH=0` | 17599 | 34082 | 445006 |
| Magma push，位 63 | 17444 | 34780 | 444758 |

分解（`mc_vanilla_draw`，ns/draw）：

| | Espryt | Magma |
|---|---|---|
| **T1** = push − pull（整个边界的每 draw 代价） | **+322**（pull 的 +6.3%） | **+345**（+2.0%） |
| **T2** = push(`PIPE_PUSH=0`) − pull（P1 的残余填充本身） | +550 | +699 |
| **T1 − T2**（P2 自己加的减的） | **−228** | **−354** |
| 位 63 对照 − push（CSO 内容寻址的净值） | −69（在 5 次重复的离散内，≈ 0） | +200（内容寻址每 draw 省 200） |
| blend-toggle（`mc_state_toggle`，ns/开关对） | +1116（+4.7%） | +1153（+3.5%） |
| pass switch（`mc_pass_switch`） | +2716（+0.6%） | +7661（+1.7%） |

读法：**P2 的净效果是负的**——tracker + CSO 比它替掉的 P1 残余填充便宜 228 / 354 ns/draw，两个后端一致；剩下的 T1（+322 / +345）是还没迁成句柄的那部分 `PipeInputs` 填充与 dirty 走查，随 P3a–P4a 逐子系统收缩。位 63 对照把"推送更慢"与"CSO 设计更慢"分开：Espryt 上 CSO 内容寻址不花钱也不省钱（`SyncRenderState` 本来就是 memo 化的），Magma 上省 200 ns/draw（pipeline 键从 CSO 句柄取，少一次哈希）。§11 钉的上限（T1 ≤ 45 ns/draw @ Adreno 830）是**设备**口径，`DriverBench` 只在桌面栈上跑（同一 draw 在 llvmpipe 上花 5.1 / 16.9 µs），两者不能直接比；设备上对应的读数是 §10 的 +8–14% p50——按 2026-09-08 的口径记录在案，不判门。

## 13. 计数器读数

**`resid=` 字节类（G10）已非零**，且棘轮已经压到底：`MGL_RESIDUAL_BLOCK_SIZE` 从 **1248 降到 8**（`MobileGL/MG_Pipe/MGPipeTypes.h:546`，只降不升是 `static_assert`），块里只剩 `Uint64 CapabilityBits`。桌面 push retrace，`minecraft-1.21.4-fabric-iris-bsl-in-world`，`MOBILEGL_PIPE_STATS_PERIOD=60`：

```
MGPipe stats: frames=120 window=60 draws=2293 draws/f=38.22 … resid=197.07 … cso[csom=8 csob=1415]
```

- **残余块发射率**：197.07 B/帧 ÷ 8 B × 60 帧 = **每 60 帧 1478 块，合每 draw 0.64 块**。（`CrossFrameBufferScenario` 那种"一个窗口 `resid=8.00`、其后全 `0.00`"是抑制器在起作用的最小形态，不是语料上的速率，别拿它当代表数。）
- **`csom` / `csob`（新的两个调用类）**：同一窗口 **8 次铸造对 1415 次绑定**——CSO 内容寻址在真实语料上的复用比，也是位 63 对照要打掉的那件事。

**六个 memo 门的 hit/miss、设备上的 `resid=` 与 `csom`/`csob`**：拉取侧的基线在 §3 的表里（`ers`/`etl`/`eub`/`mfp`/`mpm`/`mdt`）。推送臂的读数出自 §10 那批运行（`--env MOBILEGL_PIPE_STATS=1 --env MOBILEGL_PIPE_STATS_PERIOD=120`，取最后一个完整的 120 帧窗口；设备上只有 `mobilegl.log` 的周期行，`MOBILEGL_PIPE_STATS_FILE` 永远不会写，见 §5.1；finish 开/关两臂逐字相同，软件确定性），小米 `35d0befa`，push 臂：

| trace | Espryt `ers` / `etl` / `eub`（hit/miss） | Magma `mfp` / `mpm` / `mdt`（hit/miss） | `resid=` B/帧 | `csom` / `csob`（每 120 帧） |
|---|---|---|---|---|
| `minecraft-1.21.4-in-world` | 6020/1850 · 6884/986 · 6962/908 | 0/7278 · 6030/1248 · 5928/1350 | 178.31 | 2 / 1719 |
| `improved-transparency-minecraft-26.3` | 78960/1367 · 75378/4949 · 75199/5128 | 10740/68928 · 67660/1268 · 78800/868 | 185.36 | 0 / 1157 |
| `minecraft-1.21.4-fabric-iris-bsl-in-world` | 113/133 · 85/161 · 82/164 | 0/177 · 86/91 · 81/96 | 370.67 | 1 / 122 |
| `minecraft-1.21.4-startup` | 174/185 · 305/54 · 305/54 | 0/118 · 0/118 · 0/118 | 25.08 | 9 / 181 |

读法：pull 臂的 `resid=` 恒为 0.00（残余块只在 push 下发射），push 臂每帧 25–371 B，即 8 字节块每帧 3–46 次，与桌面的 0.64 块/draw 同量级；CSO 内容寻址在稳态窗口里几乎不再铸造（in-world 2 次对 1719 次绑定，26.3 零铸造），只有 startup 在建状态时铸 9 次；六个 memo 门的形状与 §3 的拉取基线一致（Espryt 三门以 hit 为主；Magma 的 `mfp`（`MagmaDrawFastPath`）在拉取基线上就是 miss 为主——in-world 0/10994、26.3 21360/137036——P2 没有碰这个门）。

**逐 dirty 位的触发率：未测。** 计数本身已实现（`MGPipeTracker` 的 `FireCount`/`WalkCount`，挂在 `PipeStats::Enabled()` 后面），但汇总行的格式里没有它们，所以没有任何东西把 18 个计数打出来。补法是给 `FormatWindowLine` 加一行，或从一个场景里经访问器读——两者都是 P3a 的顺带项。

**每 draw payload 直方图：未测。** 24 桶已实现，但只在 teardown 的 JSON 里输出（`MOBILEGL_PIPE_STATS_FILE`），而 trace app 从不到达那次 teardown（§5.1），所以设备上取不到；桌面也没有记录过一次。

## 14. 遗留判定

**8 条静态过近似的填充行：全部保留，逐组给了理由**，写在 `MobileGL/MG_Pipe/FillPoints.def` 的表头注释里（就是 §7 那 9 处缺填充行里静态过近似的那 8 行）。理由三组同一条：这些行不是猜的，每一条都点名一条具体的后端路径，而能退役它们的证据只能是**动态**的——语料没走到某条路径，什么也证明不了，据此删行等于把一条罕见路径变成出货构建里的 `Fatal{UnmigratedPipeInput}`。三组分别是：`kReadback` + `IsTransformFeedback{Active,Paused}`（深度/模板回读仿真自己会画一个 draw 并暂停在飞的捕获，只在仿真被驱动条件触发时才走到）；`kTextureOp` / `kDispatch` + `IsCapabilityEnabled`（Magma 的 `GenerateMipmap` 与 `PrepareStorageImageTextures` 都经 `VkClearManager` 读 `GL_FRAMEBUFFER_SRGB`——**P2 给这个 capability 补了真存储之后，这一行从读编译期常量变成了读真状态，比以前更承重**）；`kBlitOrCopy` / `kTextureOp` + 着色器 blit 的 viewport 与顶点/缓冲绑定（`TryBlitToDefaultFramebufferWithShader` 是后端自有 program 的真 draw，同样是驱动条件决定的）。真正能退役一行的是 `MOBILEGL_PIPE_POISON_OMIT` 跑遍两台设备上完整的 `gl44to46` caselist——记为 P3a 的活，不在桌面语料这种撑不住的证据上做。

**`integration` 的第二遍过滤（`MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1`，186 条）不进比对器**（`.github/workflows/test.yml:534`）。比对器的代价现在是已知的 5–10×，而这 186 条是 buffer/回读方向的过滤，P2 在那里什么也没改；**P3b 再复核**，这条决定记在这里而不是把 CI 里那句注释一直吊着。

**Track H 单位成本的日历口径：未记录。** 产出侧在案（`ARCHITECTURE.md` §9.5 那份 21 条身份 memo 普查里的 11 条直接删除与 2 条重键全部落地，两片 Track H 零回归，pre-handle 臂在 `MOBILEGL_PIPE_LEGACY_MEMOS` 下并存到 P13），但两个包各自的实际工作日没有记，所以"不超出估计的 50%"这条判据这轮是按产出而不是按日历结算的。

**句柄 ABA 对照的"重键前红"证据**（D.2）。修复前，`HandleRecycle` 的 28 条里有 1 条红——`AbaControl` 那条 VAO 用例看到的是替换对象的绿，而这个臂**期望**看到死对象的红：

```
$ ctest --test-dir build-push -R 'HandleRecycle' --no-tests=error -j 4 --output-on-failure
96% tests passed, 1 tests failed out of 28
  DirectVulkan.HandleRecycle.AbaControl.…AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexInput (Failed)
  … [AbaControl expects the STALE object's pixels …]: 11625 of 11625 pixels (100%) are not red;
  first offender at (2,2) is green
```

修好之后 32/32（多出来的四条是新增的 `AbaControlHandles` 臂，让对照能够到 P2 出货的 `{slot, gen}` 臂而不只是 pre-handle 臂），并且逐臂把判决打出来：`arm=Handles expected=FRESH observed=FRESH`、`arm=AbaControl expected=STALE observed=STALE`，两个臂都如此。**对照确实承重**：把 `MOBILEGL_PIPE_HANDLE_ABA_CONTROL` 关掉，两个臂都变成 `observed=FRESH` 并**失败**——污染由被打掉的身份产生，别无他因，而退役的旧守卫与出货的 `{slot, gen}` 都能拦住它。

