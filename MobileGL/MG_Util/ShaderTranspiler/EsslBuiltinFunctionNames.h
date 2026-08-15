// MobileGL - MobileGL/MG_Util/ShaderTranspiler/EsslBuiltinFunctionNames.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <algorithm>
#include <string_view>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            // Builtin-shadowing rename: TWO tables, split by FAILURE LAYER.
            //
            // A desktop pack may redefine a builtin; ESSL 3.x forbids the redefinition, so every
            // such helper is renamed to mg_<name>. That rename happens in two places, and which
            // names belong in which place is decided by *where the failure would occur*, not by
            // how thorough the table looks:
            //
            //  - kEsslBuiltinFunctionNames (below, the full ~146-name ESSL 3.20 set plus the
            //    GL_AMD/EXT trinary min3/mid3/max3) drives the SPIR-V OpName backstop pass. That
            //    pass is safe BY CONSTRUCTION for any name: it renames function ids, and builtin
            //    calls are GLSL.std.450 instructions that can never resolve to a user OpFunction.
            //    Overloads are distinct ids (so an overload delegating to the real builtin keeps
            //    working), dead preprocessor branches never reach SPIR-V, and there is no lexical
            //    guessing to over-fire. Everything that CAN wait for the IR belongs here only.
            //
            //  - kLexicalPreemptRenameNames (a strict handful-of-names subset) drives the source-level
            //    scan in ShaderSourceProcessor::RenameBuiltinShadowingFunctions. That scan exists
            //    for exactly one reason: glslang's relaxed parse rejects some shadowing overload
            //    shapes at PARSE time ("overloaded functions must have the same parameter
            //    precision qualifiers"), and a shadowed builtin can itself need an extension the
            //    declared #version does not enable (fma() at #version 330 wants
            //    GL_ARB_gpu_shader5) - such a shader never produces SPIR-V, so the backstop never
            //    sees it. Only names empirically observed to hit that parse-level rejection go
            //    here. A lexical scan is preprocessor-blind and cannot see overload sets, so it
            //    can over-fire (rename a live call whose definition sits in a dead #if branch, or
            //    rewrite an overload's delegating call to the real builtin) - and over-detection
            //    is UNRECOVERABLE, because the source never reaches the backstop. Keeping this
            //    table minimal keeps that exposure at its historical scope.
            //
            // "main" is deliberately absent from both.
            inline constexpr std::string_view kEsslBuiltinFunctionNames[] = {
                "EmitVertex", "EndPrimitive",
                "abs", "acos", "acosh", "all", "any", "asin", "asinh", "atan", "atanh",
                "atomicAdd", "atomicAnd", "atomicCompSwap", "atomicCounter",
                "atomicCounterDecrement", "atomicCounterIncrement", "atomicExchange",
                "atomicMax", "atomicMin", "atomicOr", "atomicXor",
                "barrier", "bitCount", "bitfieldExtract", "bitfieldInsert", "bitfieldReverse",
                "ceil", "clamp", "cos", "cosh", "cross",
                "dFdx", "dFdy", "degrees", "determinant", "distance", "dot",
                "equal", "exp", "exp2",
                "faceforward", "findLSB", "findMSB", "floatBitsToInt", "floatBitsToUint",
                "floor", "fma", "fract", "frexp", "fwidth",
                "greaterThan", "greaterThanEqual", "groupMemoryBarrier",
                "imageAtomicAdd", "imageAtomicAnd", "imageAtomicCompSwap",
                "imageAtomicExchange", "imageAtomicMax", "imageAtomicMin", "imageAtomicOr",
                "imageAtomicXor", "imageLoad", "imageSize", "imageStore", "imulExtended",
                "intBitsToFloat", "interpolateAtCentroid", "interpolateAtOffset",
                "interpolateAtSample", "inverse", "inversesqrt", "isinf", "isnan",
                "ldexp", "length", "length_squared", "lessThan", "lessThanEqual", "log", "log2",
                "matrixCompMult", "max", "max3", "memoryBarrier",
                "memoryBarrierAtomicCounter", "memoryBarrierBuffer", "memoryBarrierImage",
                "memoryBarrierShared", "mid3", "min", "min3", "mix", "mod", "modf",
                "normalize", "not", "notEqual",
                "outerProduct",
                "packHalf2x16", "packSnorm2x16", "packSnorm4x8", "packUnorm2x16",
                "packUnorm4x8", "pow",
                "radians", "reflect", "refract", "round", "roundEven",
                "sign", "sin", "sinh", "smoothstep", "sqrt", "step",
                "tan", "tanh", "texelFetch", "texelFetchOffset", "texture",
                "textureGather", "textureGatherOffset", "textureGatherOffsets",
                "textureGrad", "textureGradOffset", "textureLod", "textureLodOffset",
                "textureOffset", "textureProj", "textureProjGrad", "textureProjGradOffset",
                "textureProjLod", "textureProjLodOffset", "textureProjOffset", "textureSize",
                "transpose", "trunc",
                "uaddCarry", "uintBitsToFloat", "umulExtended", "unpackHalf2x16",
                "unpackSnorm2x16", "unpackSnorm4x8", "unpackUnorm2x16", "unpackUnorm4x8",
                "usubBorrow",
            };

            inline bool IsEsslBuiltinFunctionName(std::string_view name) {
                return std::binary_search(std::begin(kEsslBuiltinFunctionNames),
                                          std::end(kEsslBuiltinFunctionNames), name);
            }

            // The parse-level subset, sorted for std::binary_search.
            //
            // What decides membership, measured against this glslang: a redefinition whose
            // signature EXACTLY matches a builtin overload is rejected at parse time
            // ("overloaded functions must have the same parameter precision qualifiers", because
            // the builtin declaration carries precision qualifiers and the user's does not), so
            // it never produces SPIR-V and the OpName backstop never gets a turn. A definition
            // that merely ADDS an overload (a signature the builtin set does not have, e.g.
            // vec3 pow(vec3, float)) parses fine and is the backstop's job. Probed across the
            // full table with a float(float) redefinition, 38 names are rejected that way - so
            // membership here is not "everything that could ever be rejected", it is the set
            // actually seen in shipped content plus whatever the test suite pins:
            //   fma, tanh  - the bliss shaderpack's from-scratch helpers
            //   round, min3, max3 - the historical string-scan list this pass replaced
            // An EXACT-signature redefinition of any of the other 33 probed-rejected names
            // (sinh, floor, sqrt, ...) never compiled on MobileGL HEAD either - the old
            // 5-name string scan did not rescue them - so leaving them out preserves the
            // status quo for that (never-working) shape while keeping the dead-#if /
            // overload-delegation exposure at exactly its historical scope.
            // Adding a name is not free: it buys a parse-time rescue at the cost of lexical
            // over-detection risk on every shader that merely *calls* that builtin (a definition
            // in a dead #if branch, or an overload delegating to the real builtin). Add one only
            // with evidence that real content redefines it with a builtin-identical signature.
            inline constexpr std::string_view kLexicalPreemptRenameNames[] = {
                "fma", "max3", "min3", "round", "tanh",
            };

            inline bool IsLexicalPreemptRenameName(std::string_view name) {
                return std::binary_search(std::begin(kLexicalPreemptRenameNames),
                                          std::end(kLexicalPreemptRenameNames), name);
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
