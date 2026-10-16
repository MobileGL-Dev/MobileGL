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
| Magma（DirectVulkan） | ✅ 画面上屏、`present` 持续增长；竖屏窗口里也是完整画面（几何修复后） | `DEVICE LOST`（1 行），33 ms（修前构建，精确计时）；修后构建同样闩锁 1 次 | 进程还在（修前 ≥ 30 s；修后 ≥ 10 s） |

## 为了跑通做的三件事

1. **FCL 一退后台游戏就暂停。** FCL 的界面离开前台时，SDL 被切到 PAUSED，GLFW 的 focused / visible 也被撤销，渲染线程停住；而 server 窗口要在屏幕上就必须把 FCL 切到后台。
   FCL fork 里新增版本设置 **「游戏退到后台时不暂停」**（`keepRunningInBackground`，默认关，在版本设置的渲染组里）：开启后强制用 TextureView 并让系统别销毁它的 SurfaceTexture
   （SurfaceView 在界面不可见时会被销毁，游戏拿着失效窗口会报 `Failed to create backend OpenGL/Vulkan`），界面离开前台时不通知 SDL 暂停、不撤销 GLFW 焦点。
   改动在 FCL 仓库（`JVMActivity`、`VersionSetting`、`VersionSettingAdapter`、`LauncherHelper`、三份 strings），已提交并推送（FCL 仓库 `merge-upstream-surfaceview`，`241471515`）。
2. **FCL 的默认 spawn（游戏进程派生 server 子进程）走不通，只能接外部 render server。** FCL 把自己的窗口指针发给 server，server 按 P12 设计具名拒绝
   （`Fatal{UnmigratedSurface, "AndroidNativeWindow@P12"}`），client 随即 device-lost。
3. **修了一个日志洪水。** device-lost 之后每个被 DECLINED 的 verb 都打一行 ERROR（`…woke on a dead doorbell; the server is gone`），FCL 里 48 秒 113,080 行、几千行/秒。
   `ClientSession.cpp` 两处改为闩锁已经说过之后不再重复；修后同样流程 `DEVICE LOST` 1 行、`dead doorbell` 0 行。

## 没验证 / 观察到的

- （已修）竖屏 server 窗口里画面被裁：见下一节。
- 没测性能、没测输入（server 窗口不转发触摸，游戏是自己进世界的，没有人操作）、没测长时间稳定性。
- 时序有要求：FCL 的 JVMActivity 出现后要再等约 10 s 才把 server 窗口切到前台，否则游戏因 surface 未就绪卡住启动。
- 每个后端只跑了一遍。

## 竖屏窗口里画面被裁（已修）

**现象：** server 窗口是竖屏（1280×2620）时，游戏帧 2620×1280 只画出左下角 1280×1280 的一块，热键栏被切掉一半。最初只在 Magma 上看到，**Espryt 在竖屏窗口里同样被裁，Magma 在横屏窗口里是对的**——所以不是后端问题，是窗口方向。
**根因：** SDL（Minecraft 的 Android 后端）建 EGL window surface 时不带 `EGL_WIDTH` / `EGL_HEIGHT`，client 于是向 server 请求 **0×0**（"用 server 窗口自己的大小"）；server 窗口取了竖屏尺寸，client 的默认帧缓冲就成了 1280×2620，
而游戏按它自己的窗口（FCL 的 2620×1280）渲染，1:1 画进去就被裁了。server 侧其实早就支持"client 指定尺寸 → `setFixedSize` + 视图等比留黑边"（`MobileGLDisplayActivity`），只是这一路没人传尺寸。
**修法：** `EGLImpl::CreateWindowSurface`（split 构建、Android、server-owned 窗口）在没有 `EGL_WIDTH/HEIGHT` 时用传进来的 native window 的 `ANativeWindow_getWidth/Height` 作请求尺寸。
修后日志 `display geometry 2620x1280 requested (SurfaceHolder.setFixedSize)`，竖屏窗口里显示完整的一帧、上下留黑边，两个后端都一样（截图 `geom-*-portrait-fixed.png`）。
**没验证：** 修后的横屏窗口没有重测（这台机的 `cmd window user-rotation lock 1` 没让显示转过去；修前横屏两个后端都是对的，请求尺寸与窗口相同，理应不变）。
