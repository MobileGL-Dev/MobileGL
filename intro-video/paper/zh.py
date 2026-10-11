"""Simplified-Chinese strings for the MobileGL intro (MGL_LANG=zh). Untranslated strings render as-is."""
TR_ZH = {
    # ---- captions
    'Run desktop OpenGL apps where there is no desktop OpenGL driver.':
        '在没有桌面 OpenGL 驱动的地方，也能运行桌面 OpenGL 程序。',
    "Let's unfold how it works. No deep internals, promise!":
        '一起把它展开看看吧——保证不钻太深！',
    'Lots of great software speaks desktop OpenGL: games, creative tools, whole desktops.':
        '很多优秀的软件都说“桌面 OpenGL”：游戏、创作工具，甚至整个桌面环境。',
    'But phones and tablets usually ship only OpenGL ES or Vulkan drivers.':
        '可手机和平板通常只带 OpenGL ES 或 Vulkan 驱动。',
    "Same GPU, different language: the app's calls have nobody to answer them.":
        '同一块 GPU，却说着不同的语言：程序发出的调用，没人能接。',
    'Meet MobileGL: a complete desktop OpenGL library that sits between your app and the driver.':
        '认识一下 MobileGL：一个完整的桌面 OpenGL 库，位于你的程序和驱动之间。',
    'Your app sees OpenGL 4.6 Core, with just a few corners still being finished.':
        '程序看到的是 OpenGL 4.6 Core，只有少数边角还在完善中。',
    'Underneath, MobileGL speaks whatever the device understands: OpenGL ES or Vulkan.':
        '在底层，MobileGL 说设备听得懂的语言：OpenGL ES 或 Vulkan。',
    "Your app doesn't change. It simply loads MobileGL as its OpenGL library.":
        '程序本身不用改，只需把 MobileGL 当作 OpenGL 库加载。',
    'Why can this work at all? Every modern GPU API is built from the same ingredients:':
        '为什么这行得通？因为现代 GPU API 都由同样的“食材”构成：',
    'buffers, textures, shaders, and state like blending and depth testing.':
        '缓冲区、纹理、着色器，以及混合、深度测试这类状态。',
    'OpenGL keeps that state in one big global switchboard you flip one switch at a time.':
        'OpenGL 把这些状态放在一块巨大的全局开关板上，一次拨一个开关。',
    'At each draw, MobileGL snapshots the switches that matter into an explicit recipe…':
        '每次绘制时，MobileGL 把相关的开关拍成快照，写成一份明确的“配方”……',
    '…that a backend can replay on OpenGL ES or Vulkan.':
        '……再由后端照着配方，在 OpenGL ES 或 Vulkan 上重现。',
    'Step 1: the frontend. Every GL call your app makes lands here first.':
        '第一步：前端。程序发出的每个 GL 调用都先到这里。',
    'It tracks every bind, every uniform, every switch, just like a desktop driver would.':
        '它记录每一次绑定、每个 uniform、每个开关，就像桌面驱动那样。',
    'It checks each call against the spec, so mistakes become proper GL errors, not crashes.':
        '它按规范检查每个调用，出错时返回标准的 GL 错误，而不是直接崩溃。',
    'Nothing has touched the GPU yet. The frontend simply knows what the world should look like.':
        '此时 GPU 还没被碰过。前端只是清楚地知道，画面“应该”是什么样子。',
    'Shaders get folded too. Your GLSL is compiled once, by glslang…':
        '着色器也要“折”一下。你的 GLSL 先由 glslang 编译一次……',
    '…into SPIR-V: a compact, portable shader format.':
        '……折成 SPIR-V：一种紧凑、可移植的着色器格式。',
    'Vulkan takes SPIR-V as-is. For OpenGL ES, SPIRV-Cross unfolds it into GLSL ES.':
        'Vulkan 直接使用 SPIR-V；对 OpenGL ES，则由 SPIRV-Cross 把它展开成 GLSL ES。',
    'One shader, folded two ways, and your app never notices.':
        '同一个着色器，两种折法，程序却毫无察觉。',
    'Step 2: before each draw, the frontend sends the backend only what changed.':
        '第二步：每次绘制前，前端只把“变了的部分”发给后端。',
    'Each change is a small record. The draw itself is a verb that uses them.':
        '每处变化都是一条小小的“记录”，绘制本身则是使用这些记录的“动作”。',
    'New objects get their handles up front, so creating things never waits for a reply.':
        '新对象的句柄在前端就分配好，创建东西时不必等待回复。',
    'Identical state blocks are shared instead of resent. Less to carry, less to check.':
        '相同的状态块只发一次、共享使用。要搬的少了，要查的也少了。',
    "The backend never peeks into the frontend's memory. Records are all it gets.":
        '后端从不偷看前端的内存，它拿到的只有记录。',
    'Step 3: a backend turns records into real driver calls.':
        '第三步：后端把记录变成真正的驱动调用。',
    'Espryt speaks OpenGL ES. Magma speaks Vulkan. You pick one at startup.':
        'Espryt 说 OpenGL ES，Magma 说 Vulkan，启动时任选其一。',
    'Each backend is fully independent, so one can improve without disturbing the other.':
        '两个后端完全独立，改进一个不会影响另一个。',
    'Same app, same frontend. Choose whatever suits the device best.':
        '同一个程序、同一个前端，选最适合设备的那个就好。',
    "Now the fun part: records are just data, so they don't care where the backend lives.":
        '有趣的部分来了：记录只是数据，它们不在乎后端住在哪里。',
    'Run everything on one thread, or give frontend and backend a thread each…':
        '可以全部跑在一个线程里，也可以让前端和后端各占一个线程……',
    '…put them in two processes that share a ring of memory…':
        '……或者放进两个进程，共享一圈环形内存……',
    "…or even on two machines: a PC can drive a phone's GPU over the network.":
        '……甚至放在两台机器上：电脑可以通过网络驱动手机的 GPU。',
    'The frontend side is the client. The backend side, next to the GPU, is the server.':
        '前端这一侧叫客户端，靠近 GPU 的后端一侧叫服务端。',
    "Splitting up isn't slow. Under heavy load it can even be faster than one thread.":
        '拆开并不会变慢。重负载下，它甚至比单线程更快。',
    'The client runs ahead and only waits when it really needs an answer back.':
        '客户端会一路往前跑，只有真正需要回复时才停下来等。',
    'On the same machine, big buffers and window images are shared, not copied.':
        '在同一台机器上，大缓冲区和窗口图像直接共享，不做拷贝。',
    "And if the server goes away, your app sees 'device lost' instead of crashing.":
        '就算服务端没了，程序看到的也只是“设备丢失”，而不是崩溃。',
    'MobileGL grew up on Android: think Minecraft: Java Edition on a phone.':
        'MobileGL 在 Android 上长大：想象一下在手机上玩 Minecraft Java 版。',
    'It also runs on Linux, slips into Windows as opengl32.dll, and works on macOS.':
        '它也能跑在 Linux 上，在 Windows 上化身 opengl32.dll，在 macOS 上同样可用。',
    'A favorite demo: a whole KDE Plasma desktop from a Linux container, rendered by MobileGL on an Android tablet.':
        '一个很受欢迎的演示：Linux 容器里的整套 KDE Plasma 桌面，在 Android 平板上由 MobileGL 渲染。',
    'Same library everywhere. Only the window-system glue changes.':
        '处处都是同一个库，变的只是窗口系统那层“胶水”。',
    'MobileGL is free, open source, and still growing.':
        'MobileGL 自由、开源，而且仍在成长。',
    'Build it, pick a backend, and point your favorite OpenGL app at it.':
        '编译它、选一个后端，然后让你喜欢的 OpenGL 程序用上它。',
    "Something breaks? Tell us. That's the most useful thing you can do.":
        '遇到问题？告诉我们——这就是你能提供的最大帮助。',
    # ---- scene tags
    'The problem': '问题在哪', 'Meet MobileGL': '认识 MobileGL', 'Why it works': '为什么可行',
    'Step 1 · The frontend': '第一步 · 前端', 'Shaders get folded': '着色器的折法',
    'Step 2 · Records': '第二步 · 记录', 'Step 3 · Backends': '第三步 · 后端',
    'Records can travel': '记录可以远行', 'Built for the trip': '为远行而生', 'Where it runs': '在哪里运行',
    # ---- labels
    'a desktop OpenGL implementation': '一个桌面 OpenGL 实现',
    'Your app': '你的程序', 'OpenGL ES driver': 'OpenGL ES 驱动', 'Vulkan driver': 'Vulkan 驱动',
    'no app\nchanges': '程序\n无需修改',
    'OpenGL: one big switchboard': 'OpenGL：一块大开关板', 'Explicit recipes': '明确的配方',
    'buffers': '缓冲区', 'textures': '纹理', 'shaders': '着色器', 'state': '状态',
    'blend': '混合', 'depth': '深度', 'cull': '剔除', 'stencil': '模板', 'texture 0': '纹理 0',
    'program': '程序', 'viewport': '视口',
    'Draw recipe #1': '绘制配方 #1', 'Draw recipe #2': '绘制配方 #2', 'Draw recipe #3': '绘制配方 #3',
    'shaders: A + B': '着色器：A + B', 'shaders: C + B': '着色器：C + B',
    'blend: off': '混合：关', 'blend: on': '混合：开', 'depth: less': '深度：less', 'depth: always': '深度：always',
    'cull: off': '剔除：关', 'vertex format #2': '顶点格式 #2', 'vertex format #5': '顶点格式 #5',
    'GL state': 'GL 状态', 'draws': '绘制',
    'Textures': '纹理', 'Uniforms': 'Uniform', 'Blend': '混合', 'Vertex arrays': '顶点数组',
    'Magma (Vulkan): uses it as-is': 'Magma（Vulkan）：直接使用',
    'Espryt (OpenGL ES): GLSL ES': 'Espryt（OpenGL ES）：转成 GLSL ES',
    'Frontend': '前端', 'Backend': '后端',
    'texture 7 on unit 0': '纹理 7 绑到单元 0', 'viewport 1920×1080': '视口 1920×1080',
    'DRAW': '绘制', 'uniform tint': 'uniform：tint', 'texture 12 on unit 1': '纹理 12 绑到单元 1',
    'new texture': '新纹理', 'only\nchanges': '只发\n变化', 'no\nwaiting': '无需\n等待',
    'sent\nonce': '只发\n一次', 'blend on · depth less': '混合开 · 深度 less',
    'speaks OpenGL ES': '说 OpenGL ES', 'speaks Vulkan': '说 Vulkan', 'fully\nseparate': '完全\n独立',
    'One thread': '单线程', 'Two threads': '双线程', 'Two processes': '双进程', 'Two machines': '两台机器',
    'spawn + shared memory': 'spawn + 共享内存', 'frontend': '前端', 'backend': '后端',
    'client': '客户端', 'server': '服务端',
    'client = frontend, inside your app': '客户端 = 前端，在你的程序里',
    'server = backend, next to the GPU': '服务端 = 后端，紧挨着 GPU',
    'Fast split': '拆开也快', 'Run-ahead': '提前跑', 'Zero-copy': '零拷贝', 'Safe failure': '安全失败',
    'Two threads or processes can even beat one: both sides work at once.':
        '双线程或双进程甚至能胜过单线程：两边同时干活。',
    'Keep sending. Wait only for the answers you truly need.': '一直往前发，只等真正需要的回复。',
    'Big buffers and window images are shared, not copied.': '大缓冲区和窗口图像直接共享，不做拷贝。',
    "Server gone? Your app sees 'device lost', not a crash.": '服务端没了？程序只会看到“设备丢失”，不会崩溃。',
    'Its first home: Minecraft: Java Edition via launchers like FoldCraftLauncher.':
        '最初的家：通过 FoldCraftLauncher 等启动器运行 Minecraft Java 版。',
    'EGL, GLX, Wayland and glvnd. Even a whole KDE Plasma desktop.':
        'EGL、GLX、Wayland 和 glvnd，甚至能跑整套 KDE Plasma 桌面。',
    'A drop-in opengl32.dll that speaks WGL.': '即插即用的 opengl32.dll，支持 WGL。',
    'CGL and NSOpenGL, on Vulkan via MoltenVK.': 'CGL 与 NSOpenGL，经 MoltenVK 跑在 Vulkan 上。',
    'KDE Plasma\non a tablet': '平板上的\nKDE Plasma',
    'free & open source  ·  LGPL-3.0': '自由开源  ·  LGPL-3.0',
    'Come fold with us!': '一起来折吧！',
}
