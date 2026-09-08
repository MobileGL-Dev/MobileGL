// MobileGL - MobileGL/MG_Impl/Pipe/SetHashSuppressor.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// Coalescing rule 4 (ARCHITECTURE.md 5.4, P2 brief D11): every kVarTail set_* hashes the
// RESOLVED set on the client and does not emit when the hash has not moved.
//
// This is the carrier for the ~175 lines of debounce that move off the backends in P3b and
// P4b - Espryt's UnitBindingsSnapshot / CaptureUnitBindings / UnitBindingsUnchanged and
// Magma's equivalents all answer "is this set the same set as last time", and every one of
// them answers it against a shape the backend rediscovered. P2 lands the MECHANISM and ONE
// real consumer (SetVertexAttribDefaults) so the shape is pinned by a test rather than by a
// plan; the other six slots exist, are unit-tested, and are wired by the phase that moves
// the set they name. P3a wires the second, SetVertexBuffers. P4a wires SetSamplerViews,
// BindSamplerStates and SetShaderImages, and APPENDS an eighth slot, SetFramebufferState -
// which leaves only SetShaderBuffers and SetStreamOutputTargets unwired, both P4b's.
//
// A WIRED SLOT PUTS A REQUIREMENT ON ITS HASH, and SetVertexBuffers is where that first
// bites: the hash has to cover EVERY input the record carries, not only the set. Its
// baseInstance is DRAW state and moves without the buffer set moving, so a hash over the
// entries alone would suppress a record whose one changed field is the fetch shift and the
// server would keep the previous one. MG_Impl/Pipe/VertexInputEmit.h's
// MGPipeVertexBufferSetContentHash mixes Start, Count and BaseInstance in for exactly that
// reason, and VertexInputEmit's base-instance pair is the test that says so.
//
// A hash of 0 is reserved for "never emitted", so the first emission always goes out; a
// computed 0 is remapped to 1, which costs one collision in 2^64 an extra emission and
// never a missed one.
//
// Header-only for the same ownership reason as Tracker.h and CsoCache.h: the root
// CMakeLists.txt that would name a new .cpp belongs to package A and is frozen behind the
// p2/contract tag.
#if MOBILEGL_PIPE_PUSH
#include <MG_Pipe/MGPipe.h>

namespace MobileGL::MG_Pipe {

    // One slot per kVarTail set_* (ARCHITECTURE.md 5.1's call list), PLUS
    // SetFramebufferState, which is not kVarTail at all: MGPFramebufferState carries a
    // ContentHash for TWO jobs - the server's render-pass memo key and the client's emission
    // suppressor - and the second one needs a slot here like any other. The enum is
    // CLIENT-ONLY and is not a wire opcode, so appending before Count is safe.
    enum class MGPipeSuppressorSlot : Uint32 {
        SetVertexBuffers = 0,     // P3a - wired, and its hash includes BaseInstance
        // P4a - WIRED. The three unit sets' suppressors are not optional and were never a
        // later phase's: MGPipeTypes.h makes the pattern mandatory for every kVarTail set_*,
        // because GetTextureBindGeneration() bumps on a REDUNDANT rebind - MC 26.2 rebinds the
        // same sampler at every texture-unit switch - so an unsuppressed set is a
        // several-hundred-byte variable-length record per batch, which is the exact regression
        // the design names. What P3b/P4b owns is the ~175-line BACKEND debounce these replace
        // (UnitBindingsSnapshot / CaptureUnitBindings / UnitBindingsUnchanged and the two
        // g_*SyncList tables); P4a wires the carrier, P3b/P4b deletes the backend copy.
        SetSamplerViews,          // P4a - wired (backend debounce deletion: P3b/P4b)
        BindSamplerStates,        // P4a - wired (backend debounce deletion: P3b/P4b)
        SetShaderImages,          // P4a - wired (backend debounce deletion: P3b/P4b)
        SetShaderBuffers,         // P4b
        SetStreamOutputTargets,   // P4b
        SetVertexAttribDefaults,  // P2 - the one consumer that is wired
        SetFramebufferState,      // P4a - wired
        Count,
    };

    inline constexpr SizeT kMGPipeSuppressorSlotCount = static_cast<SizeT>(MGPipeSuppressorSlot::Count);

    class MGPipeSetHashSuppressor {
    public:
        // True when `contentHash` differs from what this slot last emitted, and LATCHES it.
        // False means the resolved set has not moved and the call must not go out.
        Bool ShouldEmit(MGPipeSuppressorSlot slot, Uint64 contentHash) {
            const Uint64 latched = contentHash == 0 ? 1 : contentHash;
            const SizeT index = static_cast<SizeT>(slot);
            if (m_lastEmitted[index] == latched) return false;
            m_lastEmitted[index] = latched;
            return true;
        }

        // A context change or a server reset: what the server has is no longer what this
        // slot last emitted, so the next resolved set must go out whatever it hashes to.
        void Invalidate(MGPipeSuppressorSlot slot) { m_lastEmitted[static_cast<SizeT>(slot)] = 0; }

        void InvalidateAll() {
            for (SizeT i = 0; i < kMGPipeSuppressorSlotCount; ++i) m_lastEmitted[i] = 0;
        }

        // 0 == "never emitted". Exposed for the unit test, which is what pins that the
        // reserved value really is reserved.
        Uint64 LastEmitted(MGPipeSuppressorSlot slot) const {
            return m_lastEmitted[static_cast<SizeT>(slot)];
        }

    private:
        Array<Uint64, kMGPipeSuppressorSlotCount> m_lastEmitted{};
    };

    // The monolith's one suppressor, beside the tracker and the CSO cache.
    inline MGPipeSetHashSuppressor& MGPipeSetHashSuppressorInstance() {
        // NEVER DESTROYED, for MGPipeTrackerInstance()' reason (MG_Impl/Pipe/Tracker.h): the
        // rule covers every MGPipe process singleton, not only the ones on today's death
        // paths.
        static MGPipeSetHashSuppressor* suppressor = new MGPipeSetHashSuppressor();
        return *suppressor;
    }
} // namespace MobileGL::MG_Pipe
#endif // MOBILEGL_PIPE_PUSH
