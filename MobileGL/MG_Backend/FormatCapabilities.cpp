// MobileGL - MobileGL/MG_Backend/FormatCapabilities.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <BackendCaps.h>
#include "MG_Util/Converters/MGToStr/TextureEnumConverter.h"
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace MobileGL::MG_Backend {
    namespace {
        const char* GetFormatCapabilitySupportString(const FormatCapabilityCache& cache, SizeT targetIndex,
                                                     SizeT formatIndex, FormatCapability capability) {
            if (HasFormatCapability(cache.FullCaps[targetIndex][formatIndex], capability)) return "Full";
            if (HasFormatCapability(cache.CaveatCaps[targetIndex][formatIndex], capability)) return "Caveat";
            return "None";
        }

        SizeT GetPrintedFormatNameWidth() {
            SizeT width = 0;
            for (SizeT formatIndex = 0; formatIndex < kFormatCapabilityFormatCount; ++formatIndex) {
                const auto format = static_cast<TextureInternalFormat>(formatIndex);
                width = std::max(width, MG_Util::ConvertTextureInternalFormatToString(format).size());
            }
            return width;
        }

        SizeT GetCapabilityColumnWidth(FormatCapability capability) {
            SizeT width = std::strlen(GetFormatCapabilityName(capability));
            width = std::max<SizeT>(width, std::strlen("Caveat"));
            return width;
        }

        String BuildFormatCapabilityHeader(SizeT formatNameWidth) {
            std::ostringstream line;
            line << std::left << std::setw(static_cast<Int>(formatNameWidth)) << "";
            for (FormatCapability capability : kReportedFormatCapabilities) {
                line << " | " << std::left << std::setw(static_cast<Int>(GetCapabilityColumnWidth(capability)))
                     << GetFormatCapabilityName(capability);
            }
            return line.str();
        }

        String BuildFormatCapabilityRow(const FormatCapabilityCache& cache, SizeT targetIndex, SizeT formatIndex,
                                        SizeT formatNameWidth) {
            const auto format = static_cast<TextureInternalFormat>(formatIndex);
            std::ostringstream line;
            line << std::left << std::setw(static_cast<Int>(formatNameWidth))
                 << MG_Util::ConvertTextureInternalFormatToString(format);
            for (FormatCapability capability : kReportedFormatCapabilities) {
                line << " | " << std::left << std::setw(static_cast<Int>(GetCapabilityColumnWidth(capability)))
                     << GetFormatCapabilitySupportString(cache, targetIndex, formatIndex, capability);
            }
            return line.str();
        }
    } // namespace

    void FormatCapabilityCache::Clear() {
        for (auto& row : FullCaps) {
            row.fill(FormatCapabilityFlags{});
        }
        for (auto& row : CaveatCaps) {
            row.fill(FormatCapabilityFlags{});
        }
        for (auto& row : SampleCounts) {
            for (auto& counts : row) {
                counts.clear();
            }
        }
    }

    Bool HasFormatCapability(FormatCapabilityFlags caps, FormatCapability capability) {
        return static_cast<Bool>(caps & capability);
    }

    SizeT GetFormatCapabilityTargetIndex(TextureTarget target) {
        if (target == TextureTarget::Unknown || static_cast<Int>(target) < 0 ||
            static_cast<SizeT>(target) >= kFormatCapabilityTextureTargetCount) {
            return kFormatCapabilityTargetCount;
        }
        // An external texture is a 2D one wherever a format can go.
        if (target == TextureTarget::External) return static_cast<SizeT>(TextureTarget::Texture2D);
        return static_cast<SizeT>(target);
    }

    SizeT GetRenderbufferFormatCapabilityTargetIndex() {
        return kFormatCapabilityRenderbufferTargetIndex;
    }

    const char* GetFormatCapabilityName(FormatCapability capability) {
        switch (capability) {
        case FormatCapability::Creatable:
            return "Creatable";
        case FormatCapability::Sampled:
            return "Sampled";
        case FormatCapability::LinearFilter:
            return "LinearFilter";
        case FormatCapability::GenerateMipmap:
            return "GenerateMipmap";
        case FormatCapability::TextureGather:
            return "TextureGather";
        case FormatCapability::TextureShadow:
            return "TextureShadow";
        case FormatCapability::FramebufferRenderable:
            return "FramebufferRenderable";
        case FormatCapability::FramebufferLayered:
            return "FramebufferLayered";
        case FormatCapability::MultisampleTexture:
            return "MultisampleTexture";
        case FormatCapability::MultisampleRenderbuffer:
            return "MultisampleRenderbuffer";
        case FormatCapability::ColorAttachment:
            return "ColorAttachment";
        case FormatCapability::DepthAttachment:
            return "DepthAttachment";
        case FormatCapability::StencilAttachment:
            return "StencilAttachment";
        case FormatCapability::TextureBuffer:
            return "TextureBuffer";
        }
        return "Unknown";
    }

    String GetFormatCapabilityTargetName(SizeT targetIndex) {
        if (targetIndex == kFormatCapabilityRenderbufferTargetIndex) {
            return "Renderbuffer";
        }
        if (targetIndex >= kFormatCapabilityTextureTargetCount) {
            return "Unknown";
        }
        return MG_Util::ConvertTextureTargetToString(static_cast<TextureTarget>(targetIndex));
    }

    void PrintFormatCapabilities(const FormatCapabilityCache& cache) {
        const SizeT formatNameWidth = GetPrintedFormatNameWidth();

        MGLOG_D("Backend format capabilities:");
        for (SizeT targetIndex = 0; targetIndex < kFormatCapabilityTargetCount; ++targetIndex) {
            MGLOG_D("");
            const String targetName = GetFormatCapabilityTargetName(targetIndex);
            MGLOG_D("- %s", targetName.c_str());
            const String header = BuildFormatCapabilityHeader(formatNameWidth);
            MGLOG_D("%s", header.c_str());
            for (SizeT formatIndex = 0; formatIndex < kFormatCapabilityFormatCount; ++formatIndex) {
                const String row = BuildFormatCapabilityRow(cache, targetIndex, formatIndex, formatNameWidth);
                MGLOG_D("%s", row.c_str());
            }
        }
    }
} // namespace MobileGL::MG_Backend
