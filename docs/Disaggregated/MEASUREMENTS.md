# 实测记录（P0、P1、P2、P3a）

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

> **勘误（2026-09-08，P3a 收尾时发现）**：本节两机 64 次运行所用的两臂 APK 是**未优化构建**——其 `libMobileGL.so` 43.2 MB、`.text` 21.8 MB，而同一 clang 18.0.4 的 Release 库是 15.5 MB / `.text` 9.4 MB（两者都无 `.debug_info`），同一 `improved-transparency-minecraft-26.3` 用例在 Oppo 上 Espryt pull 的 CPU p50 为 42.1 ms 对 Release 的 9.7 ms。因此本节的**绝对值与 +8–18% 的差值都是 -O0 读数**（tracker 这类内联密集的代码在 -O0 下付出最多），不能作为性能基准线。**基准线以 §20 的 P3a 两机 A/B（3e298c9a 的 Release APK）为准**：其 pull 臂与 P2 的 pull 路径符号一致（P3a 全程 G1 0/0/0/0，与 44c2b5cf 只差 d7655247 的 5 行），其 `MOBILEGL_PIPE_PUSH=0x7f` 臂就是 P2 边界在 Release 下的代价。本节其余内容（协议、计数器、harness 误杀）仍然有效。构建脚本 `~/w7/notes/tools/wsl_build_trace_apks.sh` 从此打印库尺寸（Release `.text` ≈ 9.4 MB）。

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

---

# P3a 实测（`feat/disaggregated@fde5fda3`）

> 基线：`5cb826b0`（P2 的 `44c2b5cf` 加上 `dev@9eae9858` 的合并）；两个版本号提交之后 G1 的 pull 基线在 `b9eaa474` 重取。四个包 = contract/wire、client、espryt、gates，集成顺序 contract → wire → client → espryt → gates。
>
> **口径不变**：性能对着 pull 臂**记录**、不作阻塞门；正确性门照旧是硬门。所以下面 §16 是门，§19–§21 是记录与遗留判定。

## 15. 用户的口径规则，逐字

2026-09-08 的用户规则，`docs/Disaggregated/` 里此前没有一处写下它，所以逐字抄在这里：

> (a) advance the roadmap as fast as possible without a large performance regression; performance is RECORDED against the pull baseline, never a blocking gate in P3a; correctness gates stay.

落到 P3a 的后果：五部分门的**第 4 部分**（`ARCHITECTURE.md:514` 的 monolith 性能）与 `ROADMAP.md:19` 里那句"MC 26.3 在 Adreno 上 p99 不变"都变成**测了就发布**的义务，不是通过/不通过的判据；第 1、2、3、5 部分仍是硬门。一次回归要写下数字与怀疑的成因并带进 P4a 的排期——**唯一不可谈判的是数字必须真的采到**：没测到的回归不算记录在案。

## 16. P3a 五部分门（本地 `~/w7/pipe`，HEAD `3e298c9a`，基线 `44c2b5cf`）

**第 1 部分 —— 接口纯度**

| 门 | 结果 |
|---|---|
| 三个构建（pull / push / verify） | rc 0 / 0 / 0。fail-fast 规则原样保留：任一非零就打印 `BUILD FAILED - gate aborted (no stale-binary verdicts)` 并退出，绝不拿陈旧二进制下判决 |
| A 门 include 闭包 | 4 个探针，0 skip，**0 problem** |
| C 门 `MG_Backend` 里的 `pGLContext` | 空 |
| G13 `PipeApply.h` 的 ops 块里的 `MG_State` | 空 |
| **G1** pull 构建符号（P3a 的认定 resize 集**为空**） | `.text` 10806323 → **10806323（+0，+0.000%）**；27811 → 27811 个已定义符号，**0 增 / 0 删 / 0 resize / 0 重命名** |
| **G5** 十个函数逐字节相同 | rc 0，对 `44c2b5cf` **与** `5cb826b0` 都成立（九个 pool / 延迟释放 / ring，加 ID-11 追加的 `FlushPendingRangesNow`） |
| `RenderStateImpl` 段 sha（P2 的不变量） | `d8fd1c48716056c53675`，逐字相同 |
| **G8** `HandleRecycle`（verify 构建） | **60/60**，含新增的 buffer 用例三臂 |

**第 2 部分 —— 语义影子比对（决定性的一条）**

| 门 | 结果 |
|---|---|
| `integration-verify` | **842/842**，`Fatal{` 行 **0** |
| 79 例 retrace，`MOBILEGL_PIPE_VERIFY=1` | **79/79 通过**，**79/79 全部 armed**（每份日志都带 `MGPipe verify:` 行），带 `Fatal{` 的日志 **0**——即零分歧、零未迁移读 |

**第 3 部分 —— 行为 A/B**

| 门 | 结果 |
|---|---|
| **G2** pull 与 push 的 ctest 名集合 | 差 **0** 行 |
| **G14** 测试名 | **0 删除 / +58** |
| 单元 | **1619** 全绿 × {pull, push, verify} |
| `integration-gpu` pull / push | **958/958** / **958/958** |
| `MOBILEGL_PIPE_PUSH=0`（全 pull 臂） | **958/958** |
| `MOBILEGL_PIPE_PUSH=0x7f`（P3a 两个子系统关闭，**G12**） | **958/958** |
| `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1`（具名的 kill-switch 臂，见 §21） | **958/958** |
| buffer/VAO 族（`LargeArenaAdoption`、`StorageBufferRegrow`、`VertexAttribBinding`、`MultiDraw`、`PrimitiveRestart`、`CrossFrameBuffer`、`ResidentIndex`、`BufferTexture`、`AtomicCounter`、`XfbCaptureBufferReuse`、`PackedWordReadback`、`DoublePrecision`、`VertexArrayEnableDisable`、`DrawParameters`） | **202/202** |
| P2 的 G7 setter 一致性阴性对照 | rc 0，按设计变红后树又恢复绿 |
| **G7** vertex-input 阴性对照 | rc 0 —— **变红并点名 `IsBgra`**，随后恢复、重建、再变绿 |
| `CsoContentAddressing` + `ResourceSubsystemControl`（**G12**：开关真的改变行为，不是死代码） | **10/10** |
| verify 构建的对照组（`PoisonOmitted`、`VerifyCorrupted`、`HandleRecycle`、`AbaControl*`、`ResourceSubsystem*`、`MapPersistentRoundtrips*`） | **64/64** |
| 79 例 retrace（push） | **79/79 通过**（`failed: []`），每条都在自己的 SSIM 阈值之上 |
| **G3b** 具名五条（`create-indirect`、`create-instancing`、`rd12-odinlite`、`improved-transparency-minecraft-26.3`、`fabric-sodium`） | **桌面侧全绿，但要看清是怎么绿的。** 五条**都在上面那次 79 例 push 扫描里通过**——它们全都在 `tools/trace_replay/trace_cases.json` 的语料内，而那次扫描是 79/79。**单独跑的那次具名扫描自己选中了 0 例**（`passed 0 / 0`）：`retrace_gate.py` 的 `--only` 收的是**正则**，门脚本传的却是逗号分隔的清单，于是过滤器一条都没匹配上。这是门脚本的缺陷、不是本阶段的失败，重跑要写成 `--only 'create-indirect\|create-instancing\|rd12-odinlite\|improved-transparency-minecraft-26\.3\|fabric-sodium'`。**设备侧**按 `ROADMAP.md` 开放问题 17 把 `create-indirect` 排除在 A/B 之外，所以 **G3b 整体记为"桌面全绿、设备侧部分（受阻于 `dev` 的开放问题 17）"**，不当作完整通过 |

**第 4 部分 —— 性能：记录，不设门**（规则 (a)）。见 §20。

**第 5 部分 —— 覆盖、poison、句柄纪律**

| 门 | 结果 |
|---|---|
| `gen_pipe.py --check` / `--self-test` | 生成物是最新的；self-test **7 个阴性对照全部触发**，正对照 OK |
| `gen_pipe_dirty_surface.py --check`（**G9**，扫描根已扩到 `MG_State/GLState`） | **75 个 mutator 全映射、无陈旧行**；45 条 render-state 答案由 `RenderState.cpp` 推导并吻合，另 10 条 (mutator, bit) 答案由各自在 `Tracker.h` 里的快门推导，**0 COARSE / 0 UNDECIDED** |
| `gen_pipe_dirty_surface.py --self-test` | **21 个阴性对照全部触发**，正对照 OK |
| `RenderStateSpans` / `Residual` / **`VertexInputEmit`（G6）** / **`ResourceEmit`** | **48/48** |
| `p3a_untouched_regions.sh --self-test`（**G5** 自己的阴性对照） | rc 0 —— 3 个正对照 + **2 个阴性对照**（扰动 `ClearBufferPool` 与 `FlushPendingRangesNow` 各被点名） |

**G15（完整 `gl44to46` caselist，约 56,271 例，两台设备）**：P3a 是五个架构边界之一，按 `ROADMAP.md:34` 排在 `dev` 合并时、**关键路径之外**；`MOBILEGL_PIPE_POISON_OMIT` 的清扫（§14 那 8 条静态过近似填充行）搭同一次 caselist 运行。

## 17. D.2 的"重键前红"证据：buffer 类

`ROADMAP.md:7` 要求每个门必须能因它存在的理由变红。P2 已经为 VAO 类留下了证据（§14 末尾）；P3a 为 **buffer 类**留下这一份，取自 `p3a/contract` 树上、package C 还没注册 `MGPipeResourceOps` 的时刻，`ctest --test-dir build-verify -R HandleRecycle`：

```
16/60 Test #2488: DirectGLES.HandleRecycle.Handles.HandleRecycleScenario
                  .ABufferAtARecycledAddressDoesNotInheritItsPredecessorsContents ......***Skipped
22/60 Test #2494: DirectVulkan.HandleRecycle.Handles.HandleRecycleScenario
                  .ABufferAtARecycledAddressDoesNotInheritItsPredecessorsContents ......***Skipped
28/60 Test #2500: DirectGLES.HandleRecycle.Legacy.HandleRecycleScenario
                  .ABufferAtARecycledAddressDoesNotInheritItsPredecessorsContents ......  Passed
40/60 Test #2512: DirectVulkan.HandleRecycle.AbaControl.HandleRecycleScenario
                  .ABufferAtARecycledAddressDoesNotInheritItsPredecessorsContents ......  Passed
100% tests passed, 0 tests failed out of 60
```

读法与预期逐条吻合：**`.AbaControl` 通过**——那个臂**断言的就是被污染的内容**（把句柄身份打掉之后，复用地址上的新 buffer 读到前一个的字节），这正是"重键前红"的正面证据；**`.Legacy` 通过**（pre-handle 守卫拦得住）；**`.Handles` 可见地 skip** 并写明缺什么，而不是一条消失的测试。package C 注册 `MGPipeResourceOps` 之后这两条 skip 转为断言，最终树上 `HandleRecycle` 是 **60/60**（§16 第 1 部分）。完整日志见 `~/w7/notes/p3a/p3a-results/handlerecycle-before.log`（另有 `-verbose.log`，含 22 行逐臂判决）。

## 18. 验证轮在真实流量下找到的三个缝隙缺陷

句柄路径第一次对着**真的发射器与真的 applier 本体**跑起来，是 espryt 的验证轮。默认位图那条 integration 通道当时留下 13 条红，其中 11 条是真的，掩码二分把它们干净地劈开（`0x7f`：0 条；`0xff`（只开位 7）：5 条；`0x1ff`：11 条）。三个缺陷都在本包的文件清单内，都有实测的前后对比。**这一轮是承重的**——三个都是别的门看不见的形状：

| # | 缺陷 | 位置 | 表现 |
|---|---|---|---|
| **F1** | vertex buffer 条目按属性的 **GL 绑定点**解析，而不是按**属性下标** | `MobileGL/MG_Backend/DirectGLES/Managers.cpp` 的查找助手（`VertexBufferForBindingIndex` → `VertexBufferForAttributeIndex`）与它的调用点 | `MGPVertexBuffer::BindingIndex` 与 `MGPVertexAttribWire::BindingIndex` 是两个不同的数：Espryt 消费的是**已解析**的属性，所以 client 把 `set_vertex_buffers` 发成"逐属性槽一条"，`BindingIndex == 属性下标`；另一个是 `glVertexAttribBinding` 绑上去的 GL 绑定点，而那个视图这条臂从不读。两者在所有 `glVertexAttribPointer` 配出来的属性上恰好相等——这就是别的场景全绿的原因；它们只在 `KHR-GL43.vertex_attrib_binding` 的题材上分叉，于是 `VertexAttribBindingScenario` 的 6 条读到禁用/零默认值。单这一个修完 6 → 3 |
| **F2** | ensure 路径去问一个**没有任何内容调用会刷新**的描述符，来判断影子里有没有字节 | `Managers.cpp` 的 `EnsureBufferResourceForHandle` | `MGPResourceDesc::HasDefinedContent` 陈述的是**上一次 `resource_respecify` 当时**的事实，此后 `NotifySubData` / `NotifyFlushMappedRange` / `NotifyContentWrite` / `MarkGpuWritten` / `LandBytesIntoResidentStore` 都只改前端的 `m_hasDefinedContent`、不重发描述符（逐 `glBufferSubData` 重发是新线上流量，设计上禁止）。于是在语料里最常见的惯用法 `glBufferData(size, NULL)` + `glBufferSubData(data)` 之后，句柄臂用**应用的字节**配上**applier 的描述符**，重定义出一个"已声明为当前"的空 store，应用的字节被丢掉且没有任何诊断——**静默的错像素**，不是崩溃。改为在持有前端对象时读 `HasDefinedContent()`（无对象的纯句柄排水仍读描述符）。关掉了全部 4 条 `XfbCaptureBufferReuse` 与 `LargeArenaAdoption.RespecifiedIndexArenaKeepsVaoBinding`，即所有 `0xff` 红 |
| **F3** | 惰性生成的 twin 从不发布影子基址，于是 fp64 收窄拒绝每一个 draw | `Managers.cpp` 的 `EnsureBufferResourceForHandle` | `GLESBufferResource::hostBytes` 是那些**不持有**前端对象的读者读的（回读队列排空、flush-range 的 kill-switch map 臂、`SyncFloat64AttributeAsFloat32ByHandle`）。`ResourceCreate` 是 no-op，twin 因此是**惰性**建的，第一次 `resource_respecify` 找不到 twin，把基址丢在地上；对"`glGenBuffers` + `glBufferData(size, data)` 之后再无内容调用"的普通静态顶点数组，`hostBytes` 就此终身为 null。整份重传没事（它走另一个取基址的路径），fp64 收窄却因 `sourceBase == nullptr` 直接失败 → Adreno workaround 禁用该属性 → 着色器读到 `(0, 0, 0, 1)`。修法是在已经算出基址、且每个用到该 store 的 draw 之前都会跑到的那一处补发布，且**只对非采纳资源**（采纳臂更早返回，所以 adopted store 的 `hostBytes` 仍为 null、字节仍走 `persistentPtr`）。关掉最后 3 条 `VertexAttribBinding` |

三个修完之后那条通道在 espryt 自己的树上是 920/920，最终集成树上是 §16 的 958/958。诊断用的五行临时 `MGLOG_E("TRACE …")` 在任何提交之前就已删除。

同一轮里的两条方法学记录。**包的 worktree 里 `tools/trace_replay/fixtures` 是 LFS 指针**（131/133 字节的 stub 对 `~/w7/pipe` 的 803 MB），对着指针文件跑 retrace 会报 `passed 2 / 79`、每条 Minecraft 用例 `ssim=None`，读起来跟"整体回归"一模一样——那是**假红**，建树脚本不物化 fixture。**base-instance 对照在 llvmpipe 上要先关掉原生 base-instance 才可证伪**：llvmpipe 暴露 `GL_EXT_base_instance`，原生门因此为真、`fetchBaseInstance` 被钳成 0，而属性偏移仿真才是 `MGPipeApplierState::VertexFetchBaseInstance` 在整个后端里**唯一**的消费者。把两个原生门都强制为假之后，对照两个方向都成立：三个调用点在时 10/10 绿，抠掉调用点时 8/10。**这条门是 `VertexAttribBindingScenario` 的 `BaseInstanceMovesTheInstancedArraysStartElement` 与 `BaseInstanceLeavesPerVertexArraysWhereTheyWere` 两条，不是 `DrawParametersScenario` 的八条**——后者观察的是 `gl_BaseInstance` / `gl_DrawID`，来自 `SetCurrentBaseInstance`，与取数偏移正交，两个方向都绿是对的。

## 19. Track H 单位成本普查（`ROADMAP.md:49` 的判据）

**日历口径：P3a 全部四个包 = 1 天**（2026-09-08）。contract/wire、client、espryt、gates 四个包各一轮实现加一轮对抗性评审加至多两轮返工，连同集成与本地五部分门，全部落在同一个日历日内。对着 `ROADMAP.md:66` 的 **27 天绊线**是 **1 / 27**，估时是 18–23 天，**没有超出估计的 50%**，不触发重定基线，也不需要 `ROADMAP.md:70` 的 `inproc` 证伪数字。§14 那条"Track H 单位成本的日历口径：未记录"到此补上一半——P3a 自己的口径有了，P2 两个包的仍然没有。

**diff 规模**（`git diff --stat 5cb826b0 3e298c9a`，区间内 44 个提交）：

```
37 files changed, 9273 insertions(+), 230 deletions(-)
```

**memo 账**（`ARCHITECTURE.md:363` 那份 21 条普查；P2 付了 11 条直接删除里的全部 11 条与 7 条重键里的 2 条）：

| 类别 | P3a 付了什么 |
|---|---|
| 直接删除（普查 11 条里的最后一条） | `ConvertedVertexStreamKey` 的 `sourcePin`（`ConvertedFloat64Stream::sourceLifetimeId`） |
| 句柄臂上另外退役的 twin 成员 | `m_hasSyncedConfigVersion`、`m_syncedConfigVersion`、`m_syncedAttributeVersions`（`Array<VertexAttributeVersion, 32>`，本阶段最大的一处）、`m_syncedIndexBufferVersion`、`m_syncedIndexBufferObject`（裸前端指针，堵回绕洞的那个身份补丁） |
| 重键 | VAO twin 的索引槽 memo（回绕 `Uint16` + 裸 `BufferObject*` → 一个 server `Serial`）；`ResolvedDrawBuffers`（`configVersion` → `{elementsHandle, elementsSerial, buffersSerial}`，`Entry` 与 `iboFrontend` 各加一个 `MGPipeHandle`）；twin 的同步门（config version + 32 组逐属性版本 → `{elementsHandle, elementsSerial}` + `VertexBuffersSerial`）；`ConvertedFloat64Stream`（前端 lifetime id + change serial → buffer `{slot, gen}` + applier `Serial`）；`GLESBufferResource::syncedChangeSerial` 从镜像 `BufferObject::GetChangeSerial()` 改为镜像 applier 的 `Serial` |
| **保留**（`MOBILEGL_PIPE_LEGACY_MEMOS`） | 上面每一条"退役"都只是**句柄臂上**的：成员仍在 `MOBILEGL_PIPE_LEGACY_MEMOS` 下编译（`MobileGL/MG_Backend/DirectGLES/Managers.h:1109-1130`、`:1067-1075`），pull 构建强制该开关 ON，所以 `sizeof` 一处不动——**G1 的 0/0/0/0 就是这条的度量** |
| **保留，且计划里的"grep 为空"不可达** | `g_pendingFetchBaseInstance` / `SetPendingFetchBaseInstance` / `GetPendingFetchBaseInstance` / `ScopedFetchBaseInstance` 与它的三个 scope（`MobileGL/MG_Backend/DirectGLES/Managers.h:1189-1200`、`MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:5293`）。句柄臂改由 `MGPipeApplierState::VertexFetchBaseInstance` 供给，但**真删会从 pull 构建移走两个符号**，是直接的 G1 破坏；所以计划里那条"`grep` 结果为空"在 P3a **不可达**，每一处残留命中都在一个 `MOBILEGL_PIPE_LEGACY_MEMOS` 臂里。这是记录在案的偏差，随 pull 路径在 P13 退役 |
| **保留（D-K）** | `PipeResource::m_backend`、`SetBackendResource`、`ReleaseBackend`、`BackendBufferResource`：push 下不再被写，删除会移动 pull 构建里的 `sizeof(BufferObject)`，同样随 P13 退役（`ARCHITECTURE.md:286`） |

## 20. 设备配对 A/B、MC 26.3 的 p99、DriverBench（记录项）

协议与 §10 完全相同：reboot-clean、同热窗口、按 `tools/device_bench/pin_device.sh` 定频（大核 1.96 / 小核 1.55 GHz、GPU 拉满、40 °C 门，每次运行前后各 `check` 一次，DRIFT 的那次作废重跑）、两臂背靠背、一次一台设备、`benchmark.json` 的 `frameCpuTimesMs[]` 尾 200 帧在主机侧算 p50/p99（p99 用 nearest rank，p50 按设备自己的中位数规则）。臂 = {pull APK, push APK} × {`--benchmark-no-finish`（主臂，P3a 问的是 CPU）, finish（GPU 时间没动的 sanity）} × 4 用例 × 2 后端。用例：`improved-transparency-minecraft-26.3`、`minecraft-1.21.4-rd12-odinlite-in-world`、`minecraft-1.21.4-fabric-sodium-in-world`、`minecraft-1.21.1-neoforge-create-instancing-in-world`（一个 `coherent_as_flush` fixture）。**`minecraft-1.21.1-neoforge-create-indirect-in-world` 排除在外**，理由与 P2 相同：它在 `dev@81b17c0b` 的基线 APK 上就在两台设备上失败（§5.5、`ROADMAP.md` 开放问题 17），不是本分支的回归；它留在桌面 SSIM 语料里。同一批运行里读出六个 memo 门的 hit/miss、`stage-*` 字节类与 **`mpr`**。

**读数纪律，必须写在数字上面而不是下面**：`CallClass::AccessorCalls` 是约 10 个热入口上的**静态计数**（`MobileGL/MG_Util/Metrics/PipeStats.cpp:16-100`，那份站点清单本身就是契约），所以一个只把读**挪了位置**的改动会拿到更低的 `acc/draw` 而并没有减少工作量。带头看的应当是**六个 memo 门的 hit/miss** 与 **CPU 时间序列**；引用 `acc/draw` 必须同时复核那些 tally 常量。

**两机配对 A/B（D.4.2），三臂**：APK 都是 `3e298c9a` 的 Release trace 构建（`wsl_build_trace_apks.sh`，`.text` 9.4 MB，debug 签名）；pull 臂 = pull 库；`P2` 臂 = 同一个 push APK 跑 `--env MOBILEGL_PIPE_PUSH=0x7f`（只开 P2 的七个子系统，P3a 关）；`P3a` 臂 = push APK 默认掩码 `0x1ff`。协议同 P2（reboot-clean、`pin_device.sh` 三次校验、每次运行前后 check、`--benchmark-repeats 3 --benchmark-tail-frames 200`、runner 的 best-of-3、`MOBILEGL_PIPE_STATS_PERIOD=120`）；表取 `--benchmark-no-finish` 臂，单位 ms/帧，p50 括号内为相对 pull 的增量。

| 设备 | trace | 后端 | pull p50 | P2 臂 p50 | P3a 臂 p50 | pull p99 | P3a p99 | 备注 |
|---|---|---|---|---|---|---|---|---|
| 小米 Adreno 830 | `improved-transparency-minecraft-26.3` | Espryt | 10.810 | 11.485（+6.2%） | 11.697（**+8.2%**） | 25.457 | 26.322 | finish 开：10.789 → 11.705（+8.5%） |
| 小米 | 同上 | Magma | 10.675 | 11.473（+7.5%） | 11.459（**+7.3%**） | 25.014 | 26.035 | finish 开：10.710 → 11.486 |
| 小米 | `minecraft-1.21.4-rd12-odinlite-in-world` | Espryt | 8.220 | 9.117（+10.9%） | 10.650（**+29.6%**） | 21.895 | 24.487 | finish 开：8.241 → 10.656 |
| 小米 | 同上 | Magma | — | — | — | — | — | 三臂都在启动数秒后 SIGABRT（scudo map error，见下），既有问题 |
| 小米 | `minecraft-1.21.4-fabric-sodium-in-world` | Espryt | 1.292 | 1.349（+4.4%） | 1.382（**+7.0%**） | 2.418 | 2.443 | |
| 小米 | 同上 | Magma | 0.485 | 0.504 | 1.010 | 1.469 | 2.187 | 亚毫秒帧，pull 自己两模式相差 2×（0.485 / 0.984），噪声，不读 |
| 小米 | `minecraft-1.21.1-neoforge-create-instancing-in-world` | Espryt | 944.4 | 945.2 | 949.9（+0.6%） | — | — | fixture 只有两帧（p50 = mean），是正确性 fixture 不是基准 |
| 小米 | 同上 | Magma | 1003.9 | 1019.5 | 1015.6（+1.2%） | — | — | 同上 |
| Oppo Mali | `improved-transparency-minecraft-26.3` | Espryt | 9.741 | 10.570（+8.5%） | 10.542（**+8.2%**） | 26.312 | 27.022 | finish 开：9.625 → 10.698（+11.1%） |
| Oppo | 同上 | Magma | 8.162 | 8.920（+9.3%） | 8.973（**+9.9%**） | 22.851 | 23.638 | finish 开：8.120 → 9.064 |
| Oppo | `minecraft-1.21.4-rd12-odinlite-in-world` | Espryt | 9.938 | 11.009（+10.8%） | 12.963（**+30.4%**） | 27.137 | 29.765 | finish 开：9.944 → 12.852 |
| Oppo | 同上 | Magma | 8.001 | 8.933（+11.6%） | 10.161（**+27.0%**） | 24.723 | 27.355 | finish 开：7.718 → 10.226 |
| Oppo | `minecraft-1.21.4-fabric-sodium-in-world` | Espryt | 1.896 | 1.770 | 1.841（−2.9%） | 2.993 | 3.079 | 亚 2 ms 帧，噪声内 |
| Oppo | 同上 | Magma | 0.399 | 0.431 | 0.437（+9.5%） | 1.206 | 1.409 | 亚毫秒帧 |
| Oppo | `minecraft-1.21.1-neoforge-create-instancing-in-world` | Espryt | 1075.1 | 1045.1 | 1060.1（−1.4%） | — | — | 两帧 fixture |
| Oppo | 同上 | Magma | 758.3 | 731.0 | 760.0（+0.2%） | — | — | 两帧 fixture |

所有入表运行前后 `pin_device.sh check` 都是 PINNED（小米 rd12/Magma 崩溃后 GPU pwrlevel 被重置，其后的 sodium/create-instancing 行两臂同状态）。

**读法。** (1) **P2 的边界在 Release 下的真实代价是 +6–12%**（`0x7f` 臂），两机两后端一致，比 -O0 表的 +8–18% 小但同量级；(2) **P3a 在 26.3 与 sodium 上几乎不再加价**（P3a 臂与 P2 臂在 26.3 上相差 −0.1 ～ +2 个百分点），**但在 rd12 上把差距从 +11% 推到 +27–30%**——rd12（Odin Lite 世界）每帧的 VAO/buffer 绑定切换远多于 26.3（26.3 的 1350 draw/帧大多复用同一 VAO），每次切换都走一遍 `set_vertex_buffers` 构造 + `ContentHash` + applier 记录 + Espryt 侧逐属性走查；Magma 上没有句柄消费者也多 15 个百分点，说明 client 侧发射本身就是大头；(3) **MC 26.3 在 Adreno 上的 p99**（`ROADMAP.md:19` 点名的那个数）：pull 25.46 ms → P3a 26.32 ms（+3.4%），finish 开 25.48 → 26.29；对着 `MEASUREMENTS.md:87` 的采纳基线（p99 163 → 21 ms）仍在 21–26 ms 档，没有回到采纳前的形态——按口径记录，不判门；(4) `mpr`（map-persistent-roundtrips，按窗口累加）：26.3 两机都是 8（首窗 4，之后两次 2——都是 ≥16 MiB store 定义时的采纳），sodium 1，rd12 与 create-instancing 0；P2 臂上恒 0（子系统关）——G10 在设备上成立；(5) `CreateVertexElements` 每帧字节数：统计行原本没有这一类（`vtxc` 是 client 数组），**P4a 已补上并测了**——汇总行的 `bytes/f[...]` 多了一个 **`csob-blob`**（`cso-blob-bytes`：所有 CSO create 调用的 blob 字节，含 vertex-elements、sampler、shader）。79 例 retrace、`MOBILEGL_PIPE_STATS=1 MOBILEGL_PIPE_STATS_PERIOD=60`、push 构建（`8c458cd5`）：**DirectGLES 27 个用例逐用例窗口均值的中位数 2928 B/帧、均值 42.8 KB/帧、无一为零**，最大 `rd12-odinlite` **1.06 MB/帧**（它每帧换 VAO 的次数远多于别人，正是 §20 读法 (2) 里 rd12 多花 17–19 个百分点的同一根因，这下有了字节口径）；其后依次 `minecraft-1.21.4-in-world` 11.4 KB、`common-mods-inventory` 8.3 KB、`common-mods-in-world` 7.8 KB、`rei-inventory` 7.2 KB。**DirectVulkan 侧恒 0**——Magma 没有注册 `MGPipeResourceOps`，P4a 的消费者门因此让四族一条不发（见 `ARCHITECTURE.md` 的 `MOBILEGL_PIPE_PUSH` 行），这也是这个计数器第一次把那条门量化出来；(6) 计数器（`acc/draw`、六个 memo 门、`resid=`、`csom/csob`）在 pull/P2/P3a 三臂间逐字相同——它们数的是代码路径，P3a 没有改它们的定义。

**小米 rd12 + Magma 的崩溃**：三臂（含 pull）都在启动后数秒 `SIGABRT`：`scudo::reportMapError` ← `remapImpl` ← `scudo_calloc` ← `libMobileGL.so`（0x818b14 / 0x7e2c44，已剥符号），当时 MemAvailable 6.1 GB——一次巨大或负尺寸的 calloc，在 Adreno 830 + Magma + 这条 fixture 上；pull 库与 P3a 前的 Magma 路径符号一致，所以是**既有 bug**，不入 P3a 账，已开独立任务（先符号化再修）。Oppo/Magma 与小米/Espryt 上同一 fixture 正常。

**harness 与设备陷阱（本轮新增）**：Oppo ColorOS 的安装确认页是 `topResumedActivity=…InstallGuideActivity`（`mCurrentFocus` 显示 systemui 窗口，按它 grep 永远匹配不到），首次安装不点会等到 harness 超时；在 streamed install 进行中点它会让设备从 adb 掉线几秒（那一轮的三次重复全失败，需要重跑）；小米 32 次运行后立即 reboot+pin 必因热钳失败（大核钳 1689600 或 GPU 读 1050 MHz），要等 cpuss-0-0 <42 °C；一次崩溃会把 GPU pwrlevel 范围重置成 2..5。

对照的既有基线是本文件 `:87`——`dev` 上 MC 26.3 在 Adreno 上把 ≥16 MiB 可变 store 采纳为 coherent persistent map 之后的 p99 163→21 ms、稳态 40→115 fps、省 ~400 MB。P3a 的 `AcquirePersistentMap` 一个语句都没动（`ARCHITECTURE.md:474` 的 D-B4），`FlushPendingRangesNow` 的三档排水在 G5 的逐字节集合里，所以这条 p99 是"没动过的东西是否真的没动"最直接的读数。

**DriverBench（桌面 llvmpipe / lavapipe，`~/w7/notes/tools/wsl_p3a_bench.sh`：Release，240 帧，每臂 5 次重复取中位数；pull 库 = `build-linux`，push 库 = `build-push`，都是 `c20e2f2b`）**，`ns_per_op`：

| 臂 | `mc_vanilla_draw`（ns/draw） | `mc_state_toggle`（ns/开关对） | `mc_pass_switch`（ns/pass） |
|---|---|---|---|
| native | 4759 | 22715 | 438175 |
| Espryt pull | 5115 | 23287 | 435815 |
| Espryt push，默认 `0x1ff` | 6162 | 24496 | 435975 |
| Espryt push，`MOBILEGL_PIPE_PUSH=0x7f`（只开 P2 子系统） | 5463 | 24576 | 436031 |
| Espryt push，`MOBILEGL_PIPE_PUSH=0` | 5693 | 24465 | 436598 |
| Espryt push，位 63（无 CSO 内容寻址） | 6156 | 24856 | 435276 |
| Magma pull | 17099 | 32612 | 436748 |
| Magma push，默认 `0x1ff` | 17709 | 33537 | 437314 |
| Magma push，`0x7f` | 17287 | 33550 | 436458 |
| Magma push，`0` | 17739 | 33800 | 440446 |
| Magma push，位 63 | 17729 | 34222 | 436465 |

分解（`mc_vanilla_draw`，ns/draw；T2 在本阶段定义为 `0x7f` 臂 = P2 的边界）：

| | Espryt | Magma |
|---|---|---|
| **T1** = push(`0x1ff`) − pull（整个边界） | **+1048**（pull 的 +20.5%） | **+611**（+3.6%） |
| **T2** = push(`0x7f`) − pull（P2 的边界） | +349 | +189 |
| **T1 − T2 = P3a 自己加的** | **+699** | **+422** |
| push(`0`) − pull（P1 残余填充，全部拉取） | +578 | +640 |
| 位 63 − push（CSO 内容寻址净值） | −6（≈ 0） | +20（≈ 0） |
| blend-toggle（ns/开关对） | +1209（+5.2%） | +925（+2.8%） |
| pass switch | +160（≈ 0） | +566（≈ 0） |

读法（记录项，不设门）：P3a 的 buffer/VAO 句柄路径在**每个 draw** 上多花 Espryt 699 / Magma 422 ns——这是 P3a 到目前为止最大的一笔边界成本，来源是每 draw 的 vertex-input 发射（`set_vertex_buffers` 的 `MGPVertexBuffer[]` 构造与 `ContentHash`、`bind_vertex_elements`）、applier 的记录写入，以及 Espryt 侧 `SyncToBackendFromApplier` 对记录的逐属性走查；Magma 没有句柄化的 VAO 消费者，它多出的 422 ns 全是 client 侧发射 + applier 记录（P7 之前的纯开销）。与 P2 相比，T2 本身从 P2 实测的 +322/+345 变为 +349/+189（同量级；Magma 的差别是 P2 数字来自同批次内的相对比较）。这个数字进优化阶段的清单：候选是 vertex-input 发射的 per-draw 抑制（同一 VAO/同一 buffer 集合的连续 draw 不重发；现在 `ContentHash` 命中时仍走一遍构造）与 applier 记录的就地更新。

`DriverBench` 的 T1/T2 口径与 §11–§12 相同，只是分档换了：**T1** = `ns_per_op(push, 默认位图 0x1ff) − ns_per_op(pull)`（整个边界的每 draw 代价），**T2** = `ns_per_op(push, MOBILEGL_PIPE_PUSH=0x7f) − ns_per_op(pull)`（P2 的边界本身），于是 **T1 − T2 恰好隔离出 P3a 加了什么、删了什么**；`mc_state_toggle` 作为非 P3a 的对照并列发布（一次 blend 开关既不碰 buffer 也不碰 VAO，它不该动）。`DriverBench` 走独立的 `build-bench`（门的构建配了 `-DMOBILEGL_BUILD_BENCHMARK=OFF`），并且必须经 `DRIVERBENCH_EGL_LIB` 显式 `dlopen` 一个 provider、**不得靠 `LD_LIBRARY_PATH` 遮挡**——glvnd 系统上一个裸 `libEGL.so.1` 会解析到 Mesa/llvmpipe，即在基准底下把 GPU 悄悄换成软件光栅器。**不钉上限、也不强制上限**（规则 (a)）；数字发布出来，让 P4a 自己决定要不要钉。

## 21. 决定与遗留判定

**整体 diff 终审（`5cb826b0..3e298c9a`）与其返工（`3e298c9a..c20e2f2b`，12 个提交）**：终审在四个包各自过审之后又抓出两个跨包接缝——(1) client 为每个 VAO 铸的 `VertexElementsCso` 槽只有 Espryt 的死亡通知会释放，Magma 不装 `StateObjectDeathOps`，默认掩码下每个 VAO 泄漏一个槽和约 1.3 KB 的 applier 记录，过 65536 个槽后 `create_vertex_elements` 永久 `Fatal{ProtocolCorruption}`；修法是 `~VertexArrayObject` 走后端无关的死亡路径（`MGPipeEmitVertexElementsDestroyAndFree`：先删记录、再发通知、最后释放槽，Espryt 的通知退化为可重入的第二条路径），泄漏测试在 DirectVulkan 上修前 48 轮 live 2→50、修后不增长；(2) `FlushPendingRangesNow` 位于只在 pull 构建里编译的 `#else`，G5 的文本哈希覆盖不到 push 构建真正跑的 `FlushPendingRangesFrom`——裁定如下。

> **ID-15（取代 ID-13 里"定义在任何 `#if` 之外"那句）。** `Managers.cpp` 把三层 flush 排水梯各带一份、每个预处理臂一份，任一构建只编译其中之一：`#if MOBILEGL_PIPE_PUSH` 下的 `FlushPendingRangesFrom` 是 **push** 构建跑的（legacy 调用点与 `Ops_H_Readback` 都到它），`#else` 里与 `5cb826b0` 逐字节相同的 `FlushPendingRangesNow` 是 **pull** 构建跑的；push 构建根本不编译 `FlushPendingRangesNow`。转发函数不可行：G5 的提取器不认预处理，一个转发的 `FlushPendingRangesNow` 会让同名出现两个定义、门直接退出 2（门跑不了）而不是比对。因此**两个名字都是 G5 行**：十一个函数，pull 梯对着 P3a 基线比、push 梯对着钉在 `3e298c9a` 的 sha 比；两条梯都不许漂，也不许彼此漂而门不响。

> **M-3 裁定：fp64 顶点数组收窄在 handle 臂上少做 D-N 十一处 `SyncGpuWrites` 里的一处，这是 P3a 的裁定而非疏漏。** D-N 的措辞是"不把这些站点*搬*离前端"；这一臂不是搬走了它，而是**根本做不了**——`SyncFloat64AttributeAsFloat32ByHandle` 手里只有句柄，那次调用需要前端 `BufferObject`，而 server 没有回到前端对象的反向映射正是设计本身（`ARCHITECTURE.md` §4.2），不是设计的缺口。保住这个站点的两条路各会破坏 D-N 或 D-J 保护的东西：句柄→对象的映射正是拆分要去掉的东西，而 client 在每个 draw 上急切拉回字节是新行为和新成本。**影响面，写明以便日后核对**：默认掩码下，一个 64 位顶点数组、其*源* buffer 被 shader 写过且尚未回读，handle 臂收窄到的是旧字节、legacy 臂是新字节。就这一种：fp64 顶点数组、由 shader 写的 buffer 喂、中间没有显式回读。其它属性类型不走这条路，持久映射的源另行排除（memo 从不信任它）。**P8 把拉取搬到对象所在的 client 侧即关闭**；在那之前这是默认掩码下两臂唯一的行为差异。（同一段文字在 `MobileGL/MG_Backend/DirectGLES/Managers.cpp` 的站点上；带 adopted 源的 `DoublePrecisionScenario` 用例未加——它需要一条新工作负载而非既有用例的参数，留给 D 包。）

新的"看穿 GL API"接缝：`MG_IntegrationTest/Harness/PipeSlotPeek.{h,cpp}` 直接读 client 的槽分配器（泄漏测试的依据），与 `BackendCapsPeek` 并列。

**CI 独有的 teardown 崩溃（`DirectVulkan.Verify.CrossFrameBufferScenario.{Vertex,Index}CopyBufferSubData`，`double free or corruption`，本地 30 次不复现）**：ASan 定位为 `exit()` 时的静态析构顺序 UAF——命名空间作用域的 `gPipeInputs` 持有 `SharedPtr<VertexArrayObject>`，其析构链 `~VertexArrayObject → ~BufferObject → MGPipeEmitResourceDestroyAndFree → MGPipeSlotAllocator::FindByLifetimeId` 读的是已被 `~MGPipeSlotAllocator` 释放的哈希表（`MGPipeSlots()` 是首个 buffer 时才构造的 Meyers 单例，先于 `gPipeInputs` 析构）。全部 13 个用例 × 两个后端都走这条链，只有释放后的表恰好还能解析出句柄时 `Free()` 才写坏堆——CI 的分配器布局中招、本地没有；原生复现钥匙是 `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`（c20e2f2b 上恰好那两例失败，修后 0）。修法：`MGPipeSlots()`、`MGPipeResourceTrackerInstance()`、`g_applier` 改为永不析构的单例（堆上构造、退出时有意泄漏），一并覆盖 C-1 返工新增的第二条到达路径（`~VertexArrayObject` 的 `MGPipeEmitVertexElementsDestroyAndFree`）。 落地为三个提交：`d54ec57a`（三个单例永不析构）、`6515c8e6`（**从源头收口**：`gPipeInputs`、`g_snapshot`、`g_readScratch`、`probe` 这四个持有前端 `SharedPtr` 的静态 `PipeInputs` 改为退出时泄漏的存储——树里只有它们持有前端对象，于是没有任何前端析构函数会从 exit handler 里跑；这是 `Init.cpp`/`GlobalObjects.cpp` 已有的规则）、`fde5fda3`（其余四个 MGPipe 单例永不析构，作为纵深防御——C-1 返工把 `MGPipeVertexInputEmitterInstance()` 放上了 `~VertexArrayObject` 的死亡路径，ASan 在 6/6 代表用例上抓到 `VertexInputEmit.h` 的 heap-use-after-free）。证据：开着钥匙 `integration-verify` 844/844、push 臂 `integration-gpu` 966/966，ASan 10/10 干净；整条 verify 通道在 ASan 下只剩 `IntegerBorderColorScenario` 一个与本阶段无关的既有测试 bug（2×2 上传的 4 字节源配 `UNPACK_ALIGNMENT=4` 的栈越界读），另开任务。

**`MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1` 那 186 条过滤：P3a 跑，跑一次，在 push 下跑，不进比对器。** §14 里 P2 把这条推给 P3b，理由是"P2 在 buffer/回读方向什么也没改"；**P3a 改的恰恰是这个过滤的题材**——它是 `FlushPendingRangesNow` 第一档的 kill switch（`MobileGL/MG_Backend/DirectGLES/Managers.cpp:1071`、`:1331`），而 P3a 重写了那个函数的调用方。所以门里加了具名的一步 `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1 ctest --test-dir build-push -L integration-gpu`，结果 **958/958**（§16 第 3 部分）。**放进 5–10× 的比对器里跑仍然推迟到 P3b**；这次是把拆开的两半都写下来，而不是让 CI 里那句注释再吊一轮。

**`g_uploadRing` 不被重置的不对称：原样保留，记为 `dev` 侧跟进。** `OnBackendContextDestroyed`（`MobileGL/MG_Backend/DirectGLES/Managers.cpp:2481`）对 `g_uboRing` 与 `g_unpackRing` 调 `ResetRingForNewContext`（`:2492-2493`），**不对 `g_uploadRing` 调**；`RingAvailable`（`:3186`）在首次使用时按 `contextGeneration` 自愈，所以它是良性的。P3a **故意不在飞地顺手修**（`ROADMAP.md:98` 那条纪律：拆分不得借机修不相关的 `dev` 问题），把它作为 `dev` 侧跟进项留在这里。

**P4a 在它旁边加第二条同类项：`ScopedDefaultUnpackState::s_synced` 没有失效路径（D-O）。** `Managers.cpp` 的 `ScopedDefaultUnpackState` 用一个**进程级**影子记住"默认 unpack 状态已经同步过"，那个影子被写、被读，**却没有任何地方让它失效**——换上下文、别的代码路径自己调 `glPixelStorei`，它都不知道。P4a 既不修它也没让它更糟（句柄臂的 ring 路径一条 `glPixelStorei` 都不发，唯一的外部写点仍被 `!ringStaged` 挡着），按同一条纪律记为 `dev` 侧跟进。**两条并列的理由是同一个**：它们都是"缓存了一个事实、却没有让这个事实失效的路径"，而 P4a 自己在 `Tracker.h` 上被同一类问题咬了三次（`glBindSampler` 不动位 13 的快门、SSO 下 `GetCurrentProgram()` 恒 null、`glBindImageTexture` 只换 level 时三个计数器都不动）——所以下一阶段的 brief 必须带一张"记录字段 → 写它的 setter → emitter 读的快门"的完备表，而不是让每个包各自去发现。

**`FlushPendingRangesNow` 定义一次，句柄臂另有一条自己的档梯。** G5 的第十项与 G1 的空 resize 集之间有一处真冲突：就地重构那几个 helper 会 resize 五个 pull 符号（`FlushPendingRangesNow +14` 在内），G1 不允许。落地形状是：**`FlushPendingRangesNow` 只定义一次、对 `5cb826b0` 逐字节相同**（在 `#if MOBILEGL_PIPE_PUSH` 的 `#else` 臂里，`Managers.cpp:1316`，调用点 `:1723`、`:2906`），句柄臂另有一个 `FlushPendingRangesFrom(twin, hostBase, size)`（`:1051`，调用点 `:1721`、`:2044`、`:2766`、`:2904`）。**在 P3a 接受档梯在 push 构建里被复制一份**（与 respecify 核心已经用过的形状相同），代价是两条梯子会漂移；对冲是 `CrossFrameBufferScenario` 的十三条加 `StreamedArenaScenario` 的两条 recycle 用例，以及 §20 的 MC 26.3 p99。**它随 pull 臂在 P13 退役**（`ARCHITECTURE.md:367`）。共享模板加访问器接口的方案被否决：它同样 resize pull 符号（G1）。

**`MG_Test/Buffer/BufferTest.cpp` 的 fixture 只 scope 了一半的表。** push 构建下后端在 bring-up 同时装 `BufferBackendOps` 与 `MGPipeResourceOps`，而 `ScopedBackendOps` 只 scope 前者，于是 86 条 `BufferBackendOps` 分发用例里有 **26 条**被路由进了 pipe（症状是 `EnsureGpuResidentStorage()` 返回 `false`、mock 从没被调用过）。这是**合并缝**的典型形态——两个分支各自绿、合起来红：espryt 那边没有东西经 pipe 发射，client 那边没有东西注册表。集成者落了单 scope 的修法（fixture 现在像 `MG_Test/Pipe/ResourceEmitTest.cpp` 的 `ApplierGuard` scope applier 那样，保存 / 置空 / 恢复 pipe 表）：**修完 86/86，整套单元 1619/1619**。它不削弱任何东西——那 86 条是 `BufferBackendOps` 的分发测试，pipe 侧的分发有 `ResourceEmitTest` 自己的覆盖。**跟进（不属于本阶段）**：给这个 fixture 一个 pipe 形的 mock，让同样的 86 条断言在句柄路径上再跑一遍。

**逐 dirty 位的触发率与每 draw payload 直方图：仍然未测。** §13 记的那两条在 P3a 也没有补上——没有任何一份 P3a 的结果文件报过位 5 / 9 / 10 收窄前后的 `FireCount` / `WalkCount`，也没有报过 24 桶的直方图（它仍然只在 teardown 的 JSON 里输出，而 trace app 从不到达那次 teardown，§5.1）。计划把这两项列为"虽然没人要也要发布"的项，**P3a 没有做到，如实记在这里**；补法与 §13 写的一样（给 `FormatWindowLine` 加一行，或从一个场景里经访问器读）。

**峰值 RSS（push vs pull，79 例 retrace）：工具不报，所以没有数。** `~/w7/retrace_gate.py` 只有五个参数（`--tree --lib --out -j --only`），代码里没有任何 `rss` / `maxrss` / `getrusage` 引用。这个数原本是用来盯第七张 slot 表泄漏的——一个没人销毁的 buffer 会永远漏掉它的 twin，而这对每一个正确性门都不可见；**这一轮拿不到它**，要拿必须先给那个工具加测量。对冲仍在：`ResourceDestroy` 是从 `~BufferObject` **无条件**发射的、不是靠清扫，顺序（先发射、后 `MGPipeSlots().Free`）由 `HandleRecycleScenario` 的三个臂把关（§16、§17）。


---

## 22. P4a 五部分门（`6035c9d7` 全量 + `8c458cd5` 复跑，基线 `37da3c3a`）

P4a 的代码头是 **`8c458cd5`**；下面的"全量"一列跑在 `6035c9d7`（终审修复轮之前的那个头，`wsl_p4a_gate.sh` 完整五部分含三次 retrace），"复跑"一列是修复轮落地后在 `8c458cd5` 上重跑的同一组。两次之间只差终审那五个提交，门的口径没变。

| 门 | `6035c9d7`（全量） | `8c458cd5`（复跑） |
|---|---|---|
| **G1** pull 符号（认定 resize 集为空） | 0 增 / 0 删 / 0 重命名 / 0 resize，`.text` 字节不变 | 同上 |
| **G5** 字节一致区 | P3a 十一函数 rc 0；P4a 自己 **17 区 / 3 文件** rc 0，self-test **8 个阴性对照全部按名变红** | 同上 |
| **G2** pull vs push 测试名 | 差 0 | 差 0（**2902** 条） |
| **G14** 测试名增删 | 0 删除 / +275 | 0 删除 / **+314** |
| 单元 | 1772 × 3（linux / push / verify） | **1785 × 3** |
| `integration-gpu` | **1091/1091 × 七臂**（默认 `0x1fff`、`0x1ff`、`0`、`0x7f`、pull、`ESPRYT_DISABLE_INVALIDATE_FLUSH=1`、族正则 497/497） | **1117/1117 × 七臂**（多了 `0x9ff`、`0x5ff` 两个依赖拒绝臂；DirectVulkan **559/559**） |
| `integration-verify` | **896/896，零 `Fatal{`** | **920/920，零 `Fatal{`** |
| retrace（79 例） | verify 臂 **79/79 全 armed、零分歧、零 Fatal**；push 臂 **79/79**；G3b 具名 **12/12** | push 臂 **79/79** |
| 三个阴性对照脚本 | 全部 rc 0：`g7_negative_control.sh`、`p3a_vertex_input_negative_control.sh` 点名 `IsBgra`、**`p4a_descriptor_negative_control.sh` 点名 `Layered` 与 `borderColorForm`** | 同上 |
| 子系统对照套件 | `CsoContentAddressing` + `ResourceSubsystemControl` + `ObjectSubsystemControl` **24/24**，`0x9ff` 依赖拒绝臂 **14/14**，`HandleRecycle`（verify）**180/180** | 同上 + `184/184` controls |
| 八族拒绝普查 | — | **0**（16 条 needle × retrace 日志、13 行 × itest 日志） |

**两处口径，都是这一波踩出来的**：

1. **`ctest -V` 做拒绝普查是假零。** console sink 在发布配置里被编译掉，`ctest -V` 抓不到任何 `MGLOG_E`；必须逐用例单跑并读它自己的日志文件（工具 `~/w7/notes/tools/wsl_p4a_refusal_census.sh`）。一份"零拒绝"的普查如果是用 `-V` 取的，它证明的只是 sink 被关了。
2. **普查的短语表必须覆盖全部八族**，而且要小心跨字符串字面量换行的句子——最初那版漏了 sampler 与 renderbuffer 两族，正好是后来真出问题的那两族。

## 23. 这一波真正的产出：缝的分类

P4a 的契约改了七次（`c0b`…`c0g`），外加一轮缝类审计与一轮终审修复。把它们按**类**记下来，比按包记有用得多——每一类都在多个包里重复出现过，而下一阶段的 brief 应当在开工前就把这几张表写死：

| 类 | 这一波的实例 | 症状 | 预防 |
|---|---|---|---|
| **编码没定死** | `MGPSubData::Target`（低字节资源目标 + 高字节上传目标 vs 裸枚举）、`DepthStencilMode`、`MGPSurface::Kind`、缺 `TextureTarget` | 两侧各自发明一套；`TextureUploadTarget::Texture1D == 0` 与 `kMGPipeResourceTargetBuffer == 0` 撞上，applier 的"这是 buffer 吗"判定被静默污染 | **编码表**：每个字段一行，写清位布局与零值含义，放进契约而不是包头 |
| **身份 vs 内容** | 内置 sampler：client 按对象身份铸、cache 按内容铸；Espryt twin 按身份查那条内容寻址的记录 | 查找永远落空 → 整族拒绝（本波两次，其中一次让 17 条 Iris 光影 trace 全部用驱动默认采样器） | **每 kind 两侧 handle 规则表**：谁铸、按什么键、谁查、按什么键 |
| **记录键错了维度** | framebuffer 记录按"当前绑定"存，DSA 的 `BlitNamedFramebuffer` / `ClearNamedFramebuffer*` 按名字来 | 打进一个从没收到附件的 FBO；SSIM 看得见但没有任何拒绝 | 记录按**对象**存，绑定另存句柄；第四个 target 值 `Named` |
| **进程级单例 vs 每上下文命名** | `CompositeResolver` 按管线 GL 名记忆，GL 名是每上下文的 | 一次 make-current 就释放掉另一个上下文还活着的合成体 | 单例的键必须含上下文身份 |
| **破坏性客户端动作缺前置条件** | 按 acceptance 清 dirty，但 (a) Magma 根本没有消费者，(b) D-K2 依赖位只在服务端拒 | 上传丢失：66 条 DirectVulkan 用例、`0x7ff` 下 438/491 | **消费者门 + 依赖门都要在客户端侧**：没消费者/依赖不满足时**一条不发**，而不是发了再在服务端拒 |
| **快门看不见自己的主体** | `glBindSampler` 只动位 12 的世代；SSO 下 `GetCurrentProgram()` 恒 null；`glBindImageTexture` 只换 level 时三个计数器都不动 | 记录停在上一次的值，第一个真读该字段的消费者画错（`create-indirect` ssim 0.887） | **"记录字段 → setter → 快门"完备表**；新增计数器会撑大 pull 对象、G1 不允许，所以优先混入已有世代 |
| **清得太宽** | `resource_respecify` 清掉整张待上传表 | 已被接受、客户端标志已清的那一级永久丢失（读回全黑） | 作用域随调用走：一级 / 截断链 / 整资源 |
| **死亡没通知发射方** | 六个死亡 helper 只释放 slot | 已删但未复用的句柄仍解析到已释放的前端对象 → 下一个 validate 点对已释放内存调虚函数 | 死亡在 wire delete 与 free 之间转发给每个 emitter；查找按"活着"判定而不只按世代 |
| **门不能变红** | `G7` 脚本因 scoped enum 写 0 而永远编译不过、`HighWater(ShaderCso)` 取的是段顶、G9 的红前态公共 GL 不可见 | 绿得毫无意义 | 每个门都要有阴性对照并**真跑过一次红**；公共 GL 看不见的，改白盒断言（`PipeApplyPeek`） |

**一个方法论上的结论**：本波六个包的 v1 全部通过了自己的门，**六份对抗性复审全部判 REWORK**，而其中最贵的两个缺陷（丢上传、delete 后 UAF）是**整体 diff 终审**才抓到的——因为它们跨包：发射方、applier、twin 各自都自洽。所以"每包一审 + 集成后整体终审"这条流程里，**终审不是形式**，它是唯一能看见跨包契约的那一轮。

## 24. P4a 设备配对 A/B（三臂）、MC 26.3 的 p99、上传形状（记录项）

**先说口径，再看数（`MEASUREMENTS.md:440` 那条告诫在 P4a 上再次成立）**：`acc/draw` 数的是**约十个热入口上的静态计数点**，所以"把读点搬走"和"把工作去掉"在它上面长得一模一样。P4a 恰好是**搬**的一波：79 例语料上 DirectGLES 的 `acc/draw` 普遍下降（26.3 `10.49 → 8.34`、`rei-inventory` `13.33 → 11.25`、`rd12` `8.09 → 6.09`），而同一批运行的逐线程 CPU 是**上升**的。**这不是矛盾，是这个计数器的定义**：后端不再每 draw 去 `pGLContext` 上取，改成读被推送的记录，站点自然少计——工作搬到了客户端的发射侧。要判性能只看 CPU 时间序列与门的命中/未命中对，`acc/draw` 只能与站点常量表一起读。

**设备与协议（与 §20 的两台机不同，这里换了机器）**：红米 M332BF（`2f7cbe2e`，SM8750 / **Adreno 830v2**，与 §20 的小米同 SoC 同定频点，数值可比）。reboot-clean → 大核 `policy6` 钉 1958400、小核 `policy0` 钉 1555200、GPU `pwrlevel 0`；**这台的 GPU 有效定频是 1050 MHz 不是 1100**——厂商把 `kgsl-3d0/thermal_pwrlevel` 常驻 1，root 写 0 无效（33 °C + 风扇全速下验证），`pin_device.sh` 按 1050 判定。全程**主动风扇恒定 level 2（~14.5k rpm）**：它对两臂是同一个常量，作用是把每用例之间的降温从 20–30 分钟压到 1 分钟以内，**40 个样本 40 个 `pin check` 全是 PINNED**（§20 那轮有 22 个样本因热漂移作废重跑）。APK 是 `8c458cd5` 的 Release trace 双臂（pull 8504077 B / push 8626957 B）。

**三臂表**（`--benchmark-no-finish`，尾 200 帧、best-of-3、逐线程 CPU p50 ms；`0x1ff` = P2+P3a 边界，`0x1fff` = P4a 默认）：

| 用例 | 后端 | pull | `0x1ff` | `0x1fff` | Δ P2+P3a | Δ 合计 | **P4a 自己** |
|---|---|---|---|---|---|---|---|
| improved-transparency-26.3 | Espryt | 10.716 | 11.754 | 12.124 | +9.7% | +13.1% | **+3.4 pt / +0.37 ms** |
| improved-transparency-26.3 | Magma | 10.603 | 11.543 | 11.585 | +8.9% | +9.3% | +0.4 pt（噪声） |
| rd12-odinlite | Espryt | 8.210 | 10.798 | 11.182 | +31.5% | +36.2% | **+4.7 pt / +0.38 ms** |
| rd12-odinlite | Magma | — | — | — | — | — | 三臂全 `rc=1`，见下 |
| fabric-sodium | Espryt | 1.312 | 1.406 | 1.454 | +7.2% | +10.8% | +3.6 pt / +0.05 ms |
| fabric-sodium | Magma | 0.474 | 0.502 | 0.507 | +5.9% | +7.0% | +1.1 pt（噪声） |
| 1.21.4-in-world | Espryt | 2.369 | 2.732 | 2.867 | +15.3% | +21.0% | **+5.7 pt / +0.14 ms** |
| 1.21.4-in-world | Magma | 1.028 | 1.147 | 1.145 | +11.6% | +11.4% | −0.2 pt（噪声） |
| fabric-iris-bsl | Espryt | 1.727 | 1.740 | 1.811 | +0.8% | +4.9% | +4.1 pt / +0.08 ms |
| fabric-iris-bsl | Magma | 0.742 | 0.788 | 0.786 | +6.2% | +5.9% | −0.3 pt（噪声） |

**读法。** (1) **P4a 自己在 Espryt 上是 +3.4 ～ +5.7 个百分点**（绝对值 0.05–0.38 ms/帧），大头仍然是 P2+P3a 那条边界——rd12 上 36.2% 里有 31.5% 是它。(2) **Magma 的 `0x1ff` 与 `0x1fff` 两臂在四个用例上逐个落在噪声内**（+1.1 / +0.4 / −0.2 / −0.3 pt），这是 c0f 那道"没有后端注册 `MGPipeResourceOps` 就一条不发"的门在设备上的读数——Magma 仍然要付 P2+P3a 的客户端发射（它消费那些族），但 P4a 的四族对它完全免费。(3) **`vanilla`（1.21.4-in-world）是 Espryt 上 P4a 占比最高的用例**（+5.7 pt），它 draw 少、状态切换密，正是句柄化最不划算的形状；`sodium`/`iris-bsl` 这种把状态压平的语料几乎不受影响。

**头条一：MC 26.3 在 Adreno 上的 p99。** pull **25.297** → P4a **26.841 ms（+6.1%）**，`0x1ff` 臂 26.387；Magma 侧 24.979 → 26.021。对照 §20 的 P3a 读数（25.457 → 26.322，+3.4%）与 `MEASUREMENTS.md:87` 的采纳基线（p99 163 → 21 ms），**仍在 21–26 ms 档内、没有回到采纳前的形态**——按口径记录，不判门。

**头条二：GUI/atlas 的纹理上传形状，pull 与 push 逐项相同。** 79 例语料两臂各跑一遍带 `MOBILEGL_PIPE_STATS=1` 的 retrace（`8c458cd5`，客户端计数器 `tex[emit/box/rect/jobs]`）：

| | emit | box | rect | jobs |
|---|---|---|---|---|
| pull | 18451 | 16060 | 2391 | 39926 |
| push | 18453 | 16062 | 2391 | 39928 |
| 差 | **+2** | **+2** | **0** | **+2** |

**整份语料上唯一的形状差异是 2 次**，而且正是那 2 次 `trp`（纹理重铸拉取，见 `ROADMAP.md` 开放问题 2）带来的重放上传——即"盒 vs 矩形"的分解一格没动。这是 SSIM 看不见、Mali 那道 ~+6 ms/帧的悬崖就藏在里面的那个数（`ARCHITECTURE.md` §6），P4a 在这里是**中性**的。逐用例看也一致：`rei-inventory` Espryt 两臂都是 16/16/0/16，Magma 两臂都是 47/46/1/75。

**一条留给优化阶段的线索（不是缺陷，是读数）**：设备上 26.3 Espryt 的 `sve`（真正发出去的 sampler-view 集合数）**≈ draw 数**（9143 次 / 9138 draw，每窗口 120 帧），而桌面同一 fixture 只有 ~0.07/draw。`PipeStats.h` 给这四个集合计数器写的用途正是这个——"抑制器不再抑制时，它的计数会跟着 draw 数走而不是跟着状态变化走"。桌面与设备的差异说明这跟负载形状有关而不是无条件失效，但 **26.3 在设备上每 draw 重发一次 sampler-view 集合**是 Espryt 侧 P4a 那 +0.37 ms 最值得先查的去处，列进 P3b/P4b 的优化清单。

**`rd12` + Magma 在这台机上照样崩**：三臂（含 pull）全部 `rc=1`，与 §20 在小米上的记录一致（`scudo::reportMapError` ← `remapImpl` ← `scudo_calloc` ← `libMobileGL.so`）。**换了一台同 SoC 的机器仍然复现，进一步确认它是 `dev` 侧的问题而不是设备个例**；按 `ROADMAP.md:7` 的纪律不在本分支顺手修，处置沿用 §20：排除在 A/B 之外、留在桌面语料里（桌面两臂均通过）。

## 25. P4a DriverBench：T1 / T2 / T3（桌面，lavapipe + llvmpipe，记录项）

`wsl_p4a_bench.sh` 在 `8c458cd5` 上重跑（原始表 `~/w7/notes/p4a/bench/driverbench.{csv,md}`，repeats=5 / frames=240，空闲机）。臂：`pull`、`push`（`0x1fff`）、**`push7f` 一列在 P4a 里装的是 `0x1ff`**（P2+P3a 边界 = 设备侧那个 T2 的桌面对应物）、`push0`（`PIPE_PUSH=0`）、`nocso`（关 CSO 内容寻址的负面对照）。

| 臂 | `mc_vanilla_draw` | `mc_state_toggle` | `mc_pass_switch` |
|---|---|---|---|
| native | 4398.9 | 21387.3 | 412657.8 |
| espryt-pull | 4684.4 | 22010.4 | 410936.2 |
| espryt-push（`0x1fff`） | 5760.5 | 22974.9 | 419703.1 |
| espryt-`0x1ff` | 5749.1 | 23768.2 | 419288.0 |
| espryt-push0 | 5355.2 | 23618.8 | 420362.2 |
| espryt-nocso | 5929.0 | 23685.0 | 418677.0 |
| magma-pull | 15394.9 | 31678.5 | 420197.6 |
| magma-push（`0x1fff`） | 16023.9 | 32417.4 | 415641.9 |
| magma-`0x1ff` | 15847.5 | 32100.3 | 416670.4 |
| magma-push0 | 15946.6 | 31610.2 | 417410.5 |
| magma-nocso | 15846.2 | 32495.9 | 411306.4 |

`mc_vanilla_draw` 上：**espryt T1（`0x1fff` − pull）= +1076.1 ns/draw，T2（`0x1ff` − pull）= +1064.7，T1 − T2 = +11.4**；magma T1 = +629.0、T2 = +452.6、T1 − T2 = +176.4。blend toggle：espryt +964.5 / magma +738.9 ns per toggle pair；pass switch：espryt +8766.9、magma −4555.7（后者符号为负，属该项的噪声量级）。

**桌面这台机上，"P4a 自己"落在本 bench 的噪声底以下，所以不要单独引用它。** 同一份脚本在 `6035c9d7`（终审修复前）上跑出的是 espryt T1 +1269.7 / T2 +1135.8 / **T1 − T2 = +133.9**，本轮是 +1076.1 / +1064.7 / **+11.4**——**两臂的绝对值在两轮之间各自漂了 ~200 ns，而它们的差只有 10–130 ns**，也就是说这个 bench 分辨不出 P4a 这一档的增量。真正可引用的是：(1) **T1 ≈ +1.1 µs/draw 的总边界**（对 pull 基线，Espryt；这条在两轮之间是稳的）；(2) **设备侧的三臂表（§24）**——那里 P4a 自己是 +3.4 ～ +5.7 个百分点、0.05–0.38 ms/帧，样本全部在验证过的定频窗口里。**Magma 的 T1 − T2 = +176.4 ns 不是"Magma 在跑 P4a"**：c0f 的消费者门让它一条 P4a 记录都不发（设备侧 §24 的 Magma 两臂差也在噪声内），这 176 ns 是 tracker 多算的那几个快门加噪声。
