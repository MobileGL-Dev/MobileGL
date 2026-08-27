// MobileGL - MobileGL/MG_Util/ShaderTranspiler/Types.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/ShaderTranspiler/CompileEnv.h>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            inline const char* GLOBAL_UBO_NAME = "MGL_GLOBAL_UBO";
            // The default-block uniform InjectNumSamplesBuiltinShim declares to stand in for the
            // gl_NumSamples built-in, which glslang does not put in the symbol table under a
            // SPIR-V target (Initialize.cpp guards both the desktop and the ES declaration on
            // `spvVersion.spv == 0`, and MobileGL always targets SPIR-V). The relaxed parse folds
            // it into GLOBAL_UBO_NAME like any other default-block uniform, which is what lets
            // BOTH backends pick the value up from the one buffer they already upload; the link
            // task keeps it out of the GL-visible uniform surface, and the draw path writes the
            // current draw framebuffer's sample count into it.
            //
            // RESERVED, not merely conventional: a shader that declares this name itself keeps
            // the shim from firing (the injector bails on it), but if it declares the name AND
            // uses gl_NumSamples the link task will still hide its uniform. That is the same
            // bargain every mg_-prefixed rewrite in this pipeline strikes.
            inline const char* NUM_SAMPLES_UNIFORM_NAME = "mg_NumSamples";
            // glslang's Vulkan-relaxed parse rewrites every atomic_uint into a member of a
            // synthesized storage block named "<this>_<GL atomic-counter binding>"
            // (ParseContextBase::growAtomicCounterBlock). That block IS the GL atomic counter
            // buffer, and the trailing number is the only place the GL binding survives.
            inline constexpr const char* ATOMIC_COUNTER_BLOCK_PREFIX = "gl_AtomicCounterBlock";

            // "gl_AtomicCounterBlock_5" -> 5; -1 for any name that is not one of these blocks.
            // Recovering N from the NAME is not a shortcut, it is the only way: the block reaches
            // a backend auto-mapped to whatever storage-block slot the IO mapper had free, and
            // that number has no relation to the GL atomic-counter binding the application asked
            // for (see TMglGlslIoResolver). A backend that resolves the block from the
            // shader-storage binding points therefore binds the wrong buffer - or, worse, the
            // application's own SSBO at the same slot.
            inline Int AtomicCounterBlockGlBinding(StringView name) {
                const SizeT prefixLength = StringView(ATOMIC_COUNTER_BLOCK_PREFIX).size();
                // Needs the prefix, the '_' and at least one digit.
                if (name.size() <= prefixLength + 1) return -1;
                if (name.compare(0, prefixLength, ATOMIC_COUNTER_BLOCK_PREFIX) != 0) return -1;
                if (name[prefixLength] != '_') return -1;
                Int binding = 0;
                for (SizeT i = prefixLength + 1; i < name.size(); ++i) {
                    if (name[i] < '0' || name[i] > '9') return -1;
                    binding = binding * 10 + (name[i] - '0');
                    if (binding > 0x0FFFFFFF) return -1; // absurd suffix; treat as not-a-counter
                }
                return binding;
            }

            // Atomic-counter limits, in ONE place because GL 4.6 requires glGetIntegerv and the
            // shading language's gl_MaxAtomicCounter* constants to report the same numbers
            // (KHR-GL43.shader_atomic_counters.basic-glsl-built-in compares them directly).
            // They used to be two unreconciled tables: BuildTBuiltInResource compiled against one
            // binding and glGetIntegerv advertised thirty-six.
            //
            // The binding count is what the backends can actually serve. glslang lowers every
            // atomic_uint onto a storage block, so one counter BUFFER costs one of the ES
            // driver's shader-storage binding points, and DirectGLES reserves this many at the
            // top of that range (see AtomicCounterEsslBindingTop in the DirectGLES managers).
            inline constexpr Int MAX_ATOMIC_COUNTER_BUFFER_BINDINGS = 8;
            // GL_MAX_ATOMIC_COUNTER_BUFFER_SIZE, in basic machine units. Independent of the
            // counter COUNTS below - it bounds the byte offset a counter may be declared at, and
            // the conformance suite declares counters well past the eighth one (offsets 32 and
            // 128 in a two-counter buffer). KHR-GL44.multi_bind splits it evenly across every
            // advertised binding point and binds them all in one glBindBuffersRange, so it must
            // stay a multiple of, and comfortably larger than, four times the binding count.
            inline constexpr Int MAX_ATOMIC_COUNTER_BUFFER_SIZE = 16384;
            // GL_MAX_{FRAGMENT,COMPUTE,COMBINED}_ATOMIC_COUNTER_BUFFERS and the matching
            // _ATOMIC_COUNTERS. Eight is the GL 4.6 core minimum for the compute stage
            // (table 23.45) and every other stage this implementation serves counters on.
            inline constexpr Int MAX_ATOMIC_COUNTER_BUFFERS_PER_STAGE = 8;
            inline constexpr Int MAX_ATOMIC_COUNTERS_PER_STAGE = 8;

            // ---- Tessellation per-stage resource limits ----
            //
            // Here for exactly the reason the atomic-counter block above is here. GL 4.6 requires
            // glGetIntegerv and the matching gl_MaxTess* built-in constant to report the same
            // number (KHR-GL45.limits.max_tess_* reads the query and then compiles a shader that
            // writes the built-in into an SSBO and demands equality), and these numbers used to
            // exist ONLY inside BuildTBuiltInResource - so gl_MaxTessControlInputComponents
            // compiled fine while glGetIntegerv of the same limit had no case at all and answered
            // GL_INVALID_ENUM. Never move one of these without the other.
            //
            // The values are the GL 4.6 core minimums (table 23.55), which is what a frontend that
            // synthesizes the tessellation stages onto ES/Vulkan can honestly promise.
            inline constexpr Int MAX_TESS_CONTROL_INPUT_COMPONENTS = 128;
            inline constexpr Int MAX_TESS_CONTROL_OUTPUT_COMPONENTS = 128;
            inline constexpr Int MAX_TESS_CONTROL_TEXTURE_IMAGE_UNITS = 16;
            inline constexpr Int MAX_TESS_CONTROL_UNIFORM_COMPONENTS = 1024;
            inline constexpr Int MAX_TESS_CONTROL_TOTAL_OUTPUT_COMPONENTS = 4096;
            inline constexpr Int MAX_TESS_EVALUATION_INPUT_COMPONENTS = 128;
            inline constexpr Int MAX_TESS_EVALUATION_OUTPUT_COMPONENTS = 128;
            inline constexpr Int MAX_TESS_EVALUATION_TEXTURE_IMAGE_UNITS = 16;
            inline constexpr Int MAX_TESS_EVALUATION_UNIFORM_COMPONENTS = 1024;
            inline constexpr Int MAX_TESS_PATCH_COMPONENTS = 120;

            // ---- Varying and default-block uniform capacities ----
            //
            // The *_VECTORS limits are the *_COMPONENTS ones counted in vec4s, so they are DERIVED
            // rather than typed independently: GL_MAX_VARYING_COMPONENTS said 64 while
            // GL_MAX_VARYING_VECTORS said 8, and GL_MAX_VERTEX_UNIFORM_COMPONENTS said 4096 while
            // GL_MAX_VERTEX_UNIFORM_VECTORS said 128 - two pairs that cannot both describe the
            // same capacity, and both *_VECTORS answers were below the GL 4.5 core minimum
            // (15 and 256 respectively). Shared with BuildTBuiltInResource because
            // gl_MaxVaryingVectors and gl_MaxVertexUniformVectors expand from the same numbers.
            inline constexpr Int MAX_VARYING_COMPONENTS = 64;
            inline constexpr Int MAX_VARYING_VECTORS = MAX_VARYING_COMPONENTS / 4;
            inline constexpr Int MAX_VERTEX_UNIFORM_COMPONENTS = 4096;
            inline constexpr Int MAX_VERTEX_UNIFORM_VECTORS = MAX_VERTEX_UNIFORM_COMPONENTS / 4;

            // GL 4.6 core table 23.53 sets the GL_MAX_SAMPLES minimum at 4, and MobileGL floors
            // the backend's answer at it (GL_Getter's GetAdvertisedMaxSamples). gl_MaxSamples has
            // to expand to the SAME number - it is also what sizes gl_SampleMask[] /
            // gl_SampleMaskIn[] and what bounds a constant index into them - so the floor lives
            // here and both sides apply it. NOTE the deliberate asymmetry: only MAX_SAMPLES has a
            // floor of 4. MAX_INTEGER_SAMPLES, MAX_COLOR_TEXTURE_SAMPLES and
            // MAX_DEPTH_TEXTURE_SAMPLES have a minimum of ONE in the same table and are reported
            // as the backend probed them.
            inline constexpr Int MIN_ADVERTISED_MAX_SAMPLES = 4;

            struct EmptyType {};

            enum class ShaderCompileBits : Uint {
                CompileForOpenGL = 1 << 0,
                EmitDiscardAsDemote = 1 << 1,
            };

            struct ShaderAttrib {
                GLenum shaderType;
                StringView sourceStr;
                Flags<ShaderCompileBits> flags;
                // The compile-time backend snapshot the glslang resource limits come from.
                // Null means "read them off the live backend object" - only legal on the GL
                // thread, and only used by the standalone/test entry points. Non-owning: the
                // env outlives the attrib (it is a per-context SharedPtr).
                const CompileEnv* env = nullptr;
            };

            struct ProgramAttrib {
                Vector<SharedPtr<glslang::TShader>> shaders;
                UnorderedMap<String, Uint> explicitVertexInLocations;
                UnorderedMap<String, Uint> explicitFragmentOutLocations;
                // Dual-source blend color index per fragment output (glBindFragDataLocationIndexed) ->
                // emitted as layout(index = N).
                UnorderedMap<String, Uint> explicitFragmentOutIndices;
                // ---- OUT parameters, written by TMglGlslIoResolver during mapIO ----
                // Neither is an input: the resolver only ever writes them. They exist because
                // the IO mapper's collect callback is the last point at which a resource's
                // qualifier still says what the SHADER declared rather than what glslang
                // assigned - see the comment on TMglGlslIoResolver::reserverResourceSlot.
                UnorderedMap<String, Uint>* explicitOpaqueUniformBindings = nullptr;
                std::set<String>* storageBlocksWithoutBinding = nullptr;
                std::set<String>* uniformBlocksWithoutBinding = nullptr;
            };

            struct ProgramBinaryAttrib {
                Vector<GLenum> shaderTypes;
                const glslang::TProgram& program;
            };

            struct ResultInfo {
                Int errc = 0;
                String log;
            };

            template <typename T>
            using Result = std::expected<T, ResultInfo>;

            struct InterfaceVariable {
                String name;
                Uint32 location;

                Bool operator<(const InterfaceVariable& other) const { return location < other.location; }

                Bool operator==(const InterfaceVariable& other) const {
                    return location == other.location && name == other.name;
                }
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
