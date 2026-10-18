// MobileGL - MobileGL/MG_Test/Wire/RemoteClientE1Controls.inc
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

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
    Bool OnApplierReset(const MGPApplierReset&) override { return Apply(MGPWireOp::ApplierReset); }
    Bool OnClear(const MGPClear&) override { return Apply(MGPWireOp::Clear); }
    void Install() {
        if (Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void* self) {
            auto& decoder = Srv::ServerSessionInstance().Applier().*PeerMember(DecoderTag{});
            decoder.SetVerbSink(static_cast<E1BoundaryPeer*>(self));
            return MOBILEGL_OK;
        }, this) != MOBILEGL_OK) ::_exit(82);
    }
};

// How the probe's record leaves the client. Rule is EmitAndWait, whose wait rule the cases
// test. PublishOnly encodes through the session's own encoder and publishes with no wait at all,
// so a case can show the probe reads a missing wait as one.
enum class E1Emit { Rule, PublishOnly };

void CheckE1Boundary(Bool serverCap, MGPWireOp op, Bool expectWait, const char* diagnostic,
                     E1Emit emit = E1Emit::Rule) {
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
        Srv::ServerSessionInstance().SetBackend(ControlCapsPeer());
        auto& session = ClientSessionInstance();
        if (session.Start(MG_Config::TransportMode::InProcess, {}) != MOBILEGL_OK) ::_exit(81);
        auto& producer = session.Producer();
        BudgetBell bell(producer.SelfDoorbell());
        E1BoundaryPeer peer(bell, op);
        peer.Install();
        const Uint64 before = session.Control()->submittedSeq.load();
        producer.*PeerMember(ProducerBellTag{}) = &bell;
        const MGPApplierReset reset{};
        const MGPClear clear{};
        const void* payload = op == MGPWireOp::ApplierReset ? static_cast<const void*>(&reset) : &clear;
        const Uint64 payloadBytes = op == MGPWireOp::ApplierReset ? sizeof(reset) : sizeof(clear);
        // P15: applier_reset is the one kWaitApplied row left (generate_mipmap went async).
        Uint64 seq = Codec::kInvalidSeq;
        if (emit == E1Emit::PublishOnly) {
            seq = session.Encoder().EncodeRecord(op, payload, payloadBytes);
            if (seq == Codec::kInvalidSeq) ::_exit(87);
            producer.PublishAndNotify(seq);
        } else {
            seq = session.EmitAndWait(op, payload, payloadBytes, nullptr, 0, nullptr, 0, nullptr);
        }
        // Observe BEFORE allowing the peer to complete a non-waiting emission.
        const Bool parked = bell.entered.load();
        const Bool appliedBeforeReturn = session.Control()->Progress.appliedSeq.load() >= seq;
        peer.clientReturned.store(true);
        producer.*PeerMember(ProducerBellTag{}) = bell.original;
        if (session.WaitForApplied(seq, 2000) != Transport::SessionWait::Reached) ::_exit(85);
        if (seq <= before || peer.seen.load() != 1 || session.Control()->Progress.appliedSeq.load() < seq) ::_exit(86);
        const Bool correct = expectWait ? parked && appliedBeforeReturn : !parked && !appliedBeforeReturn;
        std::printf("E1 observation: op=%s cap=%u barrier=%u parked=%u applied_before_return=%u emitted=1 peer_seen=1\n",
                    op == MGPWireOp::ApplierReset ? "ApplierReset" : "Clear",
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
    ASSERT_EQ(MGPipeWaitClassFor(MGPWireOp::ApplierReset), kWaitApplied);
    CheckE1Boundary(true, MGPWireOp::ApplierReset, true,
                    "E1 wait boundary: ApplierReset returned before apply");
#else
    GTEST_SKIP() << "E1 requires POSIX child isolation";
#endif
}

// The control for the case above: the same ApplierReset, published with no wait. The probe has
// to read it as not parked, or a wait that never happened would pass the cases that must wait.
TEST(RemoteWaitBoundaryControl, AnUnwaitedPublishIsSeenAsNoWait) {
#if MGTEST_HAVE_FORK
    ASSERT_EQ(MGPipeWaitClassFor(MGPWireOp::ApplierReset), kWaitApplied);
    CheckE1Boundary(true, MGPWireOp::ApplierReset, false,
                    "E1 control: an ApplierReset published without a wait was observed as a wait",
                    E1Emit::PublishOnly);
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
