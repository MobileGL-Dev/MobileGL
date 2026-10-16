
// P12 (on-screen server window): A SERVER SESSION ENDS IN A PROCESS THAT OUTLIVES IT.
//
// The in-process display server (the display Activity's process) runs its sessions one after
// another in ONE process, and each new client mints its handles from the same {slot, gen} space
// again. The twin tables are process globals, so the twins the previous session built - naming ids
// of the context its backend destroyed - answered for the next session's objects: on the device
// the second on-screen OpenRA replay linked no program (ssim 0.00004) and the third crashed inside
// Adreno's glDrawElements; on llvmpipe the second replay failed the same way. The server backend's
// destruction under a transport - the end of a session - now drops every twin (without a driver
// call), and monolith keeps its twins. Red once: remove the DropEveryTwinForEndedServerSession
// call from ~BackendObject_DirectGLES and the first half of this case fails at every kind.
#if MOBILEGL_BUILD_DISAGGREGATED
TEST(EsprytServerSession, TheServerBackendsDestructionDropsEveryTwinTheSessionBuilt) {
    using namespace MobileGL;
    namespace GL = MG_Backend::DirectGLES;
    const auto previousTransport = MG_Config::Transport;
    const MG_Pipe::MGPipeHandle program{7u, 3u};
    const MG_Pipe::MGPipeHandle texture{4u, 2u};
    const MG_Pipe::MGPipeHandle buffer{5u, 1u};
    // A session's twins: LIVE entries at the generations its client minted. A null twin object is
    // enough for the program and texture kinds - what the next session trips over is the live
    // entry itself (its generation, and the driver id a real twin would carry).
    const auto populate = [&] {
        ASSERT_NE(GL::PrgramImpl::g_backendProgramObjects.GetOrCreateByHandle(program), nullptr);
        ASSERT_NE(GL::TextureImpl::g_backendTextureObjects.GetOrCreateByHandle(texture), nullptr);
        GL::BufferImpl::g_backendBufferResources.GetOrCreate(buffer) =
            MakeShared<GL::BufferImpl::GLESBufferResource>();
        ASSERT_EQ(GL::PrgramImpl::g_backendProgramObjects.LiveGenAt(program.Slot), 3u);
        ASSERT_EQ(GL::TextureImpl::g_backendTextureObjects.LiveGenAt(texture.Slot), 2u);
        ASSERT_NE(GL::BufferImpl::g_backendBufferResources.FindByHandle(buffer), nullptr);
    };

    // Under a transport this backend is the SERVER's: its destruction ends the session.
    MG_Config::Transport = MG_Config::TransportMode::Spawn;
    populate();
    { GL::BackendObject_DirectGLES serverBackend; }
    EXPECT_EQ(GL::PrgramImpl::g_backendProgramObjects.LiveGenAt(program.Slot), 0u)
        << "the ended session's program twin is still live: the next session's client mints {7, 0} "
           "and is refused as ProtocolCorruption, or adopts a program of the destroyed context";
    EXPECT_EQ(GL::TextureImpl::g_backendTextureObjects.LiveGenAt(texture.Slot), 0u)
        << "the ended session's texture twin is still live";
    EXPECT_EQ(GL::BufferImpl::g_backendBufferResources.FindByHandle(buffer), nullptr)
        << "the ended session's buffer twin is still live";
    // The next session starts from empty tables: its first handle at that slot is a fresh twin.
    MG_Pipe::MGPipeHandle nextSession{7u, 0u};
    auto* fresh = GL::PrgramImpl::g_backendProgramObjects.GetOrCreateByHandle(nextSession);
    ASSERT_NE(fresh, nullptr);
    EXPECT_EQ(*fresh, nullptr);

    // Monolith keeps its twins: its backend is not a session's, and nothing here changed for it.
    GL::PrgramImpl::g_backendProgramObjects = {};
    MG_Config::Transport = MG_Config::TransportMode::Monolith;
    populate();
    { GL::BackendObject_DirectGLES monolithBackend; }
    EXPECT_EQ(GL::PrgramImpl::g_backendProgramObjects.LiveGenAt(program.Slot), 3u);
    EXPECT_EQ(GL::TextureImpl::g_backendTextureObjects.LiveGenAt(texture.Slot), 2u);
    EXPECT_NE(GL::BufferImpl::g_backendBufferResources.FindByHandle(buffer), nullptr);

    GL::PrgramImpl::g_backendProgramObjects = {};
    GL::TextureImpl::g_backendTextureObjects = {};
    GL::BufferImpl::g_backendBufferResources = {};
    MG_Config::Transport = previousTransport;
}
#else
TEST(EsprytServerSession, TheServerBackendsDestructionDropsEveryTwinTheSessionBuilt) {
    GTEST_SKIP() << "a server backend exists only in the split build (MOBILEGL_BUILD_DISAGGREGATED)";
}
#endif
