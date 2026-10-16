# a6 item 5 - link experiment raw data

Head b3b9c2ee. Build: cmake -DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON -DMOBILEGL_PIPE_PUSH=ON, Release, clang 22.1.6, target MobileGL.
Method: nm over the per-object output of the single MobileGL target. No CMake edit (there is no module target to edit - see the report).

SERVER set    = MG_Backend/** + MG_Pipe/** + MG_Remote/{Transport,Protocol,Wire,Server}/** + MG_Remote/CapsCodec.o   (45 objects)
FRONTEND set  = MG_Impl/** + MG_State/** + MG_Remote/Client/**                                                      (71 objects)
SHARED set    = MG_Util/** + Init/GlobalObjects/ConfigLoader                                                        (87 objects)

server undefined symbols total          : 1486
  not satisfied by SERVER or SHARED     : 552
  OF WHICH defined by FRONTEND          : 184

## The 184, demangled, grouped by defining file

### MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.cpp.o
  - MobileGL::MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo

### MG_Impl/GLImpl/Texture/GL_Texture.cpp.o
  - MobileGL::MG_Impl::GLImpl::CopyTextureImageToClientOrPBO_State(std::shared_ptr<MobileGL::MG_State::GLState::ITextureObject> const&, MobileGL::TextureUploadTarget, int, unsigned int, unsigned int, int, void*, char const*)

### MG_Impl/Pipe/PipeFill.cpp.o
  - MobileGL::MG_Pipe::MGPipeNoteAggregate(MobileGL::MG_Pipe::MGPipeAggregate)
  - MobileGL::MG_Pipe::PipeInputs::GetBufferBindingPointCount(MobileGL::BufferTarget) const
  - MobileGL::MG_Pipe::PipeInputs::GetProgramObject(unsigned int)
  - MobileGL::MG_Pipe::PipeInputs::GetTextureObject(unsigned int)
  - MobileGL::MG_Pipe::PipeInputs::HasOpenTransformFeedbackSpan(unsigned long) const
  - MobileGL::MG_Pipe::PipeInputs::InvalidateCompileEnv()
  - MobileGL::MG_Pipe::PipeInputs::IsLive() const
  - MobileGL::MG_Pipe::PipeInputs::RecordError(MobileGL::ErrorCode, std::unique_ptr<MobileGL::ErrorInfo, std::default_delete<MobileGL::ErrorInfo> >)
  - MobileGL::MG_Pipe::PipeInputs::ValidateProgramName(unsigned int) const

### MG_Impl/Pipe/SlotAllocator.cpp.o
  - MobileGL::MG_Pipe::MGPipeApplierIsUnbarrieredApply()
  - MobileGL::MG_Pipe::MGPipeRefuseAllocatorFromApplyThread(char const*)
  - MobileGL::MG_Pipe::MGPipeRefuseFrontendKeyedRegistryFromApplyThread(char const*)
  - MobileGL::MG_Pipe::MGPipeSlotAllocator::Acquire(MobileGL::MG_Pipe::MGPipeKind, unsigned long)
  - MobileGL::MG_Pipe::MGPipeSlotAllocator::FindByLifetimeId(MobileGL::MG_Pipe::MGPipeKind, unsigned long) const
  - MobileGL::MG_Pipe::MGPipeSlotAllocator::Free(MobileGL::MG_Pipe::MGPipeKind, MobileGL::MG_Pipe::MGPipeHandle)
  - MobileGL::MG_Pipe::MGPipeSlots()

### MG_Remote/Client/BackendObject_Remote.cpp.o
  - MobileGL::MG_Remote::Client::BackendObject_Remote::BackendObject_Remote()

### MG_Remote/Client/ClientSession.cpp.o
  - MobileGL::MG_Remote::Client::ClientSession::Active()
  - MobileGL::MG_Remote::Client::ClientSessionInstance()
  - MobileGL::MG_Remote::Client::ClientSession::NoteApplyThreadEnteredApplier()
  - MobileGL::MG_Remote::Client::ClientSession::NoteApplyThreadLeftApplier()
  - MobileGL::MG_Remote::Client::ClientSession::Start(MobileGL::MG_Config::TransportMode, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&)
  - MobileGL::MG_Remote::Client::ClientSession::Stop()

### MG_Remote/Client/PersistentMapTracker.cpp.o
  - MobileGL::MG_Remote::Client::AdoptTierIsEmulate()

### MG_Remote/Client/WireTables.cpp.o
  - MobileGL::MG_Remote::Client::EmitObjectDeathRecord(MobileGL::MG_Pipe::MGPipeKind, unsigned long)

### MG_State/GLState/BufferState/BufferObject.cpp.o
  - MobileGL::MG_State::GLState::BufferObject::DownloadSubData(void*, unsigned long, unsigned long) const
  - MobileGL::MG_State::GLState::BufferObject::EnsureGpuResidentStorage()
  - MobileGL::MG_State::GLState::BufferObject::GetBackendResource() const
  - MobileGL::MG_State::GLState::BufferObject::GetChangeSerial() const
  - MobileGL::MG_State::GLState::BufferObject::GetExternalIndex() const
  - MobileGL::MG_State::GLState::BufferObject::GetSize() const
  - MobileGL::MG_State::GLState::BufferObject::GetUsage() const
  - MobileGL::MG_State::GLState::BufferObject::HasDefinedContent() const
  - MobileGL::MG_State::GLState::BufferObject::IsBackendPersistentMapped() const
  - MobileGL::MG_State::GLState::BufferObject::IsMapped() const
  - MobileGL::MG_State::GLState::BufferObject::MappedData() const
  - MobileGL::MG_State::GLState::BufferObject::MarkGpuWritten()
  - MobileGL::MG_State::GLState::BufferObject::SetBackendResource(std::shared_ptr<MobileGL::MG_State::GLState::BackendBufferResource>)
  - MobileGL::MG_State::GLState::BufferObject::SyncGpuWrites()
  - MobileGL::MG_State::GLState::BufferObject::SyncPersistentMappedRange()
  - MobileGL::MG_State::GLState::BufferObject::WritebackFromBackend(MobileGL::DataPtr, unsigned long)
  - MobileGL::MG_State::GLState::GetBufferBackendOps()
  - MobileGL::MG_State::GLState::SetBufferBackendOps(MobileGL::MG_State::GLState::BufferBackendOps const*)

### MG_State/GLState/Core.cpp.o
  - MobileGL::MG_State::GLState::ClassifyVertexAttribType(unsigned int)

### MG_State/GLState/FramebufferState/FramebufferObject.cpp.o
  - MobileGL::MG_State::GLState::FramebufferAttachmentObject::GetRenderbuffer() const
  - MobileGL::MG_State::GLState::FramebufferAttachmentObject::GetSize() const
  - MobileGL::MG_State::GLState::FramebufferAttachmentObject::GetTexture() const
  - MobileGL::MG_State::GLState::FramebufferAttachmentObject::GetTextureLayer() const
  - MobileGL::MG_State::GLState::FramebufferAttachmentObject::GetTextureLevel() const
  - MobileGL::MG_State::GLState::FramebufferAttachmentObject::GetTextureUploadTarget() const
  - MobileGL::MG_State::GLState::FramebufferAttachmentObject::IsComplete() const
  - MobileGL::MG_State::GLState::FramebufferAttachmentObject::IsEmpty() const
  - MobileGL::MG_State::GLState::FramebufferAttachmentObject::IsLayered() const
  - MobileGL::MG_State::GLState::FramebufferAttachmentObject::IsRenderbuffer() const
  - MobileGL::MG_State::GLState::FramebufferAttachmentObject::IsTexture() const
  - MobileGL::MG_State::GLState::FramebufferAttachmentObject::IsValid() const
  - MobileGL::MG_State::GLState::FramebufferObject::GetAllAttachmentObjects() const
  - MobileGL::MG_State::GLState::FramebufferObject::GetAttachment(MobileGL::FramebufferAttachmentType) const
  - MobileGL::MG_State::GLState::FramebufferObject::GetDrawBuffers() const
  - MobileGL::MG_State::GLState::FramebufferObject::GetExternalIndex() const

### MG_State/GLState/ProgramState/ProgramArtifactsCodec.cpp.o
  - MobileGL::MG_State::GLState::DecodeProgramArchive(unsigned char const*, unsigned long, MobileGL::MG_State::GLState::ProgramArchive&)

### MG_State/GLState/ProgramState/ProgramObject.cpp.o
  - MobileGL::MG_State::GLState::ProgramObject::AllocateLifetimeId()
  - MobileGL::MG_State::GLState::ProgramObject::AttachShader(std::shared_ptr<MobileGL::MG_State::GLState::ShaderObject> const&)
  - MobileGL::MG_State::GLState::ProgramObject::JoinPendingLink() const
  - MobileGL::MG_State::GLState::ProgramObject::JoinPendingSpirv() const
  - MobileGL::MG_State::GLState::ProgramObject::Link(bool)
  - MobileGL::MG_State::GLState::ProgramObject::~ProgramObject()

### MG_State/GLState/ProgramState/ShaderObject.cpp.o
  - MobileGL::MG_State::GLState::ShaderObject::Compile()
  - MobileGL::MG_State::GLState::ShaderObject::DropCompileNode() const
  - MobileGL::MG_State::GLState::ShaderObject::JoinPendingCompile() const
  - MobileGL::MG_State::GLState::ShaderObject::ReleaseCompileNode()
  - MobileGL::MG_State::GLState::ShaderObject::SetShaderSource(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&&)

### MG_State/GLState/RenderbufferState/RenderbufferObject.cpp.o
  - MobileGL::MG_State::GLState::RenderbufferObject::GetExternalIndex() const
  - MobileGL::MG_State::GLState::RenderbufferObject::GetHeight() const
  - MobileGL::MG_State::GLState::RenderbufferObject::GetInternalFormat() const
  - MobileGL::MG_State::GLState::RenderbufferObject::GetSamples() const
  - MobileGL::MG_State::GLState::RenderbufferObject::GetWidth() const
  - MobileGL::MG_State::GLState::RenderbufferObject::IsAllocated() const

### MG_State/GLState/SamplerState/SamplerObject.cpp.o
  - MobileGL::MG_State::GLState::SamplerObject::GetAllSamplerParameters() const
  - MobileGL::MG_State::GLState::SamplerObject::GetBorderColor() const
  - MobileGL::MG_State::GLState::SamplerObject::GetBorderColorI() const
  - MobileGL::MG_State::GLState::SamplerObject::GetBorderColorUI() const
  - MobileGL::MG_State::GLState::SamplerObject::GetCompareMode() const
  - MobileGL::MG_State::GLState::SamplerObject::GetExternalIndex() const
  - MobileGL::MG_State::GLState::SamplerObject::GetLifetimeId() const
  - MobileGL::MG_State::GLState::SamplerObject::GetLodBias() const
  - MobileGL::MG_State::GLState::SamplerObject::GetMagFilter() const
  - MobileGL::MG_State::GLState::SamplerObject::GetMaxAnisotropy() const
  - MobileGL::MG_State::GLState::SamplerObject::GetMaxLod() const
  - MobileGL::MG_State::GLState::SamplerObject::GetMinFilter() const
  - MobileGL::MG_State::GLState::SamplerObject::GetMinLod() const
  - MobileGL::MG_State::GLState::SamplerObject::GetMipmapMode() const
  - MobileGL::MG_State::GLState::SamplerObject::GetSamplerCompareFunc() const
  - MobileGL::MG_State::GLState::SamplerObject::GetVersion() const
  - MobileGL::MG_State::GLState::SamplerObject::GetWrapR() const
  - MobileGL::MG_State::GLState::SamplerObject::GetWrapS() const
  - MobileGL::MG_State::GLState::SamplerObject::GetWrapT() const
  - MobileGL::MG_State::GLState::SamplerObject::~SamplerObject()
  - MobileGL::MG_State::GLState::SamplerObject::SamplerObject(unsigned int)
  - MobileGL::MG_State::GLState::SamplerObject::SetCompareMode(MobileGL::SamplerCompareMode)
  - MobileGL::MG_State::GLState::SamplerObject::SetLodRange(float, float)
  - MobileGL::MG_State::GLState::SamplerObject::SetMagFilter(MobileGL::SamplerFilterMode)
  - MobileGL::MG_State::GLState::SamplerObject::SetMinFilter(MobileGL::SamplerFilterMode)
  - MobileGL::MG_State::GLState::SamplerObject::SetMipmapMode(MobileGL::SamplerMipmapMode)
  - MobileGL::MG_State::GLState::SamplerObject::SetSamplerCompareFunc(MobileGL::SamplerCompareFunc)
  - MobileGL::MG_State::GLState::SamplerObject::SetWrapR(MobileGL::SamplerWrapMode)
  - MobileGL::MG_State::GLState::SamplerObject::SetWrapS(MobileGL::SamplerWrapMode)
  - MobileGL::MG_State::GLState::SamplerObject::SetWrapT(MobileGL::SamplerWrapMode)

### MG_State/GLState/TextureState/MipmapStorage.cpp.o
  - MobileGL::MG_State::GLState::MGPipeTextureLegacyArmScope::~MGPipeTextureLegacyArmScope()
  - MobileGL::MG_State::GLState::MGPipeTextureLegacyArmScope::MGPipeTextureLegacyArmScope()

### MG_State/GLState/TextureState/TextureObject1D.cpp.o
  - MobileGL::MG_State::GLState::TextureObject1D::TextureObject1D(unsigned int)

### MG_State/GLState/TextureState/TextureObject2D.cpp.o
  - MobileGL::MG_State::GLState::TextureObject2D::TextureObject2D(unsigned int)

### MG_State/GLState/TextureState/TextureObject2DCube.cpp.o
  - MobileGL::MG_State::GLState::TextureObject2DCube::TextureObject2DCube(unsigned int)

### MG_State/GLState/TextureState/TextureObject3D.cpp.o
  - MobileGL::MG_State::GLState::TextureObject3D::TextureObject3D(unsigned int)

### MG_State/GLState/TextureState/TextureObjectBuffer.cpp.o
  - MobileGL::MG_State::GLState::TextureObjectBuffer::GetBufferBindingSlot(MobileGL::TextureUploadTarget)

### MG_State/GLState/TextureState/TextureObject.cpp.o
  - MobileGL::MG_State::GLState::SamplesAsIncompleteTexture(MobileGL::MG_State::GLState::ITextureObject const*, MobileGL::MG_State::GLState::SamplerObject const*)
  - MobileGL::MG_State::GLState::TextureObjectBase::GetAllSwizzleParams() const
  - MobileGL::MG_State::GLState::TextureObjectBase::GetBorderColor() const
  - MobileGL::MG_State::GLState::TextureObjectBase::GetBorderColorForm() const
  - MobileGL::MG_State::GLState::TextureObjectBase::GetBorderColorI() const
  - MobileGL::MG_State::GLState::TextureObjectBase::GetBorderColorUI() const
  - MobileGL::MG_State::GLState::TextureObjectBase::GetContentVersion() const
  - MobileGL::MG_State::GLState::TextureObjectBase::GetExternalIndex() const
  - MobileGL::MG_State::GLState::TextureObjectBase::GetFormat() const
  - MobileGL::MG_State::GLState::TextureObjectBase::GetImmutableLevels() const
  - MobileGL::MG_State::GLState::TextureObjectBase::GetLevelRange() const
  - MobileGL::MG_State::GLState::TextureObjectBase::GetLifetimeId() const
  - MobileGL::MG_State::GLState::TextureObjectBase::GetSamplerObject() const
  - MobileGL::MG_State::GLState::TextureObjectBase::GetSamples() const
  - MobileGL::MG_State::GLState::TextureObjectBase::GetShapeVersion() const
  - MobileGL::MG_State::GLState::TextureObjectBase::GetSwizzleParam(MobileGL::TextureSwizzleParam) const
  - MobileGL::MG_State::GLState::TextureObjectBase::GetTarget() const
  - MobileGL::MG_State::GLState::TextureObjectBase::GetTextureParamsVersion() const
  - MobileGL::MG_State::GLState::TextureObjectBase::GetViewStorageOwner() const
  - MobileGL::MG_State::GLState::TextureObjectBase::HasFixedSampleLocations() const
  - MobileGL::MG_State::GLState::TextureObjectBase::IsImmutable() const
  - MobileGL::MG_State::GLState::TextureObjectBase::IsMipmapCompleteForFilterCached(bool) const
  - MobileGL::MG_State::GLState::TextureObjectBase::PipePublishParams()
  - MobileGL::MG_State::GLState::TextureObjectBase::SetBaseLevel(unsigned int)
  - MobileGL::MG_State::GLState::TextureObjectBase::SetBorderColorI(MobileGL::Vec4<int> const&)
  - MobileGL::MG_State::GLState::TextureObjectBase::SetBorderColor(MobileGL::Vec4<float> const&)
  - MobileGL::MG_State::GLState::TextureObjectBase::SetBorderColorUI(MobileGL::Vec4<unsigned int> const&)
  - MobileGL::MG_State::GLState::TextureObjectBase::SetFixedSampleLocations(bool)
  - MobileGL::MG_State::GLState::TextureObjectBase::SetImmutableLevels(unsigned int)
  - MobileGL::MG_State::GLState::TextureObjectBase::SetInternalFormat(MobileGL::TextureInternalFormat)
  - MobileGL::MG_State::GLState::TextureObjectBase::SetMaxLevel(unsigned int)
  - MobileGL::MG_State::GLState::TextureObjectBase::SetSamples(int)
  - MobileGL::MG_State::GLState::TextureObjectBase::SetSwizzleParam(MobileGL::TextureSwizzleParam, MobileGL::TextureSwizzleParam)
  - MobileGL::MG_State::GLState::TextureObjectBase::SetSwizzleParamRGBA(MobileGL::Vec4<MobileGL::TextureSwizzleParam> const&)
  - MobileGL::MG_State::GLState::TextureObjectBase::~TextureObjectBase()
  - MobileGL::MG_State::GLState::TextureObjectBase::TextureObjectBase(MobileGL::TextureTarget, unsigned int)
  - MobileGL::MG_State::GLState::TextureObjectWithOneMipmap::AllocateStorage(MobileGL::TextureUploadTarget, unsigned int, MobileGL::MG_State::GLState::MipmapInput)
  - MobileGL::MG_State::GLState::TextureObjectWithOneMipmap::GetBaseSize() const
  - MobileGL::MG_State::GLState::TextureObjectWithOneMipmap::GetMipmapByteSize(MobileGL::TextureUploadTarget, unsigned int) const
  - MobileGL::MG_State::GLState::TextureObjectWithOneMipmap::GetMipmapCompressedByteSize(MobileGL::TextureUploadTarget, unsigned int) const
  - MobileGL::MG_State::GLState::TextureObjectWithOneMipmap::GetMipmapCompressedFormat(MobileGL::TextureUploadTarget, unsigned int) const
  - MobileGL::MG_State::GLState::TextureObjectWithOneMipmap::GetMipmapLevelCount() const
  - MobileGL::MG_State::GLState::TextureObjectWithOneMipmap::GetMipmapRequestedCompressedFormat(MobileGL::TextureUploadTarget, unsigned int) const
  - MobileGL::MG_State::GLState::TextureObjectWithOneMipmap::GetMipmapTexelSize(MobileGL::TextureUploadTarget, unsigned int) const
  - MobileGL::MG_State::GLState::TextureObjectWithOneMipmap::GetStorageDirtyRects(MobileGL::TextureUploadTarget, unsigned int, MobileGL::MG_State::GLState::MipmapDirtyRegion*, unsigned long) const
  - MobileGL::MG_State::GLState::TextureObjectWithOneMipmap::GetStorageDirtyRegion(MobileGL::TextureUploadTarget, unsigned int) const
  - MobileGL::MG_State::GLState::TextureObjectWithOneMipmap::IsComplete() const
  - MobileGL::MG_State::GLState::TextureObjectWithOneMipmap::IsStorageDirty(MobileGL::TextureUploadTarget, unsigned int) const
  - MobileGL::MG_State::GLState::TextureObjectWithOneMipmap::MapMipmapCompressedImage(MobileGL::TextureUploadTarget, unsigned int) const
  - MobileGL::MG_State::GLState::TextureObjectWithOneMipmap::MapMipmapData(MobileGL::TextureUploadTarget, unsigned int)
  - MobileGL::MG_State::GLState::TextureObjectWithOneMipmap::MarkStorageDirty(MobileGL::TextureUploadTarget, unsigned int, bool)
  - MobileGL::MG_State::GLState::TextureObjectWithOneMipmap::MarkStorageDirtyRegion(MobileGL::TextureUploadTarget, unsigned int, MobileGL::Vec3<int>, MobileGL::Vec3<int>)
  - MobileGL::MG_State::GLState::TextureObjectWithOneMipmap::SetMipmapCompressedImage(MobileGL::TextureUploadTarget, unsigned int, unsigned int, void const*, unsigned long)
  - MobileGL::MG_State::GLState::TextureObjectWithOneMipmap::SetMipmapRequestedCompressedFormat(MobileGL::TextureUploadTarget, unsigned int, unsigned int)
  - MobileGL::MG_State::GLState::TextureObjectWithOneMipmap::TruncateMipmapLevels(MobileGL::TextureUploadTarget, unsigned int)
  - MobileGL::MG_State::GLState::TextureObjectWithOneMipmap::UpdateMipmapSubData(MobileGL::TextureUploadTarget, unsigned int, MobileGL::DataPtr)
  - typeinfo for MobileGL::MG_State::GLState::TextureObjectBase
  - typeinfo for MobileGL::MG_State::GLState::TextureObjectWithOneMipmap
  - vtable for MobileGL::MG_State::GLState::TextureObjectWithOneMipmap

### MG_State/GLState/TextureState/TextureUnit.cpp.o
  - MobileGL::MG_State::GLState::TextureUnit::GetAllBindingSlots()
  - MobileGL::MG_State::GLState::TextureUnit::GetBindingSlot(MobileGL::TextureTarget)
  - MobileGL::MG_State::GLState::TextureUnit::GetSamplerObject() const

### MG_State/GLState/VertexArrayState/VertexArrayObject.cpp.o
  - MobileGL::MG_State::GLState::VertexArrayObject::GetAllAttributes() const
  - MobileGL::MG_State::GLState::VertexArrayObject::GetAllAttributeVersions() const
  - MobileGL::MG_State::GLState::VertexArrayObject::GetAttribute(unsigned int) const
  - MobileGL::MG_State::GLState::VertexArrayObject::GetIndexBufferBindingSlot()
  - MobileGL::MG_State::GLState::VertexArrayObject::GetIndexBufferBindingSlot() const

