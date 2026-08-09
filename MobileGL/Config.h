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
    inline const Version CoreVersion = {26, 8, 0, "-dev", VersionType::Development};
    inline const VersionStringFormatAttrib DefaultVersionStringFormatAttrib = {2, 2, 0, true, true};
    inline const Uint64 CacheVersion = 0;

    extern BackendType ActiveBackendType;

    // Tri-state override for device-specific quirks: Auto lets the detected device decide,
    // ForceOn/ForceOff bypass the detection in either direction. ForceOn only bypasses the
    // device gate - each quirk keeps its structural safety checks.
    enum class QuirkOverride : Uint8 {
        Auto = 0,
        ForceOn,
        ForceOff,
    };

    // Preferred DirectVulkan dispatch tier for the glMultiDraw* families. A preference,
    // never a demand: the renderer clamps it to what the device supports at device
    // creation, falling down the chain ext -> indirect -> unroll with one log line.
    enum class MultiDrawMode : Uint8 {
        Auto = 0, // unset: best supported tier
        Ext,      // VK_EXT_multi_draw: one vkCmdDrawMultiEXT / vkCmdDrawMultiIndexedEXT
        Indirect, // multiDrawIndirect feature: one vkCmdDraw*Indirect over a transient command array
        Unroll,   // one vkCmdDraw* per sub-draw
    };

    // Preferred DirectGLES emulation tier for glMultiDrawElements(BaseVertex). GLES has no
    // such entry point in core, so every tier below is an emulation; they differ only in
    // which driver capability they lean on and how many driver calls a batch costs. Like
    // the Magma knob this is a preference, clamped at resolution time to what the ES
    // driver actually supports, with one log line when it falls back.
    enum class GLESMultiDrawMode : Uint8 {
        Auto = 0,      // unset: best supported tier
        Ext,           // one glMultiDrawElementsBaseVertexEXT
        MultiIndirect, // one glMultiDrawElementsIndirectEXT over a scratch command buffer
        Indirect,      // one glDrawElementsIndirect per sub-draw over that same buffer
        BaseVertex,    // one glDrawElementsBaseVertex per sub-draw
        DrawElements,  // baseVertex folded into a scratch index buffer on the CPU, then plain
                       // glDrawElements per sub-draw (for drivers with no base-vertex draw at all)
        Compute,       // a compute shader flattens every sub-draw into one rebased index buffer,
                       // drawn by a single glDrawElements
    };

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
    //   - MOBILEGL_ASYNC_POOL: a ShaderCompilePool is constructed by binaries that never call
    //     MobileGL::Initialize() and so never run MG_ConfigLoader::Init - MG_Test's
    //     JobNodeTest builds pools directly, and it is the suite that runs the whole async
    //     matrix against both execution engines. Mirroring it here would resolve to the
    //     default in exactly the tests that exist to tell the engines apart (see
    //     MG_Util/Async/ShaderCompilePool.cpp, DetectAsyncPoolEngine).
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
        // MOBILEGL_DISABLE_UBO_RING: force the DirectGLES global-UBO upload back to the
        // per-draw glBufferSubData path instead of the persistent-mapped ring allocator
        // (negative control / driver-bug escape hatch).
        Bool DisableUboRing = false;
        // MOBILEGL_RELAXED_SEMANTICS: relax strict core-profile rules (e.g. VAO-0 draws,
        // texture-name reuse after delete) even on contexts that explicitly requested a core
        // profile. Without it, relaxed semantics still apply to every context that did not
        // explicitly request a core profile via EGL_CONTEXT_OPENGL_PROFILE_MASK / a >=3.1
        // version request.
        Bool RelaxedSemantics = false;
        // MOBILEGL_QUIRK_SUBGROUP_PREFIX_SCAN: overrides the shader-source quirk that
        // rewrites the recognized workgroup prefix-scan template on Qualcomm devices with
        // subgroups wider than 32 lanes (see ShaderSourceProcessor's quirk registry).
        QuirkOverride SubgroupPrefixScanQuirk = QuirkOverride::Auto;
        // MOBILEGL_MAGMA_DISABLE_BLENDED_DEPTH_WRITE: overrides the DirectVulkan quirk that
        // strips depth writes from accumulation-blended pipelines (MIN/MAX or additive
        // ONE+ONE - the multi-pass depth-equality signature) on drivers without
        // cross-pipeline vertex position invariance. Sorted-transparency "over" blends,
        // gl_FragDepth writers, and fully color-masked attachments are exempt (see
        // PipelineFactory::ShouldSuppressDepthWrite). Auto detects Qualcomm.
        QuirkOverride MagmaDisableBlendedDepthWriteQuirk = QuirkOverride::Auto;
        // MOBILEGL_DISABLE_ROBUST_BUFFER_ACCESS: leave the Vulkan robustBufferAccess device
        // feature off. It is enabled by default to match GL's defined out-of-range fetch
        // behavior; this escape hatch exists to measure or dodge its GPU cost on a device.
        Bool DisableRobustBufferAccess = false;
        // MOBILEGL_MAGMA_MULTIDRAW_MODE: preferred DirectVulkan multi-draw dispatch tier
        // ("ext" | "indirect" | "unroll", see MultiDrawMode). Clamped to device support;
        // unset picks the best supported tier.
        MultiDrawMode MagmaMultiDrawMode = MultiDrawMode::Auto;
        // MOBILEGL_ESPRYT_MULTIDRAW_MODE: preferred DirectGLES glMultiDrawElements emulation
        // tier ("ext" | "multiindirect" | "indirect" | "basevertex" | "drawelements" |
        // "compute", see GLESMultiDrawMode). Clamped to driver support; unset picks the best
        // supported tier, which never includes "compute" - see the note on its resolution.
        GLESMultiDrawMode EsprytMultiDrawMode = GLESMultiDrawMode::Auto;
        // MOBILEGL_ASYNC_SHADER_COMPILE: overrides asynchronous shader compilation. Unset
        // keeps the built-in default (MG_Util::Async::kAsyncShaderCompileDefault); falsy
        // forces every glCompileShader/glLinkProgram to run synchronously on the calling
        // thread AND withdraws GL_KHR_parallel_shader_compile, so the single switch reverts
        // both the threading and the application-visible behaviour change.
        QuirkOverride AsyncShaderCompile = QuirkOverride::Auto;
        // MOBILEGL_ASYNC_SHADER_COMPILE_THREADS: shader-compile worker count. 0 (unset) means
        // auto, which is min(4, big cores); an explicit value is honoured as given.
        Uint32 AsyncShaderCompileThreads = 0;
    };
    extern FeaturesTable Features;
} // namespace MobileGL::MG_Config
