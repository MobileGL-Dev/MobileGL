# P5f FCL / Minecraft 端到端补测

2026-09-20，Redmi `2f7cbe2e`，FCL `com.tungsten.fcl.mgdebug.debug`，Minecraft
`26.3-rc-3`，现有单人世界 `test`。此前 [device-report.md](device-report.md) 是公开 GL
像素 runner，不是 FCL E2E；本文是用户要求后新增的真实游戏测试。

**两个后端均已实际启动测试；Magma inproc 失败，不能表述为两后端 split 均可玩。**
GLES inproc、GLES monolith 与 Magma monolith 进入世界并完成至少 60 秒连续运行，截图人工复核。
Magma inproc 在加载阶段触发明确的 P7 buffer-consumer 拒绝，未到进世界阶段。

## 被测制品与安装

- MobileGL 源头：`aa78f102ff8ecbe32be34b79d1d654c039867d19`，已推至
  `origin/feat/disaggregated`。相对上一份验证库源头 `cfca93c7` 没有 C++ 行为变化。
- 正常 Gradle 构建 `:FCL:assembleFordebug`，`-Darch=arm64` 与
  `mobilegl.pipePush=ON`、`mobilegl.buildDisaggregated=ON`、
  `mobilegl.buildDisaggregatedInproc=ON`；**BUILD SUCCESSFUL in 52s**。
- `settings.gradle.kts` 的已有测试接线指向 `MobileGL-disagg`；NDK 27.3.13750724、
  arm64、RelWithDebInfo。FCL 原有 Java / Kotlin 任务复用有效增量产物；未修改其源代码。
- APK：`FCL/build/outputs/apk/fordebug/FCL-fordebug-1.3.3.2-arm64-v8a.apk`，326039983 bytes，
  applicationId 精确为 `com.tungsten.fcl.mgdebug.debug`，versionCode 1332。
- APK SHA256：`864fb685893abd346ac62aef1921dddddb6cd49c616ff0812ba9225cc90d11d7`。
- `libMobileGL.so` SHA256：`76c61d5990f52e5ffcad9741532ea2dec443d6677f97b55585df886df0f1a1e7`。
- APK 与设备实际 nativeLibraryDir 的 MobileGL / libc++ hash 逐项一致；libc++ 为
  `46b51d661454b9cfaf42c1dc90893b5ad601b7a7ffc2e09d44bb3cc9d20e7ae2`。
  这是本轮 Windows/Gradle 的实际产物，不混用先前 WSL pixel runner 的库 hash。
- 签名沿用已有 FCL debug key，证书 SHA256
  `b8f82162dbfd193d2c8383bda495d5d5cdbabcd00c4e9064bc59483422a8b765`；`adb install -r`
  成功，设备 lastUpdateTime `2026-09-20 13:49:19`。配置、options 与 55 MiB 的 test 世界已备份，
  没有清应用数据或改其他 FCL 包。

## 实际运行结果

| 后端 / transport | 入世界 | 稳定窗口 | 窗口内帧增量 | 轮询近似 FPS（仅记录） | 判定 |
|---|---:|---:|---:|---:|---|
| GLES / inproc，strict=1、role-split=1 | 32.029 s | 64.005 s | 14280 | 223.109 | PASS，真实 apply verbs、所有128个统计窗口 rsp=0 |
| Magma / inproc，strict=1、role-split=1 | 未进入 | — | — | — | FAIL，21.454 s 内退出，P7 buffer consumer |
| Magma / monolith | 28.435 s | 62.251 s | 10560 | 169.636 | PASS，90个统计窗口 rsp=0、vbs=0 |
| GLES / monolith | 38.864 s | 60.696 s | 6840 | 112.692 | PASS，64个统计窗口 rsp=0、vbs=0 |

成功项同时核对：实际 backend 日志、inproc 的 capability/strict/role 配置、fresh 游戏日志
中的玩家进入世界、持续递增的 frames、进程存活与截图。GLES inproc 已 ARMED；Magma inproc
明确运行 lockstep，没有以界面选项或环境文件代替运行臂证明。每组前重启；GLES inproc
首次启动失败后的重试在同一次 boot 内完成。

画面人工检查范围是地形、树木、天空、手部、快捷栏与正常菜单；未见黑屏或明显纹理破坏。
场景时间/视角在运行中有变化，未定频，也不是固定帧像素比对；表中 FPS 不能用于后端性能
比较或回归判定。只验证这个版本、这个世界及数分钟内的游戏流程，不宣称完整功能覆盖。

### Magma inproc 的真实失败

```text
[13:58:45] [Android mgl-srv-apply/FATAL]: MGPipe: Fatal{RoleViolation, "buffer-legacy-arm"} (Magma P7 buffer consumer)
```

同次日志先报告 subsystem `0x80` 没有 server consumer，随后在 apply 线程拒绝 legacy buffer
路径。该文案对应 Magma draw / indirect buffer 的 P7 守卫，区别于 UniformManager 的另一条
`buffer descriptors remain P7` 文案。现有抓取没有 native PC / tombstone，不能进一步指定
是哪一个 GL draw 或 VBO/EBO 触发；FCL 自身的 SIGABRT hook 会接管退出。
这不是测试跳过，也没有通过关闭 strict / dualblock 绕过拒绝。Magma monolith 成功仅说明
该库的单体路径能跑此游戏，不能代替 split 支持。P5f 的已完成范围仍以 wire ownership 门
为准，完整 Magma 应用缓冲消费继续属于 P7。

### 其余诊断没有隐去

GLES 首次启动 PID 12166 在 MobileGL 初始化前被 signal 34 结束，没有生成 library log；
根因未定位。原始日志保留在 `GLES-inproc/`，同配置重试保留在 `GLES-inproc-retry/`。
两次都有 `ClassNotFoundException: sun/java2d/SurfaceManagerFactory`，成功重试证明不能
把这条被处理的 CTC agent 警告认作首轮退出原因。

成功运行仍有初始化诊断：GLES 的 `eglSwapBuffersWithDamageEXT` 动态入口缺失带历史 FATAL
日志级别，但没有导致 abort；还有 `glShaderStorageBlockBinding` 入口缺失、两后端的
`GL_STENCIL_INDEX8` 归一化诊断，Magma 的 separate depth/stencil 不支持提示。
因此本文不宣称“日志零错误”；通过项是本场景实际进世界、运行与画面检查，且无 wire
`Fatal{...}` / 进程退出。Magma 的具名 P7 拒绝单列为失败。

## 复现与证据

证据根：`C:/Users/geekerwan/.codex/tmp/p5f-fcl-e2e-aa78f102/`。其中有 Gradle 日志、
`artifact-identity.json`、设备库 hash、构建接线快照、备份，以及每臂的 library/latest/logcat、
boot-id、逐次 pid/统计采样、summary 和截图。`run_fcl_arm.py` 只负责测试采样，未清 logcat、
未 pin 时钟；机器成功状态需经过人工截图检查再判 PASS。

FCL 当前的 `mg_transport.txt` / `mg_env.txt` 接线只在内置 Espryt renderer 分支执行；因此四臂
保持该入口，通过 `MOBILEGL_BACKEND_TYPE` 明确覆盖后端，并核运行日志。直接点内置 Magma
选项默认走 monolith，不能把这种操作误记成 Magma inproc。两后端加载同一个 `libMobileGL.so`。
所有 inproc 组使用 stage=256 MiB、strict=1、role-split-state=1、run-ahead=1、stats period=120；
monolith 对照明确设 transport=monolith、role-split-state=0。

测试结束已从游戏菜单保存退出，恢复原 `mg_transport.txt=inproc` 与空 `mg_env.txt`，
核对文件 hash 一致。新 APK 保留安装；未改系统时钟/定频设置，未回滚运行中发生的世界变更。
完整 test 世界备份仍在证据根的 `backup/test/`。父仓原有冲突索引和未提交构建配置未改动。
