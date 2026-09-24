# 构建、运行与测试

> 上手说明。文档导航见 [`../README.md`](../README.md)；全部开关见 [`../design/09-build-switches-counters.md`](../design/09-build-switches-counters.md) 附 A。

## 四种构建

| 构建目录 | CMake | 用途 |
|---|---|---|
| `build-linux` | 默认（pull） | 出货形态；G1 的基准——它的二进制必须逐字节不变 |
| `build-push` | `-DMOBILEGL_PIPE_PUSH=ON` | 单进程，但后端改读前端推来的状态 |
| `build-verify` | push + `-DMOBILEGL_PIPE_VERIFY=ON` | 每个 draw 把推来的状态与前端实际状态逐字段比对；慢，永不出货 |
| `build-split` | push + `-DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON` | 可以拆成两线程 / 两进程 / 两台机器 |

## 选拓扑

split 构建在运行时用 `MOBILEGL_TRANSPORT` 选拓扑；**不设就是单进程（monolith）对照**：

| `MOBILEGL_TRANSPORT` | 形态 |
|---|---|
| 不设 / `monolith` | 单线程直调 |
| `inproc` | 同一进程两个线程（CI 的主验收形态） |
| `spawn` | 两个进程；再用 `MOBILEGL_IPC_CONTROL`（`fork` / `unix:<path>` / `tcp://host:port`）与 `MOBILEGL_IPC_DATA`（`auto` / `shm` / `stream`）选控制面与数据面，跨机时还要 `MOBILEGL_IPC_TOKEN`（≥16 字节） |

```text
MOBILEGL_TRANSPORT=inproc MOBILEGL_ITEST_REQUIRE_GPU=1 ctest --test-dir build-split -L integration-split --output-on-failure
ctest --test-dir build-split -L 'integration-(spawn|tcp)' --output-on-failure   # 条目自带 spawn / tcp 环境与私有日志
```

其他常用旋钮：`MOBILEGL_IPC_RUN_AHEAD`（默认 1，client 发完就走）、`MOBILEGL_IPC_STAGE_MB`（默认 32，其 1/4 是大块上传的分块预算）、`MOBILEGL_IPC_SURFACE=server`（使用 Android server 自己的窗口）、`MOBILEGL_PIPE_STATS=1`（边界计数器，日志里的 `MGPipe stats:` 行）。

## Android

- 三份 APK flavour：pull / push / split（Gradle 属性 `mobilegl.pipePush`、`mobilegl.buildDisaggregated`、`mobilegl.buildDisaggregatedInproc`）。split APK 同样要设 `MOBILEGL_TRANSPORT`，不设就是单进程对照。
- 设备侧 server 有两种形态：离屏的前台 Service（`MobileGLServerService`）与上屏的 `MobileGLDisplayActivity`；同一时刻只有一个。
- 电脑当 client、手机当 server 的跨机配置（起 server、设令牌、`adb forward` 备用路线）见 [`../notes/p65/README.md`](../notes/p65/README.md) 的"两边怎么配"。
- 性能对比只用 Redmi `2f7cbe2e`，按 [`pin-verification-2026-09-07.md`](pin-verification-2026-09-07.md) 定频，reboot-clean、同热窗口、配对 A/B。
