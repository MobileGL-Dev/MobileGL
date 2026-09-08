// MobileGL - MobileGL/MG_IntegrationTest/Harness/PipeSlotPeek.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "PipeSlotPeek.h"

#if !defined(__ANDROID__)
#include <MG_Pipe/MGPipe.h>
#if MOBILEGL_PIPE_PUSH
#include <MG_Impl/Pipe/SlotAllocator.h>
#define MGITEST_PIPE_SLOT_PEEK_LIVE 1
#endif
#endif

namespace MGITest {

#if defined(MGITEST_PIPE_SLOT_PEEK_LIVE)
    namespace {
        MobileGL::MG_Pipe::MGPipeKind Translate(PipeSlotKind kind) {
            switch (kind) {
                case PipeSlotKind::Buffer: return MobileGL::MG_Pipe::MGPipeKind::Buffer;
                default: return MobileGL::MG_Pipe::MGPipeKind::VertexElementsCso;
            }
        }
    } // namespace

    bool PeekPipeSlotLiveCount(PipeSlotKind kind, unsigned* outLive) {
        if (outLive == nullptr) return false;
        *outLive = static_cast<unsigned>(MobileGL::MG_Pipe::MGPipeSlots().LiveCount(Translate(kind)));
        return true;
    }

    bool PeekPipeSlotHighWater(PipeSlotKind kind, unsigned* outHighWater) {
        if (outHighWater == nullptr) return false;
        *outHighWater = static_cast<unsigned>(MobileGL::MG_Pipe::MGPipeSlots().HighWater(Translate(kind)));
        return true;
    }
#else
    bool PeekPipeSlotLiveCount(PipeSlotKind, unsigned*) { return false; }
    bool PeekPipeSlotHighWater(PipeSlotKind, unsigned*) { return false; }
#endif

} // namespace MGITest
