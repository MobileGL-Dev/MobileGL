// P11 B2 (MG_Remote/CONTRACT-P11.md B2): T0'S PROTOCOL ON THE HOST, over a live inproc session.
//
// A host has no AHardwareBuffer, so what these prove is the protocol, the negotiation and the
// fallback - never GPU correctness, which is the device's (B2's device gates). Two halves:
//   * the real host platform: every T0 ask falls back to T2 by name (no AHB, the allow switch,
//     knob 1), and never dies - A1's handshake Fatal for SharedSegments + 0/1 is gone;
//   * a STAND-IN platform (a memfd for the AHB, AdoptT0::SetPlatformForTest) plus a stand-in
//     importer (MGPipeResourceOps::ImportExternal): the whole T0 path runs - the grant, the caps
//     bit, the client's store, the Offer on the real aux channel matched by seq, the import - and
//     the store lands with the client's bytes, including when its Offer comes after its record;
//     a lost Offer is a named DECLINED within the bounded wait, not a hang.
#if MGTEST_HAVE_FORK
#include <MG_Remote/Transport/AdoptT0.h>
#include <MG_Remote/Transport/AdoptTier.h>
#include <MG_Remote/Transport/FdPassing.h>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/socket.h>

namespace {
namespace T0 = MobileGL::MG_Remote::Transport::AdoptT0;

constexpr Uint64 kT0Bytes = 64 * 1024;

// ---- the stand-in AHB: a memfd, mapped shared, reference-counted -------------------------
struct FakeAhb {
    int fd = -1;
    void* map = nullptr;
    Uint64 size = 0;
    std::atomic<int> refs{1};
};
void FakeDrop(FakeAhb* f) {
    if (f != nullptr && --f->refs == 0) {
        if (f->map != nullptr) ::munmap(f->map, f->size);
        if (f->fd >= 0) ::close(f->fd);
        delete f;
    }
}
bool FakeHasAhb() { return true; }
bool FakeAllocate(std::uint64_t size, const void* seed, T0::HeldStore& out, std::string& why) {
    auto* f = new FakeAhb;
    f->fd = static_cast<int>(::syscall(SYS_memfd_create, "t0-fake", 1u /*MFD_CLOEXEC*/));
    if (f->fd < 0 || ::ftruncate(f->fd, static_cast<off_t>(size)) != 0) {
        why = "memfd";
        FakeDrop(f);
        return false;
    }
    f->size = size;
    f->map = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, f->fd, 0);
    if (f->map == MAP_FAILED) {
        f->map = nullptr;
        why = "mmap";
        FakeDrop(f);
        return false;
    }
    if (seed != nullptr) std::memcpy(f->map, seed, size);
    out.ahb = f;
    out.ptr = f->map;
    out.size = size;
    return true;
}
int FakeMakeHop(const T0::HeldStore* store, std::string& why) {
    int pair[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
        why = "socketpair";
        return -1;
    }
    if (store != nullptr && store->ahb != nullptr) {
        auto* f = static_cast<FakeAhb*>(store->ahb);
        const Uint64 size = f->size;
        if (Transport::FdPassing::SendFd(pair[0], f->fd, MobileGLByteSpan{&size, sizeof(size)}) != MOBILEGL_OK) {
            why = "send";
            ::close(pair[0]);
            ::close(pair[1]);
            return -1;
        }
    }
    ::close(pair[0]);
    return pair[1];
}
void FakeReleaseHeld(T0::HeldStore& store) {
    FakeDrop(static_cast<FakeAhb*>(store.ahb));
    store = T0::HeldStore{};
}
bool FakeReceive(int hopFd, std::uint64_t size, void** outAhb, std::string& why) {
    *outAhb = nullptr;
    int fd = -1;
    Uint8 sideband[Transport::FdPassing::kMaxSidebandBytes] = {};
    std::uint64_t sidebandSize = 0;
    if (Transport::FdPassing::ReceiveFd(hopFd, &fd, MobileGLMutableByteSpan{sideband, sizeof(sideband)},
                                        &sidebandSize, 1000) != MOBILEGL_OK) {
        why = "no stand-in handle on the hop";
        return false;
    }
    auto* f = new FakeAhb;
    f->fd = fd;
    f->size = size;
    f->map = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (f->map == MAP_FAILED) {
        f->map = nullptr;
        FakeDrop(f);
        why = "mmap";
        return false;
    }
    *outAhb = f;
    return true;
}
void FakeAcquire(void* ahb) { ++static_cast<FakeAhb*>(ahb)->refs; }
void FakeRelease(void* ahb) { FakeDrop(static_cast<FakeAhb*>(ahb)); }

const T0::PlatformForTest kFakePlatform{&FakeHasAhb,  &FakeAllocate, &FakeMakeHop, &FakeReleaseHeld,
                                        &FakeReceive, &FakeAcquire,  &FakeRelease};

// ---- the stand-in importer: records what it was handed, keeps its own reference ----------
struct FakeImports {
    MGPipeHandle handle{};
    Uint64 size = 0;
    Uint64 count = 0;
    std::vector<Uint8> bytes;
    void* held = nullptr;
};
FakeImports g_fakeImports;
Bool FakeImportExternal(MGPipeHandle res, void* ahb, Uint64 size) {
    auto* f = static_cast<FakeAhb*>(ahb);
    g_fakeImports.handle = res;
    g_fakeImports.size = size;
    ++g_fakeImports.count;
    const auto* first = static_cast<const Uint8*>(f->map);
    g_fakeImports.bytes.assign(first, first + size);
    T0::AcquireImported(ahb);
    g_fakeImports.held = ahb;
    return true;
}
Bool FakeSelfTest(char* why, Uint64 whyBytes) {
    std::snprintf(why, static_cast<size_t>(whyBytes), "stand-in platform (host protocol test, no GPU)");
    return true;
}
MGPipeResourceOps MakeFakeOps() {
    MGPipeResourceOps ops{};
    ops.ImportExternal = &FakeImportExternal;
    ops.SelfTestExternal = &FakeSelfTest;
    return ops;
}
MGPipeResourceOps g_fakeOps = MakeFakeOps();

// What the server's first native bind does for T0, on the apply thread like the real one: the
// grant (and its POST), then the caps that bind republishes - which the client's make-current
// adopts, as here.
void GrantAtFirstBind() {
    if (Srv::ServerLoopInstance().RunProbeOnApplyThreadForTesting([](void*) {
            Srv::ServerSessionInstance().SettleAdoptT0AtBind();
            return Srv::ServerSessionInstance().PublishCapsSnapshot();
        }, nullptr) != MOBILEGL_OK) ::_exit(120);
    (void)ClientSessionInstance().PumpControlPlane();
}

MGPHandleOnly MakeBufferStore(Uint32 slot, Uint64 bytes) {
    MGPResourceDesc desc{};
    desc.Resource = MGPipeHandle{slot, 1};
    desc.Target = MG_Pipe::kMGPipeResourceTargetBuffer;
    desc.Width = static_cast<Uint32>(bytes);
    desc.Height = desc.Depth = 1;
    if (!MGPipeRouteResourceCreate(desc)) ::_exit(121);
    (void)MGPipeRouteResourceRespecify(desc, nullptr);
    MGPHandleOnly handle{};
    handle.Handle = desc.Resource;
    handle.Kind = static_cast<Uint32>(MGPipeKind::Buffer);
    return handle;
}

std::vector<Uint8> Pattern(Uint64 bytes, Uint8 salt) {
    std::vector<Uint8> out(bytes);
    for (Uint64 i = 0; i < bytes; ++i) out[i] = static_cast<Uint8>((i * 131u + salt) & 0xFF);
    return out;
}

size_t CountOf(const std::string& log, const std::string& needle) {
    size_t n = 0;
    for (size_t at = log.find(needle); at != std::string::npos; at = log.find(needle, at + 1)) ++n;
    return n;
}

// The grant with the stand-ins, the store, and the map_persistent: returns the adopted pointer.
void* AdoptOneStore(Uint8 salt, std::vector<Uint8>& seed) {
    MG_Config::Ipc.AdoptTier = 0;
    T0::SetPlatformForTest(&kFakePlatform);
    MG_Pipe::MGPipeSetResourceOps(&g_fakeOps);
    StartControlSession();
    GrantAtFirstBind();
    seed = Pattern(kT0Bytes, salt);
    const MGPHandleOnly handle = MakeBufferStore(5, kT0Bytes);
    return MGPipeRouteMapPersistent(handle, kT0Bytes, seed.data());
}
} // namespace

// A1's "SharedSegments + 0 dies at the handshake" is GONE (ID-P11-14): knob 0 over shared segments
// ASKS T0 in the Hello, and a server that does not grant it (here: no bind ever ran, so no
// kCapAdoptT0) leaves the session on T2 with ONE named client line at the first map_persistent.
// RED-ONCE: restore the client's SessionFail in SettleAdoptTierAtHandshake -> the child dies
// before 'S'.
TEST(RemoteClientAdoptT0, KnobZeroOverSharedSegmentsAsksT0AndFallsBackToT2ByNameWithoutAGrant) {
    const auto child = RunInChild([] {
        MG_Config::Ipc.AdoptTier = 0;
        StartControlSession();
        if (!Srv::ServerSessionInstance().AdoptT0Asked()) ::_exit(130); // the Hello asked T0
        MGPHandleOnly handle{};
        handle.Kind = static_cast<Uint32>(MGPipeKind::Buffer);
        if (MGPipeRouteMapPersistent(handle, 64, nullptr) != nullptr) ::_exit(131);
        if (MGPipeRouteMapPersistent(handle, 64, nullptr) != nullptr) ::_exit(132);
        if (ClientSessionInstance().AdoptT0()) ::_exit(133);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
    EXPECT_EQ(CountOf(child.Log, "Refuse{AdoptT0Unavailable, \"no kCapAdoptT0\"}"), 1u) << child.Log;
    EXPECT_EQ(child.Log.find("Fatal{"), std::string::npos) << child.Log;
}

// Knob 1 names T1, which P11 closed: one named refusal, the Hello asks T2, never a Fatal.
TEST(RemoteClientAdoptT0, KnobOneIsRefusedByNameAndAsksT2) {
    const auto child = RunInChild([] {
        MG_Config::Ipc.AdoptTier = 1;
        StartControlSession();
        if (Srv::ServerSessionInstance().AdoptT0Asked()) ::_exit(140);
        MGPHandleOnly handle{};
        handle.Kind = static_cast<Uint32>(MGPipeKind::Buffer);
        if (MGPipeRouteMapPersistent(handle, 64, nullptr) != nullptr) ::_exit(141);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
    EXPECT_EQ(CountOf(child.Log, "Refuse{AdoptTierClosed, \"T1\"}"), 1u) << child.Log;
    EXPECT_EQ(child.Log.find("Fatal{"), std::string::npos) << child.Log;
}

// The host's own platform: the grant refuses by name (no AHB) and the client falls back by name.
TEST(RemoteClientAdoptT0, NoAhardwareBufferOnThisPlatformIsT2AndTwoNamedLines) {
    const auto child = RunInChild([] {
        MG_Config::Ipc.AdoptTier = 0;
        MG_Pipe::MGPipeSetResourceOps(&g_fakeOps); // a backend WITH the members: the platform decides
        StartControlSession();
        GrantAtFirstBind();
        if (Srv::ServerSessionInstance().AdoptT0()) ::_exit(150);
        if (ClientSessionInstance().AdoptT0()) ::_exit(151);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
    EXPECT_EQ(CountOf(child.Log, "Refuse{AdoptT0Unavailable, \"no AHardwareBuffer\"}"), 1u) << child.Log;
    EXPECT_EQ(CountOf(child.Log, "Refuse{AdoptT0Unavailable, \"no kCapAdoptT0\"}"), 1u) << child.Log;
}

// The server's allow switch off: T2 and a named line, even where T0 would otherwise work.
TEST(RemoteClientAdoptT0, TheServersAllowSwitchOffIsT2AndANamedLine) {
    const auto child = RunInChild([] {
        MG_Config::Ipc.AllowAdoptT0 = 0;
        std::vector<Uint8> seed;
        if (AdoptOneStore(0x11, seed) != nullptr) ::_exit(160);
        if (g_fakeImports.count != 0) ::_exit(161);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
    EXPECT_EQ(CountOf(child.Log, "Refuse{AdoptT0Unavailable, \"disallowed\"}"), 1u) << child.Log;
    EXPECT_NE(child.Log.find("MOBILEGL_IPC_ALLOW_ADOPT_T0=0"), std::string::npos) << child.Log;
}

// THE WHOLE PATH: granted, the caps bit read, the store allocated and seeded from the shadow,
// its Offer queued before its record, matched by seq, imported - and the importer holds exactly
// the client's bytes, the client's pointer is the store, and both arms count it.
// RED-ONCE (host): do not publish kCapAdoptT0 (ServerSession::PublishCapsSnapshot) -> the client
// never adopts, 170.
TEST(RemoteClientAdoptT0, AGrantedStoreLandsWithTheClientsBytes) {
    const auto child = RunInChild([] {
        std::vector<Uint8> seed;
        void* adopted = AdoptOneStore(0x22, seed);
        if (adopted == nullptr) ::_exit(170);
        if (std::memcmp(adopted, seed.data(), kT0Bytes) != 0) ::_exit(171);
        if (g_fakeImports.count != 1 || g_fakeImports.size != kT0Bytes) ::_exit(172);
        if (g_fakeImports.handle.Slot != 5 || g_fakeImports.handle.Gen != 1) ::_exit(173);
        if (g_fakeImports.bytes != seed) ::_exit(174);
        // The import and the client's pointer are one store: a write through either is the other's.
        static_cast<Uint8*>(adopted)[100] = 0xA5;
        if (static_cast<const Uint8*>(static_cast<FakeAhb*>(g_fakeImports.held)->map)[100] != 0xA5) ::_exit(175);
        if (ClientSessionInstance().T0StoresImported() != 1 || ClientSessionInstance().T0BytesImported() != kT0Bytes)
            ::_exit(176);
        if (Srv::ServerSessionInstance().T0StoresImported() != 1) ::_exit(177);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
    EXPECT_NE(child.Log.find("T0 granted"), std::string::npos) << child.Log;
    EXPECT_NE(child.Log.find("T0 imported store {slot=5, gen=1} - 65536 bytes"), std::string::npos) << child.Log;
    EXPECT_NE(child.Log.find("T0 store {slot=5, gen=1} adopted - 65536 bytes"), std::string::npos) << child.Log;
    EXPECT_EQ(child.Log.find("Fatal{"), std::string::npos) << child.Log;
}

// ORDER: the Offer arrives 300 ms AFTER its record was published and decoded - the apply thread
// waits for it (bounded) and the store still lands with the client's bytes.
TEST(RemoteClientAdoptT0, AnOfferThatArrivesAfterItsRecordStillLands) {
    const auto child = RunInChild([] {
        ClientSession::SetT0OfferFaultForTest(300);
        std::vector<Uint8> seed;
        const auto start = std::chrono::steady_clock::now();
        void* adopted = AdoptOneStore(0x33, seed);
        const auto waited = std::chrono::steady_clock::now() - start;
        if (adopted == nullptr) ::_exit(180);
        if (g_fakeImports.bytes != seed) ::_exit(181);
        if (waited < std::chrono::milliseconds(300)) ::_exit(182); // it really waited for the late Offer
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
    EXPECT_EQ(child.Log.find("AdoptT0NoStore"), std::string::npos) << child.Log;
}

// A LOST Offer is a NAMED DECLINE within the bounded wait - never a hang: the store runs T2.
TEST(RemoteClientAdoptT0, AnOfferThatNeverArrivesIsANamedDeclineNotAHang) {
    const auto child = RunInChild([] {
        ClientSession::SetT0OfferFaultForTest(-1);
        std::vector<Uint8> seed;
        const auto start = std::chrono::steady_clock::now();
        void* adopted = AdoptOneStore(0x44, seed);
        const auto waited = std::chrono::steady_clock::now() - start;
        if (adopted != nullptr) ::_exit(190);
        if (g_fakeImports.count != 0) ::_exit(191);
        if (waited > std::chrono::milliseconds(T0::kOfferWaitMs + 3000)) ::_exit(192);
        if (ClientSessionInstance().T0Declined() != 1) ::_exit(193);
        ClientSessionInstance().Stop();
    });
    ExpectChildSuccess(child);
    EXPECT_EQ(CountOf(child.Log, "Refuse{AdoptT0NoStore, \"record"), 1u) << child.Log;
    EXPECT_EQ(child.Log.find("Fatal{"), std::string::npos) << child.Log;
}
#endif
