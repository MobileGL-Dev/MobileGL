# P8-0 普查（S 包）

> 插桩不提交（树外 `~/w7/notes/p8/s/instrument.diff`）；全文 `~/w7/notes/p8/s-report.md`。裁定 ID-P8-5。设备计数是下限（Android 不跑退出 dump，按 10 / 600 帧周期 dump）。

## 覆盖

| 语料 | 臂 | 数 |
|---|---|---|
| 主机 trace | monolith / inproc，两后端 | 80 / 78 次重放 |
| 红米 trace APK（插桩） | monolith / spawn `--matrix` | 77 / 77，达 0.99：68 / 70 |
| 红米未插桩对照 | create-indirect、create-instancing | 7 次，区分设备缺陷与插桩 |
| FCL 实机 | 原版 26.3-rc-3，两后端 | 约 15,000 帧；无 Iris / Complementary / Create（未下载） |

## 命中（真实内容）

| 谓词 | 主机 | 设备 trace | FCL | 结论 |
|---|---|---|---|---|
| Magma wire indirect CPU 展开（D） | create-indirect 322 次调用 | create-indirect 321 次 | Magma inproc 44,768 次调用 / 13,028,030 draw | **唯一真实命中** |
| B1 split 存储检查会失败 | 0 / 1375 次生成（R11F 1357、D16 18） | 0 / 391 | 0 | 路径热、失败形状 0 |
| B2 RGB16F / RGB32F 生成 mip | 0 | 0 | 0 | 未见（两条 Complementary trace 只有 R11F 与 RGBA8） |
| C GPU 写过的字节被 CPU 读 | 0（只有指针解析 322/322） | 0 | 0 | 未见 |
| E 拷贝后经影子 `glGetTexImage` | 0（1,288 次拷进 DEPTH / R32F，无一回读） | 0 | 0 | 未见 |
| Magma 生成 mip 着色器形状 | 0 | 0 | 0 | 未见 |

## G（大 blob）

| 量 | 最大值 | arena / 4 | 比例 |
|---|---|---|---|
| program archive | 354,846 B（complementary-unbound） | 8 MiB（SEG_STAGE 32 MiB） | 4.2 % |
| `draw_vbo` 记录 | 1,936 B（156 段） | 2 MiB（SEG_CMD / 4） | 0.1 % |

结论：不做分片。

## create-indirect 门（ID-P8-2）

| trace | 帧 | 加载帧非 `ResourceCreate` 等待 |
|---|---|---|
| rd12 odinlite（设备 spawn，两后端） | 250 | 0 |
| improved-transparency-26.3（设备 spawn，两后端） | 1210 | 0 |
| create-indirect（1 帧） | 1 | 8（设备）/ 10（主机）：`ResourceReadback` 6 + `MapPersistent` |
| FCL 原版 26.3（实机 inproc） | ~15,250 | 16（`MapPersistent`） |

## 设备上基线就有的问题（两臂都有，不归 P8）

- create-indirect 在 Adreno 830 上 kgsl `Preemption Fault`（两后端两臂；ID-P7-4）。
- 1.21.11-main-menu（Espryt 0.890、Magma 0.165）、derivative-main（Espryt 0.851）SSIM < 0.99。
- create-instancing × Espryt × monolith 0.870（spawn 0.99998）——monolith 缺陷，进第二波 dev。
