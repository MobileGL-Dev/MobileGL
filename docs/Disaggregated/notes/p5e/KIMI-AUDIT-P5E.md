# Read-only audit brief (Kimi): every apply-thread read of client-owned memory on the DirectGLES inproc draw path

Repository: the current directory (MobileGL, branch `feat/disaggregated`, head `2fde7034`). READ-ONLY:
do not edit any file, do not build, do not run tests, do not touch adb or WSL. Use your file reading
and grep tools only. Write the report file named at the end BEFORE your final message.

## Context
MobileGL's split (`MOBILEGL_TRANSPORT=inproc`) runs the GL frontend on the application's GL thread
("client role") and the backend (`MobileGL/MG_Backend/DirectGLES`) on an apply thread ("server role",
`MobileGL/MG_Remote/Server/ServerLoop.cpp`, records applied by `MobileGL/MG_Remote/Server/PipeApplier.cpp`).
Today every draw/clear/blit/dispatch waits for the apply thread (`MobileGL/MG_Remote/Client/ClientSession.cpp`,
`EmitAndWaitTails`, the R-1 barrier). The next phase (P5e) wants the client to publish those records
and continue WITHOUT waiting. That is only safe if, while applying a record, the apply thread reads
nothing that the GL thread may be mutating at the same time.

Client-owned memory = anything reachable from `MG_State::pGLContext` / the `GLContext` object (binding
tables, `TextureUnit` arrays, `BindingSlot`s, `VertexArrayObject` attributes, `ProgramObject` internals,
`FramebufferObject` attachments, `BufferObject` shadow bytes), the frontend objects those slots hold
(`ITextureObject`, `BufferObject`, `SamplerObject`, ...), the client slot allocator `MGPipeSlots()`
(`MobileGL/MG_Impl/Pipe/SlotAllocator.*`, probed via `HandleOf`/`FindByLifetimeId`/`GetOrCreate(const StatePtr&)`
in `MobileGL/MG_Backend/DirectGLES/SlotTables.h` and `HandleOfBuffer` in `Managers.cpp`), and the
BARRIER_PULLED rows of `gPipeInputs` (`MobileGL/MG_Pipe/FieldOwnership.def`, storage
`MobileGL/MG_Backend/MGPipe/PipeInputs.h`, read through `MGB_CTX->...`). Server-owned memory =
`MG_Pipe::MGPipeApplier()` state, the twin objects in the DirectGLES registries, the server staged
stores (`ServerStaged()`, `ServerStagedTexture()`), records and `SEG_STAGE` bytes.

## Task
Starting from the record sinks in `MobileGL/MG_Remote/Server/PipeApplier.cpp` (`ServerVerbSink::OnDrawVbo`,
`OnClear`, `OnBlit...`, `OnDispatch...`, `OnGenerateMipmap`, `OnCopyFramebufferToTexture`, and every
`MGPipeApply*` record apply in `MobileGL/MG_Pipe/PipeApply.cpp`), follow the calls into
`MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp` and `Managers.cpp` (PrepareForDraw, PrepareForCompute,
SyncNeccessaryBuffers, SyncVaoAttributeBuffersByHandle, SyncNeccessaryTextures, SyncCurrentProgram,
BindCurrentProgramWithResources, BindCurrentTextures, SyncCurrentFBO, BindCurrentFBO,
SyncImageTextureBindingsForDraw, SyncBufferBindingPoints, Clear, BlitFramebuffer, GenerateMipmap,
EnsureBufferResource*, IsBufferDrawClean*, and everything they call) and list EVERY site that reads
client-owned memory, with the arm it is on when the transport is active (some sites have a record
arm and a legacy arm; say which runs under `MG_Config::Transport != Monolith` with all
`MOBILEGL_PIPE_PUSH` subsystem bits set).

For each site: `file:line`, the function, what is read (which client object/field/table), the kind
(live dereference / lifetime-id + allocator probe / allocator mint / `gPipeInputs` BARRIER_PULLED row),
whether it runs on every draw or only on a memo miss / rare path, and which pushed record or applier
state field already carries the same information (look in `MobileGL/MG_Pipe/PipeApply.h`
`MGPipeApplierState`, `MobileGL/MG_Pipe/MGPipeTypes.h`, `MobileGL/MG_Pipe/PipeCalls.def`) or "none".

Also list: (a) every `MGPipeFrontendKeyedRegistryScope` / `MGPipeReverseAnnouncementScope` construction
site and what it wraps; (b) every place the apply thread holds a `SharedPtr` to a frontend object
across records (memos, sync lists); (c) every `MGB_CTX->` accessor used from `MG_Backend/DirectGLES`
whose row in `FieldOwnership.def` is BARRIER_PULLED.

Output: a markdown table per family (VAO/vertex, buffers, textures/samplers, framebuffers/attachments,
images, programs/shader state, binding-point tables, XFB, misc), then the three lists, then a short
"what would have to change for the apply thread to read only server-owned memory" per family
(one paragraph each, no code). No process narration. <= 400 lines. Every `file:line` must resolve
at head `2fde7034`.

Write the report to: `C:/Users/geekerwan/AppData/Local/Temp/claude/C--Users-geekerwan-AndroidStudioProjects-FoldCraftLauncher-MobileGL-disagg/5007453d-cb19-4ebf-96a3-60ba4fb55b47/scratchpad/p5e/kimi-audit.md`
