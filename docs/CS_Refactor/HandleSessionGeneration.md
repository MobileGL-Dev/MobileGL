# MobileGL C/S: Handle / Session / Generation 统一语义

> 状态：Phase 0 契约文档（与 `C_S_REFACTOR_PLAN.md` §5 一一对应）
> 范围：FullServer、Client、BackendObject、UtilRuntime 共用的对象标识语义
> 读者：所有实现 BFA / 外部协议的模块

---

## 1. ID 层级

```text
DisplayId
  └─ SharedGroupId
       └─ SessionId
            └─ ObjectHandle（共享对象在 SharedGroup 级；上下文私有对象在 Session 级）
```

所有 ID 均为 `uint64_t`，由 FullServer 在进程生命周期内分配，**不复用**。`0` 保留为无效 ID。

| ID | 分配者 | 生命周期 | 语义 |
|---|---|---|---|
| `MobileGLDisplayId` | FullServer (EGL) | `eglGetDisplay` → `eglTerminate` | 一个 EGLDisplay 的句柄；持有 backend device |
| `MobileGLSharedGroupId` | FullServer | 首个无 share 的 context 创建 → group 内全部 context 销毁 | 共享对象表、ShareGroup 级 backend native 资源 |
| `MobileGLSessionId` | FullServer | `eglCreateContext` → `eglDestroyContext` | 一个 GL context；持有 current bindings / error queue |
| `MobileGLBackendHandle` | FullServer | 对象创建 → 对象销毁 | 不透明对象句柄，与 GL name 解耦 |

## 2. 归属规则

| 状态 | 归属 |
|---|---|
| EGL display / config / surface | DisplayId |
| GL 共享对象表（buffer/texture/sampler/VAO/program/shader/pipeline/FBO/RBO/TF） | SharedGroupId |
| 上下文私有对象（default FBO/VAO/texture 0、query、sync 等） | SessionId |
| current bindings / render state / viewport / error queue | SessionId |
| backend native 共享资源（VkBuffer/VkImage/pipeline/sampler） | SharedGroupId |
| command pool / descriptor pool / frame tracking / deferred release | SessionId |
| 硬件设备 / loader | DisplayId |

## 3. ObjectHandle 模型

```text
ObjectHandle = uint64_t
  - FullServer 分配，进程生命周期内不复用
  - 共享对象：同一 handle 在 SharedGroup 内所有 Session 可见
  - 上下文私有对象：仅所属 Session 有效
  - 与 GL name 解耦
  - 映射：
      共享对象     (SharedGroupId, ObjectKind, GLname)  → handle
      上下文私有  (SessionId, ObjectKind, name/slot)   → handle
```

设计理由：

- 天然支持 name 0 默认对象、proxy texture、texture view、reserved-but-not-materialized 对象；
- 避免 `SharedPtr<T>` / 裸指针跨插件边界；
- 未来 share group 语义扩展不需要改接口。

## 4. 映射表（FullServer 拥有）

FullServer 维护以下 Registry（线程安全）：

```text
DisplayRegistry  : DisplayId -> DisplayInfo
SharedGroupRegistry : SharedGroupId -> SharedGroupInfo
SessionRegistry  : SessionId -> SessionInfo
SharedObjectRegistry : { SharedGroupId, ObjectKind, GLname } -> BackendHandle
PrivateObjectRegistry : { SessionId, ObjectKind, name/slot } -> BackendHandle
```

Backend 不再持有 frontend `SharedPtr<T>`：只接收 `BackendHandle` 并通过
`BackendHost` 查询信息。Frontend 对象销毁时调用
`BackendVTable::OnObjectDestroyed` 显式通知。

## 5. Generation 机制

| frontend 现状 | BFA 字段 |
|---|---|
| `GetShapeVersion / GetContentVersion / GetTextureParamsVersion` | `TextureInfo.generation / contentGeneration / paramsGeneration` |
| `GetTextureBindGeneration / GetSamplingResolutionGeneration` | `TextureInfo.bindGeneration / samplingGeneration` |
| `GetRenderStateParametersVersion / GetPipelineStateVersion` | `RenderState.version / pipelineVersion` |
| `GetLinkVersion / GetBackendStateVersion / GetUniformWriteSetVersion / GetUBOContentVersion / GetBlockBindingVersion` | `ProgramInfo.*Generation` |
| `GetConfigVersion / GetAttributeVersion` | `VertexArrayInfo.generation / attribs[].generation` |
| `GetObjectVersion / GetAllFramebufferAttachmentVersions` | `FramebufferInfo.generation` |
| `SamplerObject::GetVersion` | `SamplerInfo.generation` |
| `GetTransformFeedbackGeneration` | `TransformFeedbackInfo.generation` |
| `GetTextureContextId` | SessionId 来源 |

Backend 缓存 key：

```text
{ SharedGroupId, ObjectHandle, objectGeneration }
```

约定：

- Generation 只在 FullServer 修改（单调递增，不回绕）；
- Backend 只比较、不推导；
- `generation` 变了就整级/整对象重传，v1 不做 dirty-rect 增量。

## 6. 跨层数据通路（只读快照，调用期有效）

| 数据 | Host 接口 | 生命周期 |
|---|---|---|
| Buffer shadow | `GetBufferShadow` | 调用期有效，Backend 必须 copy |
| Buffer range | `ReadBufferRange` | 同步拷贝 |
| Texture mip | `GetTextureMipInfo` + `GetTextureMipData` | 同步拷贝 |
| Program 模块 | `GetProgramModules` | 返回的 SPIR-V 指针调用期有效 |
| Persistent map | `BufferAcquirePersistentMap` | Backend 管理的 GPU 映射 |

## 7. 线程模型

- 同一 `(sessionId, clientThreadId)` 流严格 FIFO；
- 同一 session 的 backend 调用串行；
- 不同 session/thread 可并发，Host 查询线程安全；
- v1 禁止 backend 自建线程回调 Host。

## 8. C ABI 类型映射

| 语义 | C 类型 |
|---|---|
| DisplayId | `uint64_t` |
| SharedGroupId | `uint64_t` |
| SessionId | `uint64_t` |
| BackendHandle | `uint64_t` |
| Generation | `uint64_t` |
| Bool | `bool`（`stdbool.h`） |
| Size | `uint64_t` |
| 错误码 | `int32_t` + Host `RecordError` |

所有 BFA / RuntimeApi 契约头不包含任何 C++ 类型。
