# 各阶段的落地形状（原 ARCHITECTURE §17）

> 设计细节。设计要点与全部章节的索引见 [`../ARCHITECTURE.md`](../ARCHITECTURE.md)。章节编号沿用原 `ARCHITECTURE.md`，代码注释里的 `ARCHITECTURE.md §N` 按编号在这里找到。

## 17. 阶段落地形状（索引）

各阶段落地时的形状、偏差与论证原文在 notes 里；下表只列仍然生效的规则。

| 原节 | 主题 | 仍生效的规则 | 全文 |
|---|---|---|---|
| 17.1 | verb barrier 与诚实的同地址空间传输（P5） | 同地址空间不得成为旁路（R-2）：encoder 把 `MGHostSpan::Ptr` 恒写 `nullptr`；reply 以记录序号为 slot id（R-3）；等待看门狗 120 s 只判死锁，不是性能门 | [`notes/p5/README.md`](../notes/p5/README.md) |
| 17.2 | 后端槽三类、caps、tight readback（P5） | A 本地答 / B 发射 / C 具名拒绝，永不回落 monolith（R-4）；能力只读 `CallMask`（R-8）；`ReadPixels` 线上恒 tight，client 按自己的 pack state 散射 | 同上 |
| 17.3 | server 角色、shadow 与退出（P5） | apply 线程终身持有 native context，EGL 罕见操作经控制邮箱进 apply 线程；staged shadow 只交 server-owned 拷贝；有界退出顺序 | 同上 |
| 17.4 | lane 隔离（P5） | 每条 split 条目私有日志；E1 / E3 控制把"选中条目被跳过"判为失败 | 同上 |
| 17.5 | class-C verb 迁移（P5b） | 迁移顺序由动态普查决定；一次迁移 = 一个发射器 + 一个 sink 体；记录逐字带 GL 调用参数，未测量的形式按名拒绝（规则 D） | [`notes/p5b/README.md`](../notes/p5b/README.md) |
| 17.6 | 共享地址空间访问归零（P5c） | server 端纹理 staged shadow；`SEG_EVENT` 是唯一反向通道；sink / twin 按记录句柄解析；`applier_reset` / `object_death` 控制记录；角色守卫 `Fatal{RoleViolation}` | [`notes/p5c/README.md`](../notes/p5c/README.md) |
| 17.7 | run-ahead（P5e） | 规则 F：unbarriered 记录的 apply 不读任何 client 内存（违反即具名 Fatal，与 strict 开关无关）；`gPipeInputs` 是 server 角色内存；`RunAheadArmed()` 在首次 caps 采纳时锁存，只能关不能开；`RUN_AHEAD=0` 是 A/B 对照，`VERB_BARRIER=0` 会连带关掉 run-ahead | [`notes/p5e/README.md`](../notes/p5e/README.md) |
