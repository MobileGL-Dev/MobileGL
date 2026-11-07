// MobileGL - MobileGL/MG_Util/SelfTest/DriverPostClipDistance.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DriverPostClipDistance.h"
#include "DriverBugProbes.h"
#include <Config.h>
#include <format>

namespace MobileGL::MG_Util::SelfTest {
    namespace {
        using Verdict = ClipDistancePostRow::Verdict;

        // What the emulation still cannot do, shared by both backends' WARN rows.
        constexpr const char* kClipDistanceEmulationResidue =
            "MobileGL emulates 8 distances: gl_ClipDistance becomes two ordinary varyings and the "
            "fragment stage discards where a distance is negative, honoring GL_CLIP_DISTANCEi. It costs "
            "two varying locations and early depth/stencil for programs that clip; transform-feedback "
            "capture of gl_ClipDistance and gl_in[].gl_ClipDistance reads are not emulated";

        // Culling is per primitive, which a per-fragment discard cannot reproduce.
        constexpr const char* kNoCullDistance =
            "GL_MAX_CULL_DISTANCES = 0. Culling is per primitive, which a per-fragment discard cannot "
            "reproduce, so a gl_CullDistance shader fails to compile";

        String GlesEmulationReason(const MG_External::GLESCapabilities& caps, const String& probeFailure) {
            if (!caps.SupportsClipDistance) return "no GL_EXT_clip_cull_distance";
            if (caps.MaxClipDistances <= 0) return "GL_EXT_clip_cull_distance reports no distances";
            if (!probeFailure.empty()) {
                return "GL_EXT_clip_cull_distance is listed, but the native probe was inconclusive (" + probeFailure +
                       ")";
            }
            if (!caps.NativeClipDistanceClips) {
                return "GL_EXT_clip_cull_distance is listed, but in the probe an enabled negative distance did not "
                       "clip";
            }
            if (!caps.NativeClipDistanceHonorsEnables) return "the driver clips by DISABLED distances too (probe)";
            return "forced by MOBILEGL_CLIP_DISTANCE_EMULATION=1";
        }
    } // namespace

    Vector<ClipDistancePostRow> GlesClipDistancePostRows(const MG_External::GLESCapabilities& caps,
                                                         const MG_External::GLESFunctionsTable& glesFuncs) {
        Vector<ClipDistancePostRow> rows;
        // Only meaningful where the loader ran it (extension present); cached per process.
        String probeFailure;
        if (caps.SupportsClipDistance && caps.MaxClipDistances > 0) {
            const NativeClipDistanceMeasurement& probe = NativeClipDistance(glesFuncs);
            if (!probe.ran) probeFailure = probe.failure;
        }
        const Bool native = caps.SupportsClipDistance && caps.MaxClipDistances > 0 && caps.NativeClipDistanceClips &&
                            caps.NativeClipDistanceHonorsEnables;
        if (!caps.EmulatesClipDistance && native) {
            rows.push_back({Verdict::Pass, "Clip distances",
                            std::format("native via GL_EXT_clip_cull_distance ({} distances); probe: an enabled "
                                        "distance clips and a disabled one does not",
                                        caps.MaxClipDistances)});
        } else if (caps.EmulatesClipDistance) {
            rows.push_back(
                {Verdict::Warn, "Clip distances",
                 std::format("{}; {}", GlesEmulationReason(caps, probeFailure), kClipDistanceEmulationResidue)});
        } else {
            rows.push_back({Verdict::FailOptional, "Clip distances",
                            caps.SupportsClipDistance
                                ? "GL_EXT_clip_cull_distance is listed but misbehaves in the probe, and the emulation "
                                  "is disabled (MOBILEGL_CLIP_DISTANCE_EMULATION=0)"
                                : "no GL_EXT_clip_cull_distance and the emulation is disabled "
                                  "(MOBILEGL_CLIP_DISTANCE_EMULATION=0): GL_MAX_CLIP_DISTANCES = 0, so a "
                                  "gl_ClipDistance shader fails to compile"});
        }
        if (caps.SupportsClipDistance && caps.MaxCullDistances > 0) {
            rows.push_back({Verdict::Pass, "Cull distances",
                            std::format("native via GL_EXT_clip_cull_distance ({} distances)", caps.MaxCullDistances)});
        } else {
            rows.push_back({Verdict::FailOptional, "Cull distances",
                            std::format("no GL_EXT_clip_cull_distance: {}", kNoCullDistance)});
        }
        return rows;
    }

    Vector<ClipDistancePostRow> VulkanClipDistancePostRows(const Bool hasClipDistance, const Bool hasCullDistance,
                                                           const Uint32 maxClipDistances,
                                                           const Uint32 maxCullDistances) {
        Vector<ClipDistancePostRow> rows;
        Bool emulate = !hasClipDistance;
        if (MG_Config::Features.ClipDistanceEmulation == MG_Config::QuirkOverride::ForceOn) emulate = true;
        if (MG_Config::Features.ClipDistanceEmulation == MG_Config::QuirkOverride::ForceOff) emulate = false;
        if (!emulate && hasClipDistance) {
            rows.push_back({Verdict::Pass, "shaderClipDistance",
                            std::format("native ({} distances); Vulkan has no per-distance enable state, so MobileGL "
                                        "applies GL_CLIP_DISTANCEi in the last pre-rasterization stage (a disabled "
                                        "distance is written as +1)",
                                        maxClipDistances)});
        } else if (emulate) {
            rows.push_back({Verdict::Warn, "shaderClipDistance",
                            std::format("{}; {}",
                                        hasClipDistance ? "forced by MOBILEGL_CLIP_DISTANCE_EMULATION=1"
                                                        : "unsupported by the device",
                                        kClipDistanceEmulationResidue)});
        } else {
            rows.push_back({Verdict::FailOptional, "shaderClipDistance",
                            "unsupported, and the emulation is disabled (MOBILEGL_CLIP_DISTANCE_EMULATION=0): "
                            "GL_MAX_CLIP_DISTANCES = 0, so a gl_ClipDistance shader fails to compile"});
        }
        if (hasCullDistance) {
            rows.push_back(
                {Verdict::Pass, "shaderCullDistance", std::format("native ({} distances)", maxCullDistances)});
        } else {
            rows.push_back(
                {Verdict::FailOptional, "shaderCullDistance", std::format("unsupported: {}", kNoCullDistance)});
        }
        return rows;
    }
} // namespace MobileGL::MG_Util::SelfTest
