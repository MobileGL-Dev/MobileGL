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
        // MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW: advertise GL_ARB_texture_view on DirectGLES when
        // the host ES driver has EXT/OES_texture_view. Off by default: the host extension is
        // present on Adreno 830 and the functional half of KHR-GL4{2,3}.texture_view still fails
        // there, because the view's ES internalformat is normalized independently of the storage
        // it aliases (see BackendObject_DirectGLES::BuildAdvertisedExtensions). The flag exists
        // so that work can be done without editing the gate.
        Bool EsprytEnableTextureView = false;
        // MOBILEGL_ENABLE_SPIRV_VALIDATION: validate generated and transformed SPIR-V.
        // Disabled by default because validation is a diagnostics-only cost.
        Bool EnableSpirvValidation = false;
        // MOBILEGL_ESPRYT_USE_ANGLE: load ANGLE EGL/GLES libraries.
        Bool EsprytUseAngle = false;
#if defined(MOBILEGL_TRACE_ANGLE_VARIANTS)
        // MOBILEGL_TRACE_ANGLE_VARIANT: signed trace-APK ANGLE build short hash.
        String TraceAngleVariant;
#endif
        // MOBILEGL_MAGMA_DISABLE_SUBGROUP: force-disable Vulkan shader subgroup support,
        // including the opt-in emulated compute path below.
        Bool MagmaDisableSubgroup = false;
        // MOBILEGL_MAGMA_EMULATE_SUBGROUP: implement GL_KHR_shader_subgroup's compute
        // stage on a 32-lane VIRTUAL subgroup lowered to workgroup-shared memory
        // (ShaderTranspiler::EmulateSubgroupsPass). Strictly a last resort: it only ever
        // engages when this flag is set AND the device has no native subgroup support at
        // all - a device with real subgroup operations always uses them natively,
        // whatever their width (the known iterationRP defect is patched by
        // MagmaFixIterationRPSubgroupScratch below instead). Off by default.
        Bool MagmaEmulateSubgroup = false;
        // MOBILEGL_MAGMA_FIX_ITERATIONRP_SUBGROUP_SCRATCH: patch iterationRP's own bug - the
        // pack declares `shared vec2 prefixSumCache[32]` for a 512-invocation exposure
        // reduction and indexes it by gl_SubgroupID, so any device with sub-16-lane
        // subgroups (8-lane lavapipe -> 64 subgroups) writes shared memory out of
        // bounds. The pass grows that one array to what the device's topology needs and
        // touches nothing else; it only rewrites modules positively matching the pack's
        // reduction fingerprint (ShaderTranspiler::FixIterationRPSubgroupScratchPass),
        // so every other shader passes through byte-identical - as does iterationRP
        // itself on >= 16-lane devices. Auto is ON; ForceOff replays the pack's bug
        // verbatim.
        QuirkOverride MagmaFixIterationRPSubgroupScratch = QuirkOverride::Auto;
        // MOBILEGL_MAGMA_ITERATIONRP_FIX_BARRIER: repair Program 203's missing workgroup
        // rendezvous between its two reductions over prefixSumCache. Off by default and
        // fingerprint-gated by FixIterationRPBarrierPass when enabled.
        Bool MagmaIterationRPFixBarrier = false;
        // MOBILEGL_MAGMA_DERIVE_NUM_SUBGROUPS: replace compute gl_NumSubgroups loads with
        // ceil(workgroup invocations / gl_SubgroupSize) on the NATIVE subgroup path
        // (ShaderTranspiler::DeriveNumSubgroupsPass). Auto is ON: GL requires
        // gl_SubgroupID < gl_NumSubgroups, Adreno's builtin reports 1 while the same
        // dispatch emits IDs 0..7, and the derived value is the one Vulkan guarantees
        // whenever the pipeline can request REQUIRE_FULL_SUBGROUPS (which the renderer
        // does whenever local_size_x is a multiple of the native width). ForceOff returns
        // to the raw driver builtin.
        QuirkOverride MagmaDeriveNumSubgroups = QuirkOverride::Auto;
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
        // MOBILEGL_ESPRYT_AVOID_SAMPLER_MIPMAP_MIN_FILTER: avoid mipmap min filters in samplers,
        // resolves certain rendering bugs on ANGLE + llvmpipe.
        Bool EsprytAvoidSamplerMipmapMinFilter = false;
        // MOBILEGL_ESPRYT_AVOID_EXPLICIT_LOD_BIAS: leave an already-explicit LOD argument alone when
        // emulating GL_TEXTURE_LOD_BIAS, instead of adding the bias uniform to it. Injecting
        // the uniform turns a compile-time-constant LOD into a runtime expression, which
        // sends ANGLE + llvmpipe down a mip-selection path that dereferences a NULL
        // descriptor and kills the process. Deviates from spec (Vulkan adds the bias to
        // OpImageSampleExplicitLod), so it is an avoidance for that stack only.
        Bool EsprytAvoidExplicitLodBias = false;
        // MOBILEGL_ESPRYT_UNLOCATED_IO_BLOCKS: emit a tessellation/geometry program's
        // inter-stage interface blocks WITHOUT their layout(location=) qualifier, letting ES
        // match them by block name and member sequence instead. The Mali ES driver delivers
        // nothing at all through a located block once a tessellation or geometry stage is in
        // the pipeline; the driver POST measures that and turns this on by itself, so Auto is
        // the right setting everywhere. ForceOn exists so the emulation can be exercised on a
        // healthy driver - which is what the integration lane does, since llvmpipe and
        // lavapipe carry a located block correctly and would otherwise never run this code -
        // and ForceOff is the negative control. See StripIoBlockLocationsPass.
        QuirkOverride EsprytUnlocatedIoBlocks = QuirkOverride::Auto;
        // MOBILEGL_POINT_SIZE_DEMOTION: demote gl_PointSize out of tessellation/geometry
        // stages into an ordinary varying (ShaderCompiler::
        // DemoteTessellationGeometryPointSizeForProgram) instead of declining such programs
        // on a device that advertises neither EXT/OES_tessellation_point_size /
        // geometry_point_size (DirectGLES) nor shaderTessellationAndGeometryPointSize
        // (DirectVulkan). Auto arms it exactly where the detection says the capability is
        // absent, which is the right setting everywhere. ForceOn exists so the demotion can
        // be exercised on a healthy driver - llvmpipe and lavapipe host the built-in
        // natively and would otherwise never run this code, which is what the pinned
        // integration lane uses - and ForceOff restores the plain declines (escape hatch /
        // negative control). Cross-backend by design: the demotion runs in the shared
        // phase-B chain, so one switch covers both. See DemotePointSizePass.
        QuirkOverride PointSizeDemotion = QuirkOverride::Auto;
        // MOBILEGL_COHERENT_AS_FLUSH: app-compat for engines (e.g. Flywheel) that write
        // GPU-read data through persistent GL_MAP_FLUSH_EXPLICIT_BIT maps they never
        // flush. Persistent FLUSH_EXPLICIT map requests are rewritten to coherent
        // semantics: writes reach the backend without glFlushMappedBufferRange, and
        // flush calls on rewritten maps become error-free no-ops. Non-persistent maps
        // keep spec FLUSH_EXPLICIT behavior.
        Bool CoherentAsFlush = false;
        // MOBILEGL_TRACE_SKIP_AUTODESTROY: skip teardown in the ELF destructor (Init.cpp).
        Bool TraceSkipAutodestroy = false;
        // MOBILEGL_ESPRYT_DISABLE_UBO_RING: force the DirectGLES global-UBO upload back to the
        // per-draw glBufferSubData path instead of the persistent-mapped ring allocator
        // (negative control / driver-bug escape hatch).
        Bool EsprytDisableUboRing = false;
        // MOBILEGL_ESPRYT_DISABLE_UNPACK_RING: force DirectGLES texture uploads back to
        // glTexSubImage from the client pointer instead of staging them through the
        // persistent-mapped unpack-PBO ring (negative control / driver-bug escape
        // hatch).
        Bool EsprytDisableUnpackRing = false;
        // MOBILEGL_ESPRYT_DISABLE_UPLOAD_RING: force DirectGLES app buffer updates
        // (glBufferSubData / map flushes) back to the immediate driver upload instead
        // of queueing them for the staged-copy flush through the persistent-mapped
        // upload ring (negative control / driver-bug escape hatch; the immediate
        // upload stalls on drivers that resolve the WAR hazard on the CPU, e.g. Mali).
        Bool EsprytDisableUploadRing = false;
        // MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH: skip the glMapBufferRange(WRITE |
        // INVALIDATE_RANGE) tier of the DirectGLES pending-range flush and go straight
        // to the upload ring's staged glCopyBufferSubData (negative control / escape
        // hatch for a driver whose range-invalidating map misbehaves). The map tier is
        // what keeps a partial write into a large in-flight buffer priced by the RANGE:
        // on Mali both the immediate glBufferSubData and a staged copy into a busy
        // mutable store ghost the whole destination on the CPU.
        Bool EsprytDisableInvalidateFlush = false;
        // MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION: keep mesh-arena-sized buffer stores
        // (>= 16MiB) on the CPU-shadow model instead of backing them with the backend's
        // persistently+coherently mapped storage at definition time (negative control /
        // escape hatch). Frontend-scoped: it engages only where the active backend
        // provides AcquirePersistentMap. With adoption on, an app SubData into a busy
        // 128MB arena is a plain memcpy into GPU-visible memory; every driver-mediated
        // route for the same write stalls the thread or ghost-copies the whole arena on
        // this class of Mali driver, and the arena stops costing its size again in RAM.
        Bool DisableLargeBufferAdoption = false;
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
        // MOBILEGL_MAGMA_DISABLE_ROBUST_BUFFER_ACCESS: leave the Vulkan robustBufferAccess device
        // feature off. It is enabled by default to match GL's defined out-of-range fetch
        // behavior; this escape hatch exists to measure or dodge its GPU cost on a device.
        Bool MagmaDisableRobustBufferAccess = false;
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
        // MOBILEGL_SHADER_CACHE: the three-level, in-memory shader translation memo
        // (MG_Util/ShaderTranspiler/TranslationCache.h). The levels follow the GL
        // entry points - L1c memoizes one glCompileShader's PARSE VERDICT, L1 a
        // linked program's whole front end, L2 DirectGLES's emitted ESSL. Auto is
        // ON; ForceOff turns ALL THREE off and makes every translation run from
        // scratch. The escape hatch exists because a wrong cache hit is a silently
        // miscompiled shader: if a device ever renders differently with the cache
        // on, one run with this falsy says so.
        QuirkOverride ShaderTranslationCache = QuirkOverride::Auto;
        // MOBILEGL_ESPRYT_FORCE_VIEWPORT_ARRAY_EMULATION: DirectGLES' gl_ViewportIndex routing
        // emulation - the builtin becomes a flat varying, the fragment stage gets a
        // per-pass gate, and a routed draw is REPLAYED once per distinct viewport state
        // with the real glViewport/glScissor/glDepthRangef set for it. Auto is ON, and
        // it is ON even where the driver advertises GL_OES_viewport_array, because that
        // extension only ever gave the SHADER a compilable name: MobileGL has never
        // programmed a driver's INDEXED viewport state (SyncRenderState pushes index 0
        // and nothing else), so on an extension-capable driver every index rasterized as
        // index 0 exactly as it did without one. ForceOff returns to that behaviour -
        // the pre-emulation path, extension passthrough where it exists and
        // LowerViewportIndexPass' demote-to-a-plain-global where it does not - and is
        // the negative control the emulation is measured against.
        QuirkOverride EsprytViewportArrayEmulation = QuirkOverride::Auto;
        // MOBILEGL_ESPRYT_WIDEN_PACKED16_STORAGE: DirectGLES stores GL_RGB565/GL_RGB5(A1)/GL_RGBA4
        // images as 8-bit-per-channel ES storage (GL_RGB8/GL_RGBA8) instead of the driver's
        // native 16-bit packed formats. Auto defers to a POST driver-bug probe
        // (SelfTest::CopyImageMirrorsPacked16FieldOrder): some Mali drivers store SOME
        // packed16 allocations with a MIRRORED field order (allocation-scoped and
        // shape/context dependent - the failing 30x30x12 GL_TEXTURE_2D_ARRAYs are mirrored
        // at every level), so glCopyImageSubData - a raw texel-block move - lands R/G/B/A
        // reversed whenever exactly one endpoint sits in a mirrored allocation
        // (KHR-GL4x.copy_image.functional rgb5/rgb5_a1/rgba4 x every *2d_array* pair).
        // With no 16-bit packed ES image left there is no field order to disagree about; the
        // client word still round-trips exactly, because the canonical shadow is already
        // UNorm8 and an n-bit field encodes to UNorm8 and back losslessly for n <= 8.
        // ForceOn widens on any driver (the llvmpipe suites use it to exercise the widened
        // path); ForceOff keeps the native narrow storage even where the probe fires - the
        // negative control that replays the corruption. Costs 2x the memory of the affected
        // formats where it engages, which is why Auto is probe-gated rather than always-on.
        QuirkOverride EsprytWidenPacked16Storage = QuirkOverride::Auto;
        // MOBILEGL_MAGMA_PRIMGEN_QUERY_REROUTE: DirectVulkan's GL_PRIMITIVES_GENERATED
        // reroute for draws made while transform feedback is INACTIVE. The stream query
        // (VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT primitivesNeeded) is defined to count
        // them, but a Mali driver - and Mesa lavapipe - answers 0 unless a capture span is
        // open, which is exactly the shape the CTS uses to measure the tessellator, so ~29
        // tessellation tests per tree size a capture buffer from the 0 and die on the
        // zero-length map. Auto defers to a device probe at renderer bring-up
        // (SelfTest::RunPrimitivesGeneratedNoXfbProbe), which measures two substitutes on
        // the same capture-less draws and arms the best proven one: the dedicated
        // VK_EXT_primitives_generated_query (exact semantics by definition; lavapipe passes
        // it, rasterizer discard included), else a clipping-invocations pipeline-statistics
        // pool (see the verdict vocabulary for its rasterizer-discard split). ForceOn pins
        // the reroute structurally wherever a pool can exist (the arming-observable lane,
        // immune to the probe's verdict moving), and ForceOff is the negative control that
        // replays the driver's silence.
        QuirkOverride MagmaPrimGenQueryReroute = QuirkOverride::Auto;
        // --- MGPipe (the disaggregation plan's explicit frontend/backend boundary) ---
        // MOBILEGL_PIPE_PUSH: per-subsystem bitmask selecting which state the frontend
        // PUSHES over MGPipe instead of leaving the backend to pull it out of GLContext.
        // 0 - the default and the only shipped value until the migration lands - is "pull
        // everything", i.e. exactly today's behaviour. One bit of it also turns OFF
        // client-side content addressing of CSOs, which is the negative control the CSO
        // design is measured against. Accepts decimal or 0x-prefixed hex.
        Uint64 PipePush = 0;
        // MOBILEGL_PIPE_VERIFY: per-draw, per-FIELD shadow comparison of the pushed state
        // against a snapshot taken from GLContext the old way, printing the first field
        // that differs and the draw serial. Roughly 5-10x slower and never shipped; it is
        // the semantic gate that replaces byte identity, and it catches the dangerous
        // direction - a dirty bit that fires too RARELY - which no purity gate can see.
        Bool PipeVerify = false;
        // MOBILEGL_PIPE_STATS: dump the boundary counters (bytes, calls, roundtrips,
        // texture pulls, upload shapes, residual-block bytes, index mirror bytes).
        Bool PipeStats = false;
        // MOBILEGL_PIPE_LEGACY_MEMOS: keep the pre-handle registries and TwinLookupMemos
        // alive so the first handle waves have a real old-versus-new arm to be compared
        // against. ON by default for the whole migration window, deleted with the pull
        // path itself.
        Bool PipeLegacyMemos = true;
        // MOBILEGL_PIPE_TEXEL_RETAIN_MB: LRU budget for texels retained against a
        // server-initiated texture re-send. Default 0, i.e. OFF: MipmapStorage already
        // holds a complete CPU shadow, so this cache buys latency, never correctness.
        Uint32 PipeTexelRetainMb = 0;
        // MOBILEGL_PIPE_INDEX_MIRROR_MB: budget for the server-side index host mirror,
        // which is what lets primitive-restart rewriting and multi-draw flattening stay on
        // the server without shipping index bytes per draw. Over budget it degrades to
        // per-draw staging, counted separately in the stats.
        Uint32 PipeIndexMirrorMb = 64;
        // MOBILEGL_PIPE_STATS_FILE: where the boundary counters' teardown JSON dump goes.
        // Empty (the default) means no dump; the per-120-frame summary line still goes to
        // the log whenever PipeStats is on, so a device run needs no writable path.
        String PipeStatsFile;
    };
    extern FeaturesTable Features;
} // namespace MobileGL::MG_Config
