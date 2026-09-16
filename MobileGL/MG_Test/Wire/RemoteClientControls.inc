// c1f: exercise installed client producers over a live inproc session.
// Peer substitutions supply adversarial INPUTS, never client state being asserted.
#if MGTEST_HAVE_FORK
namespace {
namespace Srv = MobileGL::MG_Remote::Server;
namespace Codec = MobileGL::MG_Remote::Wire;

// Test-only member access for replacing the peer at its existing interfaces. Explicit
// instantiations may name private members; no production test knob or ABI change is needed.
template<class Tag, typename Tag::Type member> struct PeerAccess {
    friend typename Tag::Type PeerMember(Tag) { return member; }
};
struct DecoderTag {
    using Type = Codec::PipeWireDecoder Srv::PipeApplier::*;
    friend Type PeerMember(DecoderTag);
};
template struct PeerAccess<DecoderTag, &Srv::PipeApplier::m_decoder>;
struct RepliesTag {
    using Type = Codec::ReplySink* Codec::PipeWireDecoder::*;
    friend Type PeerMember(RepliesTag);
};
template struct PeerAccess<RepliesTag, &Codec::PipeWireDecoder::m_replies>;
struct BackendTag {
    using Type = UniquePtr<MG_Backend::BackendObject> Srv::ServerLoop::*;
    friend Type PeerMember(BackendTag);
};
template struct PeerAccess<BackendTag, &Srv::ServerLoop::m_backend>;

void StartControlSession() {
    ::alarm(15);
    MG_Config::Transport = MG_Config::TransportMode::InProcess;
    Srv::ServerSessionInstance().SetCapabilityBits(0);
    Srv::ServerSessionInstance().SetConsumedSubsystems(kMGPipeSubsystemsMigratedAtP4a);
    if (ClientSessionInstance().Start(MG_Config::TransportMode::InProcess, {}) != MOBILEGL_OK)
        ::_exit(81);
}

struct ReadPeer : Codec::WireVerbSink {
    Int32 status = 0;
    Uint64 size = 48;
    Uint64 observed = 0;
    Bool OnReadPixels(const MGPReadbackInfo& info, Uint64 seq, Codec::ReplySink* replies) override {
        observed = info.DstSize;
        Uint8 pixels[48];
        std::memset(pixels, 0x37, sizeof(pixels));
        replies->PostReply(seq, status, pixels, size);
        return true;
    }
    void Install() {
        auto rc = Srv::ServerLoopInstance().RunOnApplyThread([](void* self) {
            auto& decoder = Srv::ServerSessionInstance().Applier().*PeerMember(DecoderTag{});
            decoder.SetVerbSink(static_cast<ReadPeer*>(self));
            return MOBILEGL_OK;
        }, this);
        if (rc != MOBILEGL_OK) ::_exit(82);
    }
};

struct ErrorPeer : Codec::ReplySink {
    Int32 status = 2;
    void PostReply(Uint64 seq, Int32, const void*, Uint64) override {
        // Keep the decoded request's sequence: this is an ERROR answer, not a wrong-slot test.
        Srv::ServerSessionInstance().Replies().PostReply(seq, status, nullptr, 0);
    }
    void Install() {
        auto rc = Srv::ServerLoopInstance().RunOnApplyThread([](void* self) {
            auto& decoder = Srv::ServerSessionInstance().Applier().*PeerMember(DecoderTag{});
            decoder.*PeerMember(RepliesTag{}) = static_cast<ErrorPeer*>(self);
            return MOBILEGL_OK;
        }, this);
        if (rc != MOBILEGL_OK) ::_exit(83);
    }
};

struct CapsPeer : MG_Backend::BackendObject {
    MG_Backend::DynamicBackendParameters dynamic{};
    MG_Backend::GlobalBackendFunctionsTable table{};
    RendererInfo renderer{};
    void Initialize() override {}
    Bool InitCapabilities() override { return true; }
    Bool InitWindowSurface() override { return true; }
    Bool InitPbufferSurface(EGLint, EGLint) override { return true; }
    const RendererInfo& GetRendererInfo() const override { return renderer; }
    String GetBackendAPIVersionString() const override { return "4.6"; }
    const MG_Backend::GlobalBackendFunctionsTable& GetBackendFunctions() const override { return table; }
    const MG_Backend::DynamicBackendParameters& GetDynamicParameters() const override { return dynamic; }
    BackendType GetBackendType() const override { return BackendType::DirectGLES; }
};

void ExpectNamedAbort(const ChildResult& child, const char* diagnostic) {
    EXPECT_TRUE(DiedOfAbort(child)) << DescribeStatus(child) << '\n' << child.Log;
    EXPECT_NE(child.Log.find(diagnostic), std::string::npos) << child.Log;
}
void ExpectChildSuccess(const ChildResult& child) {
    EXPECT_TRUE(WIFEXITED(child.Status) && WEXITSTATUS(child.Status) == 0)
        << DescribeStatus(child) << '\n' << child.Log;
}
}

#define C1F_TEARDOWN(Name, Call) \
TEST(RemoteClientControls, TeardownRefuses##Name##OnALiveSession) { \
    const auto child = RunInChild([] { \
        StartControlSession(); \
        UninstallClientWireTables(); \
        RemoteEmitTable().Call; \
    }); \
    ExpectNamedAbort(child, "Fatal{ClientTablesUninstalled, \"" #Name "\"}"); \
}
C1F_TEARDOWN(Clear, GL.Clear(0))
C1F_TEARDOWN(DrawArrays, GL.DrawArrays(0x0004, 0, 0))
C1F_TEARDOWN(ReadPixels, GL.ReadPixels(0, 0, 0, 0, 0x1908, 0x1401, nullptr))
C1F_TEARDOWN(BlitFramebuffer, GL.BlitFramebuffer(0,0,0,0,0,0,0,0,0,0x2600))
C1F_TEARDOWN(Present, Present())
#undef C1F_TEARDOWN

TEST(RemoteClientControls, ReadPixelsPutsTheTightExtentOnTheWire) {
    const auto child = RunInChild([] {
        StartControlSession();
        ReadPeer peer;
        peer.Install();
        Uint8 pixels[48]{};
        RemoteEmitTable().GL.ReadPixels(0, 0, 4, 3, 0x1908, 0x1401, pixels);
        if (peer.observed != 48 || pixels[47] != 0x37) ::_exit(84);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

TEST(RemoteClientControls, ShortOkReadPixelsReplyRefusesByName) {
    const auto child = RunInChild([] {
        StartControlSession();
        ReadPeer peer;
        peer.size = 32;
        peer.Install();
        Uint8 pixels[48]{};
        RemoteEmitTable().GL.ReadPixels(0, 0, 4, 3, 0x1908, 0x1401, pixels);
    });
    ExpectNamedAbort(child, "Fatal{ReadbackReplyShort, \"ReadPixels 32 < 48\"}");
}

TEST(RemoteClientControls, ErrorReadPixelsReplyRefusesByName) {
    const auto child = RunInChild([] {
        StartControlSession();
        ReadPeer peer;
        peer.status = 2;
        peer.size = 0;
        peer.Install();
        Uint8 pixels[48]{};
        RemoteEmitTable().GL.ReadPixels(0, 0, 4, 3, 0x1908, 0x1401, pixels);
    });
    ExpectNamedAbort(child, "Fatal{ReplyError, \"ReadPixels\"}");
}

TEST(RemoteClientControls, ResourceRespecifyErrorIsNotADecline) {
    const auto child = RunInChild([] {
        StartControlSession();
        ErrorPeer peer;
        peer.Install();
        MGPResourceDesc desc{};
        (void)MGPipeRouteResourceRespecify(desc, nullptr);
    });
    ExpectNamedAbort(child, "Fatal{ReplyError, \"resource_respecify\"}");
}

TEST(RemoteClientControls, MapPersistentErrorIsNotADecline) {
    const auto child = RunInChild([] {
        StartControlSession();
        ErrorPeer peer;
        peer.Install();
        MGPHandleOnly handle{};
        (void)MGPipeRouteMapPersistent(handle, 64, nullptr);
    });
    ExpectNamedAbort(child, "Fatal{ReplyError, \"map_persistent\"}");
}

TEST(RemoteClientControls, RepeatedMakeCurrentAdoptsRepublishedCapsWithoutAPumpOrPresent) {
    const auto child = RunInChild([] {
        StartControlSession();
        if (Srv::ServerLoopInstance().RunOnApplyThread([](void*) {
            auto backend = MakeUnique<CapsPeer>();
            backend->dynamic.MaxComputeWorkGroupCount[0] = 113;
            Srv::ServerSessionInstance().SetBackend(backend.get());
            Srv::ServerLoopInstance().*PeerMember(BackendTag{}) = Move(backend);
            return MOBILEGL_OK;
        }, nullptr) != MOBILEGL_OK) ::_exit(85);
        BackendObject_Remote remote;
        auto dpy = reinterpret_cast<EGLDisplay>(1);
        auto surface = reinterpret_cast<EGLSurface>(2);
        auto ctx = reinterpret_cast<EGLContext>(3);
        if (!remote.InitializeEGLDisplay(dpy, nullptr, nullptr) ||
            !remote.CreateEGLPbufferSurface(surface, 4, 4) ||
            !remote.MakeEGLCurrent(dpy, surface, surface, ctx)) ::_exit(86);
        const auto generation = CapsMirrorInstance().Generation();
        GLint before = 0;
        remote.GetBackendFunctions().GL.GetIntegeri_v(0x91BE, 0, &before);
        if (before != 113) ::_exit(87);
        if (Srv::ServerLoopInstance().RunOnApplyThread([](void*) {
            static_cast<CapsPeer*>(Srv::ServerLoopInstance().Backend())->dynamic.MaxComputeWorkGroupCount[0] = 227;
            return MOBILEGL_OK;
        }, nullptr) != MOBILEGL_OK) ::_exit(88);
        if (!remote.MakeEGLCurrent(dpy, surface, surface, ctx)) ::_exit(89);
        GLint after = 0;
        remote.GetBackendFunctions().GL.GetIntegeri_v(0x91BE, 0, &after);
        if (CapsMirrorInstance().Generation() <= generation || after != 227) ::_exit(90);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

TEST(RemoteClientControls, BounceReadPixelsShortReplyRefusesBeforeScatter) {
    const auto child = RunInChild([] {
        StartControlSession();
        MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
        MG_State::pGLContext->SetPixelStoreParam(PixelStoreParam::PackRowLength, 8);
        ReadPeer peer;
        peer.size = 32;
        peer.Install();
        Uint8 pixels[128]{};
        RemoteEmitTable().GL.ReadPixels(0, 0, 4, 3, 0x1908, 0x1401, pixels);
    });
    ExpectNamedAbort(child, "Fatal{ReadbackReplyShort, \"ReadPixels 32 < 48\"}");
}

TEST(RemoteClientControls, DeleteTexturesAdvancesTheWireOrdinal) {
    const auto child = RunInChild([] {
        CapsPeer backend;
        Srv::ServerSessionInstance().SetBackend(&backend);
        StartControlSession();
        ErrorPeer replies;
        replies.status = 0; // The peer accepts the resource; the client must publish and delete it.
        replies.Install();
        MG_Config::Features.PipePush |= kMGPipeSubsystemResources | kMGPipeSubsystemTextureResources |
                                        kMGPipeSubsystemSamplers;
        MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
        GLuint texture = 0;
        MG_Impl::GLImpl::GenTextures(1, &texture);
        MG_Impl::GLImpl::BindTexture(0x0DE1, texture);
        MG_Impl::GLImpl::TexImage2D(0x0DE1, 0, 0x8058, 1, 1, 0, 0x1908, 0x1401, nullptr);
        const auto before = ClientWireRecordsEmitted();
        MG_Impl::GLImpl::DeleteTextures(1, &texture);
        const auto after = ClientWireRecordsEmitted();
        if (before == 0 || after <= before) ::_exit(91);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

TEST(RemoteClientControls, BoundPackBufferOffsetReadRefusesByName) {
    const auto child = RunInChild([] {
        StartControlSession();
        MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
        GLuint buffer = 0;
        MG_Impl::GLImpl::GenBuffers(1, &buffer);
        MG_Impl::GLImpl::BindBuffer(0x88EB, buffer);
        MG_Impl::GLImpl::BufferData(0x88EB, 64, nullptr, 0x88E1);
        RemoteEmitTable().GL.ReadPixels(0, 0, 1, 1, 0x1908, 0x1401, reinterpret_cast<void*>(16));
    });
    ExpectNamedAbort(child, "Fatal{UnmigratedVerb, \"ReadPixels+PACK_BUFFER\"}");
}

TEST(RemoteClientControls, ServerRoleRespecifyDoesNotEmitClientInitialBytes) {
    const auto child = RunInChild([] {
        CapsPeer backend;
        Srv::ServerSessionInstance().SetBackend(&backend);
        StartControlSession();
        MG_Config::Features.PipePush |= kMGPipeSubsystemResources;
        const auto before = ClientWireRecordsEmitted();
        if (Srv::ServerLoopInstance().RunOnApplyThread([](void*) {
            MG_State::GLState::BufferObject buffer(701);
            Uint8 data[64]{};
            buffer.Respecify(sizeof(data), data);
            return MOBILEGL_OK;
        }, nullptr) != MOBILEGL_OK) ::_exit(92);
        if (ClientWireRecordsEmitted() != before) ::_exit(93);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

TEST(RemoteClientControls, HeaderOnlyDynamicStateCrossesWithoutBlobMissing) {
    const auto child = RunInChild([] {
        StartControlSession();
        MGPDynamicState record{};
        record.ChunkMask = 0;
        const auto before = ClientWireRecordsEmitted();
        gMGPipeContext.SetDynamicState(&record, nullptr, 0);
        if (ClientWireRecordsEmitted() != before + 1) ::_exit(94);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

TEST(RemoteClientControls, ServerRoleFlushDoesNotRunTheClientSubDataFollowup) {
    const auto child = RunInChild([] {
        CapsPeer backend;
        Srv::ServerSessionInstance().SetBackend(&backend);
        StartControlSession();
        MG_Config::Features.PipePush |= kMGPipeSubsystemResources;
        const auto rc = Srv::ServerLoopInstance().RunOnApplyThread([](void*) {
            static int subdata = 0, flushes = 0;
            MGPipeResourceOps ops{};
            ops.Create = [](MGPipeHandle, const MGPResourceDesc&) {};
            ops.Respecify = [](MGPipeHandle, const MGPResourceDesc&, const void*) {};
            ops.SubData = [](MGPipeHandle, const MGPSubData&, const void*) { ++subdata; };
            ops.FlushRange = [](MGPipeHandle, const MGPFlushRange&, const void*) { ++flushes; };
            ops.Destroy = [](MGPipeHandle) {};
            MGPipeSetResourceOps(&ops);
            {
                MG_State::GLState::BufferObject buffer(702);
                buffer.Resize(64);
                subdata = flushes = 0;
                MGPipeEmitResourceFlushRange(buffer, 16, 16, 0);
                if (subdata != 0 || flushes != 1) ::_exit(95);
            }
            MGPipeSetResourceOps(nullptr);
            return MOBILEGL_OK;
        }, nullptr);
        if (rc != MOBILEGL_OK) ::_exit(96);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}
// Uses the shipped EGL/GL frontend and real DirectGLES driver. Invoked explicitly by the
// package gate because a native driver is not a prerequisite of the unit lane.
int RunPackGpuControl(bool split) {
    const auto child = RunInChild([=] {
        ::alarm(45);
        setenv("EGL_PLATFORM", "surfaceless", 1);
        setenv("MOBILEGL_BACKEND_TYPE", "DirectGLES", 1);
        setenv("MOBILEGL_TRANSPORT", split ? "inproc" : "monolith", 1);
        MobileGL::Initialize();
        namespace E = MG_Impl::EGLImpl;
        namespace G = MG_Impl::GLImpl;
        const auto display = E::GetDisplay(EGL_DEFAULT_DISPLAY);
        if (!E::Initialize(display, nullptr, nullptr)) ::_exit(101);
        E::BindAPI(EGL_OPENGL_API);
        EGLConfig config{};
        EGLint count = 0;
        const EGLint configAttribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
        if (!E::ChooseConfig(display, configAttribs, &config, 1, &count) || count < 1) ::_exit(102);
        const auto context = E::CreateContext(display, config, EGL_NO_CONTEXT, nullptr);
        const EGLint surfaceAttribs[] = {EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE};
        const auto surface = E::CreatePbufferSurface(display, config, surfaceAttribs);
        if (!E::MakeCurrent(display, surface, surface, context)) ::_exit(103);
        G::ClearColor(0, 1, 0, 1);
        G::Clear(GL_COLOR_BUFFER_BIT);
        GLuint pbo = 0;
        G::GenBuffers(1, &pbo);
        G::BindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
        G::BufferData(GL_PIXEL_PACK_BUFFER, 64, nullptr, GL_STREAM_READ);
        G::ReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, reinterpret_cast<void*>(16));
        if (split) ::_exit(104); // Must have refused by name, never returned or dereferenced 16.
        Uint8 pixels[4]{};
        G::GetBufferSubData(GL_PIXEL_PACK_BUFFER, 16, 4, pixels);
        if (pixels[0] != 0 || pixels[1] != 255 || pixels[2] != 0 || pixels[3] != 255) ::_exit(105);
        std::printf("C1F_PACK_MONOLITH bytes at offset 16: %u %u %u %u\n", pixels[0], pixels[1], pixels[2], pixels[3]);
        std::fflush(stdout);
    });
    const bool ok = split
        ? DiedOfAbort(child) && child.Log.find("Fatal{UnmigratedVerb, \"ReadPixels+PACK_BUFFER\"}") != std::string::npos
        : WIFEXITED(child.Status) && WEXITSTATUS(child.Status) == 0;
    std::printf("C1F_PACK_%s: %s (%s)\n%s", split ? "INPROC" : "MONOLITH",
                ok ? "PASS" : "FAIL", DescribeStatus(child).c_str(), child.Log.c_str());
    return ok ? 0 : 1;
}
#endif
