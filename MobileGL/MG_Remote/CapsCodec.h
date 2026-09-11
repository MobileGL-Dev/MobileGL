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
// MGPCaps (MG_Pipe/MGPipeTypes.h:126-139) has four members. Two are flat and cross by memcpy
// (DynamicBackendParameters Dynamic, Uint64 CallMask); two are MGPBlobRefs over containers
// and HAVE NO SERIALIZER IN THE TREE - the header says so itself at MGPipeTypes.h:134-136,
// "Their serializers land with the transport (P5)". They are:
//
//   FormatCapabilities -> FormatCapabilityCache (MG_Backend/BackendObject.h:93-98):
//       FullCaps + CaveatCaps (bitfield tables) + SampleCounts, a Vector<Int> per
//       (target, format) pair. The Vector is why this cannot be a memcpy.
//   RendererInfo       -> RendererInfo (MG_Util/Types.h:317): three Strings, an
//       Optional<String>, and a Vector<GLExtension> inside GLInfo.
//
// THE ABI RULING (table 0). MGPCaps has only a COMPOSITIONAL size assertion
// (MGPipeTypes.h:145-146) because DynamicBackendParameters still carries SizeT and GLenum
// members - P0.5's fixed-width rewrite did not happen. P5 does NOT rewrite it. Instead the
// handshake asserts that both peers agree on sizeof(DynamicBackendParameters),
// sizeof(MGPCaps) and the build fingerprint, and Fatal{AbiMismatch} otherwise. P6's spawn is
// same-machine and same-binary, so it inherits this unchanged; the fixed-width rewrite is on
// P7's account.

#pragma once
#include <Includes.h>

#include <MG_Backend/BackendObject.h>
#include <MG_Pipe/MGPipe.h>

namespace MobileGL::MG_Remote {

    // ---- CallMask's layout (c0's ruling, extending R-8) ---------------------------------
    //
    // R-8 requires the client's liveness gates - MGPipeResourceSubsystemEnabled() and
    // P4aFamilyHasItsConsumer() - to answer from MGPCaps::CallMask instead of from
    // MGPipeGetResourceOps(), because that op table is the SERVER's registration: under
    // inproc a client reading it is right by accident, and under spawn it is null and five
    // whole record families silently emit nothing.
    //
    // But CallMask as declared carries only the nine MGPCapBit FEATURE bits
    // (MGPipeTypes.h:108-124) and has no per-family bit at all, so "read the CallMask" was
    // not yet an implementable instruction. It is now:
    //
    //   bits  0..8   MGPCapBit, unchanged. kCapNeedsHostIndexBytes and kCapNeedsHostUboBytes
    //                are BOTH ZERO in P5 by ruling (table 0), which is what keeps every
    //                MGHostSpan out of the first IPC frame.
    //   bits  9..31  reserved for further MGPCapBits.
    //   bits 32..47  THE CONSUMER MASK: bit (32 + n) means "the server has a consumer for
    //                MGPipe subsystem bit n" - i.e. the server's own subsystem mask, shifted.
    //                Sixteen bits covers bits 0..12 allocated through P4a with room to P8.
    //   bits 48..63  reserved.
    //
    // protocol.fbs:94's `tableSlotMask: ulong` is DELETED rather than renamed (R-8 offered
    // either). Two reasons, and the second is decisive: ARCHITECTURE.md:114 says CallMask
    // REPLACES "is this table slot null" as the capability probe, so a field whose comment is
    // "which GLFunctionsTable slots the peer registered" re-introduces exactly what it
    // replaced; and GLFunctionsTable has SIXTY-NINE function-pointer slots
    // (BackendObject.h:117-292), so a 64-bit mask cannot address it and never could.
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

    // ---- the ABI assertion the handshake carries ----------------------------------------
    //
    // Mixes sizeof(DynamicBackendParameters), sizeof(MGPCaps), sizeof(GLFunctionsTable) and
    // the compile-time build fingerprint. Compared in Hello/Welcome; a mismatch is
    // Fatal{AbiMismatch} and never a downgrade, because every alternative silently reads one
    // struct as another.
    Uint64 CapsAbiFingerprint();

} // namespace MobileGL::MG_Remote
