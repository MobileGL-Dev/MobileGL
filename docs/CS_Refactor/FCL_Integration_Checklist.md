# FCL Integration Checklist (MobileGL C/S, FCL 0 改动)

> 状态：代码完成态（phase 2，禁用 shell，未编译/未真机）。
> 目标读者：MobileGL 核心开发者 + 验证者。本文件把 FCL 启动全链路与 MobileGL
> 组件逐点划上钩，并记录已知不支持面（以 `generated_trampoline_coverage.txt` 为准）。

## 1. 启动链路逐点映射

| # | FCL 行为（来源） | MobileGL 组件 | 状态 |
|---|---|---|---|
| 1 | 安装渲染器插件 APK（`top.mobilegl.plugin`） | `android-plugin/app`（`:MobileGL` 产 `jniLibs`） | ✅ 配置完成 |
| 2 | 校验 `renderer` / `pojavEnv`（`PluginNativeLoadGuard.verifyRendererLibraries`） | manifest: `MobileGL C/S:libMobileGL_Client.so:/libMobileGL_Client.so` + `DLOPEN=…` | ✅ |
| 3 | `LD_LIBRARY_PATH` 指向插件 native 目录 | 无需客户端特殊处理 | ✅ |
| 4 | `DLOPEN` 依序加载 FullServer/UtilRuntime/BackendObject | `mobilegl_fullserver_create` 再用同路径 `dlopen`（幂等） | ✅ |
| 5 | dlopen `glLib` = `libMobileGL_Client.so` | `ClientTrampoline` 首次调用 → `InitializeFromEnvironment()` | ✅ |
| 6 | `-Dorg.lwjgl.opengl.libname=…` | LWJGL 加载 Client `.so`，经导出 `egl*`/`gl*` | ✅（受支持子集） |
| 7 | 游戏线程调 `eglGetDisplay/Initialize/CreateContext/MakeCurrent` | M1 EGL trampoline（DisplayCreate/GroupCreate/SessionCreate + `SetCurrentSession`） | ✅ |
| 8 | 游戏调 GL 命令 | `generated_wire_trampoline.cpp`（`Wire::SendGl*` → FullServer → DirectGLES） | ✅（受支持子集） |
| 9 | 对象创建（`glGenTextures` 等） | `ret_bytes` 通道（服务端分配 + 客户端拷贝） | ✅ |
| 10 | 数据上传（`glBufferData` 等带 size 字段） | `AllocateShm` + `memcpy` + `ret_i64` | ✅ |
| 11 | `glGetString` | `SendGetString` 专用通道 | ✅ |
| 12 | 进程退出 | `Client::Shutdown` → `mobilegl_fullserver_destroy`（join server thread） | ✅ |

## 2. 环境变量（`MOBILEGL_CS_*`）

见 README「Runtime environment variables」表。插件 manifest 已固化：
`MOBILEGL_CS_MODE=inprocess`、`MOBILEGL_CS_BACKEND=DirectGLES`。

## 3. 已知不支持面（eglGetProcAddress 返回 nullptr / 生成器跳过）

| 类别 | 例子 | 计划 |
|---|---|---|
| 无尺寸 out 参数 | `glGetIntegerv` / `glGetFloatv`（已支持：共享 `MobileGLQueryCount` 表 + `out_capacity`） | ✅ 已实现 |
| 多级指针 | `glShaderSource`（已支持：shm NUL 拼接 + FullServer opcode 拦截重建指针数组）；`glGetActiveAttrib/Uniform` 等仍待 | 部分支持 ✅/待补 |
| 输入向量 | `glUniform*`（已支持：客户端物化 + 内联向量） | ✅ 已实现 |
| 指针返回（非 GetString） | `glMapBufferRange`（typed 通道已覆盖） | 保持 typed 路径 |
| EGL 全量 | `eglCreateSync` / `eglCreateImage` 等 | 后续补齐 |

**安全红线**：任何返回写入都必须遵守“服务端知道写入量 ≤ 客户端缓冲区容量”。
当前仅对带 `n/count/num` 的 out 向量允许自动分配；无尺寸查询保持 unsupported。

## 4. 验证步骤（需要 shell/构建工具，phase 2 禁用，留待后续阶段）

```sh
# 1) 代码生成 + 编译（Linux）
cmake -S . -B build_agent -DMOBILEGL_BUILD_TEST=ON -DMOBILEGL_BUILD_CS_REFACTOR=ON -DMOBILEGL_FLATC=/tmp/flatc_bin/flatc
cmake --build build_agent --target MobileGL_Client MobileGL_FullServer MobileGL_UtilRuntime BackendObject_DirectGLES -j

# 2) 生成覆盖报告
cat MobileGL/MG_Client/generated_trampoline_coverage.txt

# 3) 单元/传输回归
ctest --test-dir build_agent -L unit -j

# 4) 插件 APK（Android，需要 SDK/NDK 27.3）
cd android-plugin && ./gradlew :app:assemblePluginRelease
```

## 5. 验收标准（M1/M2/M3）

- **M1**：裸进程 `libMobileGL_Client.so` + in-process hosting → `eglGetDisplay/Initialize/MakeCurrent/glClear/glReadPixels` 全通。
- **M2**：FCL 选择「MobileGL C/S」渲染器，进入 Minecraft 主菜单（对象创建 + 数据上传 + GetString 必通）。
- **M3**：游戏可玩（渲染正确、crash-free、退出无挂起线程）。
