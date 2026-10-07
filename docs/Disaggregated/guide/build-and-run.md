# 构建、运行与测试

> 上手说明。文档导航见 [`../README.md`](../README.md)；全部开关见 [`../design/09-build-switches-counters.md`](../design/09-build-switches-counters.md) 附 A。

## 三种构建

P13 起只有一种单进程形态：后端只从前端推来的记录取状态（记录臂），旧的「后端直接读前端」路径与 pull / push 两套构建一起删掉了（`MOBILEGL_PIPE_PUSH` 不再是 CMake 选项）。

| 构建目录 | CMake | 用途 |
|---|---|---|
| `build-linux` | 默认 | 出货形态（FCL 内嵌库就是它）：单进程，记录臂，不编 `MG_Remote` |
| `build-verify` | `-DMOBILEGL_PIPE_VERIFY=ON` | 每个 draw 把推来的状态与前端实际状态逐字段比对；慢，永不出货 |
| `build-split` | `-DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON` | 可以拆成两线程 / 两进程 / 两台机器；不设 `MOBILEGL_TRANSPORT` 时是同一记录臂的单进程对照 |

库由七个 OBJECT 模块拼成（`mg_util` / `mg_pipe` / `mg_backend` / `mg_frontend` / `mg_remote_transport` / `mg_remote_client` / `mg_remote_server`，按 `scripts/link_ratchet.py` 的 PARTITION 划分）；split 构建另有 `MobileGL_server_linkcheck`，证明 server 那一半不带前端模块也能链接（剩下的前端引用是 link ratchet 基线里具名的债务）。

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

- 两份 APK flavour：单进程 / split（Gradle 属性 `mobilegl.buildDisaggregated`、`mobilegl.buildDisaggregatedInproc`；独立 plugin 构建默认 split，FCL 内嵌默认单进程）。split APK 同样要设 `MOBILEGL_TRANSPORT`，不设就是单进程对照。
- 设备侧 server 有两种形态：离屏的前台 Service（`MobileGLServerService`）与上屏的 `MobileGLDisplayActivity`；同一时刻只有一个。plugin 与 trace 两个 flavour 的 APK 都带（standalone 构建默认开 `mobilegl.buildDisaggregated`），入口是 APK 底部导航栏的 “Render Server” 页；plugin flavour 的这三个组件不导出，只有 trace 允许 adb 直接按组件名拉起。
- 电脑当 client、手机当 server 的跨机配置（起 server、设令牌、`adb forward` 备用路线）见 [`../notes/p65/README.md`](../notes/p65/README.md) 的"两边怎么配"。
- 性能对比只用 Redmi `2f7cbe2e`，按 [`pin-verification-2026-09-07.md`](pin-verification-2026-09-07.md) 定频，reboot-clean、同热窗口、配对 A/B。P13 之后的性能工作以 `dev` 分支构建为「之前」基线（用户 2026-10-07）。
