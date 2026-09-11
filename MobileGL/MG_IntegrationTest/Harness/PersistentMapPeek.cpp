// MobileGL - MobileGL/MG_IntegrationTest/Harness/PersistentMapPeek.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "PersistentMapPeek.h"

#if !defined(__ANDROID__)
#include <MG_State/GLState/BufferState/BufferObject.h>
#include <MG_State/GLState/Core.h>
#define MGITEST_PERSISTENT_MAP_PEEK_LIVE 1
#endif

// MGITEST_PERSISTENT_MAP_TRACKER is defined by MG_IntegrationTest/CMakeLists.txt, and only when
// BOTH halves are true: the build compiled MG_Remote (so the symbol can link) AND some source
// under MG_Remote/Client names IsLivePersistentMap (so package b1 landed it). __has_include is
// NOT enough on its own - the header exists in the source tree of every build, including the pull
// build that never compiles MG_Remote, so keying on it would turn a healthy pull build into a
// link error.
#if defined(MGITEST_PERSISTENT_MAP_PEEK_LIVE) && defined(MGITEST_PERSISTENT_MAP_TRACKER)
#include <MG_Remote/Client/PersistentMapTracker.h>
#define MGITEST_PERSISTENT_MAP_TRACKER_LIVE 1
#endif

namespace MGITest {

    bool PersistentMapPeekAvailable() {
#if defined(MGITEST_PERSISTENT_MAP_PEEK_LIVE)
        return true;
#else
        return false;
#endif
    }

    bool PersistentMapTrackerAvailable() {
#if defined(MGITEST_PERSISTENT_MAP_TRACKER_LIVE)
        return true;
#else
        return false;
#endif
    }

#if defined(MGITEST_PERSISTENT_MAP_PEEK_LIVE)
    namespace {
        // The frontend object behind a GL buffer name, or null. GetBufferObject mints on demand
        // for a name that was generated and never bound, which is harmless here: a scenario only
        // ever asks about a buffer it has already defined and mapped, and a null store answers
        // "not adopted", which is the same answer an un-mapped buffer would give.
        MobileGL::MG_State::GLState::BufferObject* FrontendBuffer(unsigned int bufferName) {
            if (bufferName == 0) return nullptr;
            auto& context = MobileGL::MG_State::pGLContext;
            if (!context) return nullptr;
            const auto& buffer = context->GetBufferObject(static_cast<MobileGL::Uint>(bufferName));
            return buffer.get();
        }
    } // namespace
#endif

    bool PeekBufferIsAdoptedPersistentMap(unsigned int bufferName, bool* outAdopted) {
#if defined(MGITEST_PERSISTENT_MAP_PEEK_LIVE)
        if (outAdopted == nullptr) return false;
        MobileGL::MG_State::GLState::BufferObject* buffer = FrontendBuffer(bufferName);
        if (buffer == nullptr) return false;
        *outAdopted = static_cast<bool>(buffer->IsBackendPersistentMapped());
        return true;
#else
        (void)bufferName;
        (void)outAdopted;
        return false;
#endif
    }

    bool PeekBufferIsLivePersistentMap(unsigned int bufferName, bool* outLive) {
#if defined(MGITEST_PERSISTENT_MAP_TRACKER_LIVE)
        if (outLive == nullptr) return false;
        MobileGL::MG_State::GLState::BufferObject* buffer = FrontendBuffer(bufferName);
        if (buffer == nullptr) return false;
        *outLive = static_cast<bool>(
            MobileGL::MG_Remote::Client::PersistentMapTracker::IsLivePersistentMap(*buffer));
        return true;
#else
        (void)bufferName;
        (void)outLive;
        return false;
#endif
    }

} // namespace MGITest
