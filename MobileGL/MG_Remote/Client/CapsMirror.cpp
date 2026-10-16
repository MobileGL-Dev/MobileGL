// MobileGL - MobileGL/MG_Remote/Client/CapsMirror.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The read accessors answer before the first snapshot instead of aborting, so an early read
// such as LogBackendInfo's startup logging cannot keep the process from starting. They return
// the default-constructed members, which Adopt() replaces in place (the accessors return
// references, so the storage outlives every caller), and a read of the placeholder logs a
// warning. ServerConsumes() is the exception and never answers from a placeholder; see
// RequireFirstSnapshot.

#include "CapsMirror.h"

#include "../CapsCodec.h"
#include "../FatalFunnel.h"

#include <Config.h> // MG_Config::SplitTransportRequestedByConfig
#include <MG_Backend/MGPipe/PipeInputs.h> // MGPipeServerArm, D1c's role predicate
#include <MG_State/GLState/Core.h>
#include <MG_Util/Async/ShaderCompilePool.h>
#include <MG_Util/Debug/Log.h>

#include <algorithm>
#include <atomic>

namespace MobileGL::MG_Remote::Client {

    namespace {
        CapsAdoptedHook g_capsAdoptedHook = nullptr;

        // F1. See CapsMirror.h for the contract of each of these three.
        CapsFirstSnapshotWait g_firstSnapshotWait = nullptr;
        ::std::atomic<Uint64> g_publishedGeneration{0};

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

        // P10 B: GL_KHR_parallel_shader_compile IS THE CLIENT'S EXTENSION, SO THE CLIENT DECIDES
        // IT. Both backends put it in their list when THEIR process compiles asynchronously
        // (BackendObject_DirectGLES.cpp / BackendObject_DirectVulkan.cpp, "MobileGL's own
        // capability"), but under split the process that compiles is this one: the pool, its
        // threads, glMaxShaderCompilerThreadsKHR and GL_MAX_SHADER_COMPILER_THREADS_KHR
        // (GL_Getter.cpp) all live in the frontend, and the server only ever sees the archive.
        // Taking the server's word advertised the string from the SERVER's configuration -
        // under inproc and spawn the server inherits this process's environment and the two
        // agree by accident; a tcp server is started on its own and they need not. Measured:
        // AsyncCompileScenario.ExtensionStringMatchesTheConfiguration with
        // MOBILEGL_ASYNC_SHADER_COMPILE=0 on the client and a default (async-on) tcp server
        // read the string advertised and the thread count 0, on both backends. So the entry is
        // re-derived here from this process's switch, on every adoption.
        void ReconcileClientCompileExtensions(RendererInfo& renderer) {
            Vector<GLExtension>& extensions = renderer.RendererGLInfo.Extensions;
            const auto found =
                std::find(extensions.begin(), extensions.end(), E_GL_KHR_parallel_shader_compile);
            const Bool advertised = found != extensions.end();
            const Bool compilesAsync = MG_Util::Async::AsyncShaderCompileEnabled();
            if (advertised == compilesAsync) return;
            if (compilesAsync) {
                extensions.push_back(E_GL_KHR_parallel_shader_compile);
            } else {
                extensions.erase(found);
            }
            MGLOG_D("MG_Remote client: GL_KHR_parallel_shader_compile %s - the server's list said the "
                    "opposite, and the compile pool it describes is this process's",
                    compilesAsync ? "advertised" : "withdrawn");
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
        ReconcileClientCompileExtensions(m_renderer);
        m_apiVersion = apiVersion;
        m_backend = backend;
        ++m_generation;
        // F1: published BEFORE the log line and before the hook, because a thread parked in
        // RequireFirstSnapshot is waiting on exactly this store and has nothing else to read.
        g_publishedGeneration.store(m_generation, ::std::memory_order_release);

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
        // The only client-side source of this answer. MGPipeGetResourceOps() is the server's
        // registration: right by accident inproc and null under spawn, where reading it would
        // silently stop whole record families while the client clears its dirty flags. The bit
        // layout is read through CapsCodec.h's helper so both sides share one spelling. A
        // placeholder mirror does not answer: the client has no pull path, so withholding would
        // drop records. The decision waits for a real snapshot or fails naming the family.
        if (m_generation == 0) {
            RequireFirstSnapshot(subsystemBit);
        }
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

    void CapsMirror::RequireFirstSnapshot(Uint64 subsystemBit) const {
        // THE SERVER ARM IS NOT A CLIENT WITH A LATE SNAPSHOT (D1c, PipeInputs.h). On the apply
        // thread, and in a spawn server process, the CLIENT's caps mirror is not merely
        // un-adopted - it is the wrong object to ask, and it will never be adopted, because
        // there is no client half here to adopt it. That arm answers through
        // RunsAsTheServerRole()'s monolith rows in WireTables.cpp and through the server
        // session's own mask in PipeApply.cpp; the placeholder's "no consumer" is what keeps a
        // server-role birth off a wire it has no business writing to, and it is correct there.
        // Only the CLIENT may not decide a family's fate from a placeholder.
        if (MG_Pipe::MGPipeServerArm()) return;

        // AND NEITHER IS A PROCESS THAT NEVER ASKED FOR A TRANSPORT. MG_Config::Transport can
        // be assigned by hand - unit fixtures do it to put the code under test on its split
        // arm while building no client at all (ServerLoopTest's main says so in as many words)
        // - and holding those to a rule about a handshake they will never perform would be
        // holding them to nothing. The gate is armed by MG_ConfigLoader::Init having RESOLVED a
        // split transport, which is the same decision that makes MG_Backend::Init bring a
        // client half up. See Config.h.
        if (!MG_Config::SplitTransportRequestedByConfig) return;

        // The wait is real only when somebody else is fetching the snapshot. ClientSession's
        // hook answers false when no bring-up is in flight and when the CALLING thread is the
        // one performing it - both of which mean nothing can land while this call blocks.
        if (g_firstSnapshotWait != nullptr && g_firstSnapshotWait(kFirstSnapshotWaitMs) &&
            m_generation != 0) {
            return;
        }
        // NOT MGLOG_W AND CARRY ON. The whole defect this closes is that carrying on was
        // indistinguishable, in every downstream artefact, from a server that genuinely
        // consumes nothing: same absence of records, same cleared dirty flags, same green
        // lane. A death names the family, publishes a SessionFault to a peer if one is
        // connected, and lands in the census like every other one.
        SessionFail(MGFatalFamily::CapsBeforeFirstSnapshot,
                    "MG_Remote client: Fatal{CapsBeforeFirstSnapshot, \"%s\"} - the liveness gate "
                    "for MGPipe subsystem 0x%llx asked a caps mirror that is still a PLACEHOLDER "
                    "(generation 0, callMask=0). Answering it would withhold that family's whole "
                    "record stream on a mask the server never published, and there is no legacy "
                    "pull path on the client to take over. The session must be started - and its "
                    "first CapsSnapshot adopted - before any frontend object of this family is "
                    "born; MobileGL::Initialize does that before MG_State::Init()",
                    CapsSubsystemName(subsystemBit),
                    static_cast<unsigned long long>(subsystemBit));
    }

    void SetCapsFirstSnapshotWait(CapsFirstSnapshotWait wait) { g_firstSnapshotWait = wait; }

    Uint64 PublishedCapsGeneration() {
        return g_publishedGeneration.load(::std::memory_order_acquire);
    }

    const char* CapsSubsystemName(Uint64 subsystemBit) {
        switch (subsystemBit) {
        case MG_Pipe::kMGPipeSubsystemResources: return "Resources";
        case MG_Pipe::kMGPipeSubsystemVertexInput: return "VertexInput";
        case MG_Pipe::kMGPipeSubsystemFramebuffer: return "Framebuffer";
        case MG_Pipe::kMGPipeSubsystemTextureResources: return "TextureResources";
        case MG_Pipe::kMGPipeSubsystemSamplers: return "Samplers";
        case MG_Pipe::kMGPipeSubsystemPrograms: return "Programs";
        case MG_Pipe::kMGPipeSubsystemBufferBindings: return "BufferBindings";
        default: break;
        }
        // A bit with no word yet, or a mask of several. Named by its value rather than by a
        // guess: G14 lets the list grow, never lets a wrong word into a Fatal.
        return "unnamed-subsystem";
    }

    Uint64 ConsumerRefusals() { return g_consumerRefusals; }
    Uint64 LastRefusedSubsystem() { return g_lastRefusedSubsystem; }
    void ResetConsumerRefusalsForTest() {
        g_consumerRefusals = 0;
        g_lastRefusedSubsystem = 0;
        g_refusedSubsystemsLogged = 0;
    }

    void ResetCapsMirrorForTest() {
        CapsMirrorInstance() = CapsMirror{};
        g_publishedGeneration.store(0, ::std::memory_order_release);
        ResetConsumerRefusalsForTest();
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
