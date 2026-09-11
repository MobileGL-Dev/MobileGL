// MobileGL - MobileGL/MG_Remote/Client/CapsMirror.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The client's copy of the server's capabilities. Owner: package c1. Signatures by c0.
//
// WHY A MIRROR AND NOT A ROUND TRIP. There are 56 client-side caps read points
// (40 GetDynamicParameters + 7 GetRendererInfo + 4 GetFormatCapabilities + 3 GetBackendType +
// 2 GetBackendAPIVersionString), and several of them - GL_Getter.cpp:2400 and
// ShaderTranspiler/CompileEnv.cpp:120-124 - bind a reference and then read many members, so a
// partial snapshot is not an option. Every one of the 56 must be answerable locally, with no
// record on the wire.
//
// GetRendererInfo() RETURNS A REFERENCE (BackendObject.h:590), so the mirror must OWN a
// RendererInfo instance to hand back - including before the first snapshot arrives, because
// LogBackendInfo() reads it at MG_Backend/Init.cpp:21, during MG_Backend::Init(), long before
// any context exists. Ruling (scout-caps-reply §1.2 option (a)): the mirror answers with a
// placeholder until the first snapshot, P5 accepts one inaccurate startup log line, and
// MG_Backend::Init() is NOT restructured.
//
// GetFormatCapabilities() is NON-VIRTUAL (BackendObject.h:594), so a remote backend object
// cannot override the accessor: it must FILL BackendObject::m_formatCapabilities from this
// mirror instead.
//
// INVALIDATION IS RE-ARRIVAL (R-12). DirectGLES has no OnCapsInvalidated producer at all - it
// re-runs UpdateAdvertisedCapabilityExtensions + UpdateDynamicBackendParameters at
// BackendObject_DirectGLES.cpp:865-871 and tells the frontend nothing, which is correct in
// monolith and a silent bug under split. Rather than add a DirectGLES-side callback (a
// dev-shaped backend edit), the SERVER re-sends the whole snapshot on every InitCapabilities
// re-run and the CLIENT treats a second arrival as the invalidation. Generation() is what a
// client-side memo keys on, and it is also the re-open signal for the server-context-death
// case that MGPipeCallbacks has no eleventh slot for (MGPipeCallbacks.h:56-58).

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
                   const RendererInfo& renderer, const String& apiVersion,
                   BackendType backend);

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
        Bool ServerConsumes(Uint64 subsystemBit) const;

        // GLFunctionsTable::PrefersCpuXfbPrimitiveAccounting (BackendObject.h:274) does NOT
        // ride in MGPCaps::Dynamic - it is a member of the function table, which is precisely
        // the thing a split client never receives. Its only non-test client reader is
        // GL_Query.cpp:221, and under split it must be answered from kCapCpuXfbPrimitiveAccounting.
        Bool PrefersCpuXfbPrimitiveAccounting() const;

    private:
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
    // (BackendObject.h:594) and returns the base class's own m_formatCapabilities member, so a
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

} // namespace MobileGL::MG_Remote::Client
