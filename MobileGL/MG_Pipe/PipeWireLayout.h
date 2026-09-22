// MobileGL - P6.5 wire compatibility facts. Generated member coverage is checked by gen_pipe.py.
#pragma once
#include "MGPipe.h"
#include "MGPipeRenderStateSpans.h"

namespace MobileGL::MG_Pipe {
#include "generated/PipeWireLayout.inc"

    static_assert(sizeof(Bool) == 1 && sizeof(Float) == 4 && sizeof(Int32) == 4,
                  "wire scalar representation is unsupported");
    static_assert(sizeof(RenderStateParameters) == 1168, "render-state wire blob changed");

    constexpr void WireMixNumber(Uint64& hash, Uint64 value) {
        for (unsigned i = 0; i < 8; ++i) {
            hash ^= (value >> (i * 8)) & 255;
            hash *= 1099511628211ull;
        }
    }
    constexpr void WireMixName(Uint64& hash, const char* name) {
        do {
            hash ^= static_cast<unsigned char>(*name);
            hash *= 1099511628211ull;
        } while (*name++ != '\0');
    }
    constexpr Uint64 WireMemberLayoutDigest(const WireLayoutMember* fields, SizeT count) {
        Uint64 hash = 1469598103934665603ull;
        for (SizeT i = 0; i < count; ++i) {
            WireMixName(hash, fields[i].Name);
            WireMixNumber(hash, fields[i].Offset);
            WireMixNumber(hash, fields[i].Size);
        }
        return hash;
    }
    inline constexpr Uint64 kMGPipeWireMemberLayoutDigest = WireMemberLayoutDigest(
        kMGPipeWireLayoutMembers, sizeof(kMGPipeWireLayoutMembers) / sizeof(WireLayoutMember));

    constexpr Uint64 WireCatalogueDigest() {
        Uint64 hash = 1469598103934665603ull;
        for (const auto& call : kMGPipeWireCallLayouts) {
            WireMixName(hash, call.Name);
            WireMixName(hash, call.Payload);
            WireMixNumber(hash, call.Op);
            WireMixNumber(hash, call.Flags);
            WireMixNumber(hash, call.Wait);
        }
        return hash;
    }
    inline constexpr Uint64 kMGPipeWireCatalogueDigest = WireCatalogueDigest();
    constexpr Uint64 WireRenderStateDigest() {
        Uint64 hash = 1469598103934665603ull;
        WireMixNumber(hash, kMGPipeRenderStateChunkTableVersion);
        for (SizeT boundary : kMGPipeRenderStateChunkBoundaries) WireMixNumber(hash, boundary);
        return hash;
    }
} // namespace MobileGL::MG_Pipe
