// MobileGL - MobileGL/MG_Backend/DirectVulkan/MagmaSession.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "MagmaSession.h"

#include <atomic>

namespace MobileGL::MG_Backend::DirectVulkan {
    namespace {
        struct SessionLocalEntry {
            Detail::SessionLocalConstruct construct;
            Detail::SessionLocalDestroy destroy;
        };

        // Construct-on-first-use: SessionLocals are namespace-scope objects in several translation
        // units and register during static initialization, in no particular order.
        Vector<SessionLocalEntry>& Registry() {
            static auto* registry = new Vector<SessionLocalEntry>();
            return *registry;
        }

        std::atomic<Uint64> g_nextSessionId{1};

        thread_local MagmaSession* t_boundSession = nullptr;

        // Leak-at-exit, like the renderer it holds (GlobalObjects.cpp): nothing may tear the
        // monolith's renderer down from a static destructor after the driver is gone.
        MagmaSession& ProcessSession() {
            static auto* session = new MagmaSession();
            return *session;
        }
    } // namespace

    Uint32 Detail::RegisterSessionLocal(SessionLocalConstruct construct, SessionLocalDestroy destroy) {
        auto& registry = Registry();
        registry.push_back({construct, destroy});
        return static_cast<Uint32>(registry.size() - 1);
    }

    MagmaSession& Detail::CurrentMagmaSession() {
        MagmaSession* bound = t_boundSession;
        return bound != nullptr ? *bound : ProcessSession();
    }

    void* Detail::SessionLocalSlot(MagmaSession& session, Uint32 index) {
        if (index >= session.m_slots.size()) session.m_slots.resize(Registry().size(), nullptr);
        void*& slot = session.m_slots[index];
        if (slot == nullptr) slot = Registry()[index].construct();
        return slot;
    }

    MagmaSession::MagmaSession() : m_id(g_nextSessionId.fetch_add(1, std::memory_order_relaxed)) {
        // Every slot is built up front: the process-wide session is reached from whichever thread
        // the app renders on, and a lazily built slot would be a first-touch race between two.
        const auto& registry = Registry();
        m_slots.resize(registry.size(), nullptr);
        for (SizeT i = 0; i < registry.size(); ++i) m_slots[i] = registry[i].construct();
    }

    MagmaSession::~MagmaSession() {
        MagmaSession* const previous = t_boundSession;
        t_boundSession = this;
        const auto& registry = Registry();
        for (SizeT i = m_slots.size(); i-- > 0;) {
            if (m_slots[i] != nullptr) registry[i].destroy(m_slots[i]);
            m_slots[i] = nullptr;
        }
        // Never back to this session: it is gone.
        t_boundSession = previous == this ? nullptr : previous;
    }

    MagmaSessionScope::MagmaSessionScope(MagmaSession* session) : m_previous(t_boundSession) {
        t_boundSession = session;
    }

    MagmaSessionScope::~MagmaSessionScope() { t_boundSession = m_previous; }

    void BindMagmaSessionToThisThread(MagmaSession* session) { t_boundSession = session; }

    MagmaSession* BoundMagmaSession() { return t_boundSession; }

} // namespace MobileGL::MG_Backend::DirectVulkan
