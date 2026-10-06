// MobileGL - MobileGL/MG_Remote/Server/SessionRuntime.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P14 S2: the per-session runtime and the thread routing (SessionRuntime.h states the shape).

#include "SessionRuntime.h"

#include <MG_Backend/DirectGLES/DirectGLES.h>
#include <MG_Remote/FatalFunnel.h>
#include <MG_Backend/Record/StagedShadow.h>
#include <MG_Remote/Wire/PipeWireCodec.h>

#include <atomic>

namespace MobileGL::MG_Remote::Server {

    namespace {
        // The calling thread's session. nullptr on every thread that is not one of a session's own
        // two, which is the whole of the fallback rule.
        thread_local SessionRuntime* t_runtime = nullptr;

        // The resolver MG_Pipe asks per thread. Installing it is a single store of a function
        // pointer that is never replaced, so it needs no lock and no teardown: the server role
        // lives for the process (ID-8).
        MG_Pipe::PipeInputs* ResolveThreadInputs() {
            SessionRuntime* runtime = t_runtime;
            return runtime != nullptr ? &runtime->Inputs() : nullptr;
        }

        // P14 S4 (docs/Disaggregated/design/11-state-ownership.md). THE NATIVE TUPLE'S KEY. The
        // backend keys its per-context native state - the EGLContext, its config, the draw/read
        // binding - on (session, client context token); this is the one place that answers the pair
        // for the calling thread. The session half is the runtime's ADDRESS, which is stable for the
        // session's whole life and unique among live sessions in the process; the token half is what
        // `bind_context` last set on the ring. False (== the {0,0} key) on any thread that is not a
        // session's own, which is the client process, the inproc one-process shape and every unit
        // case - i.e. exactly the single-context world the backend still serves unchanged.
        Bool ResolveThreadNativeContextKey(Uint64* outSessionKey, Uint64* outContextToken) {
            SessionRuntime* runtime = t_runtime;
            if (runtime == nullptr) return false;
            *outSessionKey = reinterpret_cast<Uint64>(runtime);
            *outContextToken = runtime->Session().CurrentContextToken();
            return true;
        }

        // P14 S5 (docs/Disaggregated/design/11-state-ownership.md). WHOSE APPLIER IS THIS THREAD
        // IN? The same two-word answer as the native key above, and deliberately the same
        // spelling: the session half is the ServerSession's own address - which is the key the
        // registration side uses (ServerSession::CreateContext passes `this`), so the two agree
        // by construction rather than by two conventions - and the token half is what
        // `bind_context` last set on this session's ring. A thread that is not a session's
        // answers false and gets the process-wide applier, i.e. the single-context world every
        // other shape in this tree still runs.
        Bool ResolveThreadApplierKey(MG_Pipe::MGPipeApplierKey* outKey) {
            SessionRuntime* runtime = t_runtime;
            if (runtime == nullptr) return false;
            outKey->SessionKey = reinterpret_cast<Uint64>(&runtime->Session());
            outKey->ContextToken = runtime->Session().CurrentContextToken();
            return true;
        }

        // P14 S6 (docs/Disaggregated/design/11-state-ownership.md). WHICH TWIN TABLE DOES THIS
        // THREAD'S BACKEND OBJECT BELONG TO? The backend's twin tables are per {session, share
        // group} - a twin owns a DRIVER id, and a driver id belongs to one native context's
        // object namespace - so this answers the same two words the applier's OBJECT RECORDS are
        // filed under, resolved through the same `CurrentContextToken()` and the same
        // MGPipeApplierShareGroupKeyFor rule the applier itself uses. Sharing a helper rather
        // than re-deriving the rule is the point: a record and the twin it describes must always
        // land in the same bucket, and a second spelling of "0 means its own group" is exactly
        // how they would stop doing that.
        Bool ResolveThreadTwinKey(MG_Backend::DirectGLES::TwinKey* outKey) {
            SessionRuntime* runtime = t_runtime;
            if (runtime == nullptr) return false;
            outKey->SessionKey = reinterpret_cast<Uint64>(&runtime->Session());
            outKey->ShareGroupKey = MG_Pipe::MGPipeApplierShareGroupKeyFor(
                outKey->SessionKey, runtime->Session().CurrentContextToken());
            return true;
        }

        // The server's staged stores (StagedShadow.h) file their entries under the same two
        // words as the twin tables, for the same reason one level up: the bytes they hold belong
        // to one session's group, and a session's teardown may drop only its own.
        Bool ResolveThreadStagedBucket(MG_Record::StagedBucket* outBucket) {
            MG_Backend::DirectGLES::TwinKey key;
            if (!ResolveThreadTwinKey(&key)) return false;
            outBucket->SessionKey = key.SessionKey;
            outBucket->ShareGroupKey = key.ShareGroupKey;
            return true;
        }
    } // namespace

    void Detail::InstallSessionRuntimeHooks() {
        if (MG_Pipe::g_pipeInputsThreadResolver != &ResolveThreadInputs) {
            MG_Pipe::g_pipeInputsThreadResolver = &ResolveThreadInputs;
        }
        MG_Backend::DirectGLES::SetNativeContextKeyResolver(&ResolveThreadNativeContextKey);
        MG_Pipe::MGPipeSetApplierKeyResolver(&ResolveThreadApplierKey);
        // P14 S6: the twin tables' key. Same probe, one level down, installed beside the two it
        // is the twin of (the native tuple's and the applier's).
        MG_Backend::DirectGLES::SetTwinKeyResolver(&ResolveThreadTwinKey);
        MG_Record::SetStagedBucketResolver(&ResolveThreadStagedBucket);
    }

    SessionRuntime* Detail::CurrentRuntime() { return t_runtime; }

    ServerSession* Detail::CurrentSession() {
        SessionRuntime* runtime = t_runtime;
        return runtime != nullptr ? &runtime->Session() : nullptr;
    }

    ServerLoop* Detail::CurrentLoop() {
        SessionRuntime* runtime = t_runtime;
        return runtime != nullptr ? &runtime->Loop() : nullptr;
    }

    MG_Pipe::PipeInputs* Detail::CurrentInputs() { return ResolveThreadInputs(); }

    Detail::ThreadSessionScope::ThreadSessionScope(SessionRuntime& runtime) {
        InstallSessionRuntimeHooks();
        m_previous = t_runtime;
        t_runtime = &runtime;
        m_previousLatchDomain = FatalLatchDomainBind(runtime.LatchDomain());
        // The segment resolver is the OTHER process-wide hook that used to mean "one session": the
        // thunk is one function pointer for the process, and which TABLE it answers from is now
        // per thread (PipeWireCodec.h). Bound before anything on this thread can decode a record.
        runtime.Session().Segments().BindThreadResolver();
    }

    Detail::ThreadSessionScope::~ThreadSessionScope() {
        if (m_previous != nullptr) {
            m_previous->Session().Segments().BindThreadResolver();
        } else {
            Wire::SegmentTable::UnbindThreadResolver();
        }
        FatalLatchDomainBind(m_previousLatchDomain);
        t_runtime = m_previous;
    }

    SessionRuntime::SessionRuntime()
        : m_latchDomain(FatalLatchDomainOpen()) {}

    SessionRuntime::~SessionRuntime() {
        // P14 S5: this runtime's session is the key every applier it registered was filed
        // under, so the last act of the session is to drop them. Safe here for the reason the
        // latch domain is (SessionLoop::Stop has joined the apply thread; the control thread's
        // scope outlives this object), and idempotent against Accept's own release.
        MG_Pipe::MGPipeApplierReleaseSession(reinterpret_cast<Uint64>(&m_session));
        // No thread may still be bound to this domain: the apply thread is joined by
        // ServerLoop::Stop and the control thread's scope outlives this object.
        FatalLatchDomainClose(m_latchDomain);
        m_latchDomain = 0;
    }

    Uint32 LiveServerSessionCount() { return ServerSessionCount(); }

} // namespace MobileGL::MG_Remote::Server