// MobileGL - MobileGL/MG_Remote/CapsCodec.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The MGPCaps snapshot's serializers, and the CallMask encoding. Owner: package w1 (the two
// blob codecs) and s1 (the handshake asserts). Signatures by c0.
//
// MGPCaps (MG_Pipe/MGPipeTypes.h) has four members. Two are flat and cross by memcpy
// (DynamicBackendParameters Dynamic, Uint64 CallMask); two are MGPBlobRefs over containers
// and HAVE NO SERIALIZER IN THE TREE - the header says so itself above MGPCaps::FormatCapabilities,
// "Their serializers land with the transport (P5)". They are:
//
//   FormatCapabilities -> FormatCapabilityCache (BackendCaps.h):
//       FullCaps + CaveatCaps (bitfield tables) + SampleCounts, a Vector<Int> per
//       (target, format) pair. The Vector is why this cannot be a memcpy.
//   RendererInfo       -> RendererInfo (MG_Util/Types.h:317): three Strings, an
//       Optional<String>, and a Vector<GLExtension> inside GLInfo.
//
// P6.5: wire layout and build identity are independent handshake facts.

#pragma once
#include <Includes.h>

#include <MG_Backend/BackendObject.h>
#include <MG_Pipe/MGPipe.h>

#include "Transport/SessionRings.h" // AbiFingerprintInputs / MixAbiFingerprint

namespace MobileGL::MG_Remote {

    // ---- CallMask's layout ---------------------------------------------------------------
    // The client's consumer gates (MGPipeResourceSubsystemEnabled, P4aFamilyHasItsConsumer)
    // read CallMask, never MGPipeGetResourceOps(): that op table is the server's registration.
    //   bits  0..31  MGPCapBit. Reserved bits (kCapViewportArray, kCapNeedsHost*Bytes) are
    //                never published and their positions are never reused.
    //   bits 32..47  consumer mask: bit (32 + n) = the server consumes MGPipe subsystem bit n.
    //   bits 48..63  reserved.
    // There is no table-slot mask: CallMask replaces the null-slot probe (ARCHITECTURE.md §3.3).
    inline constexpr Uint32 kMGCapsConsumerBitShift = 32;
    inline constexpr Uint64 kMGCapsConsumerMask = 0xFFFFull << kMGCapsConsumerBitShift;

    // Server side: fold the subsystems this server actually consumes into a CallMask.
    inline constexpr Uint64 MGCapsConsumerBits(Uint64 subsystemMask) {
        return (subsystemMask & 0xFFFFull) << kMGCapsConsumerBitShift;
    }

    // Client side: the ONE legal spelling of "does the server consume this family".
    // `subsystemBit` is a kMGPipeSubsystem* constant (MG_Pipe/MGPipe.h), not an index.
    inline constexpr Bool MGCapsServerConsumes(Uint64 callMask, Uint64 subsystemBit) {
        return (callMask & MGCapsConsumerBits(subsystemBit)) != 0;
    }

    // ---- the two blob codecs ------------------------------------------------------------
    //
    // Byte-stable within one build; the handshake's fingerprint is what makes that enough.
    // Both decoders must tolerate a truncated or over-long buffer by returning false, never
    // by reading past `size`: these bytes arrive over the wire.
    Bool EncodeFormatCapabilities(const MG_Backend::FormatCapabilityCache& cache, Vector<Uint8>& out);
    Bool DecodeFormatCapabilities(const void* bytes, Uint64 size, MG_Backend::FormatCapabilityCache& out);

    Bool EncodeRendererInfo(const RendererInfo& info, Vector<Uint8>& out);
    Bool DecodeRendererInfo(const void* bytes, Uint64 size, RendererInfo& out);

    // Wire compatibility never includes a git stamp or local function-pointer tables.
    Transport::AbiFingerprintInputs CapsAbiFingerprintInputs();
    Uint64 WireFingerprint();
    Uint64 CapsAbiFingerprint(); // legacy spelling of WireFingerprint
    const char* BuildFingerprint();
    Bool BuildFingerprintPresent();

} // namespace MobileGL::MG_Remote
