# 代码地图

> 参考。文档导航见 [`../README.md`](../README.md)；目录布局与构建选项见 [`../design/09-build-switches-counters.md`](../design/09-build-switches-counters.md) §16。

| 路径 | 作用 |
|---|---|
| `MobileGL/MG_Pipe/` | 接口本身：调用目录 `PipeCalls.def` 与其他 `.def` 表、payload / 句柄 / 回调头、`PipeApply`、`PipeRoute`、`generated/*.inc`（生成物，提交进树） |
| `MobileGL/MG_Impl/Pipe/` | 前端：Tracker、`PipeFill`、`SlotAllocator`、`CsoCache`、`CompositeResolver`、`ResourceTracker`、各族 `*Emit.h` |
| `MobileGL/MG_Backend/MGPipe/` | `PipeInputs`：后端读的、被前端推来的状态块 |
| `MobileGL/MG_Remote/` | 拆分专用（仅 `MOBILEGL_BUILD_DISAGGREGATED`）：`Protocol/`（控制面 schema）、`Transport/`（控制面 `ITransport` × 数据面 `ILink`）、`Wire/`（记录编解码）、`Client/`、`Server/`；各阶段的 wire 契约 `CONTRACT-*.md` |
| `MobileGL/MG_Util/Metrics/PipeStats.{h,cpp}` | 边界计数器（`MOBILEGL_PIPE_STATS=1`） |
| `MobileGL/MG_Test/Pipe/`、`MG_Test/Wire/`、`MG_IntegrationTest/Harness/*Peek` | 目录 / tracker / 发射器单元测试、wire 与 remote 单元测试、白盒断言 |
| `scripts/` | 生成器（`gen_pipe.py`、`gen_pipe_field_ownership.py`、`gen_pipe_dirty_surface.py`、`gen_protocol.py`）、门脚本（`symbol_report.py`、`check_include_closure.py`、`link_ratchet.py`、G5 保护区脚本、`check_doc_citations.py`）、`ci/` |
| `tools/trace_replay/`、`tools/device_bench/` | trace 回放（含 TCP 矩阵跑器）、设备定频与配对 A/B 工具 |
