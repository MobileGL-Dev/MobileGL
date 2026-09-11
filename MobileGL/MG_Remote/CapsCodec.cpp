// MobileGL - MobileGL/MG_Remote/CapsCodec.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "CapsCodec.h"

#include <MG_Util/Debug/Log.h>

#include <cstdlib>

namespace MobileGL::MG_Remote {

    // The consumer mask may not collide with the MGPCapBits below it. kCapNeedsHostUboBytes
    // is 1<<8 today; this asserts the gap stays a gap rather than trusting the comment.
    static_assert((static_cast<Uint64>(MG_Pipe::kCapNeedsHostUboBytes) & kMGCapsConsumerMask) == 0,
                  "an MGPCapBit has grown into CallMask's consumer block (bits 32..47)");
    static_assert(MGCapsServerConsumes(MGCapsConsumerBits(MG_Pipe::kMGPipeSubsystemResources),
                                       MG_Pipe::kMGPipeSubsystemResources),
                  "the consumer encoding does not round-trip");
    static_assert(!MGCapsServerConsumes(MGCapsConsumerBits(MG_Pipe::kMGPipeSubsystemResources),
                                        MG_Pipe::kMGPipeSubsystemPrograms),
                  "the consumer encoding answers yes for a family it was not given");
    // P4a's highest allocated subsystem bit must fit the sixteen-bit block. This is the
    // assertion that turns "room to P8" from a comment into a build break.
    static_assert(MG_Pipe::kMGPipeSubsystemsMigratedAtP4a <= 0xFFFFull,
                  "the subsystem mask no longer fits CallMask's sixteen consumer bits");

#define MGP5_C0_STUB(what)                                                                                             \
    do {                                                                                                               \
        MGLOG_F("MGPipe: Fatal{UnimplementedCapsCodec, \"%s\"} - P5 package w1 has not landed "                        \
                "this yet; c0 shipped the signature only",                                                             \
                what);                                                                                                 \
        std::abort();                                                                                                  \
    } while (0)

    Bool EncodeFormatCapabilities(const MG_Backend::FormatCapabilityCache&, Vector<Uint8>&) {
        MGP5_C0_STUB("EncodeFormatCapabilities");
    }

    Bool DecodeFormatCapabilities(const void*, Uint64, MG_Backend::FormatCapabilityCache&) {
        MGP5_C0_STUB("DecodeFormatCapabilities");
    }

    Bool EncodeRendererInfo(const RendererInfo&, Vector<Uint8>&) { MGP5_C0_STUB("EncodeRendererInfo"); }

    Bool DecodeRendererInfo(const void*, Uint64, RendererInfo&) { MGP5_C0_STUB("DecodeRendererInfo"); }

    Uint64 CapsAbiFingerprint() { MGP5_C0_STUB("CapsAbiFingerprint"); }

#undef MGP5_C0_STUB

} // namespace MobileGL::MG_Remote
