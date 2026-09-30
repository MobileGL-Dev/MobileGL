# P8 集成者裁定日志（server 侧仿真缺口与 split 覆盖）

> 形式沿用 [`../p11/INTEGRATOR-DECISIONS-P11.md`](../p11/INTEGRATOR-DECISIONS-P11.md)：一条裁定一个 ID，只记「定了什么、为什么、影响哪些文件」。计划见 [`PLAN-P8.md`](PLAN-P8.md)。

| ID | 日期 | 裁定 | 理由 | 影响 |
|---|---|---|---|---|
| ID-P8-1 | 2026-09-29 | **重定界**（用户批准）：原范围在 `README.md` 与路线图行里划掉保留；方向改为补 server 侧缺口与 split 覆盖；原 10 项里 6 项与 2 个门、开放问题 9 关闭（`PLAN-P8.md`「已关闭」表）。 | 只读核查逐项对代码：split 可达的仿真拒绝只剩 Espryt 生成 mip 两名；client 侧工作已由 P5b–P9 完成。 | `ROADMAP.md`、`notes/p8/` |
| ID-P8-2 | 2026-09-29 | 门改写：覆盖门 = 归一化后 split ⊆ monolith ∪ 豁免表；create-indirect 往返门 = 多帧设备 trace 加载帧非 `ResourceCreate` 等待 = 0。 | 原两门按字面不可达 / 是空的（trace 只 1 帧）。 | A 包、P8-0 |
| ID-P8-3 | 2026-09-29 | G1 不变；各包 split 半边放在 `MOBILEGL_BUILD_DISAGGREGATED` 下。monolith 半边（例如 B2 的 CPU 滤波读陈旧影子）若 red-once 证实，在 dev 上单独修再合并（用户同意）。 | 沿用 P9 / P11 做法。 | B、C、E |
| ID-P8-4 | 2026-09-29 | 第一波七包并行（P8-0、A、B、C、D、E、F，树 `~/w7/p8-<包>`，分支 `p8/<包>`，集成树 `~/w7/p9-main` 分支 `p8/main`）；G 等 P8-0 的测量；C、D 为 P13 前置。设备只由 P8-0 使用（红米，约半天，用户同意）。 | 各包改动区域不重叠（`DirectGLES.cpp` 按行段分开）。 | 全部 |
