// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpvcSession.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <spirv_reflect.h>
#include "Types.h"

#define SPVC_CHK_INIT auto __r = SPVC_SUCCESS;

#define SPVC_CHK_RESULT(res)                                                                                           \
    __r = res;                                                                                                         \
    if (__r != SPVC_SUCCESS) {                                                                                         \
        return __r;                                                                                                    \
    }

#define SPVC_CHK_RETURN return __r;

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            struct SpvcType {
                spvc_basetype basetype = SPVC_BASETYPE_UNKNOWN;
                Uint32 vectorSize = 0;
                Uint32 matCol = 0;

                Bool isScalar() const { return vectorSize == 1 && matCol == 1; }
                Bool isVector() const { return vectorSize > 1 && matCol == 1; }
                Bool isMatrix() const { return vectorSize > 1 && matCol > 1; }

                static Uint32 getByteSizeOfBaseType(const spvc_basetype type) {
                    switch (type) {
                    case SPVC_BASETYPE_INT8:
                    case SPVC_BASETYPE_UINT8:
                        return 1;
                    case SPVC_BASETYPE_INT16:
                    case SPVC_BASETYPE_UINT16:
                    case SPVC_BASETYPE_FP16:
                        return 2;
                    case SPVC_BASETYPE_INT32:
                    case SPVC_BASETYPE_UINT32:
                    case SPVC_BASETYPE_FP32:
                        return 4;
                    case SPVC_BASETYPE_INT64:
                    case SPVC_BASETYPE_UINT64:
                    case SPVC_BASETYPE_FP64:
                        return 8;
                    default:
                        return 0;
                    }
                }
            };

            enum class SessionUsageBit {
                Reflection = 1 << 0,
                Transpile = 1 << 1,
            };

            struct SpvcMetadata {
                UnorderedMap<String, unsigned> plainUniformOffsetsInUBO;
                UnorderedMap<String, SizeT> plainUniformMemberSizesInBytes;
                UnorderedMap<String, SpvcType> plainUniformMemberTypes;
                // Byte stride between consecutive array elements of an arrayed plain
                // uniform (0 for non-arrays). Keyed like the offset map: names are the
                // flattened leaf names glslang reflection uses ("s[0].b[1].b"), without
                // a trailing "[0]".
                UnorderedMap<String, Uint32> plainUniformArrayStridesInUBO;
                SizeT globalUboSize = 0;
            };

            class SpvcSession {
            public:
                SpvcSession() = default;

                explicit SpvcSession(const Vector<unsigned int>& spirv,
                                     Flags<SessionUsageBit> usage);

                SpvcSession(SpvcSession&) = delete;

                SpvcSession(SpvcSession&& that);

                ~SpvcSession();

                SpvcSession& operator=(SpvcSession& session) = delete;

                SpvcSession& operator=(SpvcSession&& that);

                spvc_result CreateOptions(spvc_compiler_options* options);
                spvc_result SetOptions(spvc_compiler_options options);
                Vector<InterfaceVariable> GetShaderInterface(spvc_resource_type resource_type) const;
                spvc_result SetVertexAttribLocation(const UnorderedMap<String, Uint>& location);
                // Rewrites the Binding decoration of shader storage blocks before emission, so
                // the generated source carries the EFFECTIVE binding rather than the declared
                // one. This exists for the ESSL backend: glShaderStorageBlockBinding is a GL 4.3
                // entry point with no ES equivalent (ES fixes a storage block's binding at link
                // from its layout(binding=) qualifier), so the only place a rebinding can be
                // expressed there is the qualifier the transpiler prints.
                //
                // Keyed by the GL interface-query name of the BLOCK (the block/type name; an
                // arrayed block's elements are separate GL resources spelled "B[0]", "B[1]").
                // Entries with a negative value mean "never rebound" and are skipped.
                spvc_result SetShaderStorageBlockBinding(const UnorderedMap<String, Int>& bindings);
                // Points every synthesized atomic-counter block at a RESERVED storage-block
                // binding and reports which GL atomic-counter bindings the module declares.
                //
                // glslang's relaxed parse rewrote each atomic_uint into a member of
                // gl_AtomicCounterBlock_<N>, where N is the GL binding the application declared;
                // the block itself was then auto-mapped to whatever storage-block binding was
                // free, which has no relation to N and can collide with an SSBO the application
                // binds itself. Slot N is taken from the TOP of the driver's range downwards
                // (`topBinding - N`) so the reserved window never overlaps the low bindings
                // applications use, and a block whose slot would be negative is left alone and
                // NOT reported - the caller binds nothing there rather than aliasing.
                //
                // `outGlBindings` is appended to, so one vector can collect a whole program's
                // stages; it may repeat a binding declared by several of them.
                spvc_result SetAtomicCounterBlockBindings(Int topBinding, Vector<Int>& outGlBindings);
                // Drops the Index decoration from every fragment output that carries the DEFAULT
                // colour index 0, so the emitted ESSL does not print `index = 0`.
                //
                // Index 0 is what every single-source fragment output already is, in GL and in
                // ESSL alike, and SPIR-V carries the decoration only because the application
                // spelled the qualifier out - `layout(location = 0, index = 0) out vec4 c;` is
                // legal desktop GLSL and says nothing. Printing it back into ESSL is NOT
                // harmless: GLSL ES has no `index` layout qualifier in core, so the driver
                // answers "index layout qualifier requires EXT_blend_func_extended" and refuses
                // the stage. The program then links nothing and every draw with it renders
                // NOTHING - verified on Mesa 26.1.4 llvmpipe with no MobileGL in the process,
                // and it is why KHR-GL43.shader_atomic_counters.basic-program-query read back a
                // black render target.
                //
                // A NON-zero index is left exactly as it is: that one really does select the
                // second dual-source input and cannot be expressed without the extension, so it
                // must keep reaching the driver (the frontend's own glBindFragDataLocationIndexed
                // path already emits only non-zero indices for the same reason).
                spvc_result DropDefaultFragmentOutputColorIndex();
                // Drops `readonly` and `writeonly` from every shader storage block - and every
                // block member - that carries BOTH of them.
                //
                // GL 4.6 core 4.10 lets a buffer variable be declared readonly AND writeonly at
                // once: it then cannot be read or written at all, and the only thing left that
                // it can be used for is `.length()`. The pair is therefore inert by
                // construction - the frontend has already rejected any access to it - so
                // dropping it cannot change what the shader does.
                //
                // Emitting it does change whether the shader EXISTS. SPIRV-Cross hoists the
                // qualifiers every member shares onto the block, and Mesa's ES compiler rejects
                // that spelling outright ("Interface block sets both readonly and writeonly",
                // verified on Mesa 26.1.4 llvmpipe with no MobileGL in the process, against the
                // exact source this transpiler emitted). The stage then never compiles, the
                // program links without it, and every dispatch or draw is a silent no-op -
                // which is how KHR-GL43.shader_storage_buffer_object.basic-readonly-writeonly
                // read back 0 instead of the array length.
                //
                // A block carrying only ONE of the two is left exactly as it is: those really do
                // constrain the accesses the shader makes, and the driver is entitled to know.
                spvc_result RelaxReadWriteExclusiveStorageBuffers();
                // ---- GL_ARB_gl_spirv: an APPLICATION-supplied module, not one MobileGL emitted ----
                // Select which OpEntryPoint of `model` this session compiles. A module may carry
                // several of the same execution model, and glSpecializeShader names the one the
                // shader object stands for.
                spvc_result SetEntryPoint(const char* name, SpvExecutionModel model);
                // Bake glSpecializeShader's values into the module's specialization constants.
                // Every value is a GLuint on the GL side and is reinterpreted according to the
                // constant's own scalar type, exactly as ARB_gl_spirv specifies ("the value is
                // interpreted as the type of the specialization constant"). Returns false and
                // sets `outUnknownConstantId` when an id the caller passed is not a
                // specialization constant of this module, which the extension makes
                // GL_INVALID_VALUE.
                Bool SetSpecializationConstants(const Vector<Uint32>& constantIds,
                                                const Vector<Uint32>& constantValues,
                                                Uint32& outUnknownConstantId);
                spvc_result Compile(const char** result);
                const SpvcMetadata& GetMetadata() const;
                const char* GetLastErrorString() const;

                // Should be called once, and only once, for every SPIR-V binary
                spvc_result ParseMetaData();

            private:
                Flags<SessionUsageBit> usage{};

                // SPIRV-Cross state (used when Transpile flag is set)
                spvc_context context = nullptr;
                spvc_parsed_ir ir = nullptr;
                spvc_compiler compiler = nullptr;
                spvc_compiler_options compiler_options = nullptr;
                spvc_resources resources = nullptr;

                // SPIRV-Reflect state (used when only Reflection flag is set)
                SpvReflectShaderModule reflectModule = {};
                bool reflectModuleValid = false;

                SpvcMetadata metadata;
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
