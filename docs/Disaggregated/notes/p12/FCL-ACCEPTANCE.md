# P12 出口门 (a)：FCL 里的 Minecraft 渲染到 render server 窗口，杀 server 干净 device-lost

> 2026-09-29，Redmi `2f7cbe2e`（Adreno 830）。server = trace APK `26.09.27cbc53-trace` 的 **Render Server 屏**（勾 On-screen window，
> endpoint `tcp://127.0.0.1:40613`，Extra env `MOBILEGL_BACKEND_TYPE=…;MOBILEGL_PIPE_STATS=1`，点 Start server），窗口在 `:mglwin` 进程里。
> client = FCL fordebug `com.tungsten.fcl.mgdebug.debug` 里的 Minecraft 26.3-rc-3，世界 `test`，`/sdcard/FCL/mg_transport.txt=spawn`，
> `mg_env.txt` 里 `MOBILEGL_IPC_CONTROL=tcp://127.0.0.1:40613`、`MOBILEGL_IPC_DATA=stream`、`MOBILEGL_IPC_SURFACE=server`、`MOBILEGL_BACKEND_TYPE`。
> 驱动 [`tools/device_bench/disagg/fcl_accept.sh`](../../../../tools/device_bench/disagg/fcl_accept.sh)，截图在 `.trace-work/perf-compare/fcl-accept/`。

## 结论

**门 (a) 通过，两个后端各一遍。** Minecraft 世界内的画面实时画在 render server 的窗口上；`kill -9` server 后 client 在毫秒级闩住 device-lost，FCL 进程不崩。

| 后端 | 入世界、画面上屏 | `kill -9` server | FCL 之后 |
|---|---|---|---|
| Espryt（DirectGLES） | ✅ 夜晚草原，天空 / 树 / 手 / 快捷栏都在；client `present` 计数持续增长 | `DEVICE LOST`（1 行），54 ms（修日志洪水前的构建，精确计时）；修后构建同样闩锁 1 次 | 进程还在（修前构建观察 100 s；修后构建 ≥ 20 s），后续 present 被 DECLINED 空转 |
| Magma（DirectVulkan） | ✅ 画面上屏、`present` 持续增长 | `DEVICE LOST`（1 行），33 ms（修前构建，精确计时）；修后构建同样闩锁 1 次 | 进程还在（修前 ≥ 30 s；修后 ≥ 10 s） |

## 为了跑通做的三件事

1. **FCL 一退后台游戏就暂停。** FCL 的界面离开前台时，SDL 被切到 PAUSED，GLFW 的 focused / visible 也被撤销，渲染线程停住；而 server 窗口要在屏幕上就必须把 FCL 切到后台。
   FCL fork 里新增版本设置 **「游戏退到后台时不暂停」**（`keepRunningInBackground`，默认关，在版本设置的渲染组里）：开启后强制用 TextureView 并让系统别销毁它的 SurfaceTexture
   （SurfaceView 在界面不可见时会被销毁，游戏拿着失效窗口会报 `Failed to create backend OpenGL/Vulkan`），界面离开前台时不通知 SDL 暂停、不撤销 GLFW 焦点。
   改动在 FCL 仓库（`JVMActivity`、`VersionSetting`、`VersionSettingAdapter`、`LauncherHelper`、三份 strings），**尚未提交**（父仓库处于合并冲突状态）。
2. **FCL 的默认 spawn（游戏进程派生 server 子进程）走不通，只能接外部 render server。** FCL 把自己的窗口指针发给 server，server 按 P12 设计具名拒绝
   （`Fatal{UnmigratedSurface, "AndroidNativeWindow@P12"}`），client 随即 device-lost。
3. **修了一个日志洪水。** device-lost 之后每个被 DECLINED 的 verb 都打一行 ERROR（`…woke on a dead doorbell; the server is gone`），FCL 里 48 秒 113,080 行、几千行/秒。
   `ClientSession.cpp` 两处改为闩锁已经说过之后不再重复；修后同样流程 `DEVICE LOST` 1 行、`dead doorbell` 0 行。

## 没验证 / 观察到的

- **Magma 在竖屏 server 窗口里画面位置不对**：游戏帧 2620×1280，窗口 1280×2620，画面只出现在窗口下部、被裁掉一部分；Espryt 那次是横屏窗口，没有这个现象。是几何 / 预旋转问题，还没查。
- 没测性能、没测输入（server 窗口不转发触摸，游戏是自己进世界的，没有人操作）、没测长时间稳定性。
- 时序有要求：FCL 的 JVMActivity 出现后要再等约 10 s 才把 server 窗口切到前台，否则游戏因 surface 未就绪卡住启动。
- 每个后端只跑了一遍。
