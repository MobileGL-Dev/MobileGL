<h1 align="center">MobileGL</h1>

<p align="center">
  <img src="https://img.shields.io/badge/Language-C%2B%2B-00599c?style=flat&logo=c%2B%2B" alt="C++">
  <img src="https://img.shields.io/badge/License-GNU%20LGPL%203.0-00399c?style=flat" alt="GNU LGPL 3.0">
  <img src="https://img.shields.io/badge/Status-Development-0078d7?style=flat" alt="Development">
</p>

<p align="center"><em>
一个桌面 OpenGL 实现
</em></p>

MobileGL 是一个 *free* 且 *open-source* 的项目，实现了桌面 **OpenGL** API。目标是提供一个完整的桌面 OpenGL 实现，包括状态管理层和多后端支持。

> [!NOTE]
>
> **Status:** 开发中。部分代码仍未完成。当前短期目标: **OpenGL 3.3 (Core Profile)**。

---

## 项目定位

MobileGL 是一个桌面 OpenGL 库的实现。它旨在提供:

- 完整的 OpenGL 状态管理。
- 一个暴露 OpenGL 函数的前端。
- 多个相互独立的后端实现，每个后端针对特定图形 API，并且彼此完全隔离。

本项目定位为一个实现/翻译层。

---

## 核心组件

仓库按如下顶层模块组织:

1. **MG_State** — 图形 API 的状态跟踪与管理逻辑。
2. **MG_Impl** — 图形 API 的前端实现，与 `MG_State` 和 `MG_Backend` 交互。
3. **MG_Backend** — 各后端的转换层，将前端图形 API 的语义和状态映射为具体后端 API 调用 (例如 OpenGL ES, Vulkan)。
4. **MG_Util** 及其他工具模块。

---

## 第三方组件

MobileGL 复用了多个开源项目:

- **SPIRV-Cross** by **KhronosGroup**  
  - License: [Apache License 2.0](https://github.com/KhronosGroup/SPIRV-Cross/blob/master/LICENSE)  
  - Repository: https://github.com/KhronosGroup/SPIRV-Cross

- **glslang** by **KhronosGroup**  
  - License: [Various Licenses](https://github.com/KhronosGroup/glslang/blob/main/LICENSE.txt)  
  - Repository: https://github.com/KhronosGroup/glslang

- **DiligentCore** by **Diligent Graphics**  
  - License: [Apache License 2.0](https://github.com/DiligentGraphics/DiligentCore/blob/master/License.txt)  
  - Repository: https://github.com/DiligentGraphics/DiligentCore

请参阅各组件仓库以获取准确的许可证文本。本仓库中包含的任何第三方代码均遵循其上游项目的许可证。

---

## 兼容性与目标

- **Short-term target:** `OpenGL 3.3 (Core Profile)`
- **Current development focus:**
  - 面向 `OpenGL 3.3 (Core Profile)` 的 `MG_State` 和 `MG_Impl`
  - `Direct (Vulkan)` backend
  - `Direct (OpenGL ES)` backend

---

## 构建说明

目前我们 **没有发布版本**，也 **没有预编译二进制文件**。  
如果你现在想尝试该项目，需要自行构建:

### 1. 克隆仓库

```sh
git clone https://github.com/MobileGL-Dev/MobileGL.git
cd MobileGL
```

### 2. 初始化并更新子模块

```sh
git submodule update --init --recursive
```

### 3. 按照 glslang 官方文档完成其初始化步骤

请参考:  
https://github.com/KhronosGroup/glslang

### 4. 使用 CMake 构建项目

基础构建:

```sh
cmake -B build
cmake --build build
```

现代方式 (推荐):

```sh
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

你也可以根据需要使用平台特定的构建命令。

---

## 构建选项

| Option                       | Description                                    | Default |
|------------------------------|-----------------------------------------------|---------|
| `MOBILEGL_BUILD_TEST`        | 构建 MobileGL 测试 (需要 Clang)               | ON      |
| `MOBILEGL_BUILD_BENCHMARK`   | 构建 MobileGL 基准测试 (需要 Clang)           | ON      |
| `MOBILEGL_FORCE_RELEASE_OPT` | 在 Debug 构建中启用 O3 和 LTO                 | ON      |
| `MOBILEGL_ENABLE_TRACY`      | 启用 Tracy 性能分析器                          | OFF     |

### Notes

- 项目需要 C++23。
- `MG_Test` 和 `MG_Benchmark` 只能使用 Clang 构建，不能使用 GCC。  
  如需强制使用 Clang，请添加:

```sh
-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
```

- 在 Android 平台上，测试和基准测试始终被禁用。

---

## 注意

- MobileGL 当前 **尚未达到生产可用状态**。
- 部分 `OpenGL 3.3 (Core Profile)` 功能仍缺失或正在开发中。

---

## License

本项目基于 **GNU LGPL v3.0** 发布。  
详细信息请参阅仓库中的 `LICENSE` 文件。