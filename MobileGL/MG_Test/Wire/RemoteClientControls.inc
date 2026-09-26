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
struct ProducerBellTag {
    using Type = Transport::Doorbell* Transport::SessionProducer::*;
    friend Type PeerMember(ProducerBellTag);
};
template struct PeerAccess<ProducerBellTag, &Transport::SessionProducer::m_selfBell>;

// Observe the actual transport park selected by EmitAndWait. The peer waits until
// the client parks, so no GPU or wall-clock 60-second sleep is required.
struct BudgetBell : Transport::Doorbell {
    Transport::Doorbell* original;
    std::atomic<Uint32> firstMs{0};
    std::atomic<Bool> entered{false};
    Bool shutDown = false;
    explicit BudgetBell(Transport::Doorbell* bell) : original(bell) {}
    void Notify() override { original->Notify(); }
    void Reset() override { original->Reset(); }
    bool Park(std::uint32_t timeoutMs) override {
        if (!entered.load()) {
            firstMs.store(timeoutMs);
            entered.store(true);
        }
        return shutDown ? false : original->Park(timeoutMs);
    }
    bool Dead() const override { return (shutDown && entered.load()) || original->Dead(); }
};

struct FenceBudgetPeer : Codec::WireVerbSink {
    BudgetBell& bell;
    std::atomic<Bool> release{false};
    explicit FenceBudgetPeer(BudgetBell& value) : bell(value) {}
    void AwaitPark() {
        while (!bell.entered.load() || (bell.shutDown && !release.load())) std::this_thread::yield();
    }
    Bool OnFenceWait(const MGPFenceWait&, Uint32& result) override {
        AwaitPark();
        result = GL_TIMEOUT_EXPIRED;
        return true;
    }
    Bool OnFenceWaitServer(const MGPFenceWait&) override { AwaitPark(); return true; }
    void Install() {
        if (Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void* self) {
            auto& decoder = Srv::ServerSessionInstance().Applier().*PeerMember(DecoderTag{});
            decoder.SetVerbSink(static_cast<FenceBudgetPeer*>(self));
            return MOBILEGL_OK;
        }, this) != MOBILEGL_OK) ::_exit(92);
    }
};

struct BackendTag {
    using Type = UniquePtr<MG_Backend::BackendObject> Srv::ServerLoop::*;
    friend Type PeerMember(BackendTag);
};
template struct PeerAccess<BackendTag, &Srv::ServerLoop::m_backend>;

MG_Backend::BackendObject* ControlCapsPeer();

void StartControlSession(MG_Backend::BackendObject* capsSource = nullptr) {
    ::alarm(15);
    MG_Config::Transport = MG_Config::TransportMode::InProcess;
    Srv::ServerSessionInstance().SetBackend(capsSource ? capsSource : ControlCapsPeer());
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
        auto rc = Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void* self) {
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
        auto rc = Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void* self) {
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

MG_Backend::BackendObject* ControlCapsPeer() {
    // A real encoded initial snapshot is required even when the apply fixture
    // substitutes every verb and has no GPU backend of its own.
    static CapsPeer backend;
    return &backend;
}

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

TEST(RemoteClientControls, UnknownReadPixelsReplyStatusRefusesTightAndBounce) {
    for (const Bool bounce : {false, true}) {
        const auto child = RunInChild([bounce] {
            StartControlSession();
            if (bounce) {
                MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
                MG_State::pGLContext->SetPixelStoreParam(PixelStoreParam::PackRowLength, 8);
            }
            ReadPeer peer;
            peer.status = 3;
            peer.Install();
            Uint8 pixels[128]{};
            RemoteEmitTable().GL.ReadPixels(0, 0, 4, 3, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        });
        ExpectNamedAbort(child, "Fatal{ReplyStatusInvalid, \"ReadPixels\"}");
    }
}

TEST(RemoteClientControls, FenceWaitBudgetHonorsGlTimeoutAndFiniteTransportChunks) {
    // 120000 below is ClientSession.cpp's kBarrierTimeoutMs, the transport grace every CPU bound
    // wait adds on top of the GL timeout. It was 30 s; lavapipe's texture-handle registration
    // (vkCreateImageView -> llvmpipe_register_texture) exceeds that in a single apply stall.
    struct WaitCase { Uint64 timeoutNs; MGPWireOp op; Uint32 expectedParkMs; };
    const WaitCase cases[] = {
        {0, MGPWireOp::FenceWait, 120000},
        {1, MGPWireOp::FenceWait, 120001},
        {60000000000ull, MGPWireOp::FenceWait, 180000},
        {~Uint64(0), MGPWireOp::FenceWait, Transport::kWaitForever - 1},
        {~Uint64(0), MGPWireOp::FenceWaitServer, 120000},
    };
    for (const auto test : cases) {
        const auto child = RunInChild([test] {
            StartControlSession();
            auto& session = ClientSessionInstance();
            auto& producer = session.Producer();
            BudgetBell bell(producer.SelfDoorbell());
            FenceBudgetPeer peer(bell);
            peer.Install();
            producer.*PeerMember(ProducerBellTag{}) = &bell;
            MGPFenceWait request{};
            request.TimeoutNs = test.timeoutNs;
            Uint32 result = 0;
            Int32 status = -1;
            Uint64 size = 0;
            session.EmitAndWait(test.op, &request, sizeof(request), nullptr, 0,
                                &result, sizeof(result), &status, &size);
            producer.*PeerMember(ProducerBellTag{}) = bell.original;
            const auto observed = bell.firstMs.load();
            // Doorbell converts the remaining deadline to whole milliseconds. Allow
            // scheduler delay, but neither the barrier clamp nor an unbounded sentinel.
            if (!bell.entered.load() || observed > test.expectedParkMs ||
                observed < test.expectedParkMs - 100 || observed == Transport::kWaitForever) ::_exit(93);
            if (test.op == MGPWireOp::FenceWait &&
                (status != Codec::ReplySink::kStatusOk || size != sizeof(result) ||
                 result != GL_TIMEOUT_EXPIRED)) ::_exit(94);
            session.Stop();
        });
        ExpectChildSuccess(child);
    }
}

TEST(RemoteClientControls, LongFenceWaitBudgetStillWakesOnShutdown) {
    const auto child = RunInChild([] {
        StartControlSession();
        auto& session = ClientSessionInstance();
        auto& producer = session.Producer();
        BudgetBell bell(producer.SelfDoorbell());
        bell.shutDown = true;
        FenceBudgetPeer peer(bell);
        peer.Install();
        producer.*PeerMember(ProducerBellTag{}) = &bell;
        MGPFenceWait request{};
        request.TimeoutNs = ~Uint64(0);
        Uint32 result = 0;
        Int32 status = -1;
        Uint64 size = 7;
        session.EmitAndWait(MGPWireOp::FenceWait, &request, sizeof(request), nullptr, 0,
                            &result, sizeof(result), &status, &size);
        producer.*PeerMember(ProducerBellTag{}) = bell.original;
        peer.release.store(true);
        if (status != Codec::ReplySink::kStatusDeclined || size != 0) ::_exit(95);
        session.Stop();
    });
    ExpectChildSuccess(child);
}

TEST(RemoteClientControls, ResourceRespecifyErrorIsNotADecline) {
    // THE TEXTURE HALF, WHICH IS THE HALF THAT STILL READS ITS ANSWER (P12). This case used a
    // zeroed descriptor, which is a BUFFER target - and the buffer half is now fire-and-forget
    // because both of its call sites discard the Bool (PipeFill.cpp:1105, :1142). A row that reads
    // no answer cannot fold ERROR into a decision, so the rule this case exists for moved to the
    // half that does read one: the transport fault is caught here, at the next row that waits,
    // instead of at a record whose verdict nobody ever looked at.
    const auto child = RunInChild([] {
        StartControlSession();
        ErrorPeer peer;
        peer.Install();
        MGPResourceDesc desc{};
        desc.Target = static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2D);
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

TEST(RemoteClientControls, AnIdenticalRepeatedMakeCurrentRepublishesNothing) {
    const auto child = RunInChild([] {
        StartControlSession();
        if (Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void*) {
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
        if (!remote.MakeEGLCurrent(dpy, surface, surface, ctx)) ::_exit(88);
        if (CapsMirrorInstance().Generation() != generation) ::_exit(89);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

TEST(RemoteClientControls, ADifferentTupleMakeCurrentIsAdoptedWithoutAPumpOrPresent) {
    const auto child = RunInChild([] {
        StartControlSession();
        if (Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void*) {
            auto backend = MakeUnique<CapsPeer>();
            backend->dynamic.MaxComputeWorkGroupCount[0] = 113;
            Srv::ServerSessionInstance().SetBackend(backend.get());
            Srv::ServerLoopInstance().*PeerMember(BackendTag{}) = Move(backend);
            return MOBILEGL_OK;
        }, nullptr) != MOBILEGL_OK) ::_exit(90);
        BackendObject_Remote remote;
        auto dpy = reinterpret_cast<EGLDisplay>(1);
        auto surface = reinterpret_cast<EGLSurface>(2);
        auto ctx = reinterpret_cast<EGLContext>(3);
        auto otherCtx = reinterpret_cast<EGLContext>(4);
        if (!remote.InitializeEGLDisplay(dpy, nullptr, nullptr) ||
            !remote.CreateEGLPbufferSurface(surface, 4, 4) ||
            !remote.MakeEGLCurrent(dpy, surface, surface, ctx)) ::_exit(91);
        const auto generation = CapsMirrorInstance().Generation();
        GLint before = 0;
        remote.GetBackendFunctions().GL.GetIntegeri_v(0x91BE, 0, &before);
        if (before != 113) ::_exit(92);
        if (Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void*) {
            static_cast<CapsPeer*>(Srv::ServerLoopInstance().Backend())->dynamic.MaxComputeWorkGroupCount[0] = 227;
            return MOBILEGL_OK;
        }, nullptr) != MOBILEGL_OK) ::_exit(93);
        if (!remote.MakeEGLCurrent(dpy, surface, surface, otherCtx)) ::_exit(94);
        GLint after = 0;
        remote.GetBackendFunctions().GL.GetIntegeri_v(0x91BE, 0, &after);
        if (CapsMirrorInstance().Generation() <= generation || after != 227) ::_exit(95);
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
        StartControlSession(&backend);
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
    // Historical name retained: the live path now copies the reply into the PBO.
    const auto child = RunInChild([] {
        StartControlSession();
        ReadPeer peer;
        peer.size = 4;
        peer.Install();
        MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
        GLuint buffer = 0;
        MG_Impl::GLImpl::GenBuffers(1, &buffer);
        MG_Impl::GLImpl::BindBuffer(0x88EB, buffer);
        Uint8 seed[64];
        std::memset(seed, 0x5a, sizeof(seed));
        MG_Impl::GLImpl::BufferData(0x88EB, sizeof(seed), seed, 0x88E1);
        RemoteEmitTable().GL.ReadPixels(0, 0, 1, 1, 0x1908, 0x1401, reinterpret_cast<void*>(16));
        Uint8 actual[64]{};
        MG_Impl::GLImpl::GetBufferSubData(0x88EB, 0, sizeof(actual), actual);
        for (SizeT i = 0; i < sizeof(actual); ++i)
            if (actual[i] != (i >= 16 && i < 20 ? 0x37 : 0x5a)) ::_exit(104);
        if (peer.observed != 4) ::_exit(105);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

TEST(RemoteClientControls, ServerRoleRespecifyDoesNotEmitClientInitialBytes) {
    // P5c (hd): the in-process server-role drive itself is now refused by name. The emit
    // path's first move is the client allocator's Acquire for the buffer's handle, and with
    // an active transport that entry is Fatal{RoleViolation, "MGPipeSlots"} from the apply
    // thread (CONTRACT-P5C §3.1, rule E). "No client initial bytes" therefore holds the
    // strong way - there is no client-emitter path for the server role to reach at all; ev's
    // SEG_EVENT producers are what replaces the mechanism this case used to drive.
    const auto child = RunInChild([] {
        CapsPeer backend;
        StartControlSession(&backend);
        MG_Config::Features.PipePush |= kMGPipeSubsystemResources;
        Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void*) {
            MG_State::GLState::BufferObject buffer(701);
            Uint8 data[64]{};
            buffer.Respecify(sizeof(data), data);
            return MOBILEGL_OK;
        }, nullptr);
        ClientSessionInstance().Stop();
    });
    ExpectNamedAbort(child, "Fatal{RoleViolation, \"MGPipeSlots\"}");
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
    // P5c (hd): same refusal as the respecify sibling above - a server-role flush reaching the
    // client emit path is stopped at the allocator entry before any follow-up question arises
    // (Fatal{RoleViolation, "MGPipeSlots"}; ev's events replace the mechanism).
    const auto child = RunInChild([] {
        CapsPeer backend;
        StartControlSession(&backend);
        MG_Config::Features.PipePush |= kMGPipeSubsystemResources;
        Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void*) {
            MGPipeResourceOps ops{};
            ops.Create = [](MGPipeHandle, const MGPResourceDesc&) {};
            ops.Respecify = [](MGPipeHandle, const MGPResourceDesc&, const void*) {};
            ops.SubData = [](MGPipeHandle, const MGPSubData&, const void*) {};
            ops.FlushRange = [](MGPipeHandle, const MGPFlushRange&, const void*) {};
            ops.Destroy = [](MGPipeHandle) {};
            MGPipeSetResourceOps(&ops);
            {
                MG_State::GLState::BufferObject buffer(702);
                buffer.Resize(64);
                MGPipeEmitResourceFlushRange(buffer, 16, 16, 0);
            }
            MGPipeSetResourceOps(nullptr);
            return MOBILEGL_OK;
        }, nullptr);
        ClientSessionInstance().Stop();
    });
    ExpectNamedAbort(child, "Fatal{RoleViolation, \"MGPipeSlots\"}");
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
        Uint8 pixels[4]{};
        G::GetBufferSubData(GL_PIXEL_PACK_BUFFER, 16, 4, pixels);
        if (pixels[0] != 0 || pixels[1] != 255 || pixels[2] != 0 || pixels[3] != 255) ::_exit(105);
        std::printf("C1F_PACK_%s bytes at offset 16: %u %u %u %u\n", split ? "INPROC" : "MONOLITH",
                    pixels[0], pixels[1], pixels[2], pixels[3]);
        std::fflush(stdout);
    });
    const bool ok = WIFEXITED(child.Status) && WEXITSTATUS(child.Status) == 0;
    std::printf("C1F_PACK_%s: %s (%s)\n%s", split ? "INPROC" : "MONOLITH",
                ok ? "PASS" : "FAIL", DescribeStatus(child).c_str(), child.Log.c_str());
    return ok ? 0 : 1;
}
namespace {
struct PatternReadPeer : ReadPeer {
    Bool OnReadPixels(const MGPReadbackInfo& info, Uint64 seq, Codec::ReplySink* replies) override {
        Uint8 bytes[32];
        for (Uint8 i = 0; i < sizeof(bytes); ++i) bytes[i] = i + 1;
        replies->PostReply(seq, 0, bytes, info.DstSize);
        return true;
    }
};

std::atomic<Bool> retirementRetryStarted{false};
std::atomic<Bool> retirementWaitObserved{false};
void HoldRetirementUntilProducerWaits() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (retirementRetryStarted.load() && ClientSessionInstance().Control()->producerParked.load()) {
            retirementWaitObserved.store(true);
            break;
        }
        std::this_thread::yield();
    }
    Srv::ServerLoopInstance().SetBeforeRetireHookForTesting(nullptr);
}

struct DrawPeer : Codec::WireVerbSink {
    Bool OnDrawVbo(const MGPDrawInfo&, const MGPDrawRange*, const MGHostSpan*,
                   const MGPDrawIndirect*) override { return true; }
    void Install() {
        Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void* self) {
            auto& decoder = Srv::ServerSessionInstance().Applier().*PeerMember(DecoderTag{});
            decoder.SetVerbSink(static_cast<DrawPeer*>(self));
            return MOBILEGL_OK;
        }, this);
    }
};
}

TEST(RemoteClientControls, ReadPixelsSwapsComponentsInTightAndScatteredReplies) {
    const auto child = RunInChild([] {
        StartControlSession();
        MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
        PatternReadPeer peer;
        peer.Install();
        struct Pair { GLenum format; GLenum type; Uint32 bpp; Uint32 group; };
        for (const Pair pair : {Pair{GL_RGBA, GL_UNSIGNED_SHORT, 8, 2},
                                Pair{GL_RGBA, GL_FLOAT, 16, 4},
                                Pair{GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, 4, 4},
                                Pair{GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, 8, 4},
                                Pair{GL_RGBA, GL_UNSIGNED_BYTE, 4, 1}}) {
            for (int scatter : {0, 1}) for (int swap : {0, 1}) {
                auto& context = *MG_State::pGLContext;
                context.SetPixelStoreParam(PixelStoreParam::PackSwapBytes, swap);
                context.SetPixelStoreParam(PixelStoreParam::PackRowLength, scatter ? 3 : 0);
                context.SetPixelStoreParam(PixelStoreParam::PackSkipPixels, scatter);
                Uint8 result[160];
                std::memset(result, 0xCD, sizeof(result));
                RemoteEmitTable().GL.ReadPixels(0, 0, 1, 2, pair.format, pair.type, result);
                for (Uint32 at = 0; at < sizeof(result); ++at) {
                    Uint8 expected = 0xCD;
                    for (Uint32 row = 0; row < 2; ++row) {
                        const Uint32 begin = (scatter ? 1 + 3 * row : row) * pair.bpp;
                        if (at >= begin && at < begin + pair.bpp) {
                            Uint32 i = at - begin;
                            if (swap) i = (i / pair.group) * pair.group + pair.group - 1 - i % pair.group;
                            expected = row * pair.bpp + i + 1;
                        }
                    }
                    if (result[at] != expected) ::_exit(110);
                }
            }
        }
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

TEST(RemoteClientControls, PersistentMapProducerRefusesTheApplyThread) {
    const auto child = RunInChild([] {
        StartControlSession();
        Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void*) {
            PersistentMapTracker::Instance().PushAllMembers();
            return MOBILEGL_OK;
        }, nullptr);
    });
    ExpectNamedAbort(child, "Fatal{RoleViolation, \"PushAllMembers\"}");
}

TEST(RemoteClientControls, CommandWrapWaitsForRetirementAfterThePriorRecordApplied) {
    const auto child = RunInChild([] {
        MG_Config::Ipc.RingMb = 1;
        StartControlSession();
        DrawPeer peer;
        peer.Install();
        Vector<MGPDrawRange> ranges(32768);
        MGPDrawInfo info{};
        auto emit = [&](Uint32 count) {
            info.NumDraws = count;
            return ClientSessionInstance().EmitAndWait(MGPWireOp::DrawVbo, &info, sizeof(info),
                ranges.data(), count * sizeof(MGPDrawRange), nullptr, 0, nullptr);
        };
        emit(28000); // Position the next record so that its successor needs a wrap pad.
        peer.Install(); // A mailbox round trip also lets the previous drain retire.
        Srv::ServerLoopInstance().SetBeforeRetireHookForTesting(&HoldRetirementUntilProducerWaits);
        const auto prior = emit(32768);
        if (ClientSessionInstance().Control()->Progress.retiredSeq.load() >= prior) ::_exit(111);
        retirementRetryStarted.store(true);
        const auto next = emit(32768);
        if (next != prior + 1 || !retirementWaitObserved.load()) ::_exit(112);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

TEST(RemoteClientControls, StagingWaitsForRetirementAfterThePriorRecordApplied) {
    const auto child = RunInChild([] {
        MG_Config::Ipc.StageMb = 1;
        StartControlSession();
        Vector<Uint8> bytes(768 * 1024, 0x57);
        auto& session = ClientSessionInstance();
        session.Encoder().StageBytes(bytes.data(), bytes.size());
        Srv::ServerLoopInstance().SetBeforeRetireHookForTesting(&HoldRetirementUntilProducerWaits);
        MGPBindRenderState bind{};
        const auto prior = session.EmitAndWait(MGPWireOp::BindRenderState, &bind, sizeof(bind),
                                               nullptr, 0, nullptr, 0, nullptr);
        if (session.Control()->Progress.retiredSeq.load() >= prior) ::_exit(113);
        retirementRetryStarted.store(true);
        session.Encoder().StageBytes(bytes.data(), bytes.size());
        if (!retirementWaitObserved.load() || session.Encoder().StageReclaimWaits() != 1) ::_exit(114);
        session.Stop();
    });
    ExpectChildSuccess(child);
}

// P5e (gl) RED-ONCE, ID-111. THE SERVER-SIDE MIRROR OF THE CLIENT LATCH - the line that makes
// ID-90 ("Magma keeps the lockstep for the whole of P5e") true in code rather than in prose.
//
// The control session calls SetCapabilityBits(0), so its CallMask IS set and does NOT carry
// bit 10. That is a Magma server's answer for ever (MGPipeRunAheadCapBitsFor returns kCapNone
// off DirectGLES) and an Espryt server's answer for the whole bring-up window. A DrawVbo then
// crosses for real, so what is under test is the stamp PipeApplier::ApplyOne writes and not a
// test-only branch beside it.
//
// FOUR ASSERTIONS, because each one alone can be true for the wrong reason:
//   1. the session really lacks the bit, said in its own words rather than inferred from the
//      stamp being tested (ID-102: the action under test is the probe itself);
//   2. the wire really answers UNBARRIERED for this record - draw_vbo is kWaitNone - so the
//      `wireSaysBarriered` arm is the one a flipped constant would take;
//   3. the record that actually crossed was stamped barriered;
//   4. and the stamp is STILL barriered at the POST-FLIP value of the wait-rule constant.
//
// (4) IS THE ONE THAT MATTERS AND IT IS STATED AT `true` RATHER THAN READ FROM THE CONSTANT.
// kMGPipeP5eClientWaitRuleLanded is false today, so (3) alone would pass for a server that had
// no capability conjunct at all - and would keep passing until the integration commit, at which
// point Magma SIGSEGVs on its first draw with the lane still green. Asking the pure function
// for the post-flip answer is how the claim gets tested before the day it starts deciding.
//
// THE RED: delete `serverPublishesRunAhead &&` from MGPipeApplierStampsBarriered
// (MG_Remote/Server/PipeApplier.h) and (4) flips to false - the child exits 124.
TEST(RemoteClientControls, ADrawOnASessionWithoutTheRunAheadBitStaysStampedBarriered) {
    const auto child = RunInChild([] {
        StartControlSession();
        DrawPeer peer;
        peer.Install();
        MGPDrawInfo info{};
        info.NumDraws = 1;
        MGPDrawRange range{};
        if (ClientSessionInstance().EmitAndWait(MGPWireOp::DrawVbo, &info, sizeof(info), &range,
                                                sizeof(range), nullptr, 0, nullptr) == 0)
            ::_exit(120);
        // Read the stamp on the thread that owns it: it is a thread_local describing "which
        // record is the apply thread inside right now", and ApplyOne leaves it standing.
        if (Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void*) {
                if (Srv::MGPipeServerPublishesRunAhead()) ::_exit(121);
                MGPDrawInfo probe{};
                probe.NumDraws = 1;
                if (MGPipeBarriered(MGPWireOp::DrawVbo, &probe, MGPipeApplier())) ::_exit(122);
                if (!MGPipeApplierCurrentRecordIsBarriered()) ::_exit(123);
                if (!Srv::MGPipeApplierStampsBarriered(/*clientWaitRuleLanded=*/true,
                                                       Srv::MGPipeServerPublishesRunAhead(),
                                                       /*wireSaysBarriered=*/false))
                    ::_exit(124);
                return MOBILEGL_OK;
            },
            nullptr) != MOBILEGL_OK)
            ::_exit(125);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

// =====================================================================================
// P12: a DECLINED reply is an answer, not a corruption
// =====================================================================================
//
// ReplySink's status space is three-valued and its own header says what the values mean:
// 0 OK, 1 DECLINED IS A REAL ANSWER, 2 ERROR (PipeWireCodec.h:457-460). The escape rows are
// held to the other half of the same rule - "ERROR IS NOT A DECLINE", an escape may not fold
// 2 into `false` (WireTables.cpp:446-450). The fence and query consumers folded BOTH 1 and 2
// into a single abort (EmitTables.cpp ReadFenceReply, QueryEmit.inc ReadQueryReply), so a
// client that had latched device-lost - where every verb is DECLINED by design,
// ClientSession.cpp:1837 - died of Fatal{ProtocolCorruption, "Fence.reply"} on its next
// glClientWaitSync instead of going quiet the way the design promises ("GL calls become
// no-ops", design/08-runtime-and-platform.md:33).
//
// THE PEER BELOW IS THE SERVER'S OWN DECLINE, not a substitution of the status byte: these
// verbs answer false when the server cannot answer them, and the codec posts exactly that as
// DECLINED with a zero-length payload (PipeWireCodec.cpp:1820-1830 for the two fence rows,
// :1841-1861 for the three query rows). The CLIENT's own decline is pinned separately, by
// LongFenceWaitBudgetStillWakesOnShutdown above.
//
// THE RED, RUN ONCE: take the `if (status == Wire::ReplySink::kStatusDeclined)` arm out of
// ReadFenceReply and the first case below dies of Fatal{ProtocolCorruption, "Fence.reply"}
// instead of asserting the no-op - which is the split client's Minecraft 26.2 crash.
struct DecliningSyncPeer : Codec::WireVerbSink {
    Bool OnFenceCreate(const MG_Pipe::MGPHandleOnly&) override { return true; }
    Bool OnFenceStatus(const MG_Pipe::MGPHandleOnly&, Uint32&) override { return false; }
    Bool OnFenceWait(const MGPFenceWait&, Uint32&) override { return false; }
    Bool OnQueryCreate(const MGPQueryDesc&) override { return true; }
    Bool OnQueryBegin(const MGPQueryDesc&) override { return true; }
    Bool OnQueryCounter(const MGPQueryDesc&) override { return true; }
    Bool OnQueryAvailable(const MG_Pipe::MGPHandleOnly&, Uint32&) override { return false; }
    Bool OnQueryResult(const MGPQueryResultRequest&, Codec::QueryResultReply&) override { return false; }
    Bool OnQueryTimestamp(const MGPTimestampRequest&, Int64&) override { return false; }
    void Install() {
        const auto rc = Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void* self) {
            auto& decoder = Srv::ServerSessionInstance().Applier().*PeerMember(DecoderTag{});
            decoder.SetVerbSink(static_cast<DecliningSyncPeer*>(self));
            return MOBILEGL_OK;
        }, this);
        if (rc != MOBILEGL_OK) ::_exit(130);
    }
};

TEST(RemoteClientControls, DeclinedFenceReplyAnswersSignaledInsteadOfAborting) {
    const auto child = RunInChild([] {
        StartControlSession();
        DecliningSyncPeer peer;
        peer.Install();
        auto* sync = RemoteEmitTable().GL.FenceSync();
        if (sync == nullptr) ::_exit(131);
        // glClientWaitSync on a fence nobody can wait on: signaled now, as every fallback in
        // this tree already answers (GL_Sync.cpp:103-106, PipeApplier.cpp:175-177).
        if (RemoteEmitTable().GL.ClientWaitSync(sync, 0, 0) != GL_ALREADY_SIGNALED) ::_exit(132);
        // GL_SYNC_STATUS: the same answer, in the boolean the row carries.
        if (!RemoteEmitTable().GL.GetSyncStatus(sync)) ::_exit(133);
        RemoteEmitTable().GL.DeleteSync(sync);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

TEST(RemoteClientControls, DeclinedQueryRepliesAnswerNoAnswerYetInsteadOfAborting) {
    const auto child = RunInChild([] {
        StartControlSession();
        DecliningSyncPeer peer;
        peer.Install();
        auto* query = RemoteEmitTable().GL.BeginTimeElapsedQuery();
        if (query == nullptr) ::_exit(134);
        // GL_QUERY_RESULT_AVAILABLE: "not available" - the answer an unfinished query gives,
        // and the one that makes a waiting application yield rather than spin.
        if (RemoteEmitTable().GL.IsQueryResultAvailable(query)) ::_exit(135);
        // GL_QUERY_RESULT: no value produced, and the caller's out-parameter untouched.
        Uint64 nanoseconds = 7;
        if (RemoteEmitTable().GL.GetQueryResult64(query, false, &nanoseconds) || nanoseconds != 7)
            ::_exit(136);
        // glGetInteger64v(GL_TIMESTAMP): there is no timestamp to report.
        if (RemoteEmitTable().GL.GetGpuTimestampNs() != 0) ::_exit(137);
        RemoteEmitTable().GL.DeleteBackendQuery(query);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

TEST(RemoteClientControls, ErrorFenceReplyStillRefusesByName) {
    // THE TOLERANCE IS NOT BLANKET. An ERROR is a transport fault or a server-side refusal,
    // never "the server said no" (R-5/M4), and it keeps the named Fatal it always had.
    const auto child = RunInChild([] {
        StartControlSession();
        // The rows still decline (so the fence is never handed to a backend that cannot
        // create one) and EVERY answer that is posted says ERROR instead.
        DecliningSyncPeer rows;
        rows.Install();
        ErrorPeer peer;
        peer.Install();
        auto* sync = RemoteEmitTable().GL.FenceSync();
        if (sync == nullptr) ::_exit(138);
        (void)RemoteEmitTable().GL.ClientWaitSync(sync, 0, 0);
    });
    ExpectNamedAbort(child, "Fatal{ProtocolCorruption, \"Fence.reply\"}");
}

TEST(RemoteClientControls, DeviceLostRespecifyDoesNotDieOfTheUncarriedBytesCheck) {
    // P12 exit gate (a): killing the server must give a CLEAN device-lost latch. With the
    // latch set every verb is DECLINED, so the resource_subdata that R-13.3 owes emits
    // nothing - which is the latch working, not bytes the client forgot to carry. The check
    // at PipeFill.cpp:1115 compared client record ordinals only, so the next glBufferData
    // with data aborted the client by name instead.
    const auto child = RunInChild([] {
        StartControlSession();
        // Taken through the production entry point, the way the barrier and the encoder take
        // it when a descriptor whose far end only the peer held reports hangup.
        ClientSessionInstance().LatchDeviceLost("RemoteClientControls: the self-check case");
        MG_Config::Features.PipePush |= kMGPipeSubsystemResources;
        MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
        GLuint buffer = 0;
        MG_Impl::GLImpl::GenBuffers(1, &buffer);
        MG_Impl::GLImpl::BindBuffer(0x8892, buffer);
        Uint8 seed[64];
        std::memset(seed, 0x5a, sizeof(seed));
        MG_Impl::GLImpl::BufferData(0x8892, sizeof(seed), seed, 0x88E4);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

// =====================================================================================
// P12 item B: the texture half stops owning a reply on the wire arm
// =====================================================================================
//
// THE NUMBER IS THE GATE. `wait_replies` is what the handoff measured 55,428 of in one
// Minecraft frame; LinkMetrics.cpp:76-80 counts exactly the records that OWN a reply slot, so
// it is the same quantity the per-frame P65LinkMetrics line prints. The case drives it over a
// live split session: a pure level upload must cross the wire and open NO reply wait, while a
// create row - which is kWaitReply and always was - still moves the number.
//
// THE SECOND HALF IS THE NEGATIVE CONTROL FOR THE FIRST. Without it, a session that counted
// nothing at all (metrics off, no records) would pass the half that exists to fail.
//
// THE RED, RUN ONCE: take the `kClientWire` arm out of MGPipeSubDataWantsItsReply (PipeRoute.h)
// and the upload delta becomes 1 - the child exits 142.
TEST(RemoteClientControls, TextureUploadsStopOwningAReplyOnTheWireArm) {
    const auto child = RunInChild([] {
        ::setenv("MOBILEGL_PIPE_STATS", "1", 1);
        CapsPeer backend;
        StartControlSession(&backend);
        MG_Config::Features.PipePush |= kMGPipeSubsystemResources |
                                        kMGPipeSubsystemTextureResources | kMGPipeSubsystemSamplers;
        MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
        Uint8 pixels[4 * 4 * 4];
        std::memset(pixels, 0x21, sizeof(pixels));
        // THE CONTROL ROW, NAMED RATHER THAN TOTALLED, AND MEASURED ACROSS THE WHOLE SETUP. The
        // object's create is the one record an object always pays for (kWaitReply in
        // PipeCalls.def), so ITS counter is the live control - and the baseline goes in before
        // GenTextures, because which call publishes the twin (generate, bind or the storage
        // definition) is the emitter's business and not this case's. The previous version of this
        // control read the session TOTAL and only moved because a respecify was still waiting;
        // that row is fire-and-forget for a confirmed object now, which is what made the stale
        // control visible instead of silently wrong.
        const Uint32 uploadOp = static_cast<Uint32>(MGPWireOp::ResourceSubData);
        const Uint32 createOp = static_cast<Uint32>(MGPWireOp::ResourceCreate);
        const Uint64 createRecordsBefore = Transport::LinkMetricsRecordsFor(createOp);
        GLuint texture = 0;
        MG_Impl::GLImpl::GenTextures(1, &texture);
        MG_Impl::GLImpl::BindTexture(0x0DE1, texture);
        MG_Impl::GLImpl::TexImage2D(0x0DE1, 0, 0x8058, 4, 4, 0, 0x1908, 0x1401, pixels);
        MGP_FILL(DrawArrays);
        // THE CREATE WENT OUT (the record), and the live control is the fence below.
        if (Transport::LinkMetricsRecordsFor(createOp) <= createRecordsBefore) ::_exit(140);
        // THE ROW UNDER TEST. A second write into the same level is a pure resource_subdata: the
        // resource is created, so no create crosses, and the level is dirty again - the only
        // record it can produce is the one item B is about.
        const Uint64 uploadRecordsBefore = Transport::LinkMetricsRecordsFor(uploadOp);
        const Uint64 uploadWaitsBefore = Transport::LinkMetricsReplyWaitsFor(uploadOp);
        MG_Impl::GLImpl::TexSubImage2D(0x0DE1, 0, 0, 0, 2, 2, 0x1908, 0x1401, pixels);
        MGP_FILL(DrawArrays);
        if (Transport::LinkMetricsRecordsFor(uploadOp) <= uploadRecordsBefore) ::_exit(141);
        if (Transport::LinkMetricsReplyWaitsFor(uploadOp) != uploadWaitsBefore) ::_exit(142);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

// =====================================================================================
// P12 item B: WHICH ROWS BUY THE WAITS - the measurement the handoff left open (its 5.4)
// =====================================================================================
//
// A total cannot answer it. 55,428 reply waits in one frame is the same number whether they are
// uploads (item B), creates (per resource) or parameter sets (per call), and those three have
// different fixes - so the split is what says whether B is the whole story or a fraction of one.
// An atlas-shaped load makes it visible: one sprite per texture, one glTexImage2D and four
// glTexParameteri, which is the call shape an atlas stitch has.
//
// A MEASUREMENT WITH A STRUCTURAL GATE UNDER IT, not a performance gate: the assertions are the
// ones that can be WRONG about the mechanism (the upload rows contribute zero after item B, the
// create / respecify / parameter rows do not, and the per-op tally sums to the total), and the
// numbers themselves are printed for the register.
TEST(RemoteClientControls, ReplyWaitsByOpForAnAtlasShapedLoad) {
    const auto child = RunInChild([] {
        ::setenv("MOBILEGL_PIPE_STATS", "1", 1);
        CapsPeer backend;
        StartControlSession(&backend);
        MG_Config::Features.PipePush |= kMGPipeSubsystemResources |
                                        kMGPipeSubsystemTextureResources | kMGPipeSubsystemSamplers;
        MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
        constexpr int kSprites = 64;
        Uint8 pixels[16 * 16 * 4];
        std::memset(pixels, 0x5a, sizeof(pixels));

        // SHAPE 1 - THE ATLAS STITCH, which is what item B was aimed at: ONE texture and many
        // small writes into it. If B works, this shape's wait count stops tracking the upload
        // count at all.
        GLuint atlas = 0;
        MG_Impl::GLImpl::GenTextures(1, &atlas);
        MG_Impl::GLImpl::BindTexture(0x0DE1, atlas);
        MG_Impl::GLImpl::TexImage2D(0x0DE1, 0, 0x8058, 256, 256, 0, 0x1908, 0x1401, nullptr);
        MG_Impl::GLImpl::TexParameteri(0x0DE1, 0x2801, 0x2601);
        MGP_FILL(DrawArrays);
        const Uint64 afterAtlasSetup = Transport::LinkMetricsReplyWaits();
        for (int i = 0; i < kSprites; ++i) {
            MG_Impl::GLImpl::TexSubImage2D(0x0DE1, 0, (i % 8) * 16, (i / 8) * 16, 16, 16, 0x1908,
                                           0x1401, pixels);
            MGP_FILL(DrawArrays); // the drain point
        }
        const Uint64 atlasWaits = Transport::LinkMetricsReplyWaits() - afterAtlasSetup;
        std::printf("shape 1 atlas stitch: %d sub-uploads into one level -> wait_replies=%llu\n",
                    kSprites, static_cast<unsigned long long>(atlasWaits));
        const Uint64 atlasWaitsBefore = Transport::LinkMetricsReplyWaits();

        // SHAPE 2 - A TEXTURE PER SPRITE, the other thing a stitcher can do (and what the mipmap
        // and dynamic-texture paths look like): every sprite pays a storage definition and its
        // own parameter set.
        for (int i = 0; i < kSprites; ++i) {
            GLuint texture = 0;
            MG_Impl::GLImpl::GenTextures(1, &texture);
            MG_Impl::GLImpl::BindTexture(0x0DE1, texture);
            MG_Impl::GLImpl::TexImage2D(0x0DE1, 0, 0x8058, 16, 16, 0, 0x1908, 0x1401, pixels);
            MG_Impl::GLImpl::TexParameteri(0x0DE1, 0x2802, 0x2901); // WRAP_S = CLAMP_TO_EDGE
            MG_Impl::GLImpl::TexParameteri(0x0DE1, 0x2803, 0x2901); // WRAP_T
            MG_Impl::GLImpl::TexParameteri(0x0DE1, 0x2801, 0x2601); // MIN_FILTER = LINEAR
            MG_Impl::GLImpl::TexParameteri(0x0DE1, 0x2800, 0x2601); // MAG_FILTER = LINEAR
            MGP_FILL(DrawArrays); // the drain point
        }
        // A PROBE FOR REDUNDANCY, before anyone tries to remove it: does a repeated set of the
        // SAME value produce another record? If it does not, then the device run's 18,056
        // set_texture_params records in one window are genuine changes and there is nothing to
        // dedup - and any "fix" would be a fix for a problem that does not exist.
        {
            const Uint32 paramsOp = static_cast<Uint32>(MGPWireOp::SetTextureParams);
            const Uint64 beforeSame = Transport::LinkMetricsRecordsFor(paramsOp);
            MG_Impl::GLImpl::TexParameteri(0x0DE1, 0x2801, 0x2600); // MIN_FILTER = NEAREST
            MGP_FILL(DrawArrays);
            const Uint64 afterFirst = Transport::LinkMetricsRecordsFor(paramsOp);
            MG_Impl::GLImpl::TexParameteri(0x0DE1, 0x2801, 0x2600); // the SAME value again
            MGP_FILL(DrawArrays);
            const Uint64 afterSame = Transport::LinkMetricsRecordsFor(paramsOp);
            MG_Impl::GLImpl::TexParameteri(0x0DE1, 0x2801, 0x2601); // a DIFFERENT value
            MGP_FILL(DrawArrays);
            const Uint64 afterChange = Transport::LinkMetricsRecordsFor(paramsOp);
            std::printf("params probe: first=%llu same-again=%llu changed=%llu (deltas %llu/%llu)\n",
                        static_cast<unsigned long long>(afterFirst - beforeSame),
                        static_cast<unsigned long long>(afterSame - beforeSame),
                        static_cast<unsigned long long>(afterChange - beforeSame),
                        static_cast<unsigned long long>(afterFirst - beforeSame),
                        static_cast<unsigned long long>(afterSame - afterFirst));
            std::fflush(nullptr);
        }

        // PRINT FIRST, ASSERT SECOND: the table is the point of the case, and a gate that fired
        // before it would hide the numbers on exactly the run that needs them - the red one.
        //
        // BOTH HALVES OF THE SAME FRAME: what it PAID (waits) and what it SENT (records). They are
        // different questions and the pair is the point - item B did not stop the upload records
        // from being sent, it stopped them from being waited for.
        std::printf("records sent by op (whole session):\n");
        for (Uint32 op = 1; op < Transport::LinkMetricsMaxOps; ++op) {
            const Uint64 records = Transport::LinkMetricsRecordsFor(op);
            if (records != 0)
                std::printf("  op=%-24s records=%llu waits=%llu\n",
                            Codec::WireOpName(static_cast<MGPWireOp>(op)),
                            static_cast<unsigned long long>(records),
                            static_cast<unsigned long long>(Transport::LinkMetricsReplyWaitsFor(op)));
        }
        std::fflush(nullptr);
        std::printf("shape 2 a texture per sprite: %d textures -> wait_replies=%llu\n", kSprites,
                    static_cast<unsigned long long>(Transport::LinkMetricsReplyWaits() - atlasWaitsBefore));
        for (Uint32 op = 1; op < Transport::LinkMetricsMaxOps; ++op) {
            const Uint64 waits = Transport::LinkMetricsReplyWaitsFor(op);
            if (waits != 0)
                std::printf("  op=%-24s waits=%llu\n", Codec::WireOpName(static_cast<MGPWireOp>(op)),
                            static_cast<unsigned long long>(waits));
        }
        std::fflush(nullptr);
        // THE STRUCTURAL GATE. The upload row must be free (item B) and every other row this load
        // drives must NOT be, or the case would also pass on a session that counted nothing.
        if (Transport::LinkMetricsReplyWaitsFor(static_cast<Uint32>(MGPWireOp::ResourceSubData)) != 0)
            ::_exit(143);
        // AND THE UPLOAD RECORDS WERE STILL SENT: the shape 1 stitch is 64 sub-uploads, so the row
        // must show 64 records with 0 waits. A pair that went to 0/0 together would mean the
        // emitter stopped sending, which is a different (and much worse) change than item B.
        if (Transport::LinkMetricsRecordsFor(static_cast<Uint32>(MGPWireOp::ResourceSubData)) < 64)
            ::_exit(148);
        if (Transport::LinkMetricsRecordsFor(static_cast<Uint32>(MGPWireOp::ResourceCreate)) == 0)
            ::_exit(144);
        // AND ResourceRespecify FOLLOWS SetTextureParams OFF THAT LIST FOR THE SAME REASON (P12/R3):
        // for a confirmed object it is fire-and-forget too. What must still hold is that its RECORDS
        // go out - a zero on both lines would mean the emitter stopped sending, which is a different
        // and much worse change than this one.
        if (Transport::LinkMetricsRecordsFor(static_cast<Uint32>(MGPWireOp::ResourceRespecify)) == 0)
            ::_exit(145);
        // AND SetTextureParams IS DELIBERATELY NOT ON THAT LIST ANY MORE (P12). It was the control
        // that proved this counter was live; the first-use confirmation moved the row to
        // fire-and-forget for a confirmed object, so its zero is the FIX rather than a dead counter
        // - which is why create and respecify above carry that job now, and why the records line
        // below still has to show the parameter records going out.
        if (Transport::LinkMetricsRecordsFor(static_cast<Uint32>(MGPWireOp::SetTextureParams)) == 0)
            ::_exit(146);
        Uint64 sum = 0;
        for (Uint32 op = 0; op < Transport::LinkMetricsMaxOps; ++op)
            sum += Transport::LinkMetricsReplyWaitsFor(op);
        if (sum != Transport::LinkMetricsReplyWaits()) ::_exit(147);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}
// =====================================================================================
// P12: the first-use confirmation - an object pays ONE round trip, at its first use
// =====================================================================================
//
// The rule the device run asked for: the round trip is owed by an object's CREATE (that is what
// says "the applier holds a record for this handle"), and every control record after it is
// fire-and-forget. The latch that decides is MGPipeHandleIsPublished, which the emitter sets only
// on an ACCEPTED create - so the row was asking the server a question the client had already
// answered, at ~5 ms per record (18,056 of one frame's 43,232 waits were this row).
//
// THE RED, RUN ONCE: put the unconditional EmitAndWait back into Wire_SetTextureParams and the
// last assertion below flips - the parameter records start taking replies again.
TEST(RemoteClientControls, AParameterChangeForAConfirmedObjectTakesNoReply) {
    const auto child = RunInChild([] {
        ::setenv("MOBILEGL_PIPE_STATS", "1", 1);
        CapsPeer backend;
        StartControlSession(&backend);
        MG_Config::Features.PipePush |= kMGPipeSubsystemResources |
                                        kMGPipeSubsystemTextureResources | kMGPipeSubsystemSamplers;
        MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
        const Uint32 paramsOp = static_cast<Uint32>(MGPWireOp::SetTextureParams);
        const Uint32 createOp = static_cast<Uint32>(MGPWireOp::ResourceCreate);
        GLuint texture = 0;
        MG_Impl::GLImpl::GenTextures(1, &texture);
        MG_Impl::GLImpl::BindTexture(0x0DE1, texture);
        // FIRST USE, AND THE HARD SHAPE: parameters with no storage at all - the default-texture
        // case the self-heal comment names. The confirmation is a create with no storage, it is
        // BLOCKING (the one answer this object owes), and the parameters land with it.
        MG_Impl::GLImpl::TexParameteri(0x0DE1, 0x2801, 0x2601); // MIN_FILTER = LINEAR
        MGP_FILL(DrawArrays);
        if (Transport::LinkMetricsRecordsFor(createOp) == 0) ::_exit(150);
        // THE LIVE CONTROL IS map_persistent, NOT A CREATE AND NOT A FENCE. Not a create, because
        // the create row now batches its answers behind MOBILEGL_IPC_CREATE_WINDOW. Not a FENCE,
        // because a control session has no backend object: FenceCreate is refused, FindFence then
        // dies of Fatal{ProtocolCorruption, "Fence.handle"}, and the child ABORTS - a case cannot
        // read an exit code off a corpse. map_persistent needs no backend at all: under split its
        // answer is a CONSTANT DECLINE that still costs a record and still owns a reply slot
        // (PipeRoute.h, R-6), so it is exactly "a row that always takes its answer".
        {
            const Uint64 waitsBeforeControl = Transport::LinkMetricsReplyWaits();
            MG_Pipe::MGPHandleOnly controlHandle{};
            (void)MG_Pipe::MGPipeRouteMapPersistent(controlHandle, 64, nullptr);
            if (Transport::LinkMetricsReplyWaits() <= waitsBeforeControl) ::_exit(151);
        }
        // NOW THE OBJECT IS CONFIRMED. Eight real parameter changes: every one must still be SENT,
        // and none of them may take a reply.
        const Uint64 recordsBefore = Transport::LinkMetricsRecordsFor(paramsOp);
        const Uint64 waitsBefore = Transport::LinkMetricsReplyWaitsFor(paramsOp);
        for (int i = 0; i < 8; ++i) {
            MG_Impl::GLImpl::TexParameteri(0x0DE1, 0x2801, (i % 2) != 0 ? 0x2601 : 0x2600);
            MGP_FILL(DrawArrays);
        }
        if (Transport::LinkMetricsRecordsFor(paramsOp) != recordsBefore + 8) ::_exit(152);
        if (Transport::LinkMetricsReplyWaitsFor(paramsOp) != waitsBefore) ::_exit(153);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}
// =====================================================================================
// P12: a BUFFER respecify takes no reply; a TEXTURE one still does (deliberately)
// =====================================================================================
//
// The two halves of resource_respecify are not the same row. The buffer half's Bool is discarded
// at BOTH call sites (PipeFill.cpp:1105, :1142), so its wait bought a value that was thrown away -
// fire-and-forget costs nothing and saves a round trip per glBufferData, which is the shape a
// chunk uploader produces thousands of times per frame. The texture half's Bool is load-bearing:
// it triggers the self-heal AND gates the acceptance mirror, and advancing that mirror on a
// provisional accept is ID-18 M3 (TextureEmit.h:885-888). So this case asserts BOTH directions -
// the asymmetry is the design, not an oversight.
//
// THE RED, RUN ONCE: take the bufferHalf branch out of Wire_Escape_ResourceRespecify and exit 155
// fires (the buffer respecify starts waiting again).
TEST(RemoteClientControls, ABufferRespecifyTakesNoReplyWhileATextureOneStillDoes) {
    const auto child = RunInChild([] {
        ::setenv("MOBILEGL_PIPE_STATS", "1", 1);
        CapsPeer backend;
        StartControlSession(&backend);
        MG_Config::Features.PipePush |= kMGPipeSubsystemResources |
                                        kMGPipeSubsystemTextureResources | kMGPipeSubsystemSamplers;
        MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
        const Uint32 respecifyOp = static_cast<Uint32>(MGPWireOp::ResourceRespecify);
        Uint8 seed[64];
        std::memset(seed, 0x5a, sizeof(seed));
        // ---- the BUFFER half: records out, replies not taken ----
        GLuint buffer = 0;
        MG_Impl::GLImpl::GenBuffers(1, &buffer);
        MG_Impl::GLImpl::BindBuffer(0x8892, buffer);
        MG_Impl::GLImpl::BufferData(0x8892, sizeof(seed), seed, 0x88E4);
        MGP_FILL(DrawArrays);
        const Uint64 recordsBefore = Transport::LinkMetricsRecordsFor(respecifyOp);
        const Uint64 waitsBefore = Transport::LinkMetricsReplyWaitsFor(respecifyOp);
        MG_Impl::GLImpl::BufferData(0x8892, sizeof(seed) * 2, seed, 0x88E4);
        MGP_FILL(DrawArrays);
        if (Transport::LinkMetricsRecordsFor(respecifyOp) <= recordsBefore) ::_exit(154);
        if (Transport::LinkMetricsReplyWaitsFor(respecifyOp) != waitsBefore) ::_exit(155);
        // ---- the TEXTURE half, part 1: the PREDICATE both halves read (P12/R3) ----
        // An unpublished texture still owes its answer - that is the first-use confirmation - and a
        // buffer never does. Asserted directly rather than through a record, because the only way to
        // produce an unpublished texture on the wire is to make the server REFUSE its create, and a
        // malformed descriptor would trip the applier's wire report instead of testing this.
        if (MG_Pipe::MGPipeInstalledArm() != MG_Pipe::MGPipeRouteArm::kClientWire) ::_exit(157);
        MGPResourceDesc unpublished{};
        unpublished.Target = static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2D);
        if (!MG_Pipe::MGPipeResourceRespecifyWantsItsReply(unpublished)) ::_exit(158);
        MGPResourceDesc bufferDesc{};
        if (MG_Pipe::MGPipeResourceRespecifyWantsItsReply(bufferDesc)) ::_exit(159);
        // ---- the TEXTURE half, part 2: CONFIRMED, end to end ----
        // GenTextures + a storage definition publishes the object (its create is accepted, and the
        // create is blocking). A SECOND storage definition on that same object is a real change, so
        // the record must go out - and must not take a reply.
        Uint8 pixels[8 * 8 * 4];
        std::memset(pixels, 0x21, sizeof(pixels));
        GLuint texture = 0;
        MG_Impl::GLImpl::GenTextures(1, &texture);
        MG_Impl::GLImpl::BindTexture(0x0DE1, texture);
        MG_Impl::GLImpl::TexImage2D(0x0DE1, 0, 0x8058, 8, 8, 0, 0x1908, 0x1401, pixels);
        MGP_FILL(DrawArrays);
        const Uint64 confirmedRecords = Transport::LinkMetricsRecordsFor(respecifyOp);
        const Uint64 confirmedWaits = Transport::LinkMetricsReplyWaitsFor(respecifyOp);
        MG_Impl::GLImpl::TexImage2D(0x0DE1, 0, 0x8058, 16, 16, 0, 0x1908, 0x1401, pixels);
        MGP_FILL(DrawArrays);
        if (Transport::LinkMetricsRecordsFor(respecifyOp) <= confirmedRecords) ::_exit(160);
        if (Transport::LinkMetricsReplyWaitsFor(respecifyOp) != confirmedWaits) ::_exit(161);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}
// =====================================================================================
// P12: the CREATE WINDOW - the confirmation is deferred, not dropped
// =====================================================================================
//
// The create row is the last per-record round trip on the load path (4,536 of a frame's 4,543
// waits) and the one row whose answer IS an object's confirmation. It therefore defers: up to
// MOBILEGL_IPC_CREATE_WINDOW creates go out, the caller reads a provisional accept, and the
// answers are collected in order. A late DECLINED un-latches its object, which puts every later row
// for that object back on the blocking path - the repair that already existed, not a new one.
//
// ⚠️ THIS CASE RUNS ON THE IN-PROCESS ARM, WHICH IS ShmLink, AND SINCE P12 THAT MEANS THE WINDOW
// IS OFF HERE BY DESIGN - `CreateWindowEffective()` is gated on LinkRetainsReplies() precisely
// because a shared pool's slot is `seq % slotCount` and is reused, so a deferred answer may be
// gone with no way to recover it. What this case pins is therefore THAT GATE, and it is the
// assertion the device cannot make: set the knob as high as it goes and the row must STILL take the
// blocking path on a link that cannot retain.
//
// THE WINDOW'S OWN BEHAVIOUR IS PINNED ELSEWHERE, ON THE ARM IT SHIPS ON: StreamLinkTest's
// declared-answer cases for the store, and the stream-arm window case for the row. That split is
// not tidiness - three rounds of this feature were green here while being inert on the device,
// because these gates and the device run on different transports.
//
// WHAT IS STILL READ HERE, because it is transport-independent: the environment cannot raise the
// window past the array, the array cannot exceed half the reply pool, and the invariant holds after
// every create.
TEST(RemoteClientControls, TheWindowRefusesToDeferOnALinkThatCannotRetainAnswers) {
    const auto child = RunInChild([] {
        ::setenv("MOBILEGL_PIPE_STATS", "1", 1);
        CapsPeer backend;
        StartControlSession(&backend);
        MG_Config::Features.PipePush |= kMGPipeSubsystemResources |
                                        kMGPipeSubsystemTextureResources | kMGPipeSubsystemSamplers;
        MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
        const Uint32 createOp = static_cast<Uint32>(MGPWireOp::ResourceCreate);
        Uint8 pixels[4 * 4 * 4];
        std::memset(pixels, 0x21, sizeof(pixels));
        // ---- the clamp and the ceiling, before a single record goes out ----
        // BOTH ARMS SET THE KNOB rather than inheriting it: this case is about what the code does
        // with a configured window, and not a claim about which value ships (the default is 2 -
        // Config.h carries the device numbers).
        // WHAT THE OPERATOR WROTE IS NOT WHAT THE CODE MAY USE. With the clamp gone a configured
        // window of 99 is compared against a count that the ring keeps below its own size, so the
        // fallback never fires and the push index runs off the array.
        MG_Config::Ipc.CreateWindow = 99;
        if (ClientCreateWindowEffective() != ClientCreateWindowArraySize()) ::_exit(170);
        MG_Config::Ipc.CreateWindow = 8;
        if (ClientCreateWindowEffective() != 8u) ::_exit(171);
        // AND THE ARRAY IS SIZED FROM THE STORE IT SHARES, not from a second literal (P12). This
        // line used to read "at most half the REPLY POOL", which was true while an answer was read
        // back out of a slot pool addressed seq % slotCount - and the window does not run on such
        // a link: CreateWindowEffective() is gated on ILink::RetainsReplies, false for ShmLink, so
        // the pool-addressed transport holds a window of 1. On the link where it does run, an
        // answer waits in the declared-answer store instead, so the bound that matters is the
        // store's - read from the constant rather than restated, for the reason the pool version
        // read kDefaultReplySlotCount. WireTables.cpp's static_assert is the compile-time half of
        // the same sentence, deliberately twice: that one is the guard, this one is what it is for.
        if (ClientCreateWindowArraySize() > Transport::StreamLink::kRetainedAnswersMax / 2)
            ::_exit(172);

        // TWELVE OBJECTS, ONE CREATE EACH, AND THE INVARIANT READ AFTER EVERY ONE. This is the
        // reading a silent overwrite cannot fake: the count stays at the ceiling while an entry is
        // replaced underneath it.
        Uint32 highWater = 0;
        const auto burst = [&](Int32 count) {
            for (Int32 i = 0; i < count; ++i) {
                GLuint texture = 0;
                MG_Impl::GLImpl::GenTextures(1, &texture);
                MG_Impl::GLImpl::BindTexture(0x0DE1, texture);
                MG_Impl::GLImpl::TexImage2D(0x0DE1, 0, 0x8058, 4, 4, 0, 0x1908, 0x1401, pixels);
                MGP_FILL(DrawArrays);
                if (ClientCreateWindowPending() > ClientCreateWindowEffective()) ::_exit(173);
                if (ClientCreateWindowPending() > highWater) highWater = ClientCreateWindowPending();
            }
        };

        // ---- THE KNOB IS AS HIGH AS IT GOES, AND THE ROW MUST STILL BLOCK ----
        MG_Config::Ipc.CreateWindow = ClientCreateWindowArraySize();
        const Uint64 recordsBefore = Transport::LinkMetricsRecordsFor(createOp);
        const Uint64 waitsBefore = Transport::LinkMetricsReplyWaitsFor(createOp);
        burst(12);
        const Uint64 records = Transport::LinkMetricsRecordsFor(createOp) - recordsBefore;
        const Uint64 waits = Transport::LinkMetricsReplyWaitsFor(createOp) - waitsBefore;
        std::printf("non-retaining link: window=%u records=%llu waits=%llu taken=%llu pending=%u high_water=%u\n",
                    static_cast<unsigned>(MG_Config::Ipc.CreateWindow),
                    static_cast<unsigned long long>(records),
                    static_cast<unsigned long long>(waits),
                    static_cast<unsigned long long>(ClientCreateWindowTaken()),
                    static_cast<unsigned>(ClientCreateWindowPending()),
                    static_cast<unsigned>(highWater));
        std::fflush(nullptr);
        if (records < 12) ::_exit(164);
        // NOTHING WAS DEFERRED AT ALL, and nothing was left outstanding: on a link that cannot
        // retain, a window that deferred would be holding answers it may never get back.
        if (ClientCreateWindowTaken() != 0) ::_exit(177);
        if (ClientCreateWindowPending() != 0) ::_exit(178);
        // AND EVERY CREATE PAID ITS ROUND TRIP, which is the pre-window shape exactly. This is the
        // assertion that goes red if the capability gate is removed.
        if (waits != records) ::_exit(179);

        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

// =====================================================================================
// P12: A REFUSED CREATE LEAVES THE WINDOW - read ON THE LATCH AND ON THE SUSPECT LIST
// =====================================================================================
//
// The answer is what PUBLISHES an object, so the one thing a refusal must never do is leave the
// client believing it landed: every row that reads the latch would then act on a record the server
// does not hold. This case reads the whole chain - the refusal leaves the object UNLATCHED, its
// next storage definition BLOCKS and is refused again, and giving the family its consumer back
// HEALS it (the create is accepted and the latch comes back).
//
// ⚠️ RUNS ON THE IN-PROCESS ARM (ShmLink), WHERE THE WINDOW IS OFF, so what repairs the object
// here is the LATCH ALONE - the suspect table belongs to the window's provisional accept and the
// drain is the only thing that fills it, which this case now asserts by finding it EMPTY. The
// window's own refusal path is pinned on the arm it ships on (StreamLinkTest's declared-answer
// cases and the stream-arm window case); see the note on the gate above for why that split is not
// tidiness.
//
// THE REFUSAL IS MADE REAL RATHER THAN SIMULATED, and it is the exact case the audit found the two
// ends can disagree about: the applier asks for bit 10 while the client's mirror still says the
// family is live, so every create the client emits is refused. Narrowing the server's consumed mask
// reproduces that without touching the client's mirror.
TEST(RemoteClientControls, ARefusedCreateUnlatchesItsObjectAndItsNextCreateBlocks) {
    const auto child = RunInChild([] {
        ::setenv("MOBILEGL_PIPE_STATS", "1", 1);
        CapsPeer backend;
        StartControlSession(&backend);
        MG_Config::Features.PipePush |= kMGPipeSubsystemResources |
                                        kMGPipeSubsystemTextureResources | kMGPipeSubsystemSamplers;
        MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
        // THE WINDOW IS SET HERE, not inherited: this case is about what the window does with a
        // refusal, so it must have one whatever the suite was launched with.
        MG_Config::Ipc.CreateWindow = 4;
        const Uint32 createOp = static_cast<Uint32>(MGPWireOp::ResourceCreate);
        // ROOM FOR THE LARGEST LEVEL THIS CASE DEFINES. The storage definition's texels travel as
        // resource_subdata out of the drain list, so the pointer is read after this call returns.
        Uint8 pixels[64 * 64 * 4];
        std::memset(pixels, 0x21, sizeof(pixels));
        Srv::ServerSessionInstance().SetConsumedSubsystems(
            kMGPipeSubsystemsMigratedAtP4a & ~kMGPipeSubsystemTextureResources);
        // ---- one object we can name, then enough neighbours to fill and drain the window ----
        // The drain is what READS a deferred answer, so the refusal of the first create is only
        // discovered once later creates have pushed the ring past its ceiling.
        const auto defineStorage = [&](GLuint name, Int32 extent) {
            MG_Impl::GLImpl::BindTexture(0x0DE1, name);
            MG_Impl::GLImpl::TexImage2D(0x0DE1, 0, 0x8058, extent, extent, 0, 0x1908, 0x1401, pixels);
            MGP_FILL(DrawArrays);
        };
        GLuint refused = 0;
        MG_Impl::GLImpl::GenTextures(1, &refused);
        defineStorage(refused, 8);
        for (int i = 0; i < 6; ++i) {
            GLuint filler = 0;
            MG_Impl::GLImpl::GenTextures(1, &filler);
            defineStorage(filler, 8);
        }
        // ---- (a) THE LATCH IS CLEAR, AND THE WINDOW NEVER RAN HERE AT ALL ----
        const auto refusedObject = MG_State::pGLContext->GetTextureObject(refused);
        if (refusedObject == nullptr) ::_exit(174);
        const MG_Pipe::MGPipeHandle refusedHandle =
            MG_Pipe::MGPipeTextureEmitterInstance().FindTexture(*refusedObject);
        if (MG_Pipe::MGPipeHandleIsNull(refusedHandle)) ::_exit(175);
        // THE DIRECT READING, which is what the converted rows read: not "the create was refused"
        // but "this object is not published", which is the fact every fire-and-forget row acts on.
        if (MG_Pipe::MGPipeHandleIsPublished(MG_Pipe::MGPipeKind::Texture, refusedHandle)) ::_exit(176);
        // AND THE WINDOW'S OWN BOOKS ARE EMPTY, because this arm cannot retain answers and the
        // window therefore never deferred - `refusals` and `suspects` are both filled by the
        // drain and by nothing else. A non-zero reading here would mean the capability gate had
        // been removed and the row was deferring on a link that cannot hold an answer.
        if (ClientCreateWindowRefusals() != 0) ::_exit(177);
        const Uint32 suspectsBefore = ClientCreateWindowSuspects();
        if (suspectsBefore != 0) ::_exit(178);
        if (ClientCreateWindowPending() != 0) ::_exit(183);
        // ---- (b) AND ITS NEXT CREATE BLOCKS: ONE WAIT PER CREATE, WITH THE WINDOW WIDE OPEN ----
        // The window has room (the ceiling is 4 and this case has just drained it), so a deferring
        // row would pay ZERO waits here. Every create this restorage emits pays one instead, which
        // is the blocking path and nothing else.
        const Uint64 createRecordsBefore = Transport::LinkMetricsRecordsFor(createOp);
        const Uint64 createWaitsBefore = Transport::LinkMetricsReplyWaitsFor(createOp);
        defineStorage(refused, 16);
        const Uint64 suspectRecords = Transport::LinkMetricsRecordsFor(createOp) - createRecordsBefore;
        const Uint64 suspectWaits = Transport::LinkMetricsReplyWaitsFor(createOp) - createWaitsBefore;
        std::printf("refused create: suspect_records=%llu suspect_waits=%llu suspects=%u\n",
                    static_cast<unsigned long long>(suspectRecords),
                    static_cast<unsigned long long>(suspectWaits),
                    static_cast<unsigned>(suspectsBefore));
        std::fflush(nullptr);
        if (suspectRecords == 0) ::_exit(179);
        if (suspectWaits < suspectRecords) ::_exit(180);
        // ---- (c) THE HEAL: GIVE THE FAMILY ITS CONSUMER BACK AND RE-DEFINE THE SAME OBJECT ----
        // The create still blocks (the latch is still clear), now it LANDS, and landing is what
        // re-publishes the object for every row that reads the latch.
        Srv::ServerSessionInstance().SetConsumedSubsystems(kMGPipeSubsystemsMigratedAtP4a);
        defineStorage(refused, 32);
        std::printf("refused create healed: published=%d suspects=%u\n",
                    static_cast<int>(MG_Pipe::MGPipeHandleIsPublished(MG_Pipe::MGPipeKind::Texture,
                                                                     refusedHandle)),
                    static_cast<unsigned>(ClientCreateWindowSuspects()));
        std::fflush(nullptr);
        if (!MG_Pipe::MGPipeHandleIsPublished(MG_Pipe::MGPipeKind::Texture, refusedHandle)) ::_exit(181);
        if (ClientCreateWindowSuspects() != suspectsBefore) ::_exit(182);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

// =====================================================================================
// P12 (handoff-2 §3.3): WHAT THE DROPPED RECORDS ARE REPAIRED BY - AND WHAT THEY ARE NOT
// =====================================================================================
//
// MGPipeApplierReleaseObjectRecords is the served context's teardown and the ONLY scope that
// empties the applier's object records while the frontend objects live on in the share group.
// handoff-2 §3 calls the guard for it "latent" because no production code calls it; what has to
// hold when one does is that the client REPAIRS it, and the repair on paper is the one that
// already existed: the applier REFUSES a record for a handle it holds nothing for, and
// RespecifyOnce turns that refusal into a create plus one retry.
//
// THIS CASE MEASURES WHETHER THAT STILL HAPPENS, AND THE ANSWER IS NO - the P12 first-use work
// took the signal away. The refusal can only reach the emitter through an answer the row READS,
// and both rows that used to read it now answer themselves: the respecify is fire-and-forget for
// a "confirmed" object (MGPipeResourceRespecifyWantsItsReply) and the params row posts a
// PROVISIONAL kStatusOk for one (Wire_SetTextureParams), while "confirmed" is
// MGPipeHandleIsPublished - WHICH STAYS TRUE AFTER THE RECORDS ARE DROPPED, because it answers
// "did a create GO OUT", not "does the applier still hold it". So the applier is left holding
// nothing, the client is left believing the object is published, and every later fire-and-forget
// row for it is answered provisionally. handoff-2 §3.1's "clearing the latch alone is not enough"
// is the smaller half of this; the larger half is that nothing clears the latch, and nothing can
// until the applier can say so (its own §3.2 option 3, the reverse channel).
//
// THE ASSERTIONS BELOW ARE THEREFORE A DEBT, NOT A SPEC, and they are written to be LOUD about it:
// they pin today's numbers so that whoever lands the signal sees THIS case go red and knows the
// repair started working. notes/p12/UNPUBLISH-GUARD-SIGNALS.md §4-§6 has the rest.
TEST(RemoteClientControls, DroppedApplierRecordsAreNotRepairedTodayAndThisCaseIsTheDebt) {
    const auto child = RunInChild([] {
        ::setenv("MOBILEGL_PIPE_STATS", "1", 1);
        CapsPeer backend;
        StartControlSession(&backend);
        MG_Config::Features.PipePush |= kMGPipeSubsystemResources |
                                        kMGPipeSubsystemTextureResources | kMGPipeSubsystemSamplers;
        MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
        const Uint32 createOp = static_cast<Uint32>(MGPWireOp::ResourceCreate);
        Uint8 pixels[16 * 16 * 4];
        std::memset(pixels, 0x21, sizeof(pixels));
        // The applier's own table, read where it is legal to read it: on the apply thread, through
        // the same probe channel the rest of this file uses.
        const auto applierTextureRecords = [] {
            Uint64 count = 0;
            (void)Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void* self) {
                *static_cast<Uint64*>(self) =
                    static_cast<Uint64>(MG_Pipe::MGPipeApplier().TextureResources.size());
                return MOBILEGL_OK;
            }, &count);
            return count;
        };
        GLuint texture = 0;
        MG_Impl::GLImpl::GenTextures(1, &texture);
        MG_Impl::GLImpl::BindTexture(0x0DE1, texture);
        MG_Impl::GLImpl::TexImage2D(0x0DE1, 0, 0x8058, 8, 8, 0, 0x1908, 0x1401, pixels);
        MGP_FILL(DrawArrays);
        const auto object = MG_State::pGLContext->GetTextureObject(texture);
        if (object == nullptr) ::_exit(183);
        const MG_Pipe::MGPipeHandle handle = MG_Pipe::MGPipeTextureEmitterInstance().FindTexture(*object);
        if (MG_Pipe::MGPipeHandleIsNull(handle)) ::_exit(184);
        // ---- IT IS REALLY THERE, AND THE LATCH AGREES ----
        if (applierTextureRecords() == 0) ::_exit(185);
        if (!MG_Pipe::MGPipeHandleIsPublished(MG_Pipe::MGPipeKind::Texture, handle)) ::_exit(186);
        // ---- THE DROP, ON THE APPLIER'S OWN THREAD ----
        if (Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void*) {
                MG_Pipe::MGPipeApplierReleaseObjectRecords();
                return MOBILEGL_OK;
            }, nullptr) != MOBILEGL_OK) ::_exit(187);
        if (applierTextureRecords() != 0) ::_exit(188);
        // THE HAZARD IN ONE LINE: the fact every converted row acts on is still true, and it is
        // answering a question about the PAST ("a create went out") to a reader that needs the
        // present ("the applier holds it").
        if (!MG_Pipe::MGPipeHandleIsPublished(MG_Pipe::MGPipeKind::Texture, handle)) ::_exit(189);
        // ---- THE RESHAPE THAT WOULD HAVE BEEN REFUSED ----
        const Uint64 createRecordsBefore = Transport::LinkMetricsRecordsFor(createOp);
        MG_Impl::GLImpl::TexImage2D(0x0DE1, 0, 0x8058, 16, 16, 0, 0x1908, 0x1401, pixels);
        MGP_FILL(DrawArrays);
        const Uint64 createRecords = Transport::LinkMetricsRecordsFor(createOp) - createRecordsBefore;
        const Uint64 held = applierTextureRecords();
        const Bool published = MG_Pipe::MGPipeHandleIsPublished(MG_Pipe::MGPipeKind::Texture, handle);
        std::printf("dropped-records probe: creates=%llu applier_records=%llu published=%d\n",
                    static_cast<unsigned long long>(createRecords),
                    static_cast<unsigned long long>(held), static_cast<int>(published));
        std::fflush(nullptr);
        // TODAY'S NUMBERS, AND THE DEBT: no create was emitted (nothing was refused, because the
        // row did not read its answer), the applier holds nothing, and the client still says the
        // object is published. INVERT THESE THREE WHEN THE SIGNAL LANDS.
        if (createRecords != 0) ::_exit(190);
        if (held != 0) ::_exit(191);
        if (!published) ::_exit(192);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}
#endif
