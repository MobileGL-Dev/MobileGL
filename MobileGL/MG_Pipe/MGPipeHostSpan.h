// MobileGL - MobileGL/MG_Pipe/MGPipeHostSpan.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// The ONE thing in MGPipe whose shape changes with the transport (plan B section 4.5.7).
//
// Monolith: Ptr addresses the frontend shadow or the application's own memory and the
// accessor is one predictable branch. Split: Ptr is null and the bytes live in a staging
// segment named by Seg/Offset, or - for the index bytes a server-side primitive-restart
// rewrite or multi-draw flattening consumes - in the server's own index host mirror, which
// costs no wire traffic at all (D-B7).
namespace MobileGL::MG_Pipe {
    // Seg sentinels. Anything else is a real SEG_STAGE id assigned by the transport.
    inline constexpr Uint32 kMGHostSpanSegNone = 0;
    // "The bytes are already on your side": the server reads them out of the index host
    // mirror it maintains for every resource created with the ELEMENT_ARRAY bind bit while
    // kCapNeedsHostIndexBytes is set. When the mirror is over budget the tracker degrades
    // to per-draw staging and counts the bytes in index-bytes-shipped.
    inline constexpr Uint32 kMGHostSpanSegFromServerIndexMirror = 0xFFFFFFFFu;

    struct MGHostSpan {
        // Field order is chosen so the struct is 32 bytes with natural alignment on both a
        // 64-bit and a 32-bit host: the pointer and the two 32-bit words fill the first
        // 16-byte block either way.
        const void* Ptr;
        Uint32 Seg;
        Uint32 Pad0;
        Uint64 Size;
        Uint64 Offset;
    };

    static_assert(sizeof(MGHostSpan) == 32, "MGHostSpan is the 32-byte host-bytes descriptor");
    static_assert(std::is_trivially_copyable_v<MGHostSpan>);

    // Split-mode resolution needs the transport's segment table, which does not exist in a
    // monolith build; the hook is a weak-ish indirection installed by MG_Remote when it is
    // compiled in. In P0 there is no transport, so a span that names a segment resolves to
    // null and every caller is still on the monolith branch.
    using MGPipeSegmentResolver = const void* (*)(Uint32 seg, Uint64 offset, Uint64 size);
    inline MGPipeSegmentResolver gMGPipeSegmentResolver = nullptr;

    // One predictable branch on the hot path.
    inline const void* MGPipeHostBytes(const MGHostSpan& span) {
        if (span.Ptr != nullptr) {
            return static_cast<const Uint8*>(span.Ptr) + span.Offset;
        }
        if (gMGPipeSegmentResolver == nullptr) return nullptr;
        return gMGPipeSegmentResolver(span.Seg, span.Offset, span.Size);
    }
} // namespace MobileGL::MG_Pipe
