# review-g5（集成者审查，2026-09-23）：门 5 的三个 inproc × DirectVulkan CTS 缺陷

| 包 | 提交（只取这些） | Verdict | 核对 |
|---|---|---|---|
| g5-readback | `18d3f27d`、`60eabf7b`、`e521c72b`（跳过重复的 `01f95c7b`） | **land** | 客户端把超出一个回复槽的 ReadPixels 切成行带（单行超槽则切行内片），每带是一条普通 `read_pixels`、DstSize 取带的 tight 值，server 侧 PH-3 界不变；中性 pack 态保零拷贝、bounce 只一带、scatter 与整读同一函数；`Fatal{ReplyTooLarge}` 只剩「一个像素都放不下」。7 个场景含 2 MiB 整、2 MiB+1 行、17 MB > 整个 SEG_REPLY、pack 缝隙、PBO、深度/模板，red-once 72/86 红 |
| g5-imgwin | `e8869c2a`（跳过 `8cc63c0b`）；F2 落地后按包注记把 view 窗口用例并入门控臂 | **land** | 图像单元指向纹理没有的 level/layer（GL 4.6 §8.26）= 空单元：绑存储占位（着色器声明维度、每次清零），格式无声明时取应用绑定格式；只由 `ResolveWireTextureStorage` 的「确实不在」答案触发，死记录/陈旧 view 仍是具名拒绝；monolith 不动（BASE 本就 Fail） |
| g5-msrbo | `7c1a3b74`（单采样深/模板 1:1 blit 改 copy）、`b10c8529`；`e80392ec`/`19c78bb9` 的**按 vendorID 选臂** | **rework → g5-msprobe** | 根因成立（Adreno 830 的 render-pass 深度/模板 resolve 不写目标），但按 Qualcomm vendorID 选臂违反用户 2026-08-22 规则「后端限制类判定一律 POST 探针、不按驱动名硬编码」→ 改为带对照组的运行时探针 + POST Known Driver Bugs 行（FIXED） |
| g5-msprobe（p7/g5-msrbo 续） | `52d1099b`、`d2f2971a`、`926869f0` | **land** | 删 `WirePrefersShaderDepthResolve(vendorID)`；`WireDepthResolveProbe` 在 server 侧 Magma 初始化跑一次：六种深度/模板格式 4×4×4 样本、清到 0.25/0x5A、目标预填哨兵，用**生产同一个**无 draw render-pass resolve，对照组 = 生产着色器臂；对照不过的面不计 → inconclusive 保默认序。红米 6 格式全 `render pass 0/16 / shader control 16/16` → verdict broken，CTS 3/3；强制 clean 同库即 Fail。POST Vulkan「Known Driver Bugs」加 FIXED 行（无 stencil export 为 UNFIXABLE）。补一条 should-fix：`elide-subject` 测试旋钮让 lavapipe 的真实测量路径判 broken，证明主机上探针的真测量能红 |
| w2-reducer（p7/devprep） | `fe4c8745..f93a9da9` | **land** | 门 3/门 5 化简器按合同公式、五块必齐、同一 boot_id、缺块/空读/子集不给 PASS，自测覆盖；设备共享锁 |

共同基底：三个包都带 PH-4 空层修复的重复拣选（补丁 id `92d463c8`）；只随 F2 线落一次（`58835fa4`）。
