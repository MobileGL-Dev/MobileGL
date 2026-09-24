# 术语

> 参考。文档导航见 [`../README.md`](../README.md)。

| 术语 | 意思 |
|---|---|
| 前端 / 后端 | 前端 = `MG_State` + `MG_Impl`，处理应用的 GL 调用；后端 = `MG_Backend`，真正调用驱动 |
| Espryt / Magma | 两个后端：Espryt 走 OpenGL ES（DirectGLES），Magma 走 Vulkan（DirectVulkan） |
| client / server | 拆分后的前端角色 / 后端角色；可以是两个线程、两个进程或两台机器 |
| monolith / inproc / spawn | 三种拓扑：单线程直调 / 同进程两线程 / 两个进程（可跨机） |
| pull / push | 后端自己去读前端状态（旧做法）/ 前端把状态推给后端（MGPipe） |
| verb | 会让后端真正做事的命令：draw、dispatch、clear、blit、回读、XFB、query、纹理操作。状态只在 verb 之前推送 |
| 句柄 `{slot, gen}` | 对象的身份：client 分配的槽号 + 复用代数，server 从不回传 |
| CSO | 常量状态对象（渲染状态、顶点格式、sampler、sampler view、shader）：client 按内容去重，server 按句柄缓存 |
| 控制面 / 数据面 | 握手与罕见控制消息走的连接 / 命令记录与批量数据走的通道；两者可分别选（共享内存或字节流、本机或 TCP） |
| lockstep / run-ahead | client 每条命令都等 server 做完 / client 发完就走，只有需要答案的命令才等 |
| barrier tax | 拆成两线程后多出来的逐线程 CPU 代价（split − push） |
| device-lost | server 崩溃或断线后 client 进入的状态：GL 调用变空操作、交换缓冲返回"上下文丢失"，而不是崩溃或卡死 |
| class A / B / C | 后端函数表的槽：A 本地回答，B 发成记录，C 还没迁移、按名拒绝 |
| Track V / Track H | 值类读点的迁移（整块数据过线）/ 对象类读点的迁移（对象指针换成句柄） |
| G1 / G2 / G5 / G14 | 验证门：单进程构建二进制逐字节不变 / pull 与 push 测试名集合相同 / 受保护的后端函数逐字节不变 / 测试名只增不删 |
| red-once（R-16） | 每个门都必须真的跑红过一次，证明它能因为它要抓的问题而失败 |
| `MGGen` | server 私有的"我重建了驱动对象"纪元，永不过线；与句柄代数严格分开 |
