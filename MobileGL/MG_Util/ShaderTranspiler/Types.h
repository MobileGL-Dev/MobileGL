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
