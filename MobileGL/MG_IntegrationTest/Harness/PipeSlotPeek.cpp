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
        // One arm per member, and NO `default:` on purpose: adding a PipeSlotKind without
        // deciding which MGPipeKind it names is a compiler warning here (-Wswitch) rather than
        // a row that silently counts VertexElementsCso and reports "did not leak" about a kind
        // it never looked at. The trailing return is the unreachable one the compiler needs.
        MobileGL::MG_Pipe::MGPipeKind Translate(PipeSlotKind kind) {
            switch (kind) {
                case PipeSlotKind::Buffer: return MobileGL::MG_Pipe::MGPipeKind::Buffer;
                case PipeSlotKind::VertexElementsCso:
                    return MobileGL::MG_Pipe::MGPipeKind::VertexElementsCso;
                case PipeSlotKind::Texture: return MobileGL::MG_Pipe::MGPipeKind::Texture;
                case PipeSlotKind::Renderbuffer: return MobileGL::MG_Pipe::MGPipeKind::Renderbuffer;
                case PipeSlotKind::Framebuffer: return MobileGL::MG_Pipe::MGPipeKind::Framebuffer;
                case PipeSlotKind::SamplerCso: return MobileGL::MG_Pipe::MGPipeKind::SamplerCso;
                case PipeSlotKind::SamplerViewCso:
                    return MobileGL::MG_Pipe::MGPipeKind::SamplerViewCso;
                case PipeSlotKind::ShaderCso: return MobileGL::MG_Pipe::MGPipeKind::ShaderCso;
            }
            return MobileGL::MG_Pipe::MGPipeKind::None;
        }
    } // namespace

    bool PeekPipeSlotLiveCount(PipeSlotKind kind, unsigned* outLive) {
        if (outLive == nullptr) return false;
        *outLive = static_cast<unsigned>(MobileGL::MG_Pipe::MGPipeSlots().LiveCount(Translate(kind)));
        return true;
    }

    bool PeekPipeSlotHighWater(PipeSlotKind kind, unsigned* outHighWater) {
        if (outHighWater == nullptr) return false;
        // The ORDINARY space only, for every kind including ShaderCso (contract-v2.md 4.3).
        *outHighWater = static_cast<unsigned>(MobileGL::MG_Pipe::MGPipeSlots().HighWater(Translate(kind)));
        return true;
    }

    bool PeekPipeCompositeSlotLiveCount(unsigned* outLive) {
        if (outLive == nullptr) return false;
        *outLive = static_cast<unsigned>(MobileGL::MG_Pipe::MGPipeSlots().CompositeLiveCount());
        return true;
    }

    bool PeekPipeCompositeSlotHighWater(unsigned* outHighWater) {
        if (outHighWater == nullptr) return false;
        *outHighWater = static_cast<unsigned>(MobileGL::MG_Pipe::MGPipeSlots().CompositeHighWater());
        return true;
    }

    bool PeekPipeCompositeSlotBandBase(unsigned* outBandBase) {
        if (outBandBase == nullptr) return false;
        *outBandBase = static_cast<unsigned>(MobileGL::MG_Pipe::kMGPipeShaderCsoCompositeSlotBase);
        return true;
    }
#else
    bool PeekPipeSlotLiveCount(PipeSlotKind, unsigned*) { return false; }
    bool PeekPipeSlotHighWater(PipeSlotKind, unsigned*) { return false; }
    bool PeekPipeCompositeSlotLiveCount(unsigned*) { return false; }
    bool PeekPipeCompositeSlotHighWater(unsigned*) { return false; }
    bool PeekPipeCompositeSlotBandBase(unsigned*) { return false; }
#endif

} // namespace MGITest
