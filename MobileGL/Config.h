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
    struct FeaturesTable {
        // MOBILEGL_DISABLE_TIMERQUERY: do not advertise or use GPU timer queries.
        Bool DisableTimerQuery = false;
        // MOBILEGL_ENABLE_SPIRV_VALIDATION: validate generated and transformed SPIR-V.
        // Disabled by default because validation is a diagnostics-only cost.
        Bool EnableSpirvValidation = false;
        // MOBILEGL_USE_ANGLE: load ANGLE EGL/GLES libraries.
        Bool UseAngle = false;
#if defined(MOBILEGL_TRACE_ANGLE_VARIANTS)
        // MOBILEGL_TRACE_ANGLE_VARIANT: signed trace-APK ANGLE build short hash.
        String TraceAngleVariant;
#endif
        // MOBILEGL_DISABLE_SUBGROUP: force-disable Vulkan shader subgroup support.
        Bool DisableSubgroup = false;
        // MOBILEGL_NUM_SUBGROUPS_QUIRK: derive compute gl_NumSubgroups from the local
        // workgroup dimensions and gl_SubgroupSize instead of reading Vulkan's
        // NumSubgroups builtin. Off by default; enable only for drivers whose builtin
        // disagrees with the SubgroupId topology emitted by the same dispatch.
        Bool NumSubgroupsQuirk = false;
        // MOBILEGL_ADVERTISE_FP64: add GL_ARB_gpu_shader_fp64 to the advertised extension
        // string. `double` in a shader always WORKS - it is narrowed to 32 bits before any
        // module reaches a backend (ShaderTranspiler::DemoteFloat64Pass) - but the extension
        // promises 64-bit precision, and that is the one thing the narrowing cannot deliver.
        // Off by default so an application that checks the string before using doubles keeps
        // its float path; on for measuring what the conformance suite makes of the demoted
        // precision. See the DemoteFloat64Pass header and the "fp64" POST row.
        Bool AdvertiseFp64 = false;
        // MOBILEGL_MAGMA_R11G11B10F_FALLBACK: use fallback format for R11G11B10F on Vulkan.
        Bool MagmaR11G11B10FFallback = false;
        // MOBILEGL_MAGMA_FRAMESINFLIGHT: requested Magma frames in flight, defaulting to 3.
        Uint32 MagmaFramesInFlight = 3;
        // MOBILEGL_AVOID_SAMPLER_MIPMAP_MIN_FILTER: avoid mipmap min filters in samplers,
        // resolves certain rendering bugs on ANGLE + llvmpipe.
        Bool AvoidSamplerMipmapMinFilter = false;
        // MOBILEGL_AVOID_EXPLICIT_LOD_BIAS: leave an already-explicit LOD argument alone when
        // emulating GL_TEXTURE_LOD_BIAS, instead of adding the bias uniform to it. Injecting
        // the uniform turns a compile-time-constant LOD into a runtime expression, which
        // sends ANGLE + llvmpipe down a mip-selection path that dereferences a NULL
        // descriptor and kills the process. Deviates from spec (Vulkan adds the bias to
        // OpImageSampleExplicitLod), so it is an avoidance for that stack only.
        Bool AvoidExplicitLodBias = false;
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
        // MOBILEGL_ESPRYT_FORCE_DS_READBACK_EMULATION: make DirectGLES skip the native ES
        // depth/stencil reads and always go through the shader-sampling emulation. Core GL
        // ES has no depth or stencil readback, but some drivers accept it anyway (Mesa does,
        // Adreno does not), which means the emulation is dead code on exactly the stack the
        // headless suite runs on. This forces it live so the scenarios and the CTS can
        // exercise the path, and gives the device an A/B lever over the same choice.
        Bool EsprytForceDepthStencilReadbackEmulation = false;
        // MOBILEGL_RELAXED_SEMANTICS: relax strict core-profile rules (e.g. VAO-0 draws,
        // texture-name reuse after delete) even on contexts that explicitly requested a core
        // profile. Without it, relaxed semantics still apply to every context that did not
        // explicitly request a core profile via EGL_CONTEXT_OPENGL_PROFILE_MASK / a >=3.1
        // version request.
        Bool RelaxedSemantics = false;
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
        // MOBILEGL_ASYNC_OPTIMISTIC_SHADER_STATUS: while a compile job is still in flight,
        // glGetShaderiv(GL_COMPILE_STATUS) answers GL_TRUE and the shader info log reads
        // empty, WITHOUT joining the job (latched per compile - see
        // ShaderObject::TakeOptimisticCompileAnswer). A deliberate, bounded spec violation:
        // a real failure still fails the program link with the compile log quoted. It
        // exists for applications that compile hundreds of shaders serially and read the
        // status right after each glCompileShader - Iris's shader-pack load - where those
        // per-shader joins are what serializes the batch on its main path (Iris's gbuffer
        // phase issues no program-level query between programs; program-level LINK_STATUS
        // and the program info log still join truthfully, so paths that check each link
        // immediately stay serial by their own construction). Off by default; never
        // advertise it.
        QuirkOverride AsyncOptimisticShaderStatus = QuirkOverride::Auto;
    };
    extern FeaturesTable Features;
} // namespace MobileGL::MG_Config
