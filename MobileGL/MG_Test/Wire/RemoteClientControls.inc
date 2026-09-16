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
        if (Srv::ServerLoopInstance().RunOnApplyThread([](void* self) {
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
    struct WaitCase { Uint64 timeoutNs; MGPWireOp op; Uint32 expectedParkMs; };
    const WaitCase cases[] = {
        {0, MGPWireOp::FenceWait, 30000},
        {1, MGPWireOp::FenceWait, 30001},
        {60000000000ull, MGPWireOp::FenceWait, 90000},
        {~Uint64(0), MGPWireOp::FenceWait, Transport::kWaitForever - 1},
        {~Uint64(0), MGPWireOp::FenceWaitServer, 30000},
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
            // scheduler delay, but neither a 30s clamp nor an unbounded sentinel.
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

TEST(RemoteClientControls, AnIdenticalRepeatedMakeCurrentRepublishesNothing) {
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
        if (!remote.MakeEGLCurrent(dpy, surface, surface, ctx)) ::_exit(88);
        if (CapsMirrorInstance().Generation() != generation) ::_exit(89);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
}

TEST(RemoteClientControls, ADifferentTupleMakeCurrentIsAdoptedWithoutAPumpOrPresent) {
    const auto child = RunInChild([] {
        StartControlSession();
        if (Srv::ServerLoopInstance().RunOnApplyThread([](void*) {
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
        if (Srv::ServerLoopInstance().RunOnApplyThread([](void*) {
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
        Srv::ServerLoopInstance().RunOnApplyThread([](void* self) {
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
        Srv::ServerLoopInstance().RunOnApplyThread([](void*) {
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
        if (ClientSessionInstance().Control()->retiredSeq.load() >= prior) ::_exit(111);
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
        if (session.Control()->retiredSeq.load() >= prior) ::_exit(113);
        retirementRetryStarted.store(true);
        session.Encoder().StageBytes(bytes.data(), bytes.size());
        if (!retirementWaitObserved.load() || session.Encoder().StageReclaimWaits() != 1) ::_exit(114);
        session.Stop();
    });
    ExpectChildSuccess(child);
}
#endif
