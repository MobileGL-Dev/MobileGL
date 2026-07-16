// MobileGL - MobileGL/Config.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Backend/BackendObjects.h>

namespace MobileGL::MG_Config {
    inline const String ProjectName = "MobileGL";
    inline const String CoreName = "MobileGL Core";
    inline const String CoreVendor = "MobileGL-Dev (BZLZHH, Swung0x48, Tungsten)";
    inline const Version CoreVersion = {26, 7, 0, "-dev", VersionType::Development};
    inline const VersionStringFormatAttrib DefaultVersionStringFormatAttrib = {2, 2, 0, true, true};
    inline const Uint64 CacheVersion = 0;

    extern BackendType ActiveBackendType;

    // Feature toggles parsed once from environment variables in MG_ConfigLoader::Init()
    // (ConfigLoader.cpp), before the accepted-env map is destroyed. All Bool fields share
    // one truthy rule: the variable is set, non-empty, not "0", and not "false"
    // (case-insensitive).
    //
    // Env variables intentionally NOT mirrored here (kept as live std::getenv at their
    // call sites):
    //   - DISPLAY: X11 session variable, not MobileGL configuration.
    //   - MOBILEGL_LOG_FILE_PATH: log-file init runs before MG_ConfigLoader::Init
    //     (see MG_Util/Debug/Log.cpp).
    struct FeaturesTable {
        // MOBILEGL_DISABLE_TIMERQUERY: do not advertise or use GPU timer queries.
        Bool DisableTimerQuery = false;
        // MOBILEGL_USE_ANGLE: load ANGLE EGL/GLES libraries.
        Bool UseAngle = false;
#if defined(MOBILEGL_TRACE_ANGLE_VARIANTS)
        // MOBILEGL_TRACE_ANGLE_VARIANT: signed trace-APK ANGLE build short hash.
        String TraceAngleVariant;
#endif
        // MOBILEGL_DISABLE_SUBGROUP: force-disable Vulkan shader subgroup support.
        Bool DisableSubgroup = false;
        // MOBILEGL_MAGMA_R11G11B10F_FALLBACK: use fallback format for R11G11B10F on Vulkan.
        Bool MagmaR11G11B10FFallback = false;
        // MOBILEGL_MAGMA_FRAMESINFLIGHT: requested Magma frames in flight, defaulting to 3.
        Uint32 MagmaFramesInFlight = 3;
        // MOBILEGL_AVOID_SAMPLER_MIPMAP_MIN_FILTER: avoid mipmap min filters in samplers,
        // resolves certain rendering bugs on ANGLE + llvmpipe.
        Bool AvoidSamplerMipmapMinFilter = false;
        // MOBILEGL_COHERENT_AS_FLUSH: app-compat for engines (e.g. Flywheel) that write
        // GPU-read data through persistent GL_MAP_FLUSH_EXPLICIT_BIT maps they never
        // flush. Persistent FLUSH_EXPLICIT map requests are rewritten to coherent
        // semantics: writes reach the backend without glFlushMappedBufferRange, and
        // flush calls on rewritten maps become error-free no-ops. Non-persistent maps
        // keep spec FLUSH_EXPLICIT behavior.
        Bool CoherentAsFlush = false;
        // MOBILEGL_TRACE_SKIP_AUTODESTROY: skip teardown in the ELF destructor (Init.cpp).
        Bool TraceSkipAutodestroy = false;
    };
    extern FeaturesTable Features;
} // namespace MobileGL::MG_Config
