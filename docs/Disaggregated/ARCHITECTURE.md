# 设计与架构（索引）

> 这一页只有设计要点和章节去向；每一部分的完整设计在 [`design/`](design/)。章节编号 §1–§17、附 A / B 沿用至今，代码注释里写的 `ARCHITECTURE.md §N` 按下表找到对应文件。wire 契约原文在 `MobileGL/MG_Remote/CONTRACT-*.md`，与设计文字冲突时以更新的契约为准。

## 一句话

后端本来就维护着一台贴着 GLES / Vulkan 的状态机，缺的只是"我被告知了什么"。MGPipe 让前端在每条做事的命令之前，把状态变化以**记录**的形式推给后端；后端只认这些记录、不读前端内存，所以两者之间可以隔一个线程、一个进程或一根网线。

## 设计要点

1. **一份调用目录生成一切**：`PipeCalls.def` 一行一个调用，线上编号就是行序、只追加；两张函数指针表、编解码、校验器都由它生成。
2. **对象用句柄 `{slot, gen}` 寻址**：由前端分配，后端从不回传，所以创建对象不需要往返。
3. **状态在命令之前一次性推送**，不是每个 GL setter 都推；整块数据过线、按内容去重，没变就不发。
4. **同一份后端实现，三种拓扑**：单线程直调 / 同进程两线程 / 两个进程（可跨机）。
5. **反向消息是具名回调**（错误、GPU 写回、窗口尺寸……），走一条与命令同样有序的事件通道。
6. **着色器以 SPIR-V + 反射信息过线**：编译在前端，转译与按状态特化在后端。
7. **传输分两层**：控制面（握手、EGL 控制）与数据面（命令、批量数据），各自可选本机共享内存或字节流 / TCP，可以混搭；上层代码不区分链路种类。
8. **client 发完就走（run-ahead）**：只有需要答案的命令才等；靠 present 配额限速。
9. **出错不崩**：server 崩溃或断线时 client 进入"设备丢失"状态；外来连接要令牌，畸形输入被拒绝而不是让 server 退出。
10. **可证明的迁移**：每个门都必须能变红；单进程构建的二进制在整个拆分过程中逐字节不变（G1）。

## 章节去向

| § | 内容 | 文件 |
|---|---|---|
| 1–2 | 边界：两张表、三种拓扑；对象模型：句柄、两种代数、CSO | [`design/01-boundary-and-objects.md`](design/01-boundary-and-objects.md) |
| 3–4 | 调用目录、生成器 G1–G8、能力位；记录与 payload 格式 | [`design/02-catalogue-and-records.md`](design/02-catalogue-and-records.md) |
| 5–7 | 前端 tracker（何时推、推什么、怎么去重）；纹理上传；着色器 | [`design/03-frontend.md`](design/03-frontend.md) |
| 8 | 反向通道：九个回调、有序性、错误与 ack、纹理重铸拉取、XFB | [`design/04-reverse-channel.md`](design/04-reverse-channel.md) |
| 9–10 | 后端改造（`PipeInputs`、memo 重键、A/B 臂）；server 侧 applier | [`design/05-backend-and-server.md`](design/05-backend-and-server.md) |
| 11 | 传输：共享段、环、门铃、控制面、背压、等待规则、事件、两根轴 | [`design/06-transport.md`](design/06-transport.md) |
| 12–13 | persistent map 与大缓冲采纳；回读、往返清单、五部分验证门 | [`design/07-memory-readback-verification.md`](design/07-memory-readback-verification.md) |
| 14–15 | 帧节奏与线程；进程、握手、EGL、Android（含上屏 server）、崩溃处理 | [`design/08-runtime-and-platform.md`](design/08-runtime-and-platform.md) |
| 16、附 A、附 B | 源码目录与构建选项；全部开关；边界计数器 | [`design/09-build-switches-counters.md`](design/09-build-switches-counters.md) |
| 17 | 各阶段落地时的形状与仍生效的规则（链到 `notes/`） | [`design/10-phase-shapes.md`](design/10-phase-shapes.md) |
