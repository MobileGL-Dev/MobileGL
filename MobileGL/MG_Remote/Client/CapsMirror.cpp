// MobileGL - MobileGL/MG_Remote/Client/CapsMirror.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 package c1.
//
// EVERY ACCESSOR ANSWERS BEFORE THE FIRST SNAPSHOT, AND THAT IS THE RULING, NOT A WEAKENING.
// c0's stubs aborted on all but Valid()/Generation() so that nothing could answer from a zeroed
// mirror by accident. But scout-caps-reply §1.2's option (a) - the one the brief adopts - says
// LogBackendInfo() reads GetRendererInfo() at MG_Backend/Init.cpp:21, during MG_Backend::Init(),
// BEFORE any surface exists, and that P5 accepts one imprecise startup log line rather than
// restructure MG_Backend::Init(). An abort there is not a stricter mirror; it is a process that
// cannot start. So the mirror answers a placeholder and SAYS SO, once per accessor: a wrong
// number that announced itself is auditable, and the announcement is what a case asserts on.
//
// THE PLACEHOLDER IS A REAL OBJECT, NOT A TEMPORARY. GetRendererInfo() and
// GetDynamicParameters() return const references, so the storage has to outlive every caller;
// the members below are it, default-constructed, and Adopt() replaces them in place.

#include "CapsMirror.h"

#include "../CapsCodec.h"

#include <MG_State/GLState/Core.h>
#include <MG_Util/Debug/Log.h>

namespace MobileGL::MG_Remote::Client {

    namespace {
        CapsAdoptedHook g_capsAdoptedHook = nullptr;

        // R-8's negative control, counted at the funnel. Not atomics: every reader of this
        // mirror is the GL thread, by the same argument that makes one gPipeInputs legal.
        Uint64 g_consumerRefusals = 0;
        Uint64 g_lastRefusedSubsystem = 0;
        Uint64 g_refusedSubsystemsLogged = 0;

        // One line per accessor, once, and only while the mirror is a placeholder. MGLOG_W_ONCE
        // keys on the call site, so each accessor gets its own line and a hot getter cannot
        // flood the log.
        void WarnPlaceholder(const char* what, Uint64 generation) {
            if (generation != 0) return;
            MGLOG_W("MG_Remote client: CapsMirror::%s read before the first CapsSnapshot - "
                    "answering a PLACEHOLDER. Expected exactly once at startup, from "
                    "LogBackendInfo (MG_Backend/Init.cpp:21); anything later means a caps read "
                    "beat the handshake",
                    what);
        }
    } // namespace

    void CapsMirror::Adopt(const MG_Pipe::MGPCaps& caps, const MG_Backend::FormatCapabilityCache& formats,
                           const RendererInfo& renderer, const String& apiVersion,
                           BackendType backend) {
        m_caps = caps;
        // The two blobrefs inside MGPCaps name SEG_STAGE runs that belong to the SERVER's
        // encoder and are meaningless on this side; the decoded objects beside them are the
        // answer. Clearing them is not tidiness - a later reader that resolved one would read
        // whatever the stage allocator has since put there.
        m_caps.FormatCapabilities = MG_Pipe::MGPBlobRef{};
        m_caps.RendererInfo = MG_Pipe::MGPBlobRef{};
        m_formats = formats;
        m_renderer = renderer;
        m_apiVersion = apiVersion;
        m_backend = backend;
        ++m_generation;

        // R-12: RE-ARRIVAL IS THE INVALIDATION, and this is the one place that acts on it.
        // GLContext::GetCompileEnv() memoises on the raw pActiveBackendObject pointer
        // (Core.cpp:34); under split that pointer is the single long-lived BackendObject_Remote
        // and NEVER changes, so without this line the compile env, its preprocess memos and the
        // advertised-extension list stay stale for ever after a server-side InitCapabilities
        // re-run. DirectVulkan already spells the monolith half of this exactly this way
        // (BackendObject_DirectVulkan.cpp:390).
        if (m_generation > 1 && MG_State::pGLContext != nullptr) {
            MG_State::pGLContext->InvalidateCompileEnv();
        }
        MGLOG_I("MG_Remote client: CapsMirror generation %llu adopted (backend=%d, callMask=0x%llx)",
                static_cast<unsigned long long>(m_generation), static_cast<int>(m_backend),
                static_cast<unsigned long long>(m_caps.CallMask));

        // LAST, and after the generation has moved: the hook reads this mirror.
        if (g_capsAdoptedHook != nullptr) g_capsAdoptedHook();
    }

    void SetCapsAdoptedHook(CapsAdoptedHook hook) { g_capsAdoptedHook = hook; }

    Bool CapsMirror::Valid() const { return m_generation != 0; }
    Uint64 CapsMirror::Generation() const { return m_generation; }

    const RendererInfo& CapsMirror::Renderer() const {
        WarnPlaceholder("Renderer", m_generation);
        return m_renderer;
    }

    const MG_Backend::DynamicBackendParameters& CapsMirror::Dynamic() const {
        WarnPlaceholder("Dynamic", m_generation);
        return m_caps.Dynamic;
    }

    const MG_Backend::FormatCapabilityCache& CapsMirror::Formats() const {
        WarnPlaceholder("Formats", m_generation);
        return m_formats;
    }

    const String& CapsMirror::ApiVersion() const {
        WarnPlaceholder("ApiVersion", m_generation);
        return m_apiVersion;
    }

    BackendType CapsMirror::Backend() const {
        WarnPlaceholder("Backend", m_generation);
        return m_backend;
    }

    Uint64 CapsMirror::CallMask() const { return m_caps.CallMask; }

    Bool CapsMirror::HasCap(MG_Pipe::MGPCapBit bit) const {
        return (m_caps.CallMask & static_cast<Uint64>(bit)) != 0;
    }

    Bool CapsMirror::ServerConsumes(Uint64 subsystemBit) const {
        // THE ONLY LEGAL CLIENT-SIDE SPELLING (R-8). Never MGPipeGetResourceOps(): that is the
        // SERVER's registration, a process-wide global, right by accident under inproc and null
        // under spawn - and a null read there silently stops five record families while the
        // client goes on clearing its dirty flags, which is ID-39's 66 lost uploads with a wire
        // in between. Folded through CapsCodec.h's helper rather than shifted here, because two
        // spellings of one bit layout is how the two sides come to disagree about it.
        //
        // A PLACEHOLDER MIRROR CONSUMES NOTHING, and that is the safe direction: with no
        // snapshot the mask is 0, every family answers "no consumer", the client emits nothing
        // and the LEGACY PULL PATH runs untouched. The unsafe direction - emitting to a server
        // that has no consumer - is the one that loses uploads.
        const Bool consumes = MGCapsServerConsumes(m_caps.CallMask, subsystemBit);
        if (!consumes) {
            ++g_consumerRefusals;
            g_lastRefusedSubsystem = subsystemBit;
            if ((g_refusedSubsystemsLogged & subsystemBit) == 0) {
                g_refusedSubsystemsLogged |= subsystemBit;
                MGLOG_W("MG_Remote client: the server does not consume MGPipe subsystem 0x%llx - "
                        "this family emits NOTHING and the legacy pull path runs for it "
                        "(R-8). callMask=0x%llx, caps generation %llu",
                        static_cast<unsigned long long>(subsystemBit),
                        static_cast<unsigned long long>(m_caps.CallMask),
                        static_cast<unsigned long long>(m_generation));
            }
        }
        return consumes;
    }

    Uint64 ConsumerRefusals() { return g_consumerRefusals; }
    Uint64 LastRefusedSubsystem() { return g_lastRefusedSubsystem; }
    void ResetConsumerRefusalsForTest() {
        g_consumerRefusals = 0;
        g_lastRefusedSubsystem = 0;
        g_refusedSubsystemsLogged = 0;
    }

    Bool CapsMirror::PrefersCpuXfbPrimitiveAccounting() const {
        return HasCap(MG_Pipe::kCapCpuXfbPrimitiveAccounting);
    }

    CapsMirror& CapsMirrorInstance() {
        // ID-8: leak at exit. No frontend destructor may reach pipe or backend state from an
        // exit handler, and that rule applies once per role-local singleton, not once overall.
        static CapsMirror& instance = *new CapsMirror{};
        return instance;
    }

} // namespace MobileGL::MG_Remote::Client
