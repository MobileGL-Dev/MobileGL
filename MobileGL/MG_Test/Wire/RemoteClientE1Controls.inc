// E1: real transport wait boundaries, with a peer that cannot complete until
// either the client parks or its emission has already returned. No GPU or
// scheduling-delay assumption decides whether the wait was observed.
#if MGTEST_HAVE_FORK
namespace {
struct E1BoundaryPeer : Codec::WireVerbSink {
    BudgetBell& bell;
    std::atomic<Bool> clientReturned{false};
    std::atomic<Uint32> seen{0};
    MGPWireOp expected;
    explicit E1BoundaryPeer(BudgetBell& value, MGPWireOp op) : bell(value), expected(op) {}

    Bool Apply(MGPWireOp op) {
        if (op != expected) ::_exit(84);
        ++seen;
        while (!bell.entered.load() && !clientReturned.load()) std::this_thread::yield();
        return true;
    }
    Bool OnGenerateMipmap(const MGPMipPlan&) override { return Apply(MGPWireOp::GenerateMipmap); }
    Bool OnClear(const MGPClear&) override { return Apply(MGPWireOp::Clear); }
    void Install() {
        if (Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void* self) {
            auto& decoder = Srv::ServerSessionInstance().Applier().*PeerMember(DecoderTag{});
            decoder.SetVerbSink(static_cast<E1BoundaryPeer*>(self));
            return MOBILEGL_OK;
        }, this) != MOBILEGL_OK) ::_exit(82);
    }
};

void CheckE1Boundary(Bool serverCap, MGPWireOp op, Bool expectWait, const char* diagnostic) {
    const auto child = RunInChild([=] {
        ::alarm(15);
        const char* barrier = std::getenv("MOBILEGL_IPC_VERB_BARRIER");
        if (barrier && std::strcmp(barrier, "0") && std::strcmp(barrier, "1")) ::_exit(80);
        MG_Config::Ipc.VerbBarrier = barrier ? static_cast<Uint32>(barrier[0] - '0') : 1;
        MG_Config::Ipc.BatchWaits = 1;
        MG_Config::Ipc.RunAhead = 1;
        MG_Config::Ipc.SpinUs = 0;
        MG_Config::Transport = MG_Config::TransportMode::InProcess;
        const Uint64 caps = serverCap ? static_cast<Uint64>(kCapRunAheadApply) : 0;
        Srv::ServerSessionInstance().SetCapabilityBits(caps);
        Srv::ServerSessionInstance().SetConsumedSubsystems(kMGPipeSubsystemsMigratedAtP4a);
        AdoptSnapshot(MakeSnapshot(kMGPipeSubsystemsMigratedAtP4a, caps));
        auto& session = ClientSessionInstance();
        if (session.Start(MG_Config::TransportMode::InProcess, {}) != MOBILEGL_OK) ::_exit(81);
        auto& producer = session.Producer();
        BudgetBell bell(producer.SelfDoorbell());
        E1BoundaryPeer peer(bell, op);
        peer.Install();
        const Uint64 before = session.Control()->submittedSeq.load();
        producer.*PeerMember(ProducerBellTag{}) = &bell;
        const MGPMipPlan mip{};
        const MGPClear clear{};
        const Uint64 seq = op == MGPWireOp::GenerateMipmap
            ? session.EmitAndWait(op, &mip, sizeof(mip), nullptr, 0, nullptr, 0, nullptr)
            : session.EmitAndWait(op, &clear, sizeof(clear), nullptr, 0, nullptr, 0, nullptr);
        // Observe BEFORE allowing the peer to complete a non-waiting emission.
        const Bool parked = bell.entered.load();
        const Bool appliedBeforeReturn = session.Control()->appliedSeq.load() >= seq;
        peer.clientReturned.store(true);
        producer.*PeerMember(ProducerBellTag{}) = bell.original;
        if (session.WaitForApplied(seq, 2000) != Transport::SessionWait::Reached) ::_exit(85);
        if (seq <= before || peer.seen.load() != 1 || session.Control()->appliedSeq.load() < seq) ::_exit(86);
        const Bool correct = expectWait ? parked && appliedBeforeReturn : !parked && !appliedBeforeReturn;
        std::printf("E1 observation: op=%s cap=%u barrier=%u parked=%u applied_before_return=%u emitted=1 peer_seen=1\n",
                    op == MGPWireOp::GenerateMipmap ? "GenerateMipmap" : "Clear",
                    static_cast<unsigned>(serverCap), MG_Config::Ipc.VerbBarrier,
                    static_cast<unsigned>(parked), static_cast<unsigned>(appliedBeforeReturn));
        std::fflush(stdout);
        session.Stop();
        ::_exit(correct ? 0 : 101);
    });
    ASSERT_TRUE(WIFEXITED(child.Status)) << DescribeStatus(child) << '\n' << child.Log;
    const int code = WEXITSTATUS(child.Status);
    ASSERT_TRUE(code == 0 || code == 101) << "E1 probe/setup failed: " << DescribeStatus(child) << '\n' << child.Log;
    EXPECT_EQ(code, 0) << diagnostic;
}
} // namespace
#endif

TEST(RemoteWaitBoundaryControl, AppliedClassWaitsWithRunAheadCap) {
#if MGTEST_HAVE_FORK
    ASSERT_EQ(MGPipeWaitClassFor(MGPWireOp::GenerateMipmap), kWaitApplied);
    CheckE1Boundary(true, MGPWireOp::GenerateMipmap, true,
                    "E1 wait boundary: GenerateMipmap returned before apply");
#else
    GTEST_SKIP() << "E1 requires POSIX child isolation";
#endif
}

TEST(RemoteWaitBoundaryControl, MissingServerCapKeepsClearLockstep) {
#if MGTEST_HAVE_FORK
    ASSERT_EQ(MGPipeWaitClassFor(MGPWireOp::Clear), kWaitNone);
    CheckE1Boundary(false, MGPWireOp::Clear, true,
                    "E1 wait boundary: Clear without server cap returned before apply");
#else
    GTEST_SKIP() << "E1 requires POSIX child isolation";
#endif
}

TEST(RemoteWaitBoundaryControl, ServerCapAllowsClearToRunAhead) {
#if MGTEST_HAVE_FORK
    ASSERT_EQ(MGPipeWaitClassFor(MGPWireOp::Clear), kWaitNone);
    CheckE1Boundary(true, MGPWireOp::Clear, false,
                    "E1 positive boundary: Clear with server cap waited for apply");
#else
    GTEST_SKIP() << "E1 requires POSIX child isolation";
#endif
}
