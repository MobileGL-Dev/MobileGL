| ID | 日期 | 裁定 | 影响 | 出处 |
|---|---|---|---|---|
| ID-P13-1 | 2026-10-06 | **FCL 内嵌库不编 MG_Remote**（用户）。记录臂需要的东西（暂存 store、applier 纹理 ops、两后端记录臂与选择器）从 `MOBILEGL_BUILD_DISAGGREGATED` 门控里拆出来，无 MG_Remote 的库也走记录臂；无 MG_Remote 的库同时是传输阴性对照。 | 计划 §5 的 A2；W5 | [`PLAN-P13.md`](PLAN-P13.md) §5、§7-1 |
| ID-P13-2 | 2026-10-06 | **「monolith 逐线程 CPU 不差于 P0 基线」只记录、不设门**（用户，沿用 2026-09-08）。设备 A/B 延后。 | 出口门表该行改为记录项 | §2、§7-2 |
| ID-P13-3 | 2026-10-06 | **运行期位图**（用户）：bits 0–13 固定开，清任一位在启动期具名拒绝；bit 4 退役；bit 63（关 CSO 内容寻址）保留；各 Off 车道改为具名拒绝的阴性对照；`HandleRecycle.Legacy.` / `.AbaControl.` 随遗留臂结构性删除（`Handles.` / `AbaControlHandles.` 保留）。不算 CI 降级。 | W3b、W6 | §4.1、§7-3 |
| ID-P13-4 | 2026-10-06 | **G1 退役**（用户）：W2 记最后一次读数后退役；接班 = 无 MG_Remote 符号检查 + skip 普查 + recorder 金标。 | W1、W2、W8 | §4.3、§7-4 |
