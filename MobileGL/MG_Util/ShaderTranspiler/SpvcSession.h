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
