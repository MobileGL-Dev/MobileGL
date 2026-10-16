// MobileGL - MobileGL/MG_Remote/Client/CapsMirror.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The client's copy of the server's capabilities. Every client-side caps read is answered
// locally, with no record on the wire, and several readers bind a reference and read many
// members, so the mirror holds the whole snapshot. The accessors return references, so the
// mirror owns the storage; before the first snapshot it answers a placeholder rather than abort.
// GetFormatCapabilities() is non-virtual, so the remote backend fills
// BackendObject::m_formatCapabilities from here (SetCapsAdoptedHook).
// Invalidation is re-arrival: the server re-sends the whole snapshot on every InitCapabilities
// run, and Generation() is what a client-side memo keys on.

#pragma once
#include <Includes.h>

#include <MG_Backend/BackendObject.h>
#include <MG_Pipe/MGPipe.h>

namespace MobileGL::MG_Remote::Client {

    class CapsMirror {
    public:
        // Replaces the whole mirror and bumps Generation(). Called once per CapsSnapshot,
        // including the re-sends that mean "invalidate" (R-12).
        void Adopt(const MG_Pipe::MGPCaps& caps, const MG_Backend::FormatCapabilityCache& formats,
                   const RendererInfo& renderer, const String& apiVersion, BackendType backend);

        // False until the first snapshot. The placeholder answers below are still safe to
        // read - that is the point - but a caller that can wait should.
        Bool Valid() const;

        // ++ on every Adopt. A client memo that survives a server context loss must key on
        // this; nothing else on the client can see that the server's context died.
        Uint64 Generation() const;

        const RendererInfo& Renderer() const;
        const MG_Backend::DynamicBackendParameters& Dynamic() const;
        const MG_Backend::FormatCapabilityCache& Formats() const;
        const String& ApiVersion() const;
        // The SERVER's backend type, never a new "Remote" enumerator: frontend branches
        // switch on this (GL_Framebuffer.cpp:47, GL_Texture.cpp:6536, CompileEnv.cpp:122) and
        // a value they do not know silently takes the wrong arm.
        BackendType Backend() const;

        Uint64 CallMask() const;
        Bool HasCap(MG_Pipe::MGPCapBit bit) const;

        // R-8. `subsystemBit` is a kMGPipeSubsystem* constant. THIS IS THE ONLY LEGAL SOURCE
        // of the answer on the client under split: MGPipeGetResourceOps() is the SERVER's
        // registration and is null in the client process, which would silently disable the
        // whole push path in the one mode that matters.
        //
        // F1 (P7 wave 2): IT NEVER ANSWERS FROM A PLACEHOLDER ANY MORE. See RequireFirstSnapshot.
        Bool ServerConsumes(Uint64 subsystemBit) const;

        // GLFunctionsTable::PrefersCpuXfbPrimitiveAccounting (BackendObject.h) does NOT
        // ride in MGPCaps::Dynamic - it is a member of the function table, which is precisely
        // the thing a split client never receives. Its only non-test client reader is
        // GL_Query.cpp:221, and under split it must be answered from kCapCpuXfbPrimitiveAccounting.
        Bool PrefersCpuXfbPrimitiveAccounting() const;

    private:
        // ServerConsumes() decides whether a record family reaches the wire, so it never answers
        // from the placeholder: a zero mask would drop the family's records while the emitter
        // clears their dirty flags, and the client has no pull path to fall back to. This waits,
        // bounded by kFirstSnapshotWaitMs, for a handshake in flight on another thread, else it is
        // Fatal{CapsBeforeFirstSnapshot, "<family>"}. It never waits on the thread running the
        // handshake, which would deadlock; MobileGL::Initialize brings the split roles up before
        // MG_State::Init() so no frontend object asks that early.
        void RequireFirstSnapshot(Uint64 subsystemBit) const;

        MG_Pipe::MGPCaps m_caps{};
        MG_Backend::FormatCapabilityCache m_formats{};
        RendererInfo m_renderer{};
        String m_apiVersion;
        BackendType m_backend = BackendType::Unknown;
        Uint64 m_generation = 0;
    };

    // Per client context in principle; one per process in P5, because P5 serves one context.
    // Leak-at-exit like every other MG_Remote singleton (ID-8): no frontend destructor may
    // reach pipe or backend state from an exit handler.
    CapsMirror& CapsMirrorInstance();

    // ---- c1's addition to c0's signature block -----------------------------------------
    //
    // WHY A HOOK AND NOT A READ. BackendObject::GetFormatCapabilities() is NON-VIRTUAL
    // (BackendObject.h) and returns the base class's own m_formatCapabilities member, so a
    // remote backend object cannot answer it lazily from the mirror - it has to PUSH the cache
    // into that member, and the only moment it can know to is when a snapshot lands. A raw
    // function pointer rather than std::function, for ID-8's reason: this can fire on paths
    // that must not allocate. One hook, installed by BackendObject_Remote's constructor.
    using CapsAdoptedHook = void (*)();
    void SetCapsAdoptedHook(CapsAdoptedHook hook);

    // R-8's NEGATIVE CONTROL NEEDS TO SEE THE WITHHOLDING, NOT INFER IT FROM AN ABSENCE.
    // "the client emits nothing for a family the server does not consume" is, on its own,
    // indistinguishable from "the client emits nothing because nothing called it" - and the
    // second is how a gate goes green for the wrong reason. So the one funnel that answers the
    // question counts its own refusals and names the family, once per family, in the log.
    //
    // Counted inside ServerConsumes(), which is R-8's only legal spelling, so a refusal that
    // happened cannot fail to be counted and a count that moved cannot have come from anywhere
    // else.
    Uint64 ConsumerRefusals();
    Uint64 LastRefusedSubsystem();
    void ResetConsumerRefusalsForTest();

    // F1's cases need the ONE state a running process can never get back: a mirror that has
    // adopted nothing. A fork child inherits its parent's adopted instance, so without this a
    // case about the placeholder would be a case about whatever the previous case adopted.
    // Test-only by name, like ResetConsumerRefusalsForTest above it; nothing in the library
    // calls either.
    void ResetCapsMirrorForTest();

    // ---- F1 (P7 wave 2) --------------------------------------------------------------------
    //
    // THE BOUNDED WAIT'S ONE HONEST SHAPE. ServerConsumes cannot pump a control plane itself -
    // CapsMirror deliberately knows nothing about ClientSession - so the session installs the
    // wait and the mirror calls it. The contract is narrow on purpose:
    //
    //   * it returns FALSE IMMEDIATELY when no session bring-up is in flight, or when the
    //     CALLING thread is the one performing it. Both mean the snapshot cannot arrive while
    //     this call blocks, and a wait there would burn the budget and then die anyway;
    //   * otherwise it waits up to `timeoutMs` for the first Adopt() and reports whether one
    //     landed.
    //
    // Installed once, by ClientSession's constructor. A raw function pointer for ID-8's
    // reason, like SetCapsAdoptedHook beside it.
    using CapsFirstSnapshotWait = Bool (*)(Uint32 timeoutMs);
    void SetCapsFirstSnapshotWait(CapsFirstSnapshotWait wait);

    // The generation, readable from a thread that does not own the mirror. m_generation is
    // GL-thread state by the same argument that makes one gPipeInputs legal; this is the one
    // fact a WAITING thread has to see, so it is published separately and atomically.
    Uint64 PublishedCapsGeneration();

    // `kMGPipeSubsystem*` -> the word the Fatal and the R-8 warning print. Names only grow
    // (G14); an unknown bit prints as a hex literal rather than as a guess.
    const char* CapsSubsystemName(Uint64 subsystemBit);

    // How long RequireFirstSnapshot gives a handshake that IS in flight. Bounded and small:
    // an in-process Adopt lands in microseconds and a socket one in single-digit milliseconds,
    // so anything past this is a peer that is not coming.
    inline constexpr Uint32 kFirstSnapshotWaitMs = 250;

} // namespace MobileGL::MG_Remote::Client
