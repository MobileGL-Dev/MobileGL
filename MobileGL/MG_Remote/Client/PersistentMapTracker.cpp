// MobileGL - MobileGL/MG_Remote/Client/PersistentMapTracker.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "PersistentMapTracker.h"

#include <MG_State/GLState/BufferState/BufferObject.h>
#include <MG_Util/Debug/Log.h>

#include <cstdlib>

namespace MobileGL::MG_Remote::Client {

    using MG_State::GLState::BufferObject;
    using MobileGL::BufferMappingAccessBit;

    PersistentMapTracker& PersistentMapTracker::Instance() {
        // Leaked on purpose, once, like every other role-local singleton (ID-8): a buffer's
        // destructor runs from exit handlers after this TU's globals would already be gone,
        // and it calls Forget().
        static PersistentMapTracker* instance = new PersistentMapTracker{};
        return *instance;
    }

    Uint64 PersistentMapTracker::BlockBytes() {
        return static_cast<Uint64>(MG_Config::Ipc.PersistentBlockKb) * 1024ull;
    }

    Bool PersistentMapTracker::PushIsArmed() {
        return MG_Config::Transport != MG_Config::TransportMode::Monolith;
    }

    // SyncPersistentMappedRange's early-out chain (BufferObject.cpp:341-353), in its order,
    // read as a membership test. Every line here has a line there; if one of them moves, the
    // unit case that drives both against each other is what says so.
    Bool PersistentMapTracker::IsLivePersistentMap(const BufferObject& buffer) {
        if (!buffer.IsMapped()) return false;
        // GPU-resident: the application already wrote into coherent GPU memory and there is
        // nothing to ship. At tier T2 this arm is unreachable - MapPersistent declines - but
        // the predicate must still read the chain, not the tier: a build that reaches T0/T1
        // later must see this row answer for itself.
        if (buffer.IsBackendPersistentMapped()) return false;
        const auto access = buffer.GetMappingAccess();
        if (!(access & BufferMappingAccessBit::Persistent)) return false;
        if (!(access & BufferMappingAccessBit::Write)) return false;
        // FLUSH_EXPLICIT: the application promises to announce its own writes with
        // glFlushMappedBufferRange, which already crosses as resource_flush_range. Pushing
        // here as well would ship the same bytes twice and take the upload-shape decision
        // away from the side that pays for it.
        if (access & BufferMappingAccessBit::FlushExplicit) return false;
        const auto range = buffer.GetMappedRange();
        if (range.start >= range.end) return false;
        return true;
    }

    void PersistentMapTracker::NoteMapStateChanged(BufferObject& buffer) {
        const Uint64 key = buffer.GetLifetimeId();
        if (IsLivePersistentMap(buffer)) {
            m_livePersistentMaps[key] = &buffer;
            return;
        }
        m_livePersistentMaps.erase(key);
    }

    void PersistentMapTracker::Forget(const BufferObject& buffer) {
        m_livePersistentMaps.erase(buffer.GetLifetimeId());
    }

    void PersistentMapTracker::PushBlocksFor(BufferObject& buffer) {
        if (!PushIsArmed()) return;
        // Re-checked rather than trusted. The set is maintained at five events and a sixth
        // one arriving without a NoteMapStateChanged would otherwise push a buffer whose
        // shadow has been released - an adopted store's Bytes() is the GPU map, and reading
        // it as if it were the shadow is how a "conservative" push turns into a fault.
        if (!IsLivePersistentMap(buffer)) {
            Forget(buffer);
            return;
        }
        const Uint64 blockBytes = BlockBytes();
        // 0 IS THE NEGATIVE CONTROL, NOT "unlimited" (E3(a)). Pushing one whole-span block
        // here would make the control green for the wrong reason - it has to disable the
        // push, so that PersistentCoherentMapScenario draws the last uploaded bytes and goes
        // red exactly the way an unpushed map does.
        if (blockBytes == 0) return;

        const auto range = buffer.GetMappedRange();
        const Uint64 begin = static_cast<Uint64>(range.start);
        const Uint64 end = static_cast<Uint64>(range.end);
        for (Uint64 at = begin; at < end; at += blockBytes) {
            const Uint64 length = (end - at) < blockBytes ? (end - at) : blockBytes;
            buffer.PushMappedSpanBlock(static_cast<SizeT>(at), static_cast<SizeT>(length));
            ++m_blocksPushed;
            m_bytesPushed += length;
        }
    }

    void PersistentMapTracker::PushAllMembers() {
        if (!PushIsArmed()) return;
        if (m_livePersistentMaps.empty()) return;
        // Copied out first: PushBlocksFor can erase its own entry (a member that stopped
        // being one), and ska::flat_hash_map invalidates on erase.
        Vector<BufferObject*> members;
        members.reserve(m_livePersistentMaps.size());
        for (const auto& entry : m_livePersistentMaps) members.push_back(entry.second);
        for (BufferObject* buffer : members) {
            if (buffer != nullptr) PushBlocksFor(*buffer);
        }
    }

    void PushPersistentMapsBeforeVerb() {
        PersistentMapTracker::Instance().PushAllMembers();
    }

    Bool AdoptTierIsEmulate() {
        const Uint32 tier = MG_Config::Ipc.AdoptTier;
        if (tier == 2) return true;
        // A NAMED refusal, not a silent fall back to T2. T0 (a real cross-process shared
        // mapping) and T1 (a server-side staging map) are P11's, and the reason the knob
        // parses them today is that the negative control needs a spelling before the thing
        // it controls exists. Falling back would make `MOBILEGL_IPC_ADOPT_TIER=0` look like
        // a working T0 run and silently produce pmap bytes it must not produce.
        MGLOG_F("MGPipe: MOBILEGL_IPC_ADOPT_TIER=%u names an adoption tier P11 implements and P5 "
                "does not; P5 runs at T2 (emulate) only.",
                static_cast<unsigned>(tier));
        std::abort();
    }

} // namespace MobileGL::MG_Remote::Client
