// MobileGL - MobileGL/MG_Backend/DirectVulkan/MagmaSession.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// MAGMA'S PER-SESSION STATE. Magma was written for one renderer per process: the renderer slot,
// the active buffer manager, the dynamic-state shadow, the render-pass manager's statics and the
// renderer generation are all file-scope. A multi-session server (several clients served from one
// process, each on its own apply thread with its own BackendObject_DirectVulkan) needs one copy of
// each per session, or one session's work lands in another's tables.
//
// SessionLocal<T> is that copy. It resolves through the CALLING THREAD's bound MagmaSession, and to
// the process-wide default session on every thread nothing bound - which is the monolith, the client
// role and every unit case, i.e. exactly the old behaviour, on whatever thread the app renders from.
// A served session's backend owns its MagmaSession and binds it on its apply thread (the only thread
// that runs that session's backend), so the two never mix.

#pragma once
#include <Includes.h>

#include <cstddef>
#include <new>
#include <utility>

namespace MobileGL::MG_Backend::DirectVulkan {

    class MagmaSession;

    namespace Detail {
        using SessionLocalConstruct = void* (*)();
        using SessionLocalDestroy = void (*)(void*);
        // Registers one SessionLocal<T> and returns its slot. Called from static initialization.
        Uint32 RegisterSessionLocal(SessionLocalConstruct construct, SessionLocalDestroy destroy);
        // The calling thread's bound session (MagmaSessionScope, BindMagmaSessionToThisThread), null
        // when it has none. P15: visible here, and constant-initialized, so the read inlines into
        // every SessionLocal access and one function's accesses can share a single TLS address
        // computation instead of paying one out-of-line call each.
        inline constinit thread_local MagmaSession* t_boundMagmaSession = nullptr;
        MagmaSession& ProcessMagmaSession();
        // The calling thread's session, or the process-wide default.
        inline MagmaSession& CurrentMagmaSession() {
            MagmaSession* const bound = t_boundMagmaSession;
            return bound != nullptr ? *bound : ProcessMagmaSession();
        }
        inline void* SessionLocalSlot(MagmaSession& session, Uint32 index);
        void* SessionLocalSlotSlow(MagmaSession& session, Uint32 index);
    } // namespace Detail

    // One served session's copy of every SessionLocal. Slots are built lazily on first use and
    // destroyed in reverse registration order with the session still bound, so a destructor that
    // reaches another SessionLocal finds this session's copy and not the binding thread's.
    class MagmaSession {
    public:
        MagmaSession();
        ~MagmaSession();
        MagmaSession(const MagmaSession&) = delete;
        MagmaSession& operator=(const MagmaSession&) = delete;

        // A monotonic id, unique for the process lifetime. Never an address: a later session can
        // be allocated where an ended one lived.
        Uint64 Id() const { return m_id; }

    private:
        friend void* Detail::SessionLocalSlot(MagmaSession& session, Uint32 index);
        friend void* Detail::SessionLocalSlotSlow(MagmaSession& session, Uint32 index);
        Vector<void*> m_slots;
        Uint64 m_id = 0;
    };

    // Makes `session` the calling thread's Magma session for the scope's lifetime, restoring the
    // previous binding (usually none) afterwards.
    // A built slot is returned in line; a missing one is built out of line.
    inline void* Detail::SessionLocalSlot(MagmaSession& session, Uint32 index) {
        if (index < session.m_slots.size()) {
            if (void* const slot = session.m_slots[index]) return slot;
        }
        return SessionLocalSlotSlow(session, index);
    }

    class MagmaSessionScope {
    public:
        explicit MagmaSessionScope(MagmaSession* session);
        ~MagmaSessionScope();
        MagmaSessionScope(const MagmaSessionScope&) = delete;
        MagmaSessionScope& operator=(const MagmaSessionScope&) = delete;

    private:
        MagmaSession* m_previous = nullptr;
    };

    // Binds `session` to the calling thread until the next call (nullptr unbinds). For the apply
    // thread, which binds its backend once and runs it for the rest of its life.
    void BindMagmaSessionToThisThread(MagmaSession* session);
    // The calling thread's bound session, or nullptr (the process-wide default answers).
    MagmaSession* BoundMagmaSession();

    template <class T>
    class SessionLocal {
    public:
        SessionLocal() : m_index(Detail::RegisterSessionLocal(&Construct, &Destroy)) {}
        SessionLocal(const SessionLocal&) = delete;
        SessionLocal& operator=(const SessionLocal&) = delete;

        T& Get() const {
            return *static_cast<T*>(Detail::SessionLocalSlot(Detail::CurrentMagmaSession(), m_index));
        }
        T& operator*() const { return Get(); }
        T* operator->() const { return &Get(); }

    private:
        static void* Construct() { return new T(); }
        static void Destroy(void* object) { delete static_cast<T*>(object); }
        const Uint32 m_index;
    };

} // namespace MobileGL::MG_Backend::DirectVulkan
