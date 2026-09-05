// MobileGL - MobileGL/MG_Backend/DirectGLES/Managers.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Managers.h"
#include "Utils.h"
#include "DirectGLES.h"
#include "BackendObject_DirectGLES.h"
#include <Config.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/TranslationCache.h>

#include <MG_Util/BackendLoaders/OpenGL/Loader.h>
#include <MG_Util/Metrics/PipeStats.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/MGToGL/DataTypeConverter.h>
#include <MG_Util/Converters/MGToGL/BufferEnumConverter.h>
#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToGL/ProgramEnumConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToStr/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToStr/FramebufferEnumConverter.h>
#include <MG_State/GLState/TextureState/TextureObjectBuffer.h>
#include <MG_Util/Converters/GLToMG/FramebufferEnumConverter.h>
#include <MG_Util/Converters/MGToGL/FramebufferEnumConverter.h>
#include <MG_State/GLState/FramebufferState/FramebufferObject.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <cstring>
#include <regex>

namespace MobileGL::MG_Backend::DirectGLES {
    Uint g_backendContextGeneration = 1;

    constexpr Bool PREFER_MAP_BUFFER_RANGE_FOR_BUFFER_SYNC = false;
    constexpr const char* BASE_INSTANCE_UNIFORM_NAME = "mg_BaseInstance";
    constexpr const char* DRAW_ID_UNIFORM_NAME = "mg_DrawID";
    constexpr const char* BASE_VERTEX_UNIFORM_NAME = "mg_BaseVertex";
    constexpr const char* BASE_INSTANCE_LOWERED_NAME = "mg_BaseInstanceLowered";
    constexpr const char* BASE_INSTANCE_WORD_INDEX_UNIFORM_NAME = "mg_BaseInstanceWordIndex";
    constexpr const char* INDIRECT_PARAMS_BLOCK_NAME = "mg_IndirectParams";
    constexpr const char* ZERO_BASED_INSTANCE_ID_NAME = "mg_ZeroBasedInstanceID";

    // See the block comment on ForEachViewportRoutingPass in Managers.h. Auto is ON, including on
    // a driver that advertises GL_OES_viewport_array: that extension gives the shader a name, not
    // the driver fifteen more rectangles to rasterize against, and nothing in MobileGL has ever
    // programmed the indexed state it would need.
    Bool ViewportArrayEmulationEnabled() {
        return MG_Config::Features.EsprytViewportArrayEmulation != MG_Config::QuirkOverride::ForceOff;
    }

    Bool g_anyProgramRoutesViewportIndex = false;

    // ES has no atomic-counter buffers: glslang lowers every atomic_uint onto a synthesized
    // storage block, so one GL counter BUFFER costs one of the driver's shader-storage binding
    // points. Those slots are taken from the TOP of the range downwards - below the one
    // mg_IndirectParams already reserves - so an application binding its own SSBOs from 0 upwards
    // never meets them, and the slot for GL binding N is `this - N` in every stage of the
    // program without any shared state. Negative when the driver has no room left at all.
    static Int AtomicCounterEsslBindingTop() {
        return g_GLESCapabilities.MaxShaderStorageBufferBindings - 2;
    }

    static Bool IsAngleLlvmpipeRenderer() {
        return g_GLESCapabilities.IsAngleLlvmpipeRenderer;
    }

    static Bool ShouldAvoidSamplerMipmapMinFilterOnAngleLlvmpipe() {
        // IsAngleLlvmpipeRenderer combined with the
        // MOBILEGL_ESPRYT_AVOID_SAMPLER_MIPMAP_MIN_FILTER feature toggle,
        // both resolved in FillInGLESCapabilities.
        return g_GLESCapabilities.AvoidSamplerMipmapMinFilter;
    }

    static Bool ShouldAvoidExplicitLodBiasOnAngleLlvmpipe() {
        // IsAngleLlvmpipeRenderer combined with the MOBILEGL_ESPRYT_AVOID_EXPLICIT_LOD_BIAS
        // feature toggle, both resolved in FillInGLESCapabilities.
        return g_GLESCapabilities.AvoidExplicitLodBias;
    }

    static GLenum ResolveBackendMinFilter(const SamplerParameters& samplerParams,
                                          Bool avoidMipmapMinFilter) {
        GLenum filter = MG_Util::ConvertSamplerFilterModeToGLEnum(samplerParams.minFilter,
                                                                  samplerParams.mipmapMode);
        if (!avoidMipmapMinFilter) {
            return filter;
        }
        switch (filter) {
        case GL_NEAREST_MIPMAP_NEAREST:
        case GL_NEAREST_MIPMAP_LINEAR:
            return GL_NEAREST;
        case GL_LINEAR_MIPMAP_NEAREST:
        case GL_LINEAR_MIPMAP_LINEAR:
            return GL_LINEAR;
        default:
            return filter;
        }
    }

    // GL 4.6 core table 23.53 requires GL_MAX_SAMPLES >= 4, so this is the floor MobileGL
    // advertises whatever the ES driver reports. It steers ClampMultisampleFetchesForEssl
    // AND is part of the L2 translation-memo key, so the transpile and the key must read
    // the same constant - hence one definition rather than two locals.
    // Recomputed here rather than calling GL_Getter's GetAdvertisedMaxSamples(): this is
    // backend code and must not reach into the GL frontend. 4 is that translation unit's
    // kFrontendMaxSamples, which is the source of truth - keep the two in step.
    constexpr Int kFrontendMaxSamples = 4;

    static Uint ResolveBackendEsslVersion() {
        const auto& version = g_GLESCapabilities.GLESVersion;
        if (version.Major > 3 || (version.Major == 3 && version.Minor >= 2)) {
            return 320;
        }
        if (version.Major == 3 && version.Minor >= 1) {
            return 310;
        }
        return 300;
    }

    String ReplaceIdentifier(String source, const String& from, const String& to) {
        SizeT pos = 0;
        while ((pos = source.find(from, pos)) != String::npos) {
            const Bool leftIsIdent = pos > 0 &&
                (std::isalnum(static_cast<unsigned char>(source[pos - 1])) || source[pos - 1] == '_');
            const SizeT end = pos + from.size();
            const Bool rightIsIdent = end < source.size() &&
                (std::isalnum(static_cast<unsigned char>(source[end])) || source[end] == '_');
            if (!leftIsIdent && !rightIsIdent) {
                source.replace(pos, from.size(), to);
                pos += to.size();
            } else {
                pos = end;
            }
        }
        return source;
    }

    String InjectUniformAfterVersion(String source, const String& declaration) {
        const SizeT versionPos = source.find("#version");
        if (versionPos == String::npos) {
            return declaration + "\n" + source;
        }

        const SizeT lineEnd = source.find('\n', versionPos);
        if (lineEnd == String::npos) {
            return source + "\n" + declaration + "\n";
        }
        source.insert(lineEnd + 1, declaration + "\n");
        return source;
    }

        namespace {
        Bool g_processTeardown = false;
        std::once_flag g_teardownSentinelOnce;
    } // namespace

    Bool InProcessTeardown() { return g_processTeardown; }
    void EnsureProcessTeardownSentinel() {
        std::call_once(g_teardownSentinelOnce,
                       [] { std::atexit(+[] { g_processTeardown = true; }); });
    }

    Bool VertexStageStorageBlockUsable(Int maxVertexShaderStorageBlocks) {
        // One block is all the indirect-params view needs, so this is a >= 1 test and not a
        // budget calculation. Negative is treated as unusable rather than clamped: a driver
        // that leaves the out-param untouched is telling us nothing, and guessing "yes" here
        // is what produces an unlinkable program.
        return maxVertexShaderStorageBlocks >= 1;
    }

    static Bool CanUseVertexStageStorageBlock() {
        return VertexStageStorageBlockUsable(g_GLESCapabilities.MaxVertexShaderStorageBlocks);
    }

    String EmulateBaseInstanceInVertexShader(String source, GLenum shaderType) {
        if (shaderType != GL_VERTEX_SHADER || source.find("gl_BaseInstance") == String::npos) {
            return source;
        }
        String replaced = ReplaceIdentifier(source, "gl_BaseInstance", BASE_INSTANCE_UNIFORM_NAME);
        if (replaced == source) {
            // Only a substring hit (e.g. gl_BaseInstanceARB inside a SPIRV-Cross #ifdef
            // fallback); nothing was rewritten, so nothing must be declared either.
            return source;
        }
        return InjectUniformAfterVersion(std::move(replaced),
                                         String("uniform highp int ") + BASE_INSTANCE_UNIFORM_NAME + ";");
    }

    // The LowerDrawParametersPass demotes gl_DrawID / gl_BaseInstance / gl_BaseVertex to plain
    // Private globals (mg_DrawID / mg_BaseInstanceLowered / mg_BaseVertex); SPIRV-Cross then
    // emits them as ordinary global declarations. mg_DrawID / mg_BaseVertex become uniforms fed
    // per (sub-)draw. gl_BaseInstance is special: for indirect draws its value lives in the
    // (possibly GPU-written) indirect command buffer, so its declaration expands into a
    // std430 SSBO view of that buffer indexed by a CPU-computed word index, with the plain
    // mg_BaseInstance uniform as the fallback for non-indirect draws.
    //
    // The word index is stored ONE-BASED, so that zero - the value every GLSL uniform starts
    // at - is the "not an indirect draw" sentinel. Nothing seeds this uniform before a
    // program's first draw, and the non-indirect draw entry points never write it at all, so a
    // zero-based index with a negative sentinel would leave every such draw reading
    // mg_indirectWords[0] out of a storage buffer no one bound. That is not a silent zero on a
    // real driver: it returned garbage on Adreno, and a garbage gl_BaseInstance pushed the CTS
    // shader_draw_parameters geometry clean off screen.
    String PromoteDrawParameterGlobalsToUniforms(String source, GLenum shaderType) {
        if (shaderType != GL_VERTEX_SHADER) {
            return source;
        }
        for (const char* name : {DRAW_ID_UNIFORM_NAME, BASE_VERTEX_UNIFORM_NAME}) {
            for (const char* declPrefix : {"highp int ", "mediump int ", "lowp int ", "int ", "highp uint ",
                                           "mediump uint ", "uint "}) {
                const String declaration = String(declPrefix) + name + ";";
                const SizeT pos = source.find(declaration);
                if (pos == String::npos) {
                    continue;
                }
                // Only promote a standalone global declaration, not a uniform we already emitted.
                const Bool alreadyUniform = pos >= 8 && source.compare(pos - 8, 8, "uniform ") == 0;
                if (!alreadyUniform) {
                    const Bool hasPrecision = std::strncmp(declPrefix, "int ", 4) != 0 &&
                                              std::strncmp(declPrefix, "uint ", 5) != 0;
                    const String qualifier = hasPrecision ? "uniform " : "uniform highp ";
                    source.replace(pos, declaration.size(), qualifier + declaration);
                }
                break;
            }
        }
        for (const char* declPrefix : {"highp int ", "mediump int ", "lowp int ", "int "}) {
            const String declaration = String(declPrefix) + BASE_INSTANCE_LOWERED_NAME + ";";
            SizeT pos = source.find(declaration);
            if (pos == String::npos) {
                continue;
            }
            // On drivers where native indirect draws leak the command's baseInstance into
            // gl_InstanceID (ANGLE-on-Vulkan; IndirectDrawInstanceIdIncludesBaseInstance),
            // rebase gl_InstanceID back to zero during those draws so shaders computing
            // gl_BaseInstance + gl_InstanceID don't add the base twice. Scoped to shaders
            // using gl_BaseInstance: only they take the native indirect SSBO machinery.
            const Bool rebaseInstanceId = g_GLESCapabilities.IndirectDrawInstanceIdIncludesBaseInstance &&
                                          source.find("gl_InstanceID") != String::npos;
            if (rebaseInstanceId) {
                source = ReplaceIdentifier(source, "gl_InstanceID", ZERO_BASED_INSTANCE_ID_NAME);
                pos = source.find(declaration); // the declaration contains no gl_InstanceID
            }
            const Int paramsBinding = g_GLESCapabilities.MaxShaderStorageBufferBindings > 0
                                          ? g_GLESCapabilities.MaxShaderStorageBufferBindings - 1
                                          : 0;
            // The whole indirect half of this machinery is a storage block read from the VERTEX
            // stage, and a storage block in the vertex stage is optional in both APIs: the
            // minimum for GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS is 0 (GL 4.6 table 23.64, ES 3.2
            // table 21.44) and ARM's GLES driver takes that allowance - a Mali-G925 reports 0.
            // Emitting the block anyway does not make it work; it makes the program UNLINKABLE
            // ("The number of vertex shader storage blocks (1) is greater than the maximum
            // number allowed (0)"), and because the frontend's LINK_STATUS is glslang's and not
            // the driver's, the application never learns: every draw with that program silently
            // renders nothing. Dropping just the indirect half costs strictly less.
            const Bool canReadIndirectParamsFromVertexStage = CanUseVertexStageStorageBlock();
            String machinery;
            if (source.find(String("uniform highp int ") + BASE_INSTANCE_UNIFORM_NAME + ";") == String::npos) {
                machinery += String("uniform highp int ") + BASE_INSTANCE_UNIFORM_NAME + ";\n";
            }
            if (!canReadIndirectParamsFromVertexStage) {
                // Degraded, but contained and loud. gl_BaseInstance collapses to the plain
                // mg_BaseInstance uniform, which the non-indirect draw entry points do set
                // correctly - so ordinary instanced draws are unaffected. What is lost is the
                // per-command baseInstance of an INDIRECT draw, which lives in the (possibly
                // GPU-written) command buffer and can only be read through this block: those
                // draws now see the last uniform value rather than their own command's. No
                // alternative path is attempted, deliberately - there is nowhere else in the
                // vertex stage to read a GPU-written buffer from.
                //
                // MGLOG_E_ONCE, not _D: this silently changes rendering for exactly the
                // workloads (Create/Flywheel indirect instancing) whose bug reports are
                // impossible to read without it, and once per process is bounded.
                MGLOG_E_ONCE("gl_BaseInstance: this driver reports GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS = %d, so the "
                             "%s storage block an indirect draw's baseInstance must be read through cannot be "
                             "declared in the vertex stage. Dropping indirect baseInstance support: non-indirect "
                             "draws are correct, indirect draws will see a stale per-command baseInstance.",
                             g_GLESCapabilities.MaxVertexShaderStorageBlocks, INDIRECT_PARAMS_BLOCK_NAME);
                if (rebaseInstanceId) {
                    // Without the block there is no per-command baseInstance to subtract, and
                    // the uniform is the same value the define below resolves to, so rebasing
                    // by it would cancel the base out of gl_InstanceID twice.
                    machinery += String("#define ") + ZERO_BASED_INSTANCE_ID_NAME + " gl_InstanceID\n";
                }
                machinery += String("#define ") + BASE_INSTANCE_LOWERED_NAME + " (" + BASE_INSTANCE_UNIFORM_NAME + ")";
                source.replace(pos, declaration.size(), machinery);
                break;
            }
            machinery += String("uniform highp int ") + BASE_INSTANCE_WORD_INDEX_UNIFORM_NAME + ";\n";
            machinery += String("layout(std430, binding = ") + std::to_string(paramsBinding) +
                         ") readonly buffer " + INDIRECT_PARAMS_BLOCK_NAME +
                         " { highp uint mg_indirectWords[]; };\n";
            if (rebaseInstanceId) {
                machinery += String("#define ") + ZERO_BASED_INSTANCE_ID_NAME + " (gl_InstanceID - ((" +
                             BASE_INSTANCE_WORD_INDEX_UNIFORM_NAME + " > 0) ? int(mg_indirectWords[uint(" +
                             BASE_INSTANCE_WORD_INDEX_UNIFORM_NAME + " - 1)]) : 0))\n";
            }
            machinery += String("#define ") + BASE_INSTANCE_LOWERED_NAME + " ((" +
                         BASE_INSTANCE_WORD_INDEX_UNIFORM_NAME + " > 0) ? int(mg_indirectWords[uint(" +
                         BASE_INSTANCE_WORD_INDEX_UNIFORM_NAME + " - 1)]) : " + BASE_INSTANCE_UNIFORM_NAME + ")";
            source.replace(pos, declaration.size(), machinery);
            break;
        }
        return source;
    }

    // ---- gl_ViewportIndex routing emulation, ESSL half ---------------------------------------
    //
    // LowerViewportIndexPass has already turned the BuiltIn ViewportIndex OUTPUT into a plain
    // Private global, so SPIRV-Cross printed `int mg_ViewportIndex;` at file scope and the stage
    // still stores the index the application asked for - it just goes nowhere. The two passes
    // below give it somewhere to go WITHOUT naming a builtin the language does not have: the
    // producing stage's global becomes an ordinary flat varying, and the fragment stage gets a
    // gate that discards every fragment whose primitive routed to a viewport the current replay
    // pass is not drawing. DirectGLES.cpp's ForEachViewportRoutingPass is the other half - it
    // re-issues the draw once per distinct viewport state with the real
    // glViewport/glScissor/glDepthRangef pushed for it and this uniform set to the set of
    // indices that state serves.
    //
    // FLAT is semantics, not performance: GL takes a primitive's viewport index from its
    // PROVOKING VERTEX, which is exactly what flat interpolation delivers, so a primitive whose
    // vertices carry different indices routes the way the spec says with no extra machinery.
    //
    // NO layout(location = N) on either side, deliberately. The two stages are transpiled
    // independently and neither can see the other's location assignment: the producing stage
    // knows its own outputs, the fragment stage only the subset it consumes, and a number derived
    // from either can disagree with the other. Leaving both unqualified hands the assignment to
    // the driver's linker, which then matches them BY NAME - the ordinary GLSL rule, and the only
    // one that needs no cross-stage channel. The cost is one varying slot, which a program
    // already at GL_MAX_VARYING_VECTORS cannot spare.
    constexpr const char* VIEWPORT_INDEX_VARYING_NAME = "mg_ViewportIndex";
    constexpr const char* VIEWPORT_PASS_MASK_UNIFORM_NAME = "mg_ViewportPassMask";
    constexpr const char* VIEWPORT_GATED_ENTRY_POINT_NAME = "mg_ViewportGatedMain";
    constexpr const char* ESSL_ENTRY_POINT_SIGNATURE = "void main()";
    static_assert(RenderStateParameters::MAX_VIEWPORTS == 16,
                  "the fragment gate below spells the index clamp as `& 15` and the pass mask as a "
                  "16-bit int; both follow MAX_VIEWPORTS and have to be respelled with it");

    // Producing stage (vertex / tessellation evaluation / geometry - the three GL lets write the
    // builtin). Returns whether the demoted global was found and promoted, which is also the
    // answer to "does this program route viewports at all".
    Bool PromoteViewportIndexGlobalToVarying(String& source) {
        // The same shape PromoteDrawParameterGlobalsToUniforms matches, and for the same reason:
        // SPIRV-Cross prints the demoted global with or without a precision qualifier depending
        // on what the module carried. Only a declaration that starts its own line may be
        // rewritten - `mg_ViewportIndex = gl_InvocationID;` in the body contains the name too and
        // has to be left exactly as it is.
        const String declared = String(VIEWPORT_INDEX_VARYING_NAME) + ";";
        for (const char* declPrefix : {"highp int ", "mediump int ", "lowp int ", "int "}) {
            const String declaration = String(declPrefix) + declared;
            const SizeT pos = source.find(declaration);
            if (pos == String::npos) {
                continue;
            }
            // Column 0 of its own line is what separates the declaration from the tail of any
            // other declaration or expression that ends in the same name.
            if (pos != 0 && source[pos - 1] != '\n') {
                continue;
            }
            source.replace(pos, declaration.size(),
                           String("flat out highp int ") + VIEWPORT_INDEX_VARYING_NAME + ";");
            return true;
        }
        return false;
    }

    // Fragment stage. Returns false when the stage has no entry point to gate onto, which the
    // caller reports: the program still links and still renders, it just renders every index
    // with the first replay pass's state - i.e. it degrades to the pre-emulation behaviour
    // rather than to a black screen.
    Bool InjectViewportIndexPassGate(String& source) {
        // Built beside the input and swapped in only on success, so a stage this pass declines
        // reaches the driver exactly as it arrived rather than half-rewritten.
        // A fragment stage that READS gl_ViewportIndex has no ESSL spelling for it either -
        // LowerViewportIndexPass deliberately demotes only OUTPUTS, because a demoted INPUT would
        // answer from an undefined Private global. Now that the routing varying exists and
        // carries the real per-primitive value, that read has somewhere honest to go.
        String gated = ReplaceIdentifier(source, "gl_ViewportIndex", VIEWPORT_INDEX_VARYING_NAME);

        const SizeT entryPos = gated.find(ESSL_ENTRY_POINT_SIGNATURE);
        if (entryPos == String::npos) {
            return false;
        }

        // Declarations go immediately before the entry point rather than after #version: that
        // position is already past every #extension directive (which must precede any other
        // token) and past everything the body can name, so it can invalidate neither.
        //
        // Renaming the entry point rather than splicing a prologue into its body keeps the
        // application's code byte-identical, including an early `return`.
        String preamble = String("flat in highp int ") + VIEWPORT_INDEX_VARYING_NAME + ";\n";
        preamble += String("uniform highp int ") + VIEWPORT_PASS_MASK_UNIFORM_NAME + ";\n";
        preamble += String("void ") + VIEWPORT_GATED_ENTRY_POINT_NAME + "()";
        gated.replace(entryPos, std::strlen(ESSL_ENTRY_POINT_SIGNATURE), preamble);

        // `& 15` clamps the shift operand into range for MAX_VIEWPORTS = 16. GL leaves an index
        // outside [0, MAX_VIEWPORTS) undefined, but an ESSL shift by >= 32 is undefined in a way
        // that can take the whole draw with it, so the emulation picks a defined answer instead.
        //
        // The mask, not an equality test against a pass number: viewport indices whose whole
        // state tuple is identical share ONE replay pass (see BeginViewportRoutingPasses), and
        // the overwhelmingly common case - every index still holding what glViewport broadcast -
        // is then a single pass with every bit set, i.e. a gate that discards nothing and a draw
        // that is issued exactly once.
        //
        // PERFORMANCE NOTE: a fragment shader containing `discard` cannot take the early-Z fast
        // path on a tiler, so a routed draw pays late-Z on top of its N replay passes. Accepted
        // deliberately: this runs only for a program that writes gl_ViewportIndex, and that is
        // why the gate is injected per program rather than into every fragment shader.
        gated += "\n";
        gated += String(ESSL_ENTRY_POINT_SIGNATURE) + "\n";
        gated += "{\n";
        gated += String("    if (((") + VIEWPORT_PASS_MASK_UNIFORM_NAME + " >> (" +
                 VIEWPORT_INDEX_VARYING_NAME + " & 15)) & 1) == 0)\n";
        gated += "    {\n";
        gated += "        discard;\n";
        gated += "    }\n";
        gated += "    else\n";
        gated += "    {\n";
        gated += String("        ") + VIEWPORT_GATED_ENTRY_POINT_NAME + "();\n";
        gated += "    }\n";
        gated += "}\n";
        source = std::move(gated);
        return true;
    }

    // The transpile pipeline invents image binding numbers: when the GL source declares
    // an image uniform without layout(binding), glslang auto-assigns one (desktop GL
    // allows that and lets the app pick the unit with glUniform1i, which ES forbids on
    // image uniforms). The unit the app actually addresses lives in frontend state: the
    // layout(binding) reflected at link time, or whatever glUniform1i stored afterwards.
    // Rewrite every image uniform declaration to that unit so imageLoad/Store hits the
    // unit the app bound with glBindImageTexture.
    String RebindImageUniformsToFrontendUnits(
        String source, const SharedPtr<MG_State::GLState::ProgramObject>& stateProgramObject) {
        if (!stateProgramObject || source.find("image") == String::npos) {
            return source;
        }
        static const std::regex imageDeclRegex(
            R"((layout\s*\(([^)]*)\)\s*)?uniform\s+(?:(?:readonly|writeonly|coherent|volatile|restrict|highp|mediump|lowp)\s+)*[iu]?image[A-Za-z0-9]+\s+([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\s*;)");
        static const std::regex bindingValueRegex(R"(binding\s*=\s*\d+)");

        String result;
        result.reserve(source.size());
        SizeT lineStart = 0;
        while (lineStart <= source.size()) {
            const SizeT lineEnd = source.find('\n', lineStart);
            const Bool lastLine = lineEnd == String::npos;
            String line = source.substr(lineStart, lastLine ? String::npos : lineEnd - lineStart);

            std::smatch match;
            if (std::regex_search(line, match, imageDeclRegex)) {
                const String name = match[3].str();
                Int location = stateProgramObject->GetUniformLocation(name);
                if (location < 0) {
                    location = stateProgramObject->GetUniformLocation(name + "[0]");
                }
                if (location >= 0) {
                    const Int unit = stateProgramObject->GetUniformSamplerOrImageUnitIndex(location);
                    if (unit >= 0) {
                        const String bindingText = "binding = " + std::to_string(unit);
                        if (std::regex_search(line, bindingValueRegex)) {
                            line = std::regex_replace(line, bindingValueRegex, bindingText);
                        } else if (match[1].matched) {
                            const SizeT layoutOpen = line.find('(', match.position(1));
                            line.insert(layoutOpen + 1, bindingText + ", ");
                        } else {
                            line.insert(match.position(0), "layout(" + bindingText + ") ");
                        }
                    }
                }
            }

            result += line;
            if (lastLine) {
                break;
            }
            result += '\n';
            lineStart = lineEnd + 1;
        }
        return result;
    }

    namespace BufferImpl {
        namespace {
            using MG_State::GLState::BackendBufferResource;
            using MG_State::GLState::BufferBackendOps;
            using MG_State::GLState::BufferObject;

            // GL_ARRAY_BUFFER redundant-bind cache (id 0 = unknown/none).
            Uint g_boundArrayBufferId = 0;
            Bool g_boundArrayBufferKnown = false;

            // Driver-level GL_PIXEL_PACK/UNPACK_BUFFER binding shadows (see
            // Managers.h). Resting state between operations is 0; scopes in the
            // readback/upload paths bind what they need through the cache and
            // return to 0, so a stale user PBO can never capture a later
            // readback that meant to target client memory.
            Uint g_boundPixelPackBufferId = 0;
            Bool g_boundPixelPackBufferKnown = false;
            Uint g_boundPixelUnpackBufferId = 0;
            Bool g_boundPixelUnpackBufferKnown = false;

            // Bumped whenever the backend ES context is destroyed; resources with
            // an older generation hold ids from a dead context.
            Uint g_bufferContextGeneration = 1;

            // Buffer-mutation epoch backing store (contract, mutation-site list and
            // memory-ordering rules: Managers.h at the accessor declarations).
            // Starts at 1 so the memo stamps' 0 means "never stamped". Atomic:
            // frontend buffer ops may run on non-draw threads while the draw thread
            // reads; the release-bump-AFTER-mutation / acquire-read-BEFORE-probes
            // pairing makes a stamp taken against stale state impossible to consume.
            std::atomic<Uint64> g_bufferMutationEpoch{1};

            // Defined next to the indexed-binding shadow below; forward-declared so
            // every glDeleteBuffers site in this namespace can scrub stale shadow
            // entries (GL resets a deleted buffer's bindings - indexed and pixel
            // pack/unpack alike - to 0, and a recycled name matching a stale shadow
            // entry would otherwise false-skip the rebind).
            void ScrubBufferBindingShadowsForId(Uint id);

            // Defined next to the same shadow, and the counterpart to the scrub above for a
            // buffer whose STORE was re-specified rather than deleted: the binding survives -
            // nothing unbound the id - but the extent the driver resolved for it at bind time
            // does not. Marks those points unknown so the next sync issues a real
            // glBindBufferBase/Range instead of skipping it.
            void InvalidateIndexedBufferBindingShadowsForId(Uint id);

            // Resources whose owning BufferObject died; ids deleted at the next
            // sync point with a current ES context.
            Vector<SharedPtr<BackendBufferResource>> g_deferredBufferReleases;
            std::mutex g_deferredBufferReleasesMutex;
            // Cheap emptiness probe so the per-draw drain can skip the mutex and
            // context check when nothing was enqueued (the overwhelmingly common
            // case). Written only under the mutex; read lock-free.
            std::atomic<Bool> g_hasDeferredBufferReleases{false};

            // --- Buffer-storage pool (Mesa-style BO recycle) -------------------------
            // Recycle idle GL buffer ids of an EXACT byte size instead of glDeleteBuffers
            // (which triggers the kgsl_sharedmem_free -> mmu_unmap -> smmu/power/bandwidth
            // cascade that dominated per-frame driver cost). An id retired during frame N
            // is handed back only once the GPU has completed frame N (fence watermark, see
            // DirectGLES::CompletedFrameSerial), then reseeded in place with glBufferSubData
            // (no glBufferData realloc). All GL access is on the ES-context-owning thread;
            // the mutex only guards against off-thread deferred-release enrollment races.
            struct PooledBuffer {
                Uint id = 0;
                SizeT size = 0;
                Uint contextGeneration = 0;
                Uint64 retireSerial = 0;
            };
            UnorderedMap<SizeT, Vector<PooledBuffer>> g_bufferPool;
            SizeT g_pooledBytes = 0;
            std::mutex g_poolMutex;
            constexpr SizeT kMaxPoolableBufferBytes = 8u * 1024u * 1024u; // bigger buffers: delete now
            constexpr SizeT kMaxPoolBytes = 64u * 1024u * 1024u;          // total pool budget
            constexpr SizeT kMaxEntriesPerBucket = 32;

            Bool IsPoolable(const GLESBufferResource& r) {
                // Require working fences: recycling is gated on the frame-completion
                // watermark, which only advances if Present can insert/poll fences.
                return g_GLESFuncs.glFenceSync != nullptr && g_GLESFuncs.glGetSynciv != nullptr &&
                       r.id != 0 && !r.persistentMapped && !r.immutableStorage &&
                       r.contextGeneration == g_bufferContextGeneration && r.storageInitialized &&
                       r.storageSize > 0 && r.storageSize <= kMaxPoolableBufferBytes;
            }

            // Retire a buffer id into the pool (owning thread; caller verified IsPoolable).
            // Zeroes r.id to keep the single-owner invariant {live | deferred | pool}.
            void EnrollIntoPool(GLESBufferResource& r) {
                if (g_boundArrayBufferKnown && g_boundArrayBufferId == r.id) {
                    InvalidateArrayBufferBindingCache();
                }
                // Pooling keeps the id alive (and thus any driver binding of it);
                // drop to unknown rather than claiming the post-delete 0 state.
                if ((g_boundPixelPackBufferKnown && g_boundPixelPackBufferId == r.id) ||
                    (g_boundPixelUnpackBufferKnown && g_boundPixelUnpackBufferId == r.id)) {
                    InvalidatePixelBufferBindingCaches();
                }
                const std::lock_guard<std::mutex> lock(g_poolMutex);
                auto& bucket = g_bufferPool[r.storageSize];
                if (bucket.size() >= kMaxEntriesPerBucket || g_pooledBytes + r.storageSize > kMaxPoolBytes) {
                    ScrubBufferBindingShadowsForId(r.id);
                    g_GLESFuncs.glDeleteBuffers(1, &r.id); // over budget: don't pool
                    r.id = 0;
                    return;
                }
                // +1: Present increments the serial at frame END, so during the frame
                // now being built CurrentFrameSerial() reads (frame-1). A buffer used
                // this frame is only GPU-done once THIS frame's fence (serial+1) signals.
                bucket.push_back(
                    {r.id, r.storageSize, r.contextGeneration, DirectGLES::CurrentFrameSerial() + 1});
                g_pooledBytes += r.storageSize;
                r.id = 0;
            }

            // Hand back an idle pooled id of EXACTLY `size` whose GPU work is complete,
            // else 0. Owning thread only. Drops stale-generation entries encountered.
            Uint AcquireFromPool(SizeT size) {
                const Uint64 completed = DirectGLES::CompletedFrameSerial();
                const std::lock_guard<std::mutex> lock(g_poolMutex);
                auto it = g_bufferPool.find(size);
                if (it == g_bufferPool.end()) return 0;
                auto& bucket = it->second;
                for (SizeT i = bucket.size(); i-- > 0;) { // newest-first: hottest + most-likely-idle
                    PooledBuffer& e = bucket[i];
                    if (e.contextGeneration != g_bufferContextGeneration) {
                        g_pooledBytes -= e.size; // dead-context id: drop, no GL
                        bucket[i] = bucket.back();
                        bucket.pop_back();
                        continue;
                    }
                    if (e.retireSerial <= completed) {
                        const Uint id = e.id;
                        g_pooledBytes -= e.size;
                        bucket[i] = bucket.back();
                        bucket.pop_back();
                        return id;
                    }
                }
                return 0;
            }

            // --- Persistent-mapped bump rings (see Managers.h) -----------------------
            // Shared machinery behind BOTH the global-UBO ring and the texture
            // unpack-PBO ring: one EXT_buffer_storage persistent|coherent map per ring,
            // monotonic head/tail cursors, and reclamation riding the Present()
            // frame-fence watermark. The two rings differ only in size cap, offset
            // alignment and log label - the reclamation, context-loss and
            // emergency-drain rules are the part that was hard to get right, so they
            // are shared rather than copied.
            constexpr SizeT kUboRingInitialBytes = 4u * 1024u * 1024u;
            constexpr SizeT kUboRingMaxBytes = 64u * 1024u * 1024u;
            constexpr SizeT kUnpackRingInitialBytes = 4u * 1024u * 1024u;
            constexpr SizeT kUnpackRingMaxBytes = 64u * 1024u * 1024u;
            // A PBO-sourced glTexSubImage constrains the offset only to the pixel
            // TYPE's size (GL_INVALID_OPERATION otherwise), and no ES client type is
            // wider than 4 bytes - unlike a UBO bind, which owes the driver
            // GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT. 64 covers every type with room to
            // spare and keeps consecutive staged blocks off each other's cache lines.
            constexpr SizeT kUnpackRingAlignment = 64;
            // glCopyBufferSubData carries no offset-alignment requirement at all; 64
            // keeps staged blocks cache-line separated, same as the unpack ring.
            constexpr SizeT kUploadRingInitialBytes = 4u * 1024u * 1024u;
            constexpr SizeT kUploadRingMaxBytes = 64u * 1024u * 1024u;
            constexpr SizeT kUploadRingAlignment = 64;

            struct PersistentRingStore {
                Uint id = 0;
                Uint8* mappedPtr = nullptr;
                SizeT size = 0;
                // Monotonic linear cursors: `head` counts every byte ever allocated
                // (incl. wrap padding); everything below `tail` is GPU-complete. Ring
                // offset of a linear position is pos % size, so in-flight bytes are
                // head - tail and must stay <= size.
                Uint64 head = 0;
                Uint64 tail = 0;
                Uint32 generation = 0; // bumped on every (re)create/grow; 0 = never valid
                Uint contextGeneration = 0;
                SizeT alignment = 256;
                // A hard storage-creation failure under this context; stop retrying
                // per use (cleared when the context generation moves on).
                Bool creationFailed = false;
            };

            // Grown-away ring stores: deletable only once the GPU finished the last
            // frame that could reference them (same watermark as the buffer pool).
            struct RetiredRingStore {
                Uint id = 0;
                Uint contextGeneration = 0;
                Uint64 retireSerial = 0;
            };

            // Present()-time high-water marks: every byte below headAtPresent was
            // written during frames <= frameSerial, so once frameSerial completes,
            // tail may advance to headAtPresent. FIFO by construction.
            struct RingFrameMark {
                Uint64 frameSerial = 0;
                Uint64 headAtPresent = 0;
            };

            // One ring: its live store, its two reclamation lists, and the immutable
            // knobs that tell it apart from the other one.
            struct PersistentRing {
                PersistentRingStore store;
                Vector<RetiredRingStore> retired;
                Vector<RingFrameMark> frameMarks;
                SizeT initialBytes = 0;
                SizeT maxBytes = 0;
                // 0: take the offset alignment from GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT
                // at store-creation time (the UBO ring's binds require it).
                SizeT fixedAlignment = 0;
                const char* label = "";
            };

            PersistentRing g_uboRing{{}, {}, {}, kUboRingInitialBytes, kUboRingMaxBytes, 0, "Global-UBO ring"};
            PersistentRing g_unpackRing{{},
                                        {},
                                        {},
                                        kUnpackRingInitialBytes,
                                        kUnpackRingMaxBytes,
                                        kUnpackRingAlignment,
                                        "Texture unpack ring"};
            // Staging ring for app buffer updates whose destination store may still be
            // referenced by in-flight GPU work. Mali's glBufferSubData resolves that WAR
            // hazard by BLOCKING in the call (osup_sync_object_wait) until every
            // referencing job retires - under Minecraft 26.3's per-frame UBO and
            // chunk-mesh SubData streams that serialized whole frames (~1 fps while
            // chunks stream in). Staging the bytes here and issuing a
            // glCopyBufferSubData instead keeps the hazard on the GPU timeline where it
            // is just job ordering, and the CPU never waits.
            PersistentRing g_uploadRing{{},
                                        {},
                                        {},
                                        kUploadRingInitialBytes,
                                        kUploadRingMaxBytes,
                                        kUploadRingAlignment,
                                        "Buffer upload ring"};

            // The ES context the ring's id/map belonged to is gone (or was never
            // seen): drop every handle without GL calls and re-arm creation. The
            // generation counter must survive the reset — frame serials also survive
            // context recreation, so a restarted counter could revalidate a stale
            // per-program slot cache against the new ring.
            void ResetRingForNewContext(PersistentRing& ring) {
                const Uint32 keptGeneration = ring.store.generation;
                ring.store = {};
                ring.store.generation = keptGeneration;
                ring.store.contextGeneration = g_bufferContextGeneration;
                // Keep the ring's own alignment across the wipe: the slow path rounds a
                // request with it BEFORE the store that would set it exists.
                if (ring.fixedAlignment != 0) ring.store.alignment = ring.fixedAlignment;
                ring.retired.clear();
                ring.frameMarks.clear();
            }

            GLESBufferResource* ResourceOf(BufferObject& bufferObject) {
                return static_cast<GLESBufferResource*>(bufferObject.GetBackendResource().get());
            }

            Bool CanTouchGLNow() {
                return DirectGLES::IsBackendContextCurrentOnThisThread();
            }

            // (Re)specify backend storage from the shadow copy: glBufferData.
            // The orphaning point - the ES driver performs the actual rename.
            // TODO(buffer-pool Phase 2): orphan-on-respecify is NOT yet implemented.
            // When the current id is BUSY (lastUseFrameSerial > CompletedFrameSerial())
            // && !persistentMapped && !noOrphan, express the orphan as an id-swap
            // (retire the busy id into the pool, bind a fresh/pooled id) instead of the
            // in-place glBufferData below, to avoid the driver's own rename/stall. Not
            // pursued yet: glBufferData/glBufferSubData currently sit below profiler
            // noise, so respecify is not a hot path in the profiled scenes.
            void RespecifyStorageNow(GLESBufferResource& resource, BufferObject& bufferObject) {
#ifdef TRACY_ENABLE
                ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
                const SizeT size = bufferObject.GetSize();
                // Read BEFORE the fields below are overwritten: whether this respecify changes
                // the store's EXTENT is what decides if the indexed-binding shadow still
                // describes the driver.
                const Bool extentChanged = !resource.storageInitialized || resource.storageSize != size;
                const GLenum usage = MG_Util::ConvertBufferUsageToGLEnum(bufferObject.GetUsage());
                BindBufferId(TempBufferTarget, resource.id);
                // An orphaning respecify (glBufferData with NULL, content never
                // written since) stays a pure NULL reallocation: the driver renames
                // the store without a stall and nothing is transferred. Uploading
                // the stale shadow here turned Minecraft-style orphaning into a
                // full-size synchronized upload.
                const void* initialData =
                    (size > 0 && bufferObject.HasDefinedContent()) ? bufferObject.MappedData() : nullptr;
                g_GLESFuncs.glBufferData(TempBufferTarget, (GLsizeiptr)size, initialData, usage);
                if (MG_Util::PipeStats::Enabled() && initialData != nullptr) {
                    // An ORPHANING respecify passes NULL and moves nothing, which is exactly
                    // why the test is on initialData rather than on size.
                    MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageBuffer,
                                                 static_cast<Uint64>(size));
                }
                resource.storageSize = size;
                resource.storageInitialized = true;
                resource.pendingRespecify = false;
                resource.pendingRanges.clear();
                resource.pendingResidentWrites.clear();
                resource.syncedChangeSerial = bufferObject.GetChangeSerial();
                // A GROWN store keeps its indexed bindings, and BindBufferBaseCached skips a
                // rebind whenever the shadow already records this id at that index - so on a
                // driver that resolves a whole-buffer indexed binding's extent at BIND time
                // (Adreno does; Mali does not) the shader keeps seeing the old, smaller range:
                // stores past it are dropped and loads return zero. Forget what the shadow
                // claims for this id so the next SyncBufferBindingPoints issues the bind for
                // real. Only when the extent actually moved: an orphaning respecify at the same
                // size is Minecraft's per-frame hot path and its bindings are still exact.
                if (extentChanged) {
                    InvalidateIndexedBufferBindingShadowsForId(resource.id);
                }
            }

            Bool StorageMatches(const GLESBufferResource& resource, const BufferObject& bufferObject) {
                return resource.storageInitialized && !resource.pendingRespecify &&
                       resource.storageSize == bufferObject.GetSize();
            }



            void UploadRangeNow(GLESBufferResource& resource, BufferObject& bufferObject, SizeT start, SizeT end) {
#ifdef TRACY_ENABLE
                ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
                if (start >= end) return;
                BindBufferId(TempBufferTarget, resource.id);
                g_GLESFuncs.glBufferSubData(TempBufferTarget, (GLintptr)start, (GLsizeiptr)(end - start),
                                            bufferObject.MappedData() + start);
            }

            // Ring machinery shared with the UBO/unpack rings; defined further down in
            // this same unnamed namespace.
            Bool RingAllocate(PersistentRing& ring, SizeT size, SizeT& outOffset);
            Bool RingAvailable(PersistentRing& ring);

            // True when a pending-range flush can go through the staging ring right
            // now: kill switch off, the ES copy entry point resolved, and the ring's
            // own availability gate (EXT_buffer_storage + fences + live context) up.
            Bool UploadRingUsableNow() {
                if (MG_Config::Features.EsprytDisableUploadRing) return false;
                if (!g_GLESFuncs.glCopyBufferSubData) return false;
                return RingAvailable(g_uploadRing);
            }

            // A partial range below this goes through the staging ring instead of a
            // range-invalidating map: the map's page-substitution fast path needs a
            // sizeable (page-coverable) range to engage, and below it the driver
            // falls back to waiting out the WAR hazard on the CPU.
            constexpr SizeT kInvalidateRangeMinBytes = 128u * 1024u;

            // Push every queued range of `resource` from the shadow into the backend
            // store, without ever letting a driver resolve the WAR hazard against
            // in-flight frames at the WHOLE BUFFER's expense. Three tiers:
            //
            //   1. glMapBufferRange(WRITE | INVALIDATE_RANGE) + memcpy. The entire
            //      mapped range is rewritten from the authoritative shadow, so
            //      declaring its old bytes dead is exact - and it lets the driver
            //      swap fresh pages in for JUST that range. This is the only tier
            //      whose cost scales with the RANGE on this Mali driver: both the
            //      immediate glBufferSubData (pre-queueing) and a staged
            //      glCopyBufferSubData into a busy MUTABLE store ghost the whole
            //      destination with a worker-thread memcpy - Minecraft 26.3 streams
            //      ~1MB section meshes into 128MB arenas about nine times a frame
            //      during a camera pan, and 9 x 128MB of ghosting per frame is
            //      ~380ms, the measured 2-4 fps. (Backing the arenas with immutable
            //      stores also kills the ghost, but eagerly commits every arena's
            //      full extent - +hundreds of MB - which LMK'd the whole device.)
            //   2. The staging ring + glCopyBufferSubData: the copy is ordered on
            //      the GPU timeline, no CPU wait (MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH
            //      forces this tier as the map path's negative control).
            //   3. Direct glBufferSubData (potentially stalling) when neither the
            //      map entry points nor the ring exist.
            //
            // The ranges are flushed AS QUEUED (VecRange1D::Add already merges
            // near-adjacent ones): bytes, not flush calls, are the cost axis here,
            // and collapsing a scattered flush into its union re-copied nearly whole
            // chunk-mesh arenas every frame.
            // The caller owns syncedChangeSerial; this only drains the queue.
            void FlushPendingRangesNow(GLESBufferResource& resource, BufferObject& bufferObject) {
#ifdef TRACY_ENABLE
                ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
                VecRange1D ranges;
                {
                    const std::lock_guard<std::mutex> lock(resource.pendingMutex);
                    if (resource.pendingRanges.empty()) return;
                    ranges = std::move(resource.pendingRanges);
                    resource.pendingRanges.clear();
                }
                // Clamp against BOTH extents: the readback flush may run while the
                // frontend size and the backend store disagree (a pending respecify
                // resolves that later; bytes past either end have nowhere to land).
                const SizeT limit = std::min(bufferObject.GetSize(), resource.storageSize);
                const Bool mapUsable = !MG_Config::Features.EsprytDisableInvalidateFlush &&
                                       g_GLESFuncs.glMapBufferRange && g_GLESFuncs.glUnmapBuffer;
                const Bool ringUsable = UploadRingUsableNow();
                for (const auto& range : ranges) {
                    const SizeT end = std::min(range.end, limit);
                    const SizeT start = std::min(range.start, end);
                    const SizeT size = end - start;
                    if (size == 0) continue;
                    if (MG_Util::PipeStats::Enabled()) {
                        // Counted once per queued range, before the three delivery shapes
                        // below diverge: all three move exactly these bytes, and it is the
                        // byte count - not the shape - that sizes SEG_STAGE.
                        MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageBuffer,
                                                     static_cast<Uint64>(size));
                    }
                    // The invalidating map's fast path is SHAPE-dependent on this Mali
                    // driver: a whole-buffer invalidation renames the store outright,
                    // and a large range gets fresh pages - but a small unaligned range
                    // of a busy store makes the map WAIT (osup_sync_object_wait, ~9%
                    // of a Minecraft 26.3 replay). So: whole buffer -> orphan-map;
                    // large range -> range-invalidating map; small range -> the staged
                    // ring copy, whose worst case (a whole-destination ghost) is only
                    // ever the small destination itself.
                    //
                    // The map covers EXACTLY the queued range: only those bytes are the
                    // shadow's to rewrite. Widening to page bounds looked free and was
                    // not - the widened bytes clobbered GPU-written data (an SSBO
                    // counter beside the app's SubData) with the stale shadow.
                    const Bool wholeBuffer = start == 0 && end == limit && limit == resource.storageSize;
                    if (mapUsable && (wholeBuffer || size >= kInvalidateRangeMinBytes)) {
                        BindBufferId(TempBufferTarget, resource.id);
                        const GLbitfield access =
                            GL_MAP_WRITE_BIT |
                            (wholeBuffer ? GL_MAP_INVALIDATE_BUFFER_BIT : GL_MAP_INVALIDATE_RANGE_BIT);
                        void* dst = g_GLESFuncs.glMapBufferRange(TempBufferTarget, (GLintptr)start,
                                                                 (GLsizeiptr)size, access);
                        if (dst) {
                            Memcpy(dst, bufferObject.MappedData() + start, size);
                            g_GLESFuncs.glUnmapBuffer(TempBufferTarget);
                            continue;
                        }
                    }
                    SizeT ringOffset = 0;
                    if (ringUsable && size <= kUploadRingMaxBytes &&
                        RingAllocate(g_uploadRing, size, ringOffset)) {
                        Memcpy(g_uploadRing.store.mappedPtr + ringOffset, bufferObject.MappedData() + start, size);
                        BindBufferId(GL_COPY_READ_BUFFER, g_uploadRing.store.id);
                        BindBufferId(GL_COPY_WRITE_BUFFER, resource.id);
                        g_GLESFuncs.glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
                                                        (GLintptr)ringOffset, (GLintptr)start, (GLsizeiptr)size);
                    } else {
                        UploadRangeNow(resource, bufferObject, start, end);
                    }
                }
            }

            // Land the app bytes queued for an ADOPTED store on the GPU timeline: staged
            // into the upload ring and delivered by glCopyBufferSubData. The destination
            // is the IMMUTABLE persistent store, which the driver can neither rename nor
            // ghost, so the copy is plain job ordering - after every in-flight reader,
            // before the next consumer - which is exactly glBufferSubData's contract.
            // (The in-place host write these bytes replaced tore the frames still
            // reading the old vertex data: one-frame wrong geometry during fast camera
            // movement.) Fallback: direct glBufferSubData - the adopted store carries
            // DYNAMIC_STORAGE, and immutability again forbids the whole-store ghost.
            void DrainResidentWritesNow(GLESBufferResource& resource, BufferObject& bufferObject) {
#ifdef TRACY_ENABLE
                ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
                Vector<GLESBufferResource::PendingResidentWrite> writes;
                {
                    const std::lock_guard<std::mutex> lock(resource.pendingMutex);
                    if (resource.pendingResidentWrites.empty()) return;
                    writes = std::move(resource.pendingResidentWrites);
                    resource.pendingResidentWrites.clear();
                }
                const SizeT limit = resource.storageSize;
                const Bool ringUsable = UploadRingUsableNow();
                for (const auto& write : writes) {
                    if (write.offset >= limit) continue;
                    const SizeT size = std::min(write.bytes.size(), limit - write.offset);
                    if (size == 0) continue;
                    if (MG_Util::PipeStats::Enabled()) {
                        MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageBuffer,
                                                     static_cast<Uint64>(size));
                    }
                    SizeT ringOffset = 0;
                    if (ringUsable && size <= kUploadRingMaxBytes &&
                        RingAllocate(g_uploadRing, size, ringOffset)) {
                        Memcpy(g_uploadRing.store.mappedPtr + ringOffset, write.bytes.data(), size);
                        BindBufferId(GL_COPY_READ_BUFFER, g_uploadRing.store.id);
                        BindBufferId(GL_COPY_WRITE_BUFFER, resource.id);
                        g_GLESFuncs.glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
                                                        (GLintptr)ringOffset, (GLintptr)write.offset,
                                                        (GLsizeiptr)size);
                    } else {
                        BindBufferId(TempBufferTarget, resource.id);
                        g_GLESFuncs.glBufferSubData(TempBufferTarget, (GLintptr)write.offset, (GLsizeiptr)size,
                                                    write.bytes.data());
                    }
                }
            }

            // EXT_buffer_storage bit values (same numeric values as the desktop ARB
            // tokens); defined locally so this compiles regardless of which GLES headers
            // expose the EXT tokens.
            constexpr GLbitfield kMapPersistentBit = 0x0040;
            constexpr GLbitfield kMapCoherentBit = 0x0080;
            constexpr GLbitfield kDynamicStorageBit = 0x0100;

            // Zero-copy persistent map: back the buffer with real immutable,
            // persistently+coherently mapped GL storage (EXT_buffer_storage) and hand the
            // app that mapped pointer (adopted by the frontend PipeResource). Returns
            // nullptr when the extension is unavailable or the context is not current, in
            // which case the frontend keeps its CPU-shadow model. Idempotent.
            void* Ops_AcquirePersistentMap(BufferObject& bufferObject) {
                if (!CanTouchGLNow() || !g_GLESFuncs.glBufferStorageEXT || !g_GLESFuncs.glMapBufferRange ||
                    !g_GLESFuncs.glGenBuffers) {
                    return nullptr;
                }
                const SizeT size = bufferObject.GetSize();
                if (size == 0) return nullptr;

                auto* resource = static_cast<GLESBufferResource*>(bufferObject.GetBackendResource().get());
                if (!resource) {
                    auto created = MakeShared<GLESBufferResource>();
                    resource = created.get();
                    bufferObject.SetBackendResource(std::move(created));
                }
                // Before the generation is stamped, not after: everything on the resource
                // describes a context that is gone, and the idempotency check below would
                // otherwise hand the caller the dead context's mapped pointer.
                if (resource->contextGeneration != g_bufferContextGeneration) {
                    resource->id = 0;
                    resource->persistentMapped = false;
                    resource->persistentPtr = nullptr;
                    resource->immutableStorage = false;
                    resource->storageInitialized = false;
                    resource->storageSize = 0;
                }
                resource->contextGeneration = g_bufferContextGeneration;

                if (resource->persistentMapped && resource->persistentPtr && resource->storageSize == size) {
                    return resource->persistentPtr; // idempotent
                }

                // Need a fresh id: glBufferStorage fails on a buffer that already has
                // immutable storage, and any prior mutable store is replaced anyway.
                if (resource->id != 0) {
                    NoteBufferIdDeleted(resource->id);
                    // Driver VAOs may have this id baked into attribute/element bindings
                    // keyed on frontend versions this re-mint does not move.
                    ++g_bufferBackendIdGeneration;
                    g_GLESFuncs.glDeleteBuffers(1, &resource->id);
                    resource->id = 0;
                    resource->immutableStorage = false;
                }
                g_GLESFuncs.glGenBuffers(1, &resource->id);
                if (resource->id == 0) return nullptr;

                // Seed from the shadow (MappedData() is still the shadow: the frontend
                // adopts and drops it only after this returns).
                BindBufferId(TempBufferTarget, resource->id);
                const void* initial = bufferObject.MappedData();
                g_GLESFuncs.glBufferStorageEXT(TempBufferTarget, static_cast<GLsizeiptr>(size), initial,
                                               GL_MAP_WRITE_BIT | kMapPersistentBit | kMapCoherentBit |
                                                   kDynamicStorageBit);
                // Set as soon as the store exists, not once the map succeeds: the failure
                // path below leaves this id holding immutable storage, and whoever touches
                // it next has to know that glBufferData cannot redefine it.
                resource->immutableStorage = true;
                void* ptr = g_GLESFuncs.glMapBufferRange(TempBufferTarget, 0, static_cast<GLsizeiptr>(size),
                                                         GL_MAP_WRITE_BIT | kMapPersistentBit | kMapCoherentBit);
                if (!ptr) {
                    MGLOG_E_ONCE("Ops_AcquirePersistentMap: glMapBufferRange(persistent) failed for buffer %u",
                            resource->id);
                    resource->persistentMapped = false;
                    resource->persistentPtr = nullptr;
                    return nullptr;
                }
                resource->persistentPtr = ptr;
                resource->persistentMapped = true;
                resource->storageSize = size;
                resource->storageInitialized = true;
                resource->pendingRespecify = false;
                {
                    const std::lock_guard<std::mutex> lock(resource->pendingMutex);
                    resource->pendingRanges.clear();
                    resource->pendingResidentWrites.clear();
                }
                resource->syncedChangeSerial = bufferObject.GetChangeSerial();
                return ptr;
            }

            void Ops_Respecify(BufferObject& bufferObject) {
                auto* resource = ResourceOf(bufferObject);
                if (!resource) return; // lazy: EnsureBufferResource full-uploads on creation
                // The frontend hands an adopted mapping back before it redefines the store
                // (BufferObject::RedefineStorage), so a resource that still carries the
                // persistent state here describes the OLD store - and its storage is
                // IMMUTABLE (glBufferStorageEXT), which the glBufferData below cannot
                // respecify and which the driver would refuse in silence. Retire the id so
                // EnsureBufferResource mints a mutable one, with a full upload from the
                // shadow the frontend has just filled.
                //
                // Keyed on the STORAGE, not on persistentMapped: a glMapBufferRange that
                // failed after its glBufferStorageEXT succeeded clears persistentMapped and
                // still leaves an immutable store behind, and that one reached glBufferData.
                if (resource->immutableStorage) {
                    resource->persistentMapped = false;
                    resource->persistentPtr = nullptr;
                    if (resource->id != 0 && CanTouchGLNow() &&
                        resource->contextGeneration == g_bufferContextGeneration) {
                        NoteBufferIdDeleted(resource->id);
                        g_GLESFuncs.glDeleteBuffers(1, &resource->id);
                        resource->id = 0;
                        resource->immutableStorage = false;
                    }
                    // Off the context thread the id cannot be deleted here, and dropping it
                    // would leak an immutable, persistently mapped store. It stays put, and
                    // stays flagged, until EnsureBufferResource retires it on the thread
                    // that owns the context.
                    resource->storageInitialized = false;
                    resource->storageSize = 0;
                    resource->pendingRespecify = true;
                    resource->pendingRanges.clear();
                    resource->pendingResidentWrites.clear();
                    return;
                }
                if (!CanTouchGLNow() || resource->id == 0 ||
                    resource->contextGeneration != g_bufferContextGeneration) {
                    resource->pendingRespecify = true;
                    resource->pendingRanges.clear();
                    resource->pendingResidentWrites.clear();
                    return;
                }
                if (bufferObject.GetSize() == 0) {
                    resource->storageInitialized = false;
                    resource->storageSize = 0;
                    resource->pendingRespecify = false;
                    resource->pendingRanges.clear();
                    resource->pendingResidentWrites.clear();
                    return;
                }
                RespecifyStorageNow(*resource, bufferObject);
            }

            void Ops_SubData(BufferObject& bufferObject, SizeT offset, SizeT size) {
                auto* resource = ResourceOf(bufferObject);
                if (!resource) return;
                if (resource->pendingRespecify) return; // full re-upload pending anyway
                if (!CanTouchGLNow() || resource->id == 0 ||
                    resource->contextGeneration != g_bufferContextGeneration ||
                    !StorageMatches(*resource, bufferObject)) {
                    const std::lock_guard<std::mutex> lock(resource->pendingMutex);
                    resource->pendingRanges.Add({offset, offset + size});
                    return;
                }
                // An adopted zero-copy persistent store already HAS the bytes (the
                // frontend wrote them through the coherent mapping); a driver upload
                // here would be a self-copy that re-synchronizes what coherent mapping
                // made free.
                if (resource->persistentMapped && resource->persistentPtr) {
                    resource->syncedChangeSerial = bufferObject.GetChangeSerial();
                    return;
                }
                // An immediate glBufferSubData resolves the WAR hazard against frames
                // still referencing this store on the CPU on some drivers - Mali parks
                // the thread in osup_sync_object_wait until every referencing job
                // retires, which serialized Minecraft 26.3's per-frame UBO/chunk-mesh
                // update streams into ~1 fps. Queue the range instead (the shadow
                // already holds the bytes) and let draw-time sync push the merged
                // ranges through the staging ring.
                if (MG_Config::Features.EsprytDisableUploadRing) {
                    UploadRangeNow(*resource, bufferObject, offset, offset + size);
                    resource->syncedChangeSerial = bufferObject.GetChangeSerial();
                    return;
                }
                const std::lock_guard<std::mutex> lock(resource->pendingMutex);
                resource->pendingRanges.Add({offset, offset + size});
            }

            // App bytes for an ADOPTED store: queue them untouched-by-the-mapping; the
            // draw-time sync (or a readback) lands them GPU-ordered through
            // DrainResidentWritesNow. No GL here, so the op is thread-agnostic.
            void Ops_ResidentSubData(BufferObject& bufferObject, SizeT offset, DataPtr data) {
                auto* resource = ResourceOf(bufferObject);
                if (!resource || data.size == 0) return;
                const std::lock_guard<std::mutex> lock(resource->pendingMutex);
                auto& write = resource->pendingResidentWrites.emplace_back();
                write.offset = offset;
                const auto* bytes = static_cast<const Uint8*>(data.data);
                write.bytes.assign(bytes, bytes + data.size);
            }

            void Ops_FlushMappedRange(BufferObject& bufferObject, Range1D range,
                                      Flags<BufferMappingAccessBit> appAccess) {
                auto* resource = ResourceOf(bufferObject);
                if (!resource) return;
                if (resource->pendingRespecify) return;
                if (!CanTouchGLNow() || resource->id == 0 ||
                    resource->contextGeneration != g_bufferContextGeneration ||
                    !StorageMatches(*resource, bufferObject)) {
                    const std::lock_guard<std::mutex> lock(resource->pendingMutex);
                    resource->pendingRanges.Add(range);
                    return;
                }

                // An adopted zero-copy persistent store already HAS the bytes: the
                // frontend shadow IS the coherent mapping the app (or UploadSubData)
                // wrote into, so publishing is free. The self-copy that used to run
                // here mapped a buffer this backend keeps persistently mapped (an
                // INVALID_OPERATION whose fallback was a WAR-stalling
                // glBufferSubData).
                if (resource->persistentMapped && resource->persistentPtr) {
                    resource->syncedChangeSerial = bufferObject.GetChangeSerial();
                    return;
                }

                // Same WAR-hazard rule as Ops_SubData: an immediate synchronized upload
                // (mapped or glBufferSubData) can park the thread on Mali until the
                // frames still referencing this store retire. Queue the range for the
                // staged flush at draw-time sync; the negative-control kill switch
                // keeps the immediate paths below.
                if (!MG_Config::Features.EsprytDisableUploadRing) {
                    const std::lock_guard<std::mutex> lock(resource->pendingMutex);
                    resource->pendingRanges.Add(range);
                    return;
                }

                // Honour the app's real mapping flags per call: only reach for a
                // mapped upload when the app allowed invalidation/unsynchronized
                // access, otherwise a plain glBufferSubData carries the exact
                // synchronization semantics.
                const Bool invalidate = (appAccess & BufferMappingAccessBit::InvalidateRange) ||
                                        (appAccess & BufferMappingAccessBit::InvalidateBuffer);
                const Bool unsynchronized = static_cast<Bool>(appAccess & BufferMappingAccessBit::Unsynchronized);
                if (PREFER_MAP_BUFFER_RANGE_FOR_BUFFER_SYNC && (invalidate || unsynchronized)) {
#ifdef TRACY_ENABLE
                    ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
                    BindBufferId(TempBufferTarget, resource->id);
                    void* mappedData = g_GLESFuncs.glMapBufferRange(
                        TempBufferTarget, (GLintptr)range.start, (GLsizeiptr)(range.end - range.start),
                        GL_MAP_WRITE_BIT | (invalidate ? GL_MAP_INVALIDATE_RANGE_BIT : 0) |
                            (unsynchronized ? GL_MAP_UNSYNCHRONIZED_BIT : 0));
                    if (mappedData) {
                        Memcpy(mappedData, bufferObject.MappedData() + range.start,
                               range.end - range.start);
                        g_GLESFuncs.glUnmapBuffer(TempBufferTarget);
                        resource->syncedChangeSerial = bufferObject.GetChangeSerial();
                        return;
                    }
                    MGLOG_E_ONCE("Failed to map buffer with ID: %u for flush, falling back to glBufferSubData",
                            resource->id);
                }
                UploadRangeNow(*resource, bufferObject, range.start, range.end);
                resource->syncedChangeSerial = bufferObject.GetChangeSerial();
            }

            // A shader wrote this buffer through a storage/atomic-counter binding, so the ES
            // driver's copy is ahead of the frontend shadow. Pull the whole thing back so
            // MapBuffer/GetBufferSubData/CopyBufferSubData see the real results.
            void Ops_ReadbackFromGpu(BufferObject& bufferObject) {
                auto* resource = ResourceOf(bufferObject);
                if (!resource || resource->id == 0 || !resource->storageInitialized) return;
                if (!CanTouchGLNow() || resource->contextGeneration != g_bufferContextGeneration) return;
                if (resource->persistentMapped) {
                    // Queued resident SubData bytes land first (GPU-ordered), then the
                    // finish makes them - and any shader writes already queued on this
                    // context - visible through the coherent mapping the reads use.
                    // There is no backend copy to read back in this case.
                    DrainResidentWritesNow(*resource, bufferObject);
                    if (g_GLESFuncs.glFinish) g_GLESFuncs.glFinish();
                    return;
                }
                if (!g_GLESFuncs.glMapBufferRange || !g_GLESFuncs.glUnmapBuffer) return;
                const SizeT size = std::min<SizeT>(bufferObject.GetSize(), resource->storageSize);
                if (size == 0) return;

                // Queued app writes must land in the backend store before it is read
                // back, or the writeback below would revert them in the shadow.
                FlushPendingRangesNow(*resource, bufferObject);

                BindBufferId(TempBufferTarget, resource->id);
                void* mapped = g_GLESFuncs.glMapBufferRange(TempBufferTarget, 0, static_cast<GLsizeiptr>(size),
                                                            GL_MAP_READ_BIT);
                if (mapped == nullptr) {
                    MGLOG_E_ONCE("Ops_ReadbackFromGpu: glMapBufferRange(read) failed for buffer %u", resource->id);
                    return;
                }
                bufferObject.WritebackFromBackend({mapped, size}, 0);
                g_GLESFuncs.glUnmapBuffer(TempBufferTarget);
                // The shadow now matches the backend byte for byte; without this the next
                // draw would see a newer change serial and re-upload the readback over it.
                resource->syncedChangeSerial = bufferObject.GetChangeSerial();
            }

            void Ops_OnDestroy(SharedPtr<BackendBufferResource>&& resource) {
                if (!resource) return;
                auto* glesResource = static_cast<GLESBufferResource*>(resource.get());
                if (glesResource->contextGeneration != g_bufferContextGeneration) {
                    glesResource->id = 0; // id belonged to a destroyed context
                    return;
                }
                if (CanTouchGLNow()) {
                    if (IsPoolable(*glesResource)) {
                        EnrollIntoPool(*glesResource); // recycle instead of glDeleteBuffers
                        return;
                    }
                    if (glesResource->id != 0) {
                        if (g_boundArrayBufferKnown && g_boundArrayBufferId == glesResource->id) {
                            InvalidateArrayBufferBindingCache();
                        }
                        ScrubBufferBindingShadowsForId(glesResource->id);
                        g_GLESFuncs.glDeleteBuffers(1, &glesResource->id);
                        glesResource->id = 0;
                    }
                    return;
                }
                const std::lock_guard<std::mutex> lock(g_deferredBufferReleasesMutex);
                g_deferredBufferReleases.push_back(std::move(resource));
                g_hasDeferredBufferReleases.store(true, std::memory_order_release);
            }

            // Epoch-tracking wrappers: every op bumps the buffer-mutation epoch AFTER
            // its impl returns (release; see Managers.h for why the order matters),
            // covering every mutation branch inside - including the early returns
            // that only queued pendingRanges or flagged pendingRespecify. Bumping on
            // an op that turned out to be a no-op merely re-runs the probes once.
            void Ops_RespecifyTracked(BufferObject& bufferObject) {
                Ops_Respecify(bufferObject);
                BumpBufferMutationEpoch();
            }
            void Ops_SubDataTracked(BufferObject& bufferObject, SizeT offset, SizeT size) {
                Ops_SubData(bufferObject, offset, size);
                BumpBufferMutationEpoch();
            }
            void Ops_ResidentSubDataTracked(BufferObject& bufferObject, SizeT offset, DataPtr data) {
                Ops_ResidentSubData(bufferObject, offset, data);
                BumpBufferMutationEpoch();
            }
            void Ops_FlushMappedRangeTracked(BufferObject& bufferObject, Range1D range,
                                             Flags<BufferMappingAccessBit> appAccess) {
                Ops_FlushMappedRange(bufferObject, range, appAccess);
                BumpBufferMutationEpoch();
            }
            void Ops_OnDestroyTracked(SharedPtr<BackendBufferResource>&& resource) {
                Ops_OnDestroy(std::move(resource));
                BumpBufferMutationEpoch();
            }
            void* Ops_AcquirePersistentMapTracked(BufferObject& bufferObject) {
                void* result = Ops_AcquirePersistentMap(bufferObject);
                // Bump even on decline: the frontend still enters a persistent map the
                // per-draw probes must start seeing (IsMapped-driven range pushes).
                BumpBufferMutationEpoch();
                return result;
            }
            void Ops_ReadbackFromGpuTracked(BufferObject& bufferObject) {
                Ops_ReadbackFromGpu(bufferObject);
                BumpBufferMutationEpoch();
            }

            const BufferBackendOps g_glesBufferBackendOps = {
                .Respecify = Ops_RespecifyTracked,
                .SubData = Ops_SubDataTracked,
                .ResidentSubData = Ops_ResidentSubDataTracked,
                .FlushMappedRange = Ops_FlushMappedRangeTracked,
                .OnDestroy = Ops_OnDestroyTracked,
                .AcquirePersistentMap = Ops_AcquirePersistentMapTracked,
                .ReadbackFromGpu = Ops_ReadbackFromGpuTracked,
            };
        } // namespace

        Uint64 CurrentBufferMutationEpoch() {
            return g_bufferMutationEpoch.load(std::memory_order_acquire);
        }

        void BumpBufferMutationEpoch() {
            g_bufferMutationEpoch.fetch_add(1, std::memory_order_release);
        }

        // See the declaration: re-mints of a live resource's driver id. Written only on
        // the context thread (both re-mint sites run there), read only by the VAO sync.
        Uint64 g_bufferBackendIdGeneration = 0;

        void RegisterBufferBackendOps() {
            MG_State::GLState::SetBufferBackendOps(&g_glesBufferBackendOps);
            // Frontend writes issued while ops were unregistered advanced change
            // serials with no per-op bump; re-open every draw-clean memo.
            BumpBufferMutationEpoch();
        }

        void UnregisterBufferBackendOps() {
            if (MG_State::GLState::GetBufferBackendOps() == &g_glesBufferBackendOps) {
                MG_State::GLState::SetBufferBackendOps(nullptr);
            }
            // From here on frontend writes bypass the tracked ops entirely.
            BumpBufferMutationEpoch();
            InvalidateArrayBufferBindingCache();
            // Pooled ids belong to the dying context too; drop them without glDeleteBuffers.
            ClearBufferPool();
            const std::lock_guard<std::mutex> lock(g_deferredBufferReleasesMutex);
            // The ES context owning these ids is going away; just drop the handles.
            g_deferredBufferReleases.clear();
            g_hasDeferredBufferReleases.store(false, std::memory_order_release);
        }

        void OnBackendContextDestroyed() {
            UnregisterBufferBackendOps(); // also bumps the buffer-mutation epoch
            ++g_bufferContextGeneration;
            // The generation moved AFTER the unregister bump above; re-open the
            // memos again so no stamp can predate the generation change.
            BumpBufferMutationEpoch();
            InvalidateArrayBufferBindingCache();
            InvalidateIndexedBufferBindingCache();
            InvalidatePixelBufferBindingCaches();
            // The rings' ids and persistent maps died with the context; drop the
            // handles (no GL) and let the next draw / texture upload recreate them.
            ResetRingForNewContext(g_uboRing);
            ResetRingForNewContext(g_unpackRing);
        }

        void ProcessDeferredBufferReleases() {
            // Runs on every draw; skip the context check, mutex and vector churn
            // outright when nothing was enqueued since the last drain.
            if (!g_hasDeferredBufferReleases.load(std::memory_order_acquire)) return;
            if (!CanTouchGLNow()) return;
            Vector<SharedPtr<BackendBufferResource>> releases;
            {
                const std::lock_guard<std::mutex> lock(g_deferredBufferReleasesMutex);
                releases.swap(g_deferredBufferReleases);
                g_hasDeferredBufferReleases.store(false, std::memory_order_release);
            }
            for (auto& resource : releases) {
                auto* glesResource = static_cast<GLESBufferResource*>(resource.get());
                if (glesResource->contextGeneration != g_bufferContextGeneration) {
                    glesResource->id = 0;
                    continue;
                }
                if (IsPoolable(*glesResource)) {
                    EnrollIntoPool(*glesResource); // recycle instead of glDeleteBuffers
                    continue;
                }
                if (glesResource->id != 0) {
                    if (g_boundArrayBufferKnown && g_boundArrayBufferId == glesResource->id) {
                        InvalidateArrayBufferBindingCache();
                    }
                    ScrubBufferBindingShadowsForId(glesResource->id);
                    g_GLESFuncs.glDeleteBuffers(1, &glesResource->id);
                    glesResource->id = 0;
                }
            }
        }

        GLESBufferResource* GetBufferResource(MG_State::GLState::BufferObject* bufferObject) {
            if (!bufferObject) return nullptr;
            return static_cast<GLESBufferResource*>(bufferObject->GetBackendResource().get());
        }

        Bool IsBufferDrawClean(const MG_State::GLState::BufferObject* frontend, const GLESBufferResource* resource) {
            // Identity first: a respecify path can hand the frontend a NEW resource; the
            // memoed pointer is then stale (and only kept alive by the caller's shadow).
            if (!resource || resource != frontend->GetBackendResource().get()) return false;
            if (resource->contextGeneration != g_bufferContextGeneration) return false;
            if (resource->id == 0) return false;
            // Zero-copy coherent persistent store: EnsureBufferResource's own early-out —
            // the app writes straight into the mapped GPU storage, nothing to sync.
            // Except queued resident SubData bytes, which land through the sync path
            // (same unlocked emptiness probe as pendingRanges below).
            if (resource->persistentMapped) {
                return resource->persistentPtr != nullptr && resource->pendingResidentWrites.empty();
            }
            // A live non-zero-copy map may owe a per-draw SyncPersistentMappedRange push
            // (persistent maps mutate the shadow without bumping the change serial).
            if (frontend->IsMapped()) return false;
            if (resource->pendingRespecify || !resource->storageInitialized) return false;
            // Same unlocked emptiness probe EnsureBufferResource's replay branch uses.
            if (!resource->pendingRanges.empty()) return false;
            if (resource->storageSize != frontend->GetSize()) return false;
            return resource->syncedChangeSerial.load(std::memory_order_acquire) == frontend->GetChangeSerial();
        }

        GLESBufferResource* EnsureBufferResource(const SharedPtr<MG_State::GLState::BufferObject>& bufferObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!bufferObject) return nullptr;

            auto* resource = static_cast<GLESBufferResource*>(bufferObject->GetBackendResource().get());
            if (!resource) {
                auto newResource = MakeShared<GLESBufferResource>();
                newResource->pendingRespecify = true;
                resource = newResource.get();
                bufferObject->SetBackendResource(std::move(newResource));
            }

            if (resource->contextGeneration != g_bufferContextGeneration) {
                // The id (if any) belonged to a destroyed ES context.
                resource->id = 0;
                resource->storageInitialized = false;
                resource->storageSize = 0;
                resource->pendingRespecify = true;
                resource->pendingRanges.clear();
                    resource->pendingResidentWrites.clear();
                resource->contextGeneration = g_bufferContextGeneration;
                // The persistent map (and its pointer) died with the old context; the
                // frontend re-acquires a fresh one on its next map.
                resource->persistentMapped = false;
                resource->persistentPtr = nullptr;
                resource->immutableStorage = false;
            }

            // An immutable store nothing maps any more: a respecification of a buffer that
            // had been persistently mapped, which Ops_Respecify could not retire because it
            // ran off the context thread. glBufferData cannot redefine it, so it is retired
            // here, on the thread that can, and the id is re-minted below.
            if (resource->immutableStorage && !resource->persistentMapped && resource->id != 0) {
                NoteBufferIdDeleted(resource->id);
                // Same as the persistent-map re-mint: the dying id may be baked into
                // driver VAO bindings whose frontend versions do not move for this.
                ++g_bufferBackendIdGeneration;
                g_GLESFuncs.glDeleteBuffers(1, &resource->id);
                resource->id = 0;
                resource->immutableStorage = false;
                resource->storageInitialized = false;
                resource->storageSize = 0;
                resource->pendingRespecify = true;
            }

            // Zero-copy coherent persistent buffer: the app writes straight into the
            // persistently mapped immutable store, so there is nothing to (re)upload at
            // draw time. This is where the per-draw whole-buffer glBufferSubData used to run.
            if (resource->persistentMapped && resource->persistentPtr && resource->id != 0) {
                DrainResidentWritesNow(*resource, *bufferObject);
                return resource;
            }

            if (resource->id == 0) {
                // Try to recycle an idle same-size buffer from the pool (GPU-complete,
                // exact byte size) and reseed it in place with glBufferSubData, instead
                // of glGenBuffers + fresh-storage glBufferData (the kgsl alloc path).
                const SizeT poolSize = bufferObject->GetSize();
                const Uint reused =
                    (poolSize > 0 && !resource->persistentMapped) ? AcquireFromPool(poolSize) : 0;
                if (reused != 0) {
                    resource->id = reused;
                    resource->storageSize = poolSize;
                    resource->storageInitialized = true;
                    resource->pendingRespecify = false;
                    BindBufferId(TempBufferTarget, reused);
                    g_GLESFuncs.glBufferSubData(TempBufferTarget, 0, (GLsizeiptr)poolSize,
                                                bufferObject->MappedData());
                    if (MG_Util::PipeStats::Enabled()) {
                        // The pool-recycle reseed is a whole-buffer upload on the hot path,
                        // not a bookkeeping detail: it moves the same bytes a fresh
                        // glBufferData would.
                        MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageBuffer,
                                                     static_cast<Uint64>(poolSize));
                    }
                    {
                        const std::lock_guard<std::mutex> lock(resource->pendingMutex);
                        resource->pendingRanges.clear();
                    resource->pendingResidentWrites.clear();
                    }
                    resource->syncedChangeSerial = bufferObject->GetChangeSerial();
                } else {
                    g_GLESFuncs.glGenBuffers(1, &resource->id);
                    if (resource->id == 0) {
                        MGLOG_E_ONCE("Failed to generate buffer object.");
                        MGLOG_E_ONCE("ES glGetError(): %s",
                                MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
                        return resource;
                    }
                    resource->storageInitialized = false;
                    resource->pendingRespecify = true;
                }
            }

            // Push persistently-mapped writes first; lands either as an immediate
            // SubData (fresh storage) or as part of the full re-upload below.
            bufferObject->SyncPersistentMappedRange();

            if (bufferObject->GetSize() == 0) {
                return resource;
            }

            if (resource->pendingRespecify || !resource->storageInitialized ||
                resource->storageSize != bufferObject->GetSize()) {
                RespecifyStorageNow(*resource, *bufferObject);
            } else if (!resource->pendingRanges.empty()) {
                FlushPendingRangesNow(*resource, *bufferObject);
                resource->syncedChangeSerial = bufferObject->GetChangeSerial();
            } else if (resource->syncedChangeSerial != bufferObject->GetChangeSerial()) {
                // Ops could not track some writes (e.g. the ops table was
                // unregistered between contexts); re-upload everything.
                RespecifyStorageNow(*resource, *bufferObject);
            }
            return resource;
        }

        void BindBufferId(GLenum target, Uint id) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (target == GL_ARRAY_BUFFER) {
                if (g_boundArrayBufferKnown && g_boundArrayBufferId == id) {
                    return;
                }
                g_boundArrayBufferId = id;
                g_boundArrayBufferKnown = true;
            }
            g_GLESFuncs.glBindBuffer(target, id);
        }

        void InvalidateArrayBufferBindingCache() {
            g_boundArrayBufferId = 0;
            g_boundArrayBufferKnown = false;
        }

        void BindPixelPackBufferId(Uint id) {
            if (g_boundPixelPackBufferKnown && g_boundPixelPackBufferId == id) {
                return;
            }
            g_GLESFuncs.glBindBuffer(GL_PIXEL_PACK_BUFFER, id);
            g_boundPixelPackBufferId = id;
            g_boundPixelPackBufferKnown = true;
        }

        void BindPixelUnpackBufferId(Uint id) {
            if (g_boundPixelUnpackBufferKnown && g_boundPixelUnpackBufferId == id) {
                return;
            }
            g_GLESFuncs.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, id);
            g_boundPixelUnpackBufferId = id;
            g_boundPixelUnpackBufferKnown = true;
        }

        void InvalidatePixelBufferBindingCaches() {
            g_boundPixelPackBufferId = 0;
            g_boundPixelPackBufferKnown = false;
            g_boundPixelUnpackBufferId = 0;
            g_boundPixelUnpackBufferKnown = false;
        }

        void NoteBufferIdDeleted(Uint id) {
            if (id == 0) {
                return;
            }
            if (g_boundArrayBufferKnown && g_boundArrayBufferId == id) {
                InvalidateArrayBufferBindingCache();
            }
            ScrubBufferBindingShadowsForId(id);
        }

        namespace {
            // Shadow of the GL indexed buffer bindings so redundant glBindBufferBase/Range
            // (same index + id + range) are skipped. isBase distinguishes a whole-buffer
            // base bind from a sub-range bind. Fresh/reset context: every point is base(0)
            // == unbound, which matches the GL default.
            struct IndexedBufferBinding {
                Uint id = 0;
                GLintptr offset = 0;
                GLsizeiptr size = 0;
                Bool isBase = true;
                // False when the driver's binding at this point is no longer described by the
                // fields above and the next bind must be issued whatever it asks for. Set by
                // InvalidateIndexedBufferBindingShadowsForId after a store was re-specified at
                // a new size: the id is still bound, so the entry must NOT be scrubbed to
                // base(0) (a later bind of 0 would then be false-skipped) - only distrusted.
                Bool known = true;
            };
            constexpr SizeT kMaxIndexedBufferBindings = 64;
            IndexedBufferBinding g_indexedUBOBindings[kMaxIndexedBufferBindings];
            IndexedBufferBinding g_indexedSSBOBindings[kMaxIndexedBufferBindings];
            // Transform feedback gets a shadow for a reason the other two do not have: the
            // capture points are synced from the application's TOUCHED high-water mark, which
            // deqp/glcts permanently raises to GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS by
            // clearing every point after each test case. Without a shadow every capture that
            // uses fewer points than that (i.e. every INTERLEAVED_ATTRIBS capture) re-issued a
            // redundant glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, i, 0) for the unused
            // tail immediately before glBeginTransformFeedback - calls a plain GL application
            // never makes there, and the only thing MobileGL does differently from one.
            //
            // Unlike the UBO/SSBO points these are NOT context state: they belong to the bound
            // transform feedback OBJECT, so XfbImpl::BindTransformFeedback drops the whole
            // shadow to unknown on every object switch (InvalidateTransformFeedbackBindingShadows).
            IndexedBufferBinding g_indexedXFBBindings[kMaxIndexedBufferBindings];
            IndexedBufferBinding* IndexedBindingShadow(GLenum glTarget, Uint index) {
                if (index >= kMaxIndexedBufferBindings) return nullptr; // out of range: never cache
                if (glTarget == GL_UNIFORM_BUFFER) return &g_indexedUBOBindings[index];
                if (glTarget == GL_SHADER_STORAGE_BUFFER) return &g_indexedSSBOBindings[index];
                if (glTarget == GL_TRANSFORM_FEEDBACK_BUFFER) return &g_indexedXFBBindings[index];
                return nullptr;
            }

            // glDeleteBuffers resets the deleted buffer's bindings (indexed and
            // pixel pack/unpack ones included) to 0 in the current context; mirror
            // that in the shadows, or a later buffer recycling the same name with a
            // matching shadow entry would false-skip its rebind. Default
            // IndexedBufferBinding{} == base(0) == the post-delete GL state.
            void ScrubBufferBindingShadowsForId(Uint id) {
                if (id == 0) return;
                for (auto& binding : g_indexedUBOBindings) {
                    if (binding.id == id) binding = {};
                }
                for (auto& binding : g_indexedSSBOBindings) {
                    if (binding.id == id) binding = {};
                }
                for (auto& binding : g_indexedXFBBindings) {
                    if (binding.id == id) binding = {};
                }
                if (g_boundPixelPackBufferKnown && g_boundPixelPackBufferId == id) {
                    g_boundPixelPackBufferId = 0;
                }
                if (g_boundPixelUnpackBufferKnown && g_boundPixelUnpackBufferId == id) {
                    g_boundPixelUnpackBufferId = 0;
                }
            }

            void InvalidateIndexedBufferBindingShadowsForId(Uint id) {
                if (id == 0) return;
                for (auto& binding : g_indexedUBOBindings) {
                    if (binding.id == id) binding.known = false;
                }
                for (auto& binding : g_indexedSSBOBindings) {
                    if (binding.id == id) binding.known = false;
                }
                for (auto& binding : g_indexedXFBBindings) {
                    if (binding.id == id) binding.known = false;
                }
            }
        } // namespace

        // The capture points belong to the bound transform feedback object, so a bind (or a
        // delete, which reverts to the default object) replaces all of them at once with
        // state this shadow has never seen. Distrust rather than scrub: the driver's bindings
        // are whatever the newly bound object holds, which is NOT necessarily base(0), and
        // scrubbing would let a later bind of 0 be false-skipped.
        void InvalidateTransformFeedbackBindingShadows() {
            for (auto& binding : g_indexedXFBBindings) binding.known = false;
        }

        void BindBufferBaseCached(GLenum glTarget, Uint index, Uint id) {
            auto* s = IndexedBindingShadow(glTarget, index);
            if (s && s->known && s->isBase && s->id == id) return;
            g_GLESFuncs.glBindBufferBase(glTarget, index, id);
            if (s) *s = {id, 0, 0, true, true};
        }

        void BindBufferRangeCached(GLenum glTarget, Uint index, Uint id, GLintptr offset, GLsizeiptr size) {
            auto* s = IndexedBindingShadow(glTarget, index);
            if (s && s->known && !s->isBase && s->id == id && s->offset == offset && s->size == size) return;
            g_GLESFuncs.glBindBufferRange(glTarget, index, id, offset, size);
            if (s) *s = {id, offset, size, false, true};
        }

        void InvalidateIndexedBufferBindingCache() {
            for (auto& b : g_indexedUBOBindings) b = {};
            for (auto& b : g_indexedSSBOBindings) b = {};
            for (auto& b : g_indexedXFBBindings) b = {};
        }

        void TrimBufferPool() {
            const std::lock_guard<std::mutex> lock(g_poolMutex);
            if (g_pooledBytes <= kMaxPoolBytes) return;
            // Over budget: evict oldest-retireSerial entries with real glDeleteBuffers.
            while (g_pooledBytes > kMaxPoolBytes) {
                SizeT oldestKey = 0, oldestIdx = 0;
                Uint64 oldestSerial = ~Uint64{0};
                Bool found = false;
                for (auto& kv : g_bufferPool) {
                    for (SizeT i = 0; i < kv.second.size(); ++i) {
                        if (kv.second[i].retireSerial < oldestSerial) {
                            oldestSerial = kv.second[i].retireSerial;
                            oldestKey = kv.first;
                            oldestIdx = i;
                            found = true;
                        }
                    }
                }
                if (!found) break;
                auto& bucket = g_bufferPool[oldestKey];
                PooledBuffer& e = bucket[oldestIdx];
                if (e.contextGeneration == g_bufferContextGeneration && e.id != 0) {
                    ScrubBufferBindingShadowsForId(e.id);
                    g_GLESFuncs.glDeleteBuffers(1, &e.id);
                }
                g_pooledBytes -= e.size;
                bucket[oldestIdx] = bucket.back();
                bucket.pop_back();
            }
        }

        void ClearBufferPool() {
            const std::lock_guard<std::mutex> lock(g_poolMutex);
            // Ids belong to the dying context; drop without glDeleteBuffers (mirrors
            // the g_deferredBufferReleases.clear() discipline).
            g_bufferPool.clear();
            g_pooledBytes = 0;
        }

        // --- Persistent-mapped bump rings (see Managers.h) ------------------------
        namespace {
            // (Re)create the ring store with room for at least minBytes. Any live
            // store is retired (deleted once the GPU finished the last frame that
            // could reference its slots), never deleted in place. Returns false and
            // leaves the current store untouched when minBytes cannot fit under the
            // size cap; a GL failure loses the store and latches creationFailed so
            // later uses stop retrying under this context.
            Bool CreateRingStorage(PersistentRing& ring, SizeT minBytes) {
                SizeT newSize = ring.initialBytes;
                while (newSize < minBytes) newSize *= 2;
                if (newSize > ring.maxBytes) return false;

                if (ring.store.id != 0) {
                    ring.retired.push_back(
                        {ring.store.id, ring.store.contextGeneration, DirectGLES::CurrentFrameSerial() + 1});
                }
                const Uint32 nextGeneration = ring.store.generation + 1;
                ring.store.id = 0;
                ring.store.mappedPtr = nullptr;

                Uint id = 0;
                g_GLESFuncs.glGenBuffers(1, &id);
                if (id != 0) {
                    BindBufferId(TempBufferTarget, id);
                    g_GLESFuncs.glBufferStorageEXT(TempBufferTarget, static_cast<GLsizeiptr>(newSize), nullptr,
                                                   GL_MAP_WRITE_BIT | kMapPersistentBit | kMapCoherentBit);
                    void* ptr = g_GLESFuncs.glMapBufferRange(TempBufferTarget, 0, static_cast<GLsizeiptr>(newSize),
                                                             GL_MAP_WRITE_BIT | kMapPersistentBit | kMapCoherentBit);
                    if (!ptr) {
                        // The dying id is what the array-buffer cache has recorded as
                        // bound; a later buffer recycling the name would false-skip.
                        InvalidateArrayBufferBindingCache();
                        g_GLESFuncs.glDeleteBuffers(1, &id);
                        id = 0;
                    } else {
                        ring.store.mappedPtr = static_cast<Uint8*>(ptr);
                    }
                }
                if (id == 0) {
                    MGLOG_E_ONCE("%s: persistent storage creation failed (%zu bytes); falling back to the "
                                 "client-memory upload path.",
                                 ring.label, newSize);
                    ring.store.creationFailed = true;
                    return false;
                }

                SizeT alignment = ring.fixedAlignment;
                if (alignment == 0) {
                    const GLint capsAlignment = g_GLESCapabilities.UniformBufferOffsetAlignment;
                    alignment = capsAlignment > 0 ? static_cast<SizeT>(capsAlignment) : 256;
                }
                ring.store.id = id;
                ring.store.size = newSize;
                ring.store.head = 0;
                ring.store.tail = 0;
                ring.store.generation = nextGeneration;
                ring.store.alignment = alignment;
                ring.frameMarks.clear();
                MGLOG_D("%s: %zu MiB persistent store ready (id %u, gen %u, align %zu).", ring.label,
                        newSize / (1024u * 1024u), id, nextGeneration, alignment);
                return true;
            }

            // Shared half of the availability gate (the per-ring kill switch sits in
            // the exported wrappers). Reclamation rides the Present fence watermark;
            // without working fences slots would never be provably GPU-idle (same rule
            // as IsPoolable).
            Bool RingAvailable(PersistentRing& ring) {
                if (!g_GLESFuncs.glBufferStorageEXT || !g_GLESFuncs.glMapBufferRange || !g_GLESFuncs.glGenBuffers ||
                    !g_GLESFuncs.glFenceSync || !g_GLESFuncs.glGetSynciv) {
                    return false;
                }
                if (!CanTouchGLNow()) return false;
                if (ring.store.contextGeneration != g_bufferContextGeneration) {
                    ResetRingForNewContext(ring);
                }
                return !ring.store.creationFailed;
            }

            // Division-based rounding fallback: the spec doesn't promise a power-of-two
            // alignment. Slot offsets stay multiples of the alignment because every
            // slot size is, and wrap padding restarts at ring offset 0.
            inline SizeT RingAlignUp(SizeT size, SizeT alignment) {
                if ((alignment & (alignment - 1)) == 0) {
                    return (size + alignment - 1) & ~(alignment - 1);
                }
                return (size + alignment - 1) / alignment * alignment;
            }
            Bool RingAllocateSlow(PersistentRing& ring, SizeT size, SizeT& outOffset);

            // Bump-allocate `size` bytes out of `ring`.
            //
            // Fast path: a live store under the current context with room before both
            // the wrap boundary and the in-flight tail. Touches no GL and probes no
            // frame marks - callers sit behind the ring's availability gate, so the
            // context checks have already run for this draw/upload. `tail` may be stale
            // here (marks are only retired on Present and on the slow path); staleness
            // is conservative - the in-flight span reads too large, the check fails,
            // and the slow path retires marks and re-tries.
            Bool RingAllocate(PersistentRing& ring, SizeT size, SizeT& outOffset) {
                if (size == 0) return false;
                auto& store = ring.store;
                if (store.id != 0 && store.contextGeneration == g_bufferContextGeneration) {
                    const SizeT alignedSize = RingAlignUp(size, store.alignment);
                    // Ring sizes are the ring's initial size (a power of two) doubled
                    // some number of times, so the offset modulo reduces to a mask.
                    static_assert((kUboRingInitialBytes & (kUboRingInitialBytes - 1)) == 0,
                                  "ring offset mask below requires power-of-two ring sizes");
                    static_assert((kUnpackRingInitialBytes & (kUnpackRingInitialBytes - 1)) == 0,
                                  "ring offset mask below requires power-of-two ring sizes");
                    static_assert((kUploadRingInitialBytes & (kUploadRingInitialBytes - 1)) == 0,
                                  "ring offset mask below requires power-of-two ring sizes");
                    const SizeT offset = static_cast<SizeT>(store.head & (store.size - 1));
                    if (offset + alignedSize <= store.size && store.head + alignedSize - store.tail <= store.size) {
                        store.head += alignedSize;
                        outOffset = offset;
                        return true;
                    }
                }
                return RingAllocateSlow(ring, size, outOffset);
            }

            Bool RingAllocateSlow(PersistentRing& ring, SizeT size, SizeT& outOffset) {
                if (!RingAvailable(ring)) return false;
                auto& store = ring.store;
                // Sizing the store first, so the request is rounded with the alignment
                // the live store actually carries rather than the pre-creation default.
                if (store.id == 0 && !CreateRingStorage(ring, RingAlignUp(size, store.alignment))) {
                    return false;
                }
                const SizeT alignedSize = RingAlignUp(size, store.alignment);

                // Advance tail past every frame the GPU provably finished.
                const Uint64 completed = DirectGLES::CompletedFrameSerial();
                SizeT retiredMarks = 0;
                for (const auto& mark : ring.frameMarks) {
                    if (mark.frameSerial > completed) break;
                    if (mark.headAtPresent > store.tail) store.tail = mark.headAtPresent;
                    ++retiredMarks;
                }
                if (retiredMarks > 0) {
                    ring.frameMarks.erase(ring.frameMarks.begin(),
                                          ring.frameMarks.begin() + static_cast<std::ptrdiff_t>(retiredMarks));
                }

                // A slot may not straddle the ring end; pad the cursor to the boundary.
                SizeT offset = static_cast<SizeT>(store.head % store.size);
                if (offset + alignedSize > store.size) {
                    store.head += store.size - offset;
                    offset = 0;
                }

                if (store.head + alignedSize - store.tail > store.size) {
                    // In-flight span would overrun live slots: grow instead of overwrite.
                    if (CreateRingStorage(ring, std::max(store.size * 2, alignedSize))) {
                        offset = 0;
                    } else if (store.creationFailed) {
                        return false; // store lost; callers fall back to their legacy path
                    } else {
                        // At the size cap (>maxBytes in flight). First try to free room
                        // by waiting for the OLDEST in-flight frames to retire - a
                        // bounded wait that ends as soon as enough tail space exists,
                        // instead of draining the entire queue.
                        constexpr Uint64 kFrameWaitNs = 50ull * 1000 * 1000; // 50ms per frame
                        while (!ring.frameMarks.empty() &&
                               store.head + alignedSize - store.tail > store.size) {
                            const auto& oldest = ring.frameMarks.front();
                            if (!DirectGLES::WaitForFrameSerialCompleted(oldest.frameSerial, kFrameWaitNs)) {
                                break;
                            }
                            if (oldest.headAtPresent > store.tail) store.tail = oldest.headAtPresent;
                            ring.frameMarks.erase(ring.frameMarks.begin());
                        }
                        if (store.head + alignedSize - store.tail <= store.size) {
                            offset = static_cast<SizeT>(store.head % store.size);
                            if (offset + alignedSize > store.size) {
                                store.head += store.size - offset;
                                offset = 0;
                            }
                            store.head += alignedSize;
                            outOffset = offset;
                            return true;
                        }
                        // No usable fence covers the oldest frames: drain once rather than
                        // corrupt live slots.
                        if (g_GLESFuncs.glFinish) g_GLESFuncs.glFinish();
                        store.tail = store.head;
                        ring.frameMarks.clear();
                        // Same-frame slots written before the drain may now be recycled by
                        // the very next allocations; a generation bump keeps later draws
                        // from rebinding those cached offsets.
                        ++store.generation;
                        offset = static_cast<SizeT>(store.head % store.size);
                        if (offset + alignedSize > store.size) {
                            store.head += store.size - offset;
                            offset = 0;
                        }
                    }
                }

                store.head += alignedSize;
                outOffset = offset;
                return true;
            }

            // Present()-time upkeep shared by both rings.
            void RingOnPresent(PersistentRing& ring) {
                if (!CanTouchGLNow()) return;

                // Delete grown-away stores the GPU is provably done with.
                const Uint64 completed = DirectGLES::CompletedFrameSerial();
                for (SizeT i = ring.retired.size(); i-- > 0;) {
                    RetiredRingStore& entry = ring.retired[i];
                    const Bool staleContext = entry.contextGeneration != g_bufferContextGeneration;
                    if (!staleContext && entry.retireSerial > completed) continue;
                    if (!staleContext && entry.id != 0) {
                        ScrubBufferBindingShadowsForId(entry.id);
                        g_GLESFuncs.glDeleteBuffers(1, &entry.id);
                    }
                    ring.retired[i] = ring.retired.back();
                    ring.retired.pop_back();
                }

                auto& store = ring.store;
                if (store.id == 0 || store.contextGeneration != g_bufferContextGeneration) return;
                // Retire completed marks here too — RingAllocate is the main consumer,
                // but frames that used the ring for nothing would otherwise let the list
                // grow one entry per Present, unboundedly.
                SizeT retiredMarks = 0;
                for (const auto& mark : ring.frameMarks) {
                    if (mark.frameSerial > completed) break;
                    if (mark.headAtPresent > store.tail) store.tail = mark.headAtPresent;
                    ++retiredMarks;
                }
                if (retiredMarks > 0) {
                    ring.frameMarks.erase(ring.frameMarks.begin(),
                                          ring.frameMarks.begin() + static_cast<std::ptrdiff_t>(retiredMarks));
                }
                // Record this frame's high-water mark (Present just fenced the serial now
                // reported by CurrentFrameSerial()). A fence-less Present repeats the
                // serial; fold into the existing mark.
                const Uint64 serial = DirectGLES::CurrentFrameSerial();
                if (!ring.frameMarks.empty() && ring.frameMarks.back().frameSerial == serial) {
                    ring.frameMarks.back().headAtPresent = store.head;
                } else {
                    ring.frameMarks.push_back({serial, store.head});
                }
            }
        } // namespace

        Bool UboRingAvailable() {
            if (MG_Config::Features.EsprytDisableUboRing) return false;
            return RingAvailable(g_uboRing);
        }

        Bool UboRingAllocate(SizeT size, SizeT& outOffset) { return RingAllocate(g_uboRing, size, outOffset); }

        void* UboRingMappedPtr() { return g_uboRing.store.mappedPtr; }
        Uint UboRingBufferId() { return g_uboRing.store.id; }
        Uint32 UboRingGeneration() { return g_uboRing.store.generation; }

        void UboRingOnPresent() { RingOnPresent(g_uboRing); }

        Bool UnpackRingAvailable() {
            if (MG_Config::Features.EsprytDisableUnpackRing) return false;
            return RingAvailable(g_unpackRing);
        }

        Bool UnpackRingAllocate(SizeT size, SizeT& outOffset) {
            // A request the ring could never satisfy even empty would otherwise walk
            // the whole grow/drain ladder before failing.
            if (size > kUnpackRingMaxBytes) return false;
            return RingAllocate(g_unpackRing, size, outOffset);
        }

        void* UnpackRingMappedPtr() { return g_unpackRing.store.mappedPtr; }
        Uint UnpackRingBufferId() { return g_unpackRing.store.id; }
        SizeT UnpackRingMaxBytes() { return kUnpackRingMaxBytes; }

        void UnpackRingOnPresent() { RingOnPresent(g_unpackRing); }

        void UploadRingOnPresent() { RingOnPresent(g_uploadRing); }
    } // namespace BufferImpl

    namespace VertexArrayImpl {
        namespace {
            SizeT GetDataTypeSize(DataType type) {
                switch (type) {
                case DataType::Int8:
                case DataType::Uint8:
                    return 1;
                case DataType::Int16:
                case DataType::Uint16:
                case DataType::Float16:
                    return 2;
                case DataType::Int32:
                case DataType::Uint32:
                case DataType::Float32:
                case DataType::Fixed32:
                    return 4;
                case DataType::Float64:
                    return 8;
                default:
                    return 0;
                }
            }

            // Tightly-packed byte size of one vertex element: 4 for the 2_10_10_10 types and GL_BGRA
            // (one 32-bit word / 4 bytes), componentSize * size otherwise. 0 for unknown types.
            SizeT GetAttributeByteSize(DataType type, int size, Bool isBgra) {
                if (type == DataType::Int2101010Rev || type == DataType::Uint2101010Rev || isBgra) {
                    return 4;
                }
                const SizeT componentSize = GetDataTypeSize(type);
                return componentSize == 0 ? 0 : componentSize * static_cast<SizeT>(size);
            }

            // Deinterleaves elementCount elements of componentCount doubles into a tightly packed
            // float32 stream - the fetch half of the fp64 demotion the shader side already does
            // unconditionally (DemoteFloat64Pass). GL byte strides and offsets are arbitrary, so
            // no component carries an 8-byte alignment guarantee and each is copied out before it
            // is narrowed.
            void NarrowDoubleStreamToFloat32(const Uint8* sourceBase, SizeT sourceStride, SizeT componentCount,
                                             SizeT elementCount, Vector<Float>& outData) {
                outData.resize(elementCount * componentCount);
                for (SizeT element = 0; element < elementCount; ++element) {
                    const Uint8* sourceElement = sourceBase + element * sourceStride;
                    Float* destinationElement = outData.data() + element * componentCount;
                    for (SizeT component = 0; component < componentCount; ++component) {
                        Double value = 0.0;
                        Memcpy(&value, sourceElement + component * sizeof(Double), sizeof(Double));
                        destinationElement[component] = static_cast<Float>(value);
                    }
                }
            }
        } // namespace

        BackendVertexArrayObject::BackendVertexArrayObject() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            m_clientAttributeBufferIds.fill(0);
            m_convertedAttributeBufferIds.fill(0);
            g_GLESFuncs.glGenVertexArrays(1, &m_backendVAOId);
            if (m_backendVAOId == 0) {
                MGLOG_E_ONCE("Failed to generate vertex array object.");
                MGLOG_E_ONCE("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            } else {
                MGLOG_D("Generated vertex array object with ID: %u.", m_backendVAOId);
            }
        }

        BackendVertexArrayObject::~BackendVertexArrayObject() {
            if (InProcessTeardown()) {
                return; // see InProcessTeardown(): the driver may be unloaded already
            }
            if (m_backendVAOId != 0) {
                NoteVAOIdDeleted(m_backendVAOId);
                g_GLESFuncs.glDeleteVertexArrays(1, &m_backendVAOId);
                m_backendVAOId = 0;
            }
            for (auto& bufferId : m_clientAttributeBufferIds) {
                if (bufferId != 0) {
                    BufferImpl::NoteBufferIdDeleted(bufferId);
                    g_GLESFuncs.glDeleteBuffers(1, &bufferId);
                    bufferId = 0;
                }
            }
            for (auto& bufferId : m_convertedAttributeBufferIds) {
                if (bufferId != 0) {
                    BufferImpl::NoteBufferIdDeleted(bufferId);
                    g_GLESFuncs.glDeleteBuffers(1, &bufferId);
                    bufferId = 0;
                }
            }
        }

        namespace {
            Uint g_boundBackendVAOId = 0;
            Bool g_boundBackendVAOKnown = false;
        } // namespace

        void BindBackendVAOId(Uint id) {
            if (g_boundBackendVAOKnown && g_boundBackendVAOId == id) {
                return;
            }
            g_GLESFuncs.glBindVertexArray(id);
            g_boundBackendVAOId = id;
            g_boundBackendVAOKnown = true;
        }

        void InvalidateVAOBindingCache() {
            g_boundBackendVAOKnown = false;
        }

        void NoteVAOIdDeleted(Uint id) {
            if (g_boundBackendVAOKnown && g_boundBackendVAOId == id) {
                g_boundBackendVAOId = 0; // glDeleteVertexArrays reverts a bound VAO to 0
            }
        }

        void BackendVertexArrayObject::Bind() const {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            BindBackendVAOId(m_backendVAOId);
        }

        inline Bool BindAttributeBuffer(const MG_State::GLState::VertexAttribute& attrib) {
            const auto& bufferObject = attrib.Buffer;
            if (!bufferObject) {
                MGLOG_W_ONCE("Attribute has no bound buffer, skipping.");
                return false;
            }

            auto* backendResource = BufferImpl::EnsureBufferResource(bufferObject);
            if (!backendResource || backendResource->id == 0) {
                MGLOG_E_ONCE("No backend buffer found for attribute's buffer, cannot bind attribute.");
                return false;
            }

            BufferImpl::BindBufferId(GL_ARRAY_BUFFER, backendResource->id);
            return true;
        }

        // ES 3.1 core. Queried through the loader rather than the version, because the whole
        // point of using it is to express something the pointer API cannot, and falling back
        // silently on a driver that lacks it is better than crashing on a null entry point.
        inline Bool HasVertexBindingApi() {
            return g_GLESFuncs.glBindVertexBuffer != nullptr && g_GLESFuncs.glVertexAttribFormat != nullptr &&
                   g_GLESFuncs.glVertexAttribIFormat != nullptr && g_GLESFuncs.glVertexAttribBinding != nullptr &&
                   g_GLESFuncs.glVertexBindingDivisor != nullptr;
        }

        // Draw state, not VAO state: set by the baseInstance draw entry points around
        // PrepareForDraw and back to zero as soon as the draw is issued.
        Uint32 g_pendingFetchBaseInstance = 0;

        void SetPendingFetchBaseInstance(Uint32 baseInstance) {
            g_pendingFetchBaseInstance = baseInstance;
        }

        Uint32 GetPendingFetchBaseInstance() {
            return g_pendingFetchBaseInstance;
        }

        // The "+ baseInstance" of GL's instanced-array element index, expressed as a byte shift
        // of the array's own offset. Only divisor'd arrays step per instance, so only they move.
        //
        // baseInstance is added to the ELEMENT index, not to instance/divisor - the divisor
        // therefore does not appear here, and the shift is a whole number of strides.
        //
        // A resolved stride of zero is the binding model's "never advance" (see
        // VertexAttribute::Stride), so such an array reads the same element for every instance
        // and a baseInstance cannot move it. The arithmetic already yields zero for that case.
        inline SizeT BaseInstanceByteShift(const MG_State::GLState::VertexAttribute& attrib, Uint32 baseInstance) {
            if (baseInstance == 0 || attrib.Divisor == 0) {
                return 0;
            }
            return static_cast<SizeT>(baseInstance) * static_cast<SizeT>(attrib.Stride);
        }

        // Declares one attribute through the ES binding-point API, the only spelling that can
        // carry a stride of zero. Returns false when the attribute has no usable buffer, in
        // which case nothing was emitted.
        inline Bool SyncZeroStrideAttribute(Uint attribIndex, const MG_State::GLState::VertexAttribute& attrib) {
            const auto& bufferObject = attrib.Buffer;
            if (!bufferObject) {
                MGLOG_W_ONCE("Zero-stride attribute %u has no bound buffer, skipping.", attribIndex);
                return false;
            }
            auto* backendResource = BufferImpl::EnsureBufferResource(bufferObject);
            if (!backendResource || backendResource->id == 0) {
                MGLOG_E_ONCE("No backend buffer for zero-stride attribute %u, cannot bind it.", attribIndex);
                return false;
            }

            if (!attrib.IsInteger) {
                const GLint glSize = attrib.IsBgra ? static_cast<GLint>(GL_BGRA) : attrib.Size;
                g_GLESFuncs.glVertexAttribFormat(attribIndex, glSize,
                                                 MG_Util::ConvertDataTypeToGLEnum(attrib.Type),
                                                 attrib.Normalized ? GL_TRUE : GL_FALSE, 0);
            } else {
                g_GLESFuncs.glVertexAttribIFormat(attribIndex, attrib.Size,
                                                  MG_Util::ConvertDataTypeToGLEnum(attrib.Type), 0);
            }
            g_GLESFuncs.glVertexAttribBinding(attribIndex, attribIndex);
            // The resolved offset goes on the binding point, not into a relative offset: the
            // relative offset is capped by GL_MAX_VERTEX_ATTRIB_RELATIVE_OFFSET (2047 at
            // minimum) while a buffer offset is not, so anything else would break on a large
            // one. BindBufferId is bypassed deliberately - glBindVertexBuffer binds into the
            // VAO's binding point, not the GL_ARRAY_BUFFER target that cache tracks.
            g_GLESFuncs.glBindVertexBuffer(attribIndex, backendResource->id,
                                           static_cast<GLintptr>(attrib.Offset), 0);
            return true;
        }

        void BackendVertexArrayObject::SyncToBackend(
            const SharedPtr<MG_State::GLState::VertexArrayObject>& stateVAOObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!stateVAOObject) {
                MGLOG_E_ONCE("State VAO object is null, cannot sync to backend.");
                return;
            }

            MGLOG_D("Syncing VAO with backend ID %u to backend for state ID %u", m_backendVAOId,
                    stateVAOObject->GetExternalIndex());

            // One compare instead of MAX_VERTEX_ATTRIBS x 3 per draw: the config version
            // aggregates every per-attribute version bump (see the member comment), and the
            // index-buffer slot version covers the only other thing this function reads. When
            // both are clean there is nothing to emit, and the VAO is not even bound here -
            // PrepareForDraw's BindCurrentVAO establishes the draw binding regardless.
            const Uint32 currentConfigVersion = stateVAOObject->GetConfigVersion();
            const Uint16 currentIndexBufferVersion = stateVAOObject->GetIndexBufferBindingSlot().GetVersion();
            // A live buffer's driver id was re-minted since this twin's last emit
            // (persistent-map adoption / immutable-store retire): every baked binding may
            // hold the dead id while every frontend version still matches, so force a
            // full re-emit. Read once; each buffer re-mints at most once per walk (its
            // first EnsureBufferResource this draw), before its id is baked, so stamping
            // the entry value at the end is exact - and a stale stamp only costs one
            // extra full emit.
            const Uint64 currentBufferIdGeneration = BufferImpl::g_bufferBackendIdGeneration;
            const Bool bufferIdsRemitted = m_syncedBufferIdGeneration != currentBufferIdGeneration;
            const Bool attributesDirty =
                bufferIdsRemitted || !m_hasSyncedConfigVersion || m_syncedConfigVersion != currentConfigVersion;
            // Identity joins the version compare: the slot version is a wrapping Uint16,
            // so a wrapped-back count with a different buffer bound must still read dirty.
            const MG_State::GLState::BufferObject* currentIndexBufferObject =
                stateVAOObject->GetIndexBufferBindingSlot().GetBoundObject().get();
            const Bool indexBufferDirty = bufferIdsRemitted ||
                                          currentIndexBufferVersion != m_syncedIndexBufferVersion ||
                                          currentIndexBufferObject != m_syncedIndexBufferObject;

            // The baseInstance shift lives in the attribute offsets the driver already holds, so
            // a change of baseInstance has to re-emit the divisor'd arrays even when the frontend
            // config version says nothing moved - and equally has to un-shift them for the next
            // draw that carries no baseInstance. Resting state is 0 on both sides, so a program
            // that never calls a *BaseInstance entry point never pays for this compare.
            const Uint32 fetchBaseInstance = g_pendingFetchBaseInstance;
            const Bool baseInstanceDirty = m_syncedFetchBaseInstance != fetchBaseInstance;
            // A narrowed GL_DOUBLE stream is derived from the source buffer's CONTENT, and no
            // VAO version moves when an app writes into a buffer, so the version gate cannot
            // prove such a stream is still current. Re-walk while one is live; the walk itself
            // re-checks the buffer's change serial and only re-converts on a real move.
            const Bool emitAttributes = attributesDirty || baseInstanceDirty || m_hasConvertedFloat64Attribute;
            if (!emitAttributes && !indexBufferDirty) {
                return;
            }
            m_hasConvertedFloat64Attribute = false;

            Bind();

            const auto& allAttributeVersions = stateVAOObject->GetAllAttributeVersions();
            const auto& allAttributes = stateVAOObject->GetAllAttributes();
            for (Uint attribIndex = 0; attribIndex < allAttributes.size() && emitAttributes; ++attribIndex) {
                const auto& attrib = allAttributes[attribIndex];
                // Only the divisor'd arrays carry the shift, and only an enabled one is worth
                // re-emitting - a disabled array has no pointer the draw could fetch through,
                // and may well have no buffer to bind either.
                const Bool needsSyncBaseInstance = baseInstanceDirty && attrib.Enabled && attrib.Divisor != 0;
                Bool needsSyncSwitch = allAttributeVersions[attribIndex].SwitchVersion !=
                                       m_syncedAttributeVersions[attribIndex].SwitchVersion;
                if (needsSyncSwitch) {
                    if (attrib.Enabled) {
                        g_GLESFuncs.glEnableVertexAttribArray(attribIndex);
                    } else {
                        g_GLESFuncs.glDisableVertexAttribArray(attribIndex);
                    }
                }

                Bool needsSyncFormat = bufferIdsRemitted || allAttributeVersions[attribIndex].FormatVersion !=
                                                               m_syncedAttributeVersions[attribIndex].FormatVersion;
                Bool needsSyncBuffer = bufferIdsRemitted || allAttributeVersions[attribIndex].BufferVersion !=
                                                               m_syncedAttributeVersions[attribIndex].BufferVersion;
                // This is where a 64-bit array is narrowed. glVertexAttribLFormat is a legal call in
                // a GL 4.3 context and the frontend RECORDS its format (the state queries have to
                // answer), so IsLong does arrive here - what this backend cannot do is feed it at
                // full precision: ES has no GL_DOUBLE vertex FORMAT and ESSL has no fp64 type, and
                // passing GL_DOUBLE to glVertexAttribPointer would only raise GL_INVALID_ENUM on the
                // real driver. But the FORMAT is the only 64-bit thing here: the source bytes are
                // ordinary IEEE-754 doubles, and MobileGL already narrows every fp64 value in every
                // shader (DemoteFloat64Pass) and every glUniform*d the same way, so the array is
                // deinterleaved into a float32 stream and fetched as GL_FLOAT rather than dropped.
                // glVertexAttribFormat(GL_DOUBLE) asks for exactly that conversion anyway; the L form
                // asks for more precision than any backend here can give, and gets the same stream.
                //
                // The conversion is deliberately NOT behind the version gate below: it is derived
                // from buffer CONTENT, which no VAO version covers. Its own memo (keyed on the
                // source buffer's change serial) is what keeps the repeat cost down.
                //
                // When no stream can be built the array is DISABLED rather than left alone. That
                // matters: becoming long bumps FormatVersion, not SwitchVersion, so the
                // enable/disable block above will not run again and an already-enabled array would
                // stay enabled with no pointer and no ARRAY_BUFFER binding - which did not merely
                // raise INVALID_ENUM but had the Adreno driver dereference null inside the next draw
                // and take the process with it (SIGSEGV in libGLESv2_adreno,
                // KHR-GL43.vertex_attrib_binding.basic-input-case4).
                if (attrib.IsLong || attrib.Type == DataType::Float64) {
                    if (attrib.Enabled && attrib.Type == DataType::Float64 &&
                        SyncFloat64AttributeAsFloat32(attribIndex, attrib, fetchBaseInstance)) {
                        m_hasConvertedFloat64Attribute = true;
                        // Explicit, not redundant: an earlier walk that could not build the stream
                        // disabled this array, and becoming feedable again bumps no SwitchVersion,
                        // so the enable/disable block above would never turn it back on.
                        g_GLESFuncs.glEnableVertexAttribArray(attribIndex);
                        g_GLESFuncs.glVertexAttribDivisor(attribIndex, attrib.Divisor);
                        continue;
                    }
                    if (attrib.Enabled) {
                        MGLOG_W_ONCE("DirectGLES: vertex attribute %u is a 64-bit (GL_DOUBLE) array whose source "
                                "stream could not be narrowed to float32 - disabling the array",
                                attribIndex);
                    }
                    g_GLESFuncs.glDisableVertexAttribArray(attribIndex);
                    continue;
                }

                if (!needsSyncFormat && !needsSyncBuffer && !needsSyncBaseInstance) continue;

                // A resolved stride of zero is the binding model's "never advance" (see
                // VertexAttribute::Stride) and glVertexAttribPointer cannot say it - a zero
                // stride argument there means "tightly packed" instead, i.e. exactly the
                // opposite. ES 3.1's binding-point API can, so a zero-stride attribute takes
                // that spelling: its own binding point (index == attribute index, the default
                // mapping) carrying the buffer, the whole resolved offset and stride 0, with
                // the format at relative offset 0. Everything the pointer call would have set
                // for this attribute is set here too, so the two spellings stay interchangeable
                // from one sync to the next.
                if (attrib.Stride == 0 && HasVertexBindingApi()) {
                    if (!SyncZeroStrideAttribute(attribIndex, attrib)) {
                        continue;
                    }
                    // No BaseInstanceByteShift here on purpose: a zero stride never advances, so
                    // the shift is zero by construction and adding it would only obscure that.
                    if (needsSyncFormat) {
                        g_GLESFuncs.glVertexBindingDivisor(attribIndex, attrib.Divisor);
                    }
                    continue;
                }

                if (!BindAttributeBuffer(attrib)) {
                    continue;
                }

                // GL_BGRA as a vertex SIZE is desktop-only; ES has no equivalent and rejects
                // it. That rejection is not benign: it leaves the array ENABLED with no
                // pointer, and the Adreno driver then dereferences null inside the next draw
                // and kills the process rather than reporting an error (SIGSEGV in
                // libGLESv2_adreno, KHR-GL43.vertex_attrib_binding.basic-input-case5). So the
                // refusal has to be observed and the array disabled.
                //
                // Deliberately ONLY this format. Everything else MobileGL can reach here is ES
                // core - the packed 2_10_10_10 pair included, whose size the frontend has
                // already pinned to the 4 that ES requires - so nothing else can be refused,
                // and the per-draw sync must not grow a glGetError round trip (a driver
                // pipeline stall) for the formats real applications actually use. BGRA is also
                // still ATTEMPTED rather than refused up front: some ES drivers do accept it,
                // and the ones that do should keep working.
                const Bool formatMayBeRefused = attrib.IsBgra;
                if (formatMayBeRefused) {
                    while (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                    } // start from a clean slate so the check below is about THIS call
                }

                const SizeT fetchOffset = attrib.Offset + BaseInstanceByteShift(attrib, fetchBaseInstance);

                if (!attrib.IsInteger) {
                    // GL_BGRA is passed to the driver as the size argument (the driver reorders BGRA).
                    const GLint glSize = attrib.IsBgra ? static_cast<GLint>(GL_BGRA) : attrib.Size;
                    g_GLESFuncs.glVertexAttribPointer(
                        attribIndex, glSize, MG_Util::ConvertDataTypeToGLEnum(attrib.Type),
                        attrib.Normalized ? GL_TRUE : GL_FALSE, attrib.Stride, (const void*)fetchOffset);
                } else {
                    g_GLESFuncs.glVertexAttribIPointer(attribIndex, attrib.Size,
                                                       MG_Util::ConvertDataTypeToGLEnum(attrib.Type), attrib.Stride,
                                                       (const void*)fetchOffset);
                }

                if (formatMayBeRefused && g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                    MGLOG_W_ONCE("DirectGLES: the driver refused the vertex format of attribute %u "
                            "(size=%d bgra=%d type=%s) - disabling the array so the draw cannot "
                            "fetch through a pointer the driver never accepted",
                            attribIndex, attrib.Size, attrib.IsBgra ? 1 : 0,
                            MG_Util::ConvertGLEnumToString(MG_Util::ConvertDataTypeToGLEnum(attrib.Type)).c_str());
                    g_GLESFuncs.glDisableVertexAttribArray(attribIndex);
                    continue;
                }

                if (needsSyncFormat) {
                    g_GLESFuncs.glVertexAttribDivisor(attribIndex, attrib.Divisor);
                }
            }

            if (indexBufferDirty) {
                const auto& indexBufferBinding = stateVAOObject->GetIndexBufferBindingSlot().GetBoundObject();
                Bool indexBufferSynced = false;
                if (indexBufferBinding) {
                    auto* backendResource = BufferImpl::EnsureBufferResource(indexBufferBinding);
                    if (backendResource && backendResource->id != 0) {
                        BufferImpl::BindBufferId(GL_ELEMENT_ARRAY_BUFFER, backendResource->id);
                        indexBufferSynced = true;
                    } else {
                        MGLOG_W_ONCE("No backend buffer found for index buffer binding, cannot bind index buffer.");
                    }
                } else {
                    g_GLESFuncs.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
                    indexBufferSynced = true;
                }

                if (indexBufferSynced) {
                    m_syncedIndexBufferVersion = currentIndexBufferVersion;
                    m_syncedIndexBufferObject = currentIndexBufferObject;
                }
            }

            if (attributesDirty) {
                m_syncedAttributeVersions = allAttributeVersions;
                m_syncedConfigVersion = currentConfigVersion;
                m_hasSyncedConfigVersion = true;
            }
            if (emitAttributes) {
                m_syncedFetchBaseInstance = fetchBaseInstance;
            }
            m_syncedBufferIdGeneration = currentBufferIdGeneration;
        }

        void BackendVertexArrayObject::SyncClientSideAttributesForDrawArrays(
            const SharedPtr<MG_State::GLState::VertexArrayObject>& stateVAOObject, GLint first, GLsizei count) {
            if (!stateVAOObject || count <= 0 || first < 0) {
                return;
            }

            Bind();

            const auto& allAttributes = stateVAOObject->GetAllAttributes();
            for (Uint attribIndex = 0; attribIndex < allAttributes.size(); ++attribIndex) {
                const auto& attrib = allAttributes[attribIndex];
                if (!attrib.Enabled || attrib.Buffer) {
                    continue;
                }

                // Same narrowing as SyncToBackend (the long note lives there), with the draw's own
                // fetch range standing in for the buffer extent a client array does not have. The
                // upload starts at element 0 so `first` keeps indexing the stream, exactly as the
                // unconverted upload below does. The test is on the storage rather than on IsLong
                // because glVertexAttribFormat(GL_DOUBLE) is 64-bit data without being long.
                if (attrib.IsLong || attrib.Type == DataType::Float64) {
                    const auto* sourceBase = reinterpret_cast<const Uint8*>(attrib.Offset);
                    if (attrib.Type != DataType::Float64 || sourceBase == nullptr || attrib.Size < 1 ||
                        attrib.Size > 4) {
                        g_GLESFuncs.glDisableVertexAttribArray(attribIndex);
                        continue;
                    }

                    auto& bufferId = m_clientAttributeBufferIds[attribIndex];
                    if (bufferId == 0) {
                        g_GLESFuncs.glGenBuffers(1, &bufferId);
                        if (bufferId == 0) {
                            MGLOG_E_ONCE("Failed to create client-side vertex attribute upload buffer.");
                            g_GLESFuncs.glDisableVertexAttribArray(attribIndex);
                            continue;
                        }
                    }

                    const SizeT componentCount = static_cast<SizeT>(attrib.Size);
                    const SizeT sourceElementSize = componentCount * sizeof(Double);
                    // A client array only ever reaches here through a pointer call, whose stride 0
                    // the frontend already resolved to the element size, so this is a guard rather
                    // than a case (VertexAttribute::Stride).
                    const SizeT sourceStride =
                        attrib.Stride > 0 ? static_cast<SizeT>(attrib.Stride) : sourceElementSize;
                    const SizeT elementCount = static_cast<SizeT>(first) + static_cast<SizeT>(count);
                    Vector<Float> converted;
                    NarrowDoubleStreamToFloat32(sourceBase, sourceStride, componentCount, elementCount, converted);

                    BufferImpl::BindBufferId(GL_ARRAY_BUFFER, bufferId);
                    g_GLESFuncs.glBufferData(GL_ARRAY_BUFFER,
                                             static_cast<GLsizeiptr>(converted.size() * sizeof(Float)),
                                             converted.data(), GL_STREAM_DRAW);
                    if (MG_Util::PipeStats::Enabled()) {
                        MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageVertexClient,
                                                     static_cast<Uint64>(converted.size() * sizeof(Float)));
                    }
                    // GL ignores `normalized` for floating-point array types, so it is not
                    // forwarded here either.
                    g_GLESFuncs.glVertexAttribPointer(attribIndex, attrib.Size, GL_FLOAT, GL_FALSE,
                                                      static_cast<GLsizei>(componentCount * sizeof(Float)),
                                                      nullptr);
                    g_GLESFuncs.glEnableVertexAttribArray(attribIndex);
                    continue;
                }

                const auto* clientData = reinterpret_cast<const Uint8*>(attrib.Offset);
                const SizeT elementSize = GetAttributeByteSize(attrib.Type, attrib.Size, attrib.IsBgra);
                if (!clientData || elementSize == 0 || attrib.Size <= 0) {
                    continue;
                }

                const SizeT stride = attrib.Stride > 0 ? static_cast<SizeT>(attrib.Stride) : elementSize;
                const SizeT uploadSize = static_cast<SizeT>(first + count - 1) * stride + elementSize;

                auto& bufferId = m_clientAttributeBufferIds[attribIndex];
                if (bufferId == 0) {
                    g_GLESFuncs.glGenBuffers(1, &bufferId);
                    if (bufferId == 0) {
                        MGLOG_E_ONCE("Failed to create client-side vertex attribute upload buffer.");
                        continue;
                    }
                }

                BufferImpl::BindBufferId(GL_ARRAY_BUFFER, bufferId);
                g_GLESFuncs.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(uploadSize), clientData,
                                         GL_STREAM_DRAW);
                if (MG_Util::PipeStats::Enabled()) {
                    MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageVertexClient,
                                                 static_cast<Uint64>(uploadSize));
                }

                if (!attrib.IsInteger) {
                    const GLint glSize = attrib.IsBgra ? static_cast<GLint>(GL_BGRA) : attrib.Size;
                    g_GLESFuncs.glVertexAttribPointer(
                        attribIndex, glSize, MG_Util::ConvertDataTypeToGLEnum(attrib.Type),
                        attrib.Normalized ? GL_TRUE : GL_FALSE, static_cast<GLsizei>(stride), nullptr);
                } else {
                    g_GLESFuncs.glVertexAttribIPointer(attribIndex, attrib.Size,
                                                       MG_Util::ConvertDataTypeToGLEnum(attrib.Type),
                                                       static_cast<GLsizei>(stride), nullptr);
                }
            }

        }

        Bool BackendVertexArrayObject::SyncFloat64AttributeAsFloat32(
            Uint attribIndex, const MG_State::GLState::VertexAttribute& attrib, Uint32 fetchBaseInstance) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (attribIndex >= m_convertedAttributeBufferIds.size() || attrib.Size < 1 || attrib.Size > 4) {
                return false;
            }
            const auto& bufferObject = attrib.Buffer;
            if (!bufferObject) {
                // A client-memory 64-bit array is narrowed on the draw path instead, which is the
                // only place its fetch range is known (SyncClientSideAttributesForDrawArrays).
                return false;
            }

            // The frontend shadow is what the conversion reads, so a shader write that has not
            // been pulled back yet has to land first. A no-op unless one is outstanding.
            bufferObject->SyncGpuWrites();
            const Uint8* const sourceBase = bufferObject->MappedData();
            const SizeT sourceSize = bufferObject->GetSize();
            if (sourceBase == nullptr || attrib.Offset >= sourceSize) {
                return false;
            }

            const SizeT componentCount = static_cast<SizeT>(attrib.Size);
            const SizeT sourceElementSize = componentCount * sizeof(Double);
            const SizeT available = sourceSize - attrib.Offset;
            if (available < sourceElementSize) {
                return false;
            }

            // A resolved stride of zero is the binding model's "never advance" (see
            // VertexAttribute::Stride): exactly one element exists and every vertex reads it, so
            // exactly one is converted. Otherwise the array's extent is the source buffer's own -
            // SyncToBackend has no draw range, and a whole-array conversion is affordable because
            // it is memoised on the buffer's change serial and 64-bit arrays are vanishingly rare.
            const Bool neverAdvances = attrib.Stride <= 0;
            const SizeT sourceStride = neverAdvances ? sourceElementSize : static_cast<SizeT>(attrib.Stride);
            const SizeT elementCount = neverAdvances ? 1 : ((available - sourceElementSize) / sourceStride) + 1;

            // baseInstance shifts the ELEMENT index of a divisor'd array, and one element of the
            // converted stream is componentCount floats. A zero stride never advances, so no
            // shift can move it. A shift past the array's own extent has no source data at all.
            const SizeT firstElement = (fetchBaseInstance != 0 && attrib.Divisor != 0 && !neverAdvances)
                                           ? static_cast<SizeT>(fetchBaseInstance)
                                           : 0;
            if (firstElement >= elementCount) {
                return false;
            }

            auto& stream = m_convertedAttributeStreams[attribIndex];
            Uint& convertedBufferId = m_convertedAttributeBufferIds[attribIndex];
            const Uint64 sourceLifetimeId = bufferObject->GetLifetimeId();
            const Uint64 sourceChangeSerial = bufferObject->GetChangeSerial();
            // A persistent map is written through the pointer, with no API call to bump the change
            // serial (see BufferObject::SyncPersistentMappedRange), so its serial cannot prove the
            // converted copy is still current and the memo is never trusted for one.
            const Bool memoHit =
                stream.valid && convertedBufferId != 0 && !bufferObject->IsBackendPersistentMapped() &&
                stream.sourceLifetimeId == sourceLifetimeId && stream.sourceChangeSerial == sourceChangeSerial &&
                stream.sourceOffset == attrib.Offset && stream.sourceStride == sourceStride &&
                stream.componentCount == componentCount && stream.elementCount == elementCount;
            if (!memoHit) {
                if (convertedBufferId == 0) {
                    g_GLESFuncs.glGenBuffers(1, &convertedBufferId);
                    if (convertedBufferId == 0) {
                        MGLOG_E_ONCE("Failed to create the float32 scratch buffer for the 64-bit vertex array at "
                                     "attribute %u.",
                                     attribIndex);
                        return false;
                    }
                }
                Vector<Float> converted;
                NarrowDoubleStreamToFloat32(sourceBase + attrib.Offset, sourceStride, componentCount, elementCount,
                                            converted);
                BufferImpl::BindBufferId(GL_ARRAY_BUFFER, convertedBufferId);
                g_GLESFuncs.glBufferData(GL_ARRAY_BUFFER,
                                         static_cast<GLsizeiptr>(converted.size() * sizeof(Float)),
                                         converted.data(), GL_STREAM_DRAW);
                if (MG_Util::PipeStats::Enabled()) {
                    // The VBO-backed half of the 64-bit narrowing. Same population as the
                    // client-array half above: a stream the backend synthesises per draw.
                    MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageVertexClient,
                                                 static_cast<Uint64>(converted.size() * sizeof(Float)));
                }
                stream.valid = true;
                stream.sourceLifetimeId = sourceLifetimeId;
                stream.sourceChangeSerial = sourceChangeSerial;
                stream.sourceOffset = attrib.Offset;
                stream.sourceStride = sourceStride;
                stream.componentCount = componentCount;
                stream.elementCount = elementCount;
                MGLOG_D("DirectGLES: narrowed the 64-bit vertex array at attribute %u to %zu float32 element(s).",
                        attribIndex, elementCount);
            }

            const SizeT convertedElementSize = componentCount * sizeof(Float);
            if (neverAdvances) {
                // Only the binding-point API can say "stride 0": glVertexAttribPointer's zero
                // means "tightly packed" instead, i.e. the opposite, and would walk the driver
                // straight off the end of the single converted element.
                if (!HasVertexBindingApi()) {
                    return false;
                }
                g_GLESFuncs.glVertexAttribFormat(attribIndex, attrib.Size, GL_FLOAT, GL_FALSE, 0);
                g_GLESFuncs.glVertexAttribBinding(attribIndex, attribIndex);
                g_GLESFuncs.glBindVertexBuffer(attribIndex, convertedBufferId, 0, 0);
                return true;
            }

            BufferImpl::BindBufferId(GL_ARRAY_BUFFER, convertedBufferId);
            // `normalized` is deliberately GL_FALSE rather than attrib.Normalized: GL ignores it
            // for floating-point array types, and honouring it would scale the fetched values
            // (KHR-GL43.vertex_attrib_binding.basic-input-case5 passes GL_TRUE and expects 10/20).
            g_GLESFuncs.glVertexAttribPointer(attribIndex, attrib.Size, GL_FLOAT, GL_FALSE,
                                              static_cast<GLsizei>(convertedElementSize),
                                              (const void*)(firstElement * convertedElementSize));
            return true;
        }

        StateBackendObjectRegistry<MG_State::GLState::VertexArrayObject, BackendVertexArrayObject>
            g_backendVertexArrayObjects;
    } // namespace VertexArrayImpl

    namespace TextureImpl {
        BackendTextureObject::BackendTextureObject() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_GLESFuncs.glGenTextures(1, &m_backendTextureId);
            m_contextGeneration = g_backendContextGeneration;
            if (m_backendTextureId == 0) {
                MGLOG_E_ONCE("Failed to generate texture object.");
                MGLOG_E_ONCE("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            } else {
                MGLOG_D("Generated texture object with ID: %u.", m_backendTextureId);
            }
        }

        BackendTextureObject::~BackendTextureObject() {
            if (InProcessTeardown()) {
                return; // see InProcessTeardown(): the driver may be unloaded already
            }
            if (m_backendTextureId == 0) {
                return;
            }
            // Scrub every driver-state shadow that could false-skip when the name
            // or this heap address is recycled - regardless of whether the id can
            // still be deleted.
            ScratchFBOImpl::NoteTextureIdDeleted(m_backendTextureId);
            for (auto& unitCache : g_boundTexturesCache) {
                for (auto& boundTexture : unitCache) {
                    if (boundTexture == this) {
                        boundTexture = nullptr;
                    }
                }
            }
            // TEMP-EXP (leak texture deletes): /sdcard/MG/exp_leak_texture_deletes.
            // Discriminator for the mali-mem-purge hiccup theory: never hand the
            // driver a texture free, so the purge daemon has nothing to reclaim.
            static const Bool s_expLeakTextureDeletes = [] {
                FILE* f = std::fopen("/sdcard/MG/exp_leak_texture_deletes", "rb");
                if (!f) return false;
                std::fclose(f);
                return true;
            }();
            if (m_contextGeneration == g_backendContextGeneration && g_GLESFuncs.glDeleteTextures &&
                !s_expLeakTextureDeletes) {
                g_GLESFuncs.glDeleteTextures(1, &m_backendTextureId);
                if (m_bufferImageSplitViewId != 0) {
                    g_GLESFuncs.glDeleteTextures(1, &m_bufferImageSplitViewId);
                }
            }
            m_backendTextureId = 0;
            m_bufferImageSplitViewId = 0;
        }

        void BackendTextureObject::Bind(GLenum target, Uint unit) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (g_activeTextureUnit != unit) {
                ActivateTextureUnit(unit);
            }

            auto targetN = static_cast<SizeT>(MG_Util::ConvertGLEnumToTextureTarget(target));
            if (this == g_boundTexturesCache[unit][targetN]) return;

            g_GLESFuncs.glBindTexture(target, m_backendTextureId);
            g_boundTexturesCache[unit][targetN] = this;
        }

        Uint BackendTextureObject::GetBackendTextureId() const {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            return m_backendTextureId;
        }

        void BackendTextureObject::RequireImageBindableStorage(
            const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject) {
            if (m_imageBindableStorageRequired) {
                return;
            }
            m_imageBindableStorageRequired = true;
            m_isInitialized = false;
            // Every level this object has ALREADY uploaded has to be replayed, because the
            // regeneration this transition schedules re-mints the storage in the image carrier and
            // only uploads levels the shadow still calls dirty - which, for a texture that was
            // synced before its first glBindImageTexture, is none of them. The new storage would
            // come out ALLOCATED AND EMPTY, and every texel the application defined before that
            // bind would be gone: the shader reads zeroes and the shadow still holds the data, so
            // glGetTexImage (which falls back to the shadow) keeps answering correctly and only
            // the image loads are wrong. Reached whenever anything syncs the texture first - a
            // glGetTexImage, a draw that samples it, an FBO attach - which is why it survived so
            // long: the scenario that binds the image immediately after uploading never sees it.
            if (auto* mipmapObject = MG_State::GLState::AsMipmapTexture(stateTextureObject.get())) {
                const auto levelCount = mipmapObject->GetMipmapLevelCount();
                for (const auto& uploadTarget : stateTextureObject->GetUploadTargets()) {
                    for (Uint level = 0; level < levelCount; ++level) {
                        const auto levelTexelSize = mipmapObject->GetMipmapTexelSize(uploadTarget, level);
                        if (levelTexelSize.x() <= 0 || levelTexelSize.y() <= 0) continue;
                        if (mipmapObject->GetMipmapByteSize(uploadTarget, level) == 0) continue;
                        mipmapObject->MarkStorageDirty(uploadTarget, level, true);
                    }
                }
            }
            // The storage this re-mints may also be CHANNEL WIDENED (a GL_RG32F image is not
            // bindable on this driver at all, so it becomes a GL_RGBA32F carrying two channels),
            // and a widened texture's sampled view has to answer the channels the logical format
            // does not have with 0 and 1 - which is a swizzle. The parameter sync is gated on the
            // frontend's params version, which this transition does not move, so without the
            // override an application that never touched GL_TEXTURE_SWIZZLE_* would keep the
            // driver at its defaults and sample the carrier's surplus channels raw.
            m_forceTextureParamsResync = true;
        }

        void BackendTextureObject::RecreateBackendTexture() {
            if (m_backendTextureId != 0) {
                ScratchFBOImpl::NoteTextureIdDeleted(m_backendTextureId);
                // Application FBO twins that attached the dying id memoize on FRONTEND
                // attachment versions, which this backend-side re-mint does not move;
                // without this bump their driver FBOs would keep the deleted name
                // attached forever (see g_attachmentBackendIdGeneration).
                ++FramebufferImpl::g_attachmentBackendIdGeneration;
                if (m_contextGeneration == g_backendContextGeneration) {
                    g_GLESFuncs.glDeleteTextures(1, &m_backendTextureId);
                }
                for (auto& unitCache : g_boundTexturesCache) {
                    for (auto& boundTexture : unitCache) {
                        if (boundTexture == this) {
                            boundTexture = nullptr;
                        }
                    }
                }
            }

            g_GLESFuncs.glGenTextures(1, &m_backendTextureId);
            m_contextGeneration = g_backendContextGeneration;
            if (m_backendTextureId == 0) {
                MGLOG_E_ONCE("Failed to regenerate texture object.");
                MGLOG_E_ONCE("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            } else {
                MGLOG_D("Regenerated texture object with ID: %u.", m_backendTextureId);
            }
            m_isInitialized = false;
            m_backendStorageImmutable = false;
            m_prevTextureInfo = {};
            // The new ES texture starts at the ES defaults, so every parameter this object had
            // already pushed onto the old one is gone. The change-detection caches below would
            // otherwise still claim those values are in force and SyncTextureParamsToBackend
            // would skip the whole pass on the unchanged params version, leaving the driver
            // texture at defaults for the rest of its life. Latent for swizzle, LOD range and
            // border colour long before GL_DEPTH_STENCIL_TEXTURE_MODE joined them; the mode
            // makes it visible because falling back to the default silently samples the wrong
            // aspect rather than merely mis-filtering.
            m_cacheLodRange = {0, 1000};
            m_cacheBorderColor = {0.0f, 0.0f, 0.0f, 0.0f};
            m_cacheSwizzleParams = {TextureSwizzleParam::Red, TextureSwizzleParam::Green, TextureSwizzleParam::Blue,
                                    TextureSwizzleParam::Alpha};
            m_cacheDepthStencilTextureMode = GL_DEPTH_COMPONENT;
            m_forceTextureParamsResync = true;
            // The filter/wrap/LOD cache belongs to the name that just went away, and its gate is
            // the frontend SAMPLER's version, which a backend re-mint does not move - so without
            // this the new driver texture keeps the ES defaults for life. See m_forceSamplerResync.
            m_cacheSamplerParameters = SamplerParameters{};
            m_forceSamplerResync = true;
        }

        // Sets the backend GL unpack state to MobileGL's upload default for the scope,
        // then restores it. The previous state is read from a shadow instead of via
        // glGetIntegerv - that query forces a driver pipeline sync and, because texture
        // uploads run it per dirty texture per frame, it dominated the DirectGLES draw
        // path. The backend unpack state is set ONLY by MobileGL's own save/restore
        // helpers (this class and, historically, the R32F copy path), all of
        // which restore to the resting default, so the shadow stays accurate; a one-time
        // forced sync pins the backend to that known default up front. Apply() is
        // compare-and-set, so the (now redundant) glPixelStorei calls also usually no-op.
        class ScopedDefaultUnpackState {
        public:
            ScopedDefaultUnpackState() {
                EnsureShadowSynced();
                m_prevAlignment = s_alignment;
                m_prevRowLength = s_rowLength;
                m_prevSkipRows = s_skipRows;
                m_prevSkipPixels = s_skipPixels;
                m_prevImageHeight = s_imageHeight;
                m_prevSkipImages = s_skipImages;
                // Shadow mip data is tightly packed (ProcessTexturePixelsDataUnpack emits
                // width * bpp rows with no padding), so uploads must use UNPACK_ALIGNMENT = 1.
                // Alignment 4 made the driver read e.g. 7-byte R8 rows at an 8-byte stride,
                // shifting every row of a non-multiple-of-4 upload by one pixel.
                Apply(1, 0, 0, 0, 0, 0);
            }

            ~ScopedDefaultUnpackState() {
                Apply(m_prevAlignment, m_prevRowLength, m_prevSkipRows, m_prevSkipPixels, m_prevImageHeight,
                      m_prevSkipImages);
            }

        private:
            static void EnsureShadowSynced() {
                if (s_synced) {
                    return;
                }
                s_synced = true;
                g_GLESFuncs.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                g_GLESFuncs.glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                g_GLESFuncs.glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                g_GLESFuncs.glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
                g_GLESFuncs.glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
                s_alignment = 4;
                s_rowLength = 0;
                s_skipRows = 0;
                s_skipPixels = 0;
                s_imageHeight = 0;
                s_skipImages = 0;
            }

            static void Apply(GLint alignment, GLint rowLength, GLint skipRows, GLint skipPixels, GLint imageHeight,
                              GLint skipImages) {
                if (alignment != s_alignment) { g_GLESFuncs.glPixelStorei(GL_UNPACK_ALIGNMENT, alignment); s_alignment = alignment; }
                if (rowLength != s_rowLength) { g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength); s_rowLength = rowLength; }
                if (skipRows != s_skipRows) { g_GLESFuncs.glPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows); s_skipRows = skipRows; }
                if (skipPixels != s_skipPixels) { g_GLESFuncs.glPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels); s_skipPixels = skipPixels; }
                if (imageHeight != s_imageHeight) { g_GLESFuncs.glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, imageHeight); s_imageHeight = imageHeight; }
                if (skipImages != s_skipImages) { g_GLESFuncs.glPixelStorei(GL_UNPACK_SKIP_IMAGES, skipImages); s_skipImages = skipImages; }
            }

            GLint m_prevAlignment = 4;
            GLint m_prevRowLength = 0;
            GLint m_prevSkipRows = 0;
            GLint m_prevSkipPixels = 0;
            GLint m_prevImageHeight = 0;
            GLint m_prevSkipImages = 0;

            // Shadow of the backend GL unpack state (GL defaults). See class comment.
            static inline Bool s_synced = false;
            static inline GLint s_alignment = 4;
            static inline GLint s_rowLength = 0;
            static inline GLint s_skipRows = 0;
            static inline GLint s_skipPixels = 0;
            static inline GLint s_imageHeight = 0;
            static inline GLint s_skipImages = 0;
        };

        // --- Unpack-ring staging (see BufferImpl::UnpackRingAvailable) ---------------
        // One rectangular (or whole-level) region of a level shadow, repacked TIGHTLY
        // into the persistently-mapped unpack PBO. The upload that follows passes the
        // returned ring offset with UNPACK_ROW_LENGTH / UNPACK_IMAGE_HEIGHT left at the
        // surrounding ScopedDefaultUnpackState's 0, so the driver reads exactly the
        // bytes staged here - strictly fewer than the ROW_LENGTH-strided client-pointer
        // upload it replaces, which made the driver walk the whole level's stride.
        //
        // `blocks` describes one or more source regions to stage back-to-back into a
        // SINGLE ring allocation; the ring offset of block i lands in blocks[i].offset.
        // Deliberately without default member initializers: call sites declare a
        // kMaxDirtyRects-sized array of these per dirty level and fill only the entries
        // they use, so the type stays trivially default-constructible and the
        // declaration costs nothing on the levels that never reach the ring.
        struct UnpackStagingBlock {
            const Uint8* src;      // top-left texel of the region in the level shadow
            SizeT rowBytes;        // bytes per region row (region width * bpp)
            SizeT rows;            // region height
            SizeT slices;          // region depth (1 for 2D)
            SizeT srcRowStride;    // level row pitch
            SizeT srcSliceStride;  // level slice pitch
            SizeT offset;          // out: byte offset into the ring store
        };

        // False (nothing staged, ring untouched) whenever the caller must keep the
        // client-pointer path: ring unavailable, a degenerate block, or a total that
        // overflows / exceeds the ring's cap.
        static Bool StageBlocksIntoUnpackRing(UnpackStagingBlock* blocks, SizeT blockCount) {
            if (blocks == nullptr || blockCount == 0) return false;
            if (!BufferImpl::UnpackRingAvailable()) return false;

            const SizeT maxBytes = BufferImpl::UnpackRingMaxBytes();
            SizeT total = 0;
            for (SizeT i = 0; i < blockCount; ++i) {
                const UnpackStagingBlock& b = blocks[i];
                if (b.src == nullptr || b.rowBytes == 0 || b.rows == 0 || b.slices == 0) return false;
                // Every factor is bounded by the level's own byte size, but the
                // multiplications are on SizeT: check each against the cap instead of
                // trusting a product that could have wrapped.
                if (b.rowBytes > maxBytes || b.rows > maxBytes / b.rowBytes) return false;
                const SizeT sliceBytes = b.rowBytes * b.rows;
                if (b.slices > maxBytes / sliceBytes) return false;
                const SizeT blockBytes = sliceBytes * b.slices;
                if (blockBytes > maxBytes - total) return false;
                total += blockBytes;
            }

            SizeT base = 0;
            if (!BufferImpl::UnpackRingAllocate(total, base)) return false;
            // AFTER the allocation: growing the ring replaces the store and its map.
            auto* ringBytes = static_cast<Uint8*>(BufferImpl::UnpackRingMappedPtr());
            if (ringBytes == nullptr) return false;

            SizeT cursor = base;
            for (SizeT i = 0; i < blockCount; ++i) {
                UnpackStagingBlock& b = blocks[i];
                b.offset = cursor;
                Uint8* dst = ringBytes + cursor;
                // Each block's size is a whole number of texels, so consecutive block
                // offsets stay multiples of the pixel type's size just as `base` is.
                if (b.rowBytes == b.srcRowStride && b.rowBytes * b.rows == b.srcSliceStride) {
                    Memcpy(dst, b.src, b.rowBytes * b.rows * b.slices); // region IS the level
                } else {
                    for (SizeT z = 0; z < b.slices; ++z) {
                        const Uint8* srcSlice = b.src + z * b.srcSliceStride;
                        if (b.rowBytes == b.srcRowStride) {
                            Memcpy(dst, srcSlice, b.rowBytes * b.rows); // full-width rows
                            dst += b.rowBytes * b.rows;
                            continue;
                        }
                        for (SizeT y = 0; y < b.rows; ++y) {
                            Memcpy(dst, srcSlice + y * b.srcRowStride, b.rowBytes);
                            dst += b.rowBytes;
                        }
                    }
                }
                cursor += b.rowBytes * b.rows * b.slices;
            }
            return true;
        }

        // The `pixels` argument of a PBO-sourced glTexSubImage: a byte offset dressed
        // up as a pointer.
        static const void* UnpackRingPixelOffset(SizeT offset) {
            return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(offset));
        }

        static Uint GetNormFallbackComponentCount(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::R16:
            case TextureInternalFormat::R16Snorm:
                return 1;
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RG16:
            case TextureInternalFormat::RG16Snorm:
                return 2;
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGB16:
            case TextureInternalFormat::RGB10:  // stored as RGB16 (UNorm16 shadow)
            case TextureInternalFormat::RGB12:  // stored as RGB16 (UNorm16 shadow)
            case TextureInternalFormat::RGB16Snorm:
                return 3;
            case TextureInternalFormat::RGBA8Snorm:
            case TextureInternalFormat::RGBA16:
            case TextureInternalFormat::RGBA12: // stored as RGBA16 (UNorm16 shadow)
            case TextureInternalFormat::RGBA16Snorm:
                return 4;
            default:
                return 0;
            }
        }

        static Bool IsSnormFallbackFormat(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGBA8Snorm:
            case TextureInternalFormat::R16Snorm:
            case TextureInternalFormat::RG16Snorm:
            case TextureInternalFormat::RGB16Snorm:
            case TextureInternalFormat::RGBA16Snorm:
                return true;
            default:
                return false;
            }
        }

        static Bool IsNorm8FallbackFormat(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGBA8Snorm:
                return true;
            default:
                return false;
            }
        }

        // Components per texel the frontend format's client data carries. Only the three-channel
        // formats that can be widened to a four-channel render target need an answer (see
        // PrepareChannelWidenedUpload); everything else keeps its own layout and reports 0.
        Uint GetWidenableClientComponentCount(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGB16Snorm:
            case TextureInternalFormat::RGB16:
            case TextureInternalFormat::RGB10: // stored as RGB16 (UNorm16 shadow)
            case TextureInternalFormat::RGB12: // stored as RGB16 (UNorm16 shadow)
            case TextureInternalFormat::RGB16F:
            case TextureInternalFormat::RGB32F:
            case TextureInternalFormat::SRGB8:
            case TextureInternalFormat::RGB8I:
            case TextureInternalFormat::RGB8UI:
            case TextureInternalFormat::RGB16I:
            case TextureInternalFormat::RGB16UI:
            case TextureInternalFormat::RGB32I:
            case TextureInternalFormat::RGB32UI:
                return 3;
            default:
                return 0;
            }
        }

        // True when the widened format's client data is integer rather than normalized. The two
        // classes share every narrow component type - GL_RGB8I and GL_RGB8_SNORM are both uploaded
        // as GL_BYTE - but their "1.0" differs: an integer channel's one is the integer 1, a
        // normalized channel's is the saturated field. The type alone cannot tell them apart, so
        // the source format has to.
        Bool IsIntegerWidenableFormat(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::RGB8I:
            case TextureInternalFormat::RGB8UI:
            case TextureInternalFormat::RGB16I:
            case TextureInternalFormat::RGB16UI:
            case TextureInternalFormat::RGB32I:
            case TextureInternalFormat::RGB32UI:
                return true;
            default:
                return false;
            }
        }

        // The bit pattern of 1.0 in an upload component type: what a format without alpha reads
        // back as, and therefore what the synthetic fourth channel of a widened render target has
        // to hold. Integer components carry the integer one, not a saturated field - and since
        // GL_BYTE/GL_SHORT/GL_UNSIGNED_BYTE/GL_UNSIGNED_SHORT serve both classes, `integerData`
        // is what decides, not the type.
        static Bool GetUploadComponentOneBits(GLenum uploadType, Bool integerData, Uint8* outOneBits,
                                              SizeT* outComponentSize) {
            switch (uploadType) {
            case GL_BYTE: {
                const Int8 one = integerData ? Int8(1) : Int8(0x7F);
                Memcpy(outOneBits, &one, sizeof(one));
                *outComponentSize = sizeof(one);
                return true;
            }
            case GL_UNSIGNED_BYTE: {
                const Uint8 one = integerData ? Uint8(1) : Uint8(0xFF);
                Memcpy(outOneBits, &one, sizeof(one));
                *outComponentSize = sizeof(one);
                return true;
            }
            case GL_SHORT: {
                const Int16 one = integerData ? Int16(1) : Int16(0x7FFF);
                Memcpy(outOneBits, &one, sizeof(one));
                *outComponentSize = sizeof(one);
                return true;
            }
            case GL_UNSIGNED_SHORT: {
                const Uint16 one = integerData ? Uint16(1) : Uint16(0xFFFF);
                Memcpy(outOneBits, &one, sizeof(one));
                *outComponentSize = sizeof(one);
                return true;
            }
            case GL_HALF_FLOAT: {
                const Uint16 one = 0x3C00; // half 1.0
                Memcpy(outOneBits, &one, sizeof(one));
                *outComponentSize = sizeof(one);
                return true;
            }
            case GL_FLOAT: {
                const Float one = 1.0f;
                Memcpy(outOneBits, &one, sizeof(one));
                *outComponentSize = sizeof(one);
                return true;
            }
            case GL_INT: {
                const Int32 one = 1;
                Memcpy(outOneBits, &one, sizeof(one));
                *outComponentSize = sizeof(one);
                return true;
            }
            case GL_UNSIGNED_INT: {
                const Uint32 one = 1;
                Memcpy(outOneBits, &one, sizeof(one));
                *outComponentSize = sizeof(one);
                return true;
            }
            default:
                return false;
            }
        }

        // A three-channel format widened to four to keep a colour attachment renderable (see
        // NormalizePixelFormat) is described to the driver as a four-component transfer, so the
        // three-component client data has to be repacked with an alpha of 1.0 - otherwise the
        // driver walks three texels' worth of data per four-texel row and the image shears.
        // `componentCount` is the SOURCE component count and `byteSize` the source's size, so this
        // runs after any type conversion (which keeps the component count) has already happened.
        const void* PrepareChannelWidenedUpload(Uint componentCount, const IntVec3& texelSize,
                                                const void* data, SizeT byteSize, GLenum uploadType,
                                                Vector<Uint8>& widenedData, Bool integerData,
                                                Uint32 alphaOneCodeOverride) {
            Uint8 oneBits[8] = {};
            SizeT componentSize = 0;
            // One and two source components as well as three: the image-format widening carries
            // GL_R8UI in a GL_RGBA8UI and GL_RG32F in a GL_RGBA32F (see
            // TextureImpl::GetImageBindableStorageWidening), and their surplus channels take the
            // same values the three-channel case gives its single added one - zeroes, and the
            // format's implied 1 in alpha.
            if (componentCount == 0 || componentCount > 3 || data == nullptr || byteSize == 0 ||
                !GetUploadComponentOneBits(uploadType, integerData, oneBits, &componentSize)) {
                return data;
            }
            // ...except where the carrier holds CODES of a normalized value (GL_R16 in a
            // GL_RGBA16UI), where the transfer type says GL_UNSIGNED_SHORT and neither of that
            // type's two "ones" is right: the integer 1 is a code for 1/65535 and the saturated
            // 0xFFFF is only right for the UNSIGNED 16-bit formats, not the signed ones, whose
            // saturated code is 0x7FFF. The caller passes the channel's own maximum instead.
            // Written through a value of the component's own width rather than as the low
            // `componentSize` bytes of the Uint32, so the encoding does not turn on the host's
            // byte order.
            if (alphaOneCodeOverride != 0u) {
                if (componentSize == sizeof(Uint16)) {
                    const auto one = static_cast<Uint16>(alphaOneCodeOverride);
                    Memcpy(oneBits, &one, sizeof(one));
                } else if (componentSize == sizeof(Uint32)) {
                    Memcpy(oneBits, &alphaOneCodeOverride, sizeof(alphaOneCodeOverride));
                } else if (componentSize == sizeof(Uint8)) {
                    const auto one = static_cast<Uint8>(alphaOneCodeOverride);
                    Memcpy(oneBits, &one, sizeof(one));
                }
            }

            const SizeT srcTexelBytes = componentSize * componentCount;
            // Sized from the level, never from the source: the driver reads a full
            // width*height*depth*4 components for the transfer it was handed, so a source that
            // somehow holds fewer texels must still leave a full destination behind (its tail
            // reads as transparent black with the format's implied opaque alpha) rather than a
            // short buffer the driver would run off the end of.
            const SizeT texelCount = static_cast<SizeT>(std::max(texelSize.x(), 0)) *
                                     static_cast<SizeT>(std::max(texelSize.y(), 0)) *
                                     static_cast<SizeT>(std::max(texelSize.z(), 1));
            if (texelCount == 0) {
                return data;
            }
            const SizeT copyTexelCount = std::min(texelCount, byteSize / srcTexelBytes);

            widenedData.assign(texelCount * componentSize * 4, 0);
            const auto* src = static_cast<const Uint8*>(data);
            Uint8* dst = widenedData.data();
            for (SizeT i = 0; i < texelCount; ++i, dst += componentSize * 4) {
                if (i < copyTexelCount) {
                    Memcpy(dst, src, srcTexelBytes);
                    src += srcTexelBytes;
                }
                // ALWAYS at component 3, never at `componentCount`: GL's implied 1 is the ALPHA
                // channel, and a one- or two-component source leaves the channels between it and
                // alpha at the zero `assign` already wrote. For three components the two
                // expressions coincide, which is what this used to be written as.
                Memcpy(dst + componentSize * 3, oneBits, componentSize);
            }
            return widenedData.data();
        }

        static const void* PrepareNormFloatFallbackUpload(TextureInternalFormat format,
                                                          const IntVec3& texelSize,
                                                          const void* data,
                                                          SizeT byteSize,
                                                          GLenum uploadType,
                                                          Vector<Float>& convertedData) {
            const Uint componentCount = GetNormFallbackComponentCount(format);
            if (componentCount == 0 || uploadType != GL_FLOAT || data == nullptr || byteSize == 0) {
                return data;
            }

            const SizeT texelCount = static_cast<SizeT>(std::max(texelSize.x(), 0)) *
                                     static_cast<SizeT>(std::max(texelSize.y(), 0)) *
                                     static_cast<SizeT>(std::max(texelSize.z(), 0));
            const SizeT componentTotal = texelCount * static_cast<SizeT>(componentCount);
            const SizeT sourceComponentSize = IsNorm8FallbackFormat(format) ? sizeof(Int8) : sizeof(Uint16);
            const SizeT sourceComponentTotal = byteSize / sourceComponentSize;
            if (componentTotal == 0 || sourceComponentTotal == 0) {
                return nullptr;
            }

            convertedData.assign(componentTotal, 0.0f);
            const SizeT copyComponentTotal = std::min(componentTotal, sourceComponentTotal);
            if (IsNorm8FallbackFormat(format)) {
                const Int8* src = static_cast<const Int8*>(data);
                constexpr Float invMaxSnorm8 = 1.0f / 127.0f;
                for (SizeT i = 0; i < copyComponentTotal; ++i) {
                    convertedData[i] = std::max(static_cast<Float>(src[i]) * invMaxSnorm8, -1.0f);
                }
            } else if (IsSnormFallbackFormat(format)) {
                const Int16* src = static_cast<const Int16*>(data);
                constexpr Float invMaxSnorm16 = 1.0f / 32767.0f;
                for (SizeT i = 0; i < copyComponentTotal; ++i) {
                    convertedData[i] = std::max(static_cast<Float>(src[i]) * invMaxSnorm16, -1.0f);
                }
            } else {
                const Uint16* src = static_cast<const Uint16*>(data);
                constexpr Float invMaxUnorm16 = 1.0f / 65535.0f;
                for (SizeT i = 0; i < copyComponentTotal; ++i) {
                    convertedData[i] = static_cast<Float>(src[i]) * invMaxUnorm16;
                }
            }
            return convertedData.data();
        }

        // The two shadow -> upload conversions a fallback storage format can need, in order:
        // the component type first (SNORM/UNORM shadows into the float the fallback stores), then
        // the component count (three-channel client data into a four-channel widened render
        // target). They compose: GL_RGB8_SNORM on a driver with no renderable three-channel
        // format becomes GL_RGBA16F, so its Int8x3 shadow is converted to Float x3 and then
        // repacked as Float x4 with alpha 1.0.
        //
        // Both scratch buffers belong to the caller so they outlive the returned pointer; the
        // return value is `data` itself whenever neither conversion applies, which is what the
        // sub-rect upload fast path tests for.
        static const void* PrepareFallbackUpload(TextureInternalFormat format, TextureTarget target,
                                                 const IntVec3& texelSize, const void* data, SizeT byteSize,
                                                 GLenum uploadType, Vector<Float>& convertedData,
                                                 Vector<Uint8>& widenedData) {
            const void* uploadData =
                PrepareNormFloatFallbackUpload(format, texelSize, data, byteSize, uploadType, convertedData);
            // The component-count switch first: it rules out every format that cannot be widened
            // (which is nearly all of them, including GL_RGBA8) without touching the capability
            // cache, so an ordinary atlas upload does not pay for a per-level cache lookup.
            const Uint componentCount = GetWidenableClientComponentCount(format);
            if (componentCount == 0 || !TextureImpl::BackendTextureFormatAddsAlpha(format, target)) {
                return uploadData;
            }
            // The type conversion above rewrites the level into `convertedData` at four bytes per
            // component while keeping the component count, so the widening's source size is that
            // buffer's, not the shadow's.
            const SizeT uploadByteSize = (!convertedData.empty() && uploadData == convertedData.data())
                                             ? convertedData.size() * sizeof(Float)
                                             : byteSize;
            return PrepareChannelWidenedUpload(componentCount, texelSize, uploadData, uploadByteSize, uploadType,
                                               widenedData, IsIntegerWidenableFormat(format));
        }

        // One channel of a packed r11f_g11f_b10f word as a float. The two 11-bit channels are
        // e5m6 and the 10-bit one e5m5 - IEEE-shaped but UNSIGNED, so there is no sign bit to
        // read and the exponent bias is the 15 a 5-bit exponent always carries.
        static Float DecodePackedUnsignedFloat(Uint32 bits, Uint mantissaBits) {
            const Uint32 mantissaScale = 1u << mantissaBits;
            const Uint32 mantissa = bits & (mantissaScale - 1u);
            const Uint32 exponent = bits >> mantissaBits;
            if (exponent == 0u) {
                // Subnormal, and zero with it: no implied leading 1, and the exponent is the
                // smallest NORMAL one rather than the encoded 0.
                return std::ldexp(static_cast<Float>(mantissa) / static_cast<Float>(mantissaScale), -14);
            }
            if (exponent == 31u) {
                return mantissa == 0u ? std::numeric_limits<Float>::infinity()
                                      : std::numeric_limits<Float>::quiet_NaN();
            }
            return std::ldexp(1.0f + static_cast<Float>(mantissa) / static_cast<Float>(mantissaScale),
                              static_cast<Int>(exponent) - 15);
        }

        // The r11f_g11f_b10f shadow decoded into the GL_RGBA / GL_FLOAT level its GL_RGBA16F
        // carrier is uploaded as. Alpha is the 1 GL defines for a format that has no alpha
        // channel, which is the same constant the shader-side mask writes, so a texel this
        // function produced and a texel an imageStore produced are indistinguishable.
        //
        // Sized from the LEVEL, not the source, for the reason PrepareChannelWidenedUpload is:
        // the driver reads a full width*height*depth*4 floats for the transfer it was handed.
        static const void* PreparePackedFloatWidenedUpload(const IntVec3& texelSize, const void* data,
                                                           SizeT byteSize, Vector<Uint8>& widenedData) {
            constexpr SizeT kSourceTexelBytes = sizeof(Uint32);
            if (data == nullptr || byteSize < kSourceTexelBytes) {
                return data;
            }
            const SizeT texelCount = static_cast<SizeT>(std::max(texelSize.x(), 0)) *
                                     static_cast<SizeT>(std::max(texelSize.y(), 0)) *
                                     static_cast<SizeT>(std::max(texelSize.z(), 1));
            if (texelCount == 0) {
                return data;
            }
            const SizeT copyTexelCount = std::min(texelCount, byteSize / kSourceTexelBytes);

            widenedData.assign(texelCount * 4u * sizeof(Float), 0);
            const auto* src = static_cast<const Uint8*>(data);
            auto* dst = reinterpret_cast<Float*>(widenedData.data());
            for (SizeT i = 0; i < texelCount; ++i, dst += 4) {
                Float rgb[3] = {0.0f, 0.0f, 0.0f};
                if (i < copyTexelCount) {
                    Uint32 packed = 0;
                    // Through a memcpy rather than a Uint32 read of `src`: the shadow is a byte
                    // buffer with no alignment promise of its own.
                    Memcpy(&packed, src + i * kSourceTexelBytes, sizeof(packed));
                    rgb[0] = DecodePackedUnsignedFloat(packed & 0x7FFu, 6u);
                    rgb[1] = DecodePackedUnsignedFloat((packed >> 11u) & 0x7FFu, 6u);
                    rgb[2] = DecodePackedUnsignedFloat((packed >> 22u) & 0x3FFu, 5u);
                }
                dst[0] = rgb[0];
                dst[1] = rgb[1];
                dst[2] = rgb[2];
                dst[3] = 1.0f;
            }
            return widenedData.data();
        }

        // The rgb10_a2 / rgb10_a2ui shadow split into the four GL_UNSIGNED_SHORT channel CODES its
        // GL_RGBA16UI carrier is uploaded as. GL_UNSIGNED_INT_2_10_10_10_REV puts the FIRST
        // component in the LOW bits (that is what REV means), so red is bits 0-9, green 10-19,
        // blue 20-29 and alpha 30-31.
        //
        // The same split serves both formats: an rgb10_a2ui channel's code IS its value, and an
        // rgb10_a2 channel's code is the numerator of value = code / (2^b - 1) that the shader-side
        // unpack divides out. Neither is scaled here - the carrier holds the format's own bits.
        //
        // Sized from the LEVEL, not the source, for the reason PrepareChannelWidenedUpload is: the
        // driver reads a full width*height*depth*4 shorts for the transfer it was handed.
        const void* PreparePackedIntWidenedUpload(const IntVec3& texelSize, const void* data,
                                                  SizeT byteSize, Vector<Uint8>& widenedData) {
            constexpr SizeT kSourceTexelBytes = sizeof(Uint32);
            if (data == nullptr || byteSize < kSourceTexelBytes) {
                return data;
            }
            const SizeT texelCount = static_cast<SizeT>(std::max(texelSize.x(), 0)) *
                                     static_cast<SizeT>(std::max(texelSize.y(), 0)) *
                                     static_cast<SizeT>(std::max(texelSize.z(), 1));
            if (texelCount == 0) {
                return data;
            }
            const SizeT copyTexelCount = std::min(texelCount, byteSize / kSourceTexelBytes);

            widenedData.assign(texelCount * 4u * sizeof(Uint16), 0);
            const auto* src = static_cast<const Uint8*>(data);
            auto* dst = reinterpret_cast<Uint16*>(widenedData.data());
            for (SizeT i = 0; i < texelCount; ++i, dst += 4) {
                Uint32 packed = 0;
                if (i < copyTexelCount) {
                    // Through a memcpy rather than a Uint32 read of `src`: the shadow is a byte
                    // buffer with no alignment promise of its own.
                    Memcpy(&packed, src + i * kSourceTexelBytes, sizeof(packed));
                }
                dst[0] = static_cast<Uint16>(packed & 0x3FFu);
                dst[1] = static_cast<Uint16>((packed >> 10u) & 0x3FFu);
                dst[2] = static_cast<Uint16>((packed >> 20u) & 0x3FFu);
                dst[3] = static_cast<Uint16>((packed >> 30u) & 0x3u);
            }
            return widenedData.data();
        }

        // The transfer half of the image-format widening: an image-bindable texture whose ES
        // storage was widened to a core carrier is described to the driver as a four-component
        // transfer, so its narrower client data has to be repacked the same way the three-channel
        // colour-renderable widening repacks its own.
        //
        // Three shapes, because the carriers come in three kinds. Most of them keep the frontend
        // format's component TYPE and only add channels, so padding the shadow out to four
        // components is the whole conversion. The two PACKED formats do not: their shadow is one
        // 32-bit word per texel, so the word has to be split - into four floats for
        // r11f_g11f_b10f's GL_RGBA16F, into four shorts for rgb10_a2ui's GL_RGBA16UI. Reading such
        // a word as components of the carrier's type - what the repack below would do - takes
        // twelve or sixteen bytes from a four-byte texel and shears the level, which is what the
        // allFormats LOAD walkers see and the STORE ones do not (a store overwrites every texel
        // the upload got wrong).
        //
        // Composes with PrepareFallbackUpload rather than replacing it, and the composition is a
        // no-op by construction: none of the widened formats is one GetWidenableClientComponentCount
        // reports a count for, and the SNORM shadow-to-float conversion only fires for a GL_FLOAT
        // transfer type, which the widened triple never picks for the two SNORM8 formats. So the
        // shadow reaches this untouched and one conversion is all that runs.
        static const void* PrepareImageWidenedUpload(const TextureImpl::ImageBindableStorageWidening& widening,
                                                     const IntVec3& texelSize, const void* data, SizeT byteSize,
                                                     Vector<Uint8>& widenedData) {
            if (!widening || widening.SourceChannels == 0 || widening.SourceChannels > 4) {
                return data;
            }
            switch (widening.SourceEncoding) {
            case TextureImpl::ImageWidenSourceEncoding::PackedFloat11f11f10f:
                return PreparePackedFloatWidenedUpload(texelSize, data, byteSize, widenedData);
            case TextureImpl::ImageWidenSourceEncoding::PackedInt2101010Rev:
                return PreparePackedIntWidenedUpload(texelSize, data, byteSize, widenedData);
            case TextureImpl::ImageWidenSourceEncoding::Components:
                break;
            }
            if (widening.SourceChannels == 4) {
                return data;
            }
            return PrepareChannelWidenedUpload(widening.SourceChannels, texelSize, data, byteSize, widening.Type,
                                               widenedData, widening.IntegerData,
                                               widening.CarriesNormalizedCodes() ? widening.ChannelMax[3] : 0u);
        }

        // Overwrites the (internal format, format, type) triple GenerateTextureFormatInfo chose
        // with the widened carrier's. Deliberately unconditional on anything but the widening
        // itself: whatever renderability fallback the triple carried, an image the driver refuses
        // to bind is useless, so the image constraint wins.
        static void ApplyImageBindableStorageWidening(const TextureImpl::ImageBindableStorageWidening& widening,
                                                      GLenum* inOutInternalFormat, GLenum* inOutFormat,
                                                      GLenum* inOutType) {
            if (!widening) {
                return;
            }
            if (inOutInternalFormat) *inOutInternalFormat = widening.InternalFormat;
            if (inOutFormat) *inOutFormat = widening.Format;
            if (inOutType) *inOutType = widening.Type;
        }

        // RGB565/RGB5_A1 shadow data is stored as 8-bit unorm; uploading it as GL_UNSIGNED_BYTE
        // leaves the 8-bit -> 5/6-bit requantization to the driver, whose rounding direction is
        // implementation-defined: Adreno rounds to nearest (lossless round trip) but Mali floors,
        // drifting mid-range texels one 5-bit step down and failing the KHR-GL3x
        // pixelstoragemodes.teximage3d rgb565/rgb5a1 1/32-eps checks. Repack the shadow rows into
        // the packed 16-bit client type with round-to-nearest instead - that recovers the original
        // 5/6-bit values exactly (the shadow expansion round(v * 255 / max) is injective), so the
        // driver stores them verbatim with no requantization left to its discretion. 4-bit formats
        // (RGBA4) are exempt: their 8-bit expansion (v * 17) is exact under either rounding.
        // Always retargets *inOutType for these formats (even for null data) so every upload of a
        // level uses the same client type.
        static const void* PreparePackedNormUpload(TextureInternalFormat format, const IntVec3& texelSize,
                                                   const void* data, SizeT byteSize, GLenum* inOutType,
                                                   Vector<Uint8>& packedData) {
            if (format != TextureInternalFormat::RGB5 && format != TextureInternalFormat::RGB5A1) {
                return data;
            }
            // With the storage widened to 8-bit-per-channel (the packed16 field-order quirk)
            // there is no driver requantization left for the repack to pre-empt - the shadow's
            // UNorm8 bytes ARE the stored bytes - and the packed 16-bit client type this leg
            // retargets to is not a legal upload for a GL_RGB8/GL_RGBA8 store at all.
            if (TextureImpl::UsesWidenedPacked16NormStorage(format)) {
                return data;
            }
            const Bool hasAlpha = format == TextureInternalFormat::RGB5A1;
            const GLenum packedType = hasAlpha ? GL_UNSIGNED_SHORT_5_5_5_1 : GL_UNSIGNED_SHORT_5_6_5;
            // Idempotent across a region's level loop: glType is shared, so later levels arrive with
            // the already-retargeted packed type and must still be converted.
            if (*inOutType != GL_UNSIGNED_BYTE && *inOutType != packedType) {
                return data;
            }
            *inOutType = packedType;
            if (data == nullptr || byteSize == 0) {
                return data;
            }
            const SizeT srcPixelBytes = hasAlpha ? 4 : 3;
            const SizeT texelCount = std::min(static_cast<SizeT>(std::max(texelSize.x(), 0)) *
                                                  static_cast<SizeT>(std::max(texelSize.y(), 0)) *
                                                  static_cast<SizeT>(std::max(texelSize.z(), 1)),
                                              byteSize / srcPixelBytes);
            packedData.resize(texelCount * sizeof(Uint16));
            const Uint8* src = static_cast<const Uint8*>(data);
            auto* dst = reinterpret_cast<Uint16*>(packedData.data());
            for (SizeT i = 0; i < texelCount; ++i, src += srcPixelBytes) {
                const Uint32 r = (static_cast<Uint32>(src[0]) * 31u + 127u) / 255u;
                const Uint32 b = (static_cast<Uint32>(src[2]) * 31u + 127u) / 255u;
                if (hasAlpha) {
                    const Uint32 g = (static_cast<Uint32>(src[1]) * 31u + 127u) / 255u;
                    dst[i] = static_cast<Uint16>((r << 11) | (g << 6) | (b << 1) | (src[3] >= 128 ? 1u : 0u));
                } else {
                    const Uint32 g = (static_cast<Uint32>(src[1]) * 63u + 127u) / 255u;
                    dst[i] = static_cast<Uint16>((r << 11) | (g << 5) | b);
                }
            }
            return packedData.data();
        }

        // "Some level of this texture holds an image", which is all the sync gate below actually
        // needs to know. Deliberately weaker than ITextureObject::IsComplete(): that predicate also
        // answers whether the texture SAMPLES as complete, so it must keep rejecting a chain with
        // undefined lower levels - but such a texture still has to be uploaded, or the level that
        // IS defined never reaches the driver at all.
        static Bool HasAnyDefinedMipmapLevel(const MG_State::GLState::ITextureObject* stateTextureObject) {
            const auto* mipmapObject = MG_State::GLState::AsMipmapTexture(stateTextureObject);
            if (mipmapObject == nullptr) return false;
            const auto levelCount = mipmapObject->GetMipmapLevelCount();
            for (const auto& uploadTarget : stateTextureObject->GetUploadTargets()) {
                for (Uint level = 0; level < levelCount; ++level) {
                    const auto levelTexelSize = mipmapObject->GetMipmapTexelSize(uploadTarget, level);
                    if (levelTexelSize.x() > 0 && levelTexelSize.y() > 0 && levelTexelSize.z() > 0) {
                        return true;
                    }
                }
            }
            return false;
        }

        // The ES entry point for EXT/OES_texture_view, whichever spelling this driver brought.
        // Callers must have checked g_GLESCapabilities.SupportsTextureView first - the capability
        // is the extension AND the pointer, because eglGetProcAddress hands back live-looking
        // stubs (see AcquireGLESFunctions).
        static MG_External::GLES::glTextureViewEXT_PTR ResolveTextureViewEntryPoint() {
            if (g_GLESFuncs.glTextureViewEXT != nullptr) {
                return g_GLESFuncs.glTextureViewEXT;
            }
            return reinterpret_cast<MG_External::GLES::glTextureViewEXT_PTR>(g_GLESFuncs.glTextureViewOES);
        }

        // Stamps the same per-draw clean-gate keys a completed storage sync stamps, so a view
        // that needs no work costs the same nothing per draw that any other synced texture does.
        void BackendTextureObject::StampViewSyncKeys(
            const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject) {
            if (MG_State::pGLContext) {
                m_syncedShapeContextId = MG_State::pGLContext->GetTextureContextId();
                m_syncedShapeGeneration = MG_State::pGLContext->GetSamplingResolutionGeneration();
                m_syncedShapeParamsVersion = stateTextureObject->GetTextureParamsVersion();
            }
            m_syncedContentVersion = stateTextureObject->GetContentVersion();
        }

        void BackendTextureObject::SyncTextureViewToBackend(
            const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject) {
            const auto& storageObject = stateTextureObject->GetViewStorageOwner();
            if (!storageObject) {
                MGLOG_E_ONCE("Texture %u claims to be a view but names no storage owner.",
                             stateTextureObject->GetExternalIndex());
                return;
            }
            if (!g_GLESCapabilities.SupportsTextureView) {
                // Unreachable through the API: the frontend refuses glTextureView with
                // GL_INVALID_OPERATION when the backend does not advertise GL_ARB_texture_view,
                // and DirectGLES only advertises it when this capability is set.
                MGLOG_E_ONCE("Texture view %u reached the backend on a driver without "
                             "EXT/OES_texture_view.",
                             stateTextureObject->GetExternalIndex());
                return;
            }

            // Deliberately a by-VALUE copy of the SharedPtr: SyncTextureObjectToBackend hands back
            // a reference INTO the open-addressed registry map, and the params/sampler syncs below
            // (plus any nested growth) can rehash it out from under a reference.
            const SharedPtr<BackendTextureObject> storageBackendObject =
                SyncTextureObjectToBackend(storageObject, m_imageBindableStorageRequired);
            if (!storageBackendObject) {
                MGLOG_E_ONCE("Failed to sync the storage texture of view %u.",
                             stateTextureObject->GetExternalIndex());
                return;
            }
            const Uint storageBackendTextureId = storageBackendObject->GetBackendTextureId();
            if (storageBackendTextureId == 0) {
                MGLOG_D("Storage texture of view %u has no ES name yet.",
                        stateTextureObject->GetExternalIndex());
                return;
            }
            if (m_isInitialized && m_viewSourceBackendTextureId == storageBackendTextureId) {
                StampViewSyncKeys(stateTextureObject);
                return;
            }
            // Either the first sync, or the storage was re-minted underneath us. A name that has
            // already been through glTextureView cannot be viewed again, so start from a fresh
            // one (this also scrubs the binding caches and bumps the FBO attachment generation).
            RecreateBackendTexture();

            GLenum glInternalFormat = 0;
            GLenum glFormat = 0;
            GLenum glType = 0;
            TextureImpl::GenerateTextureFormatInfo(stateTextureObject->GetFormat(), &glInternalFormat, &glFormat,
                                                   &glType, stateTextureObject->GetTarget());
            const GLenum target = ConvertTextureTargetToBackendGLEnum(stateTextureObject->GetTarget());

            DebugImpl::ErrorLopper::Clear();
            ResolveTextureViewEntryPoint()(m_backendTextureId, target, storageBackendTextureId, glInternalFormat,
                                           stateTextureObject->GetViewMinLevel(),
                                           stateTextureObject->GetViewNumLevels(),
                                           stateTextureObject->GetViewMinLayer(),
                                           stateTextureObject->GetViewNumLayers());
            const GLenum error = g_GLESFuncs.glGetError();
            if (error != GL_NO_ERROR) {
                MGLOG_E_ONCE("glTextureView(view=%u target=%s origtexture=%u internalformat=%s levels=[%u,%u) "
                             "layers=[%u,%u)) failed: %s",
                             m_backendTextureId, MG_Util::ConvertGLEnumToString(target).c_str(),
                             storageBackendTextureId, MG_Util::ConvertGLEnumToString(glInternalFormat).c_str(),
                             stateTextureObject->GetViewMinLevel(),
                             stateTextureObject->GetViewMinLevel() + stateTextureObject->GetViewNumLevels(),
                             stateTextureObject->GetViewMinLayer(),
                             stateTextureObject->GetViewMinLayer() + stateTextureObject->GetViewNumLayers(),
                             MG_Util::ConvertGLEnumToString(error).c_str());
                return;
            }

            m_viewSourceBackendTextureId = storageBackendTextureId;
            m_isInitialized = true;
            // A view's storage is immutable by construction (its origtexture had to be), which is
            // what keeps the respecify paths away from this name.
            m_backendStorageImmutable = true;
            const auto baseSize = stateTextureObject->GetBaseSize();
            m_prevTextureInfo = {stateTextureObject->GetFormat(),
                                 static_cast<SizeT>(baseSize.x()),
                                 static_cast<SizeT>(baseSize.y()),
                                 static_cast<SizeT>(baseSize.z()),
                                 static_cast<SizeT>(stateTextureObject->GetViewNumLevels()),
                                 0,
                                 stateTextureObject->GetSamples(),
                                 stateTextureObject->HasFixedSampleLocations()};
            MGLOG_D("Texture view %u (ES %u) now views storage texture %u (ES %u), levels [%u,%u) layers [%u,%u)",
                    stateTextureObject->GetExternalIndex(), m_backendTextureId, storageObject->GetExternalIndex(),
                    storageBackendTextureId, stateTextureObject->GetViewMinLevel(),
                    stateTextureObject->GetViewMinLevel() + stateTextureObject->GetViewNumLevels(),
                    stateTextureObject->GetViewMinLayer(),
                    stateTextureObject->GetViewMinLayer() + stateTextureObject->GetViewNumLayers());
            StampViewSyncKeys(stateTextureObject);
        }

        void BackendTextureObject::SyncMipmapsToBackend(
            const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject) {
            if (!stateTextureObject) {
                MGLOG_E_ONCE("State texture object is null, cannot sync to backend.");
                return;
            }

            // A texture created by glTextureView owns no storage: the levels, the format and
            // every texel belong to the texture it views, and this name only has to be made to
            // ALIAS them. Everything below - storage allocation, respecification, per-level
            // uploads - would be re-doing the storage texture's work on the wrong name.
            if (stateTextureObject->IsTextureView()) {
                SyncTextureViewToBackend(stateTextureObject);
                return;
            }

#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif

            // First-level clean gate (see the member comment): three version compares and no
            // virtual shape walk. Every mutation the slower probe below would catch bumps one of
            // the keys - shape via the context's sampling-resolution generation (coarse: any
            // texture's shape churn re-opens every gate, which only costs a fall-through to the
            // probe), CPU pixels via the content version, samples/fixed-locations via the params
            // version - and backend-side storage resets clear m_isInitialized. Restricted to
            // Mipmap storage like the probe fast path: a buffer texture's backing store can move
            // without any of these keys noticing.
            if (m_isInitialized && m_syncedShapeContextId != 0 && MG_State::pGLContext &&
                m_syncedShapeContextId == MG_State::pGLContext->GetTextureContextId() &&
                m_syncedShapeGeneration == MG_State::pGLContext->GetSamplingResolutionGeneration() &&
                m_syncedContentVersion == stateTextureObject->GetContentVersion() &&
                m_syncedShapeParamsVersion == stateTextureObject->GetTextureParamsVersion() &&
                stateTextureObject->GetStorageType() == TextureStorageType::Mipmap) {
                return;
            }

            MGLOG_D("Syncing texture mipmaps with backend ID %u to backend for state ID %u", m_backendTextureId,
                    stateTextureObject->GetExternalIndex());

            GLenum target = ConvertTextureTargetToBackendGLEnum(stateTextureObject->GetTarget());
            auto targetInternal = stateTextureObject->GetTarget();
            MGLOG_D("    Texture target for syncing is %s",
                    MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
            if (!IsSupportedTextureTarget(targetInternal)) {
                MGLOG_E_ONCE("    Texture target %s is not supported, skipping.",
                        MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
                return;
            }

            // The texture needs to be regenerated completely with glTexImage* calls if:
            // 1. Not initialized
            // 2. InternalFormat changed
            // 3. Size changed
            // 4. Mipmap levels changed

            // IsComplete() is the sampling predicate, and it calls a chain whose lower levels are
            // undefined incomplete - which is what a top-down build (upload level N, then level 0)
            // and ARB_clear_texture's conformance cases both produce. Bailing out on that shape
            // left the backend name with no levels whatsoever, so the level that WAS defined could
            // never be sampled or read back. Sync whenever some level holds an image; the per-level
            // loops below skip the degenerate ones individually.
            if (!stateTextureObject->IsComplete() && !HasAnyDefinedMipmapLevel(stateTextureObject.get())) {
                MGLOG_D("Texture object with ID: %u has no defined image level, skipping sync.",
                        stateTextureObject->GetExternalIndex());
                return;
            }

            // Fast path: a fully-synced mipmap texture is the common per-draw case.
            // SyncNeccessaryTextures re-syncs every bound texture each draw, and the
            // scratch Bind below targets the temp unit - which sequential distinct
            // textures thrash, forcing a real glBindTexture per texture per draw. When
            // nothing needs uploading, skip the bind + upload machinery entirely;
            // BindCurrentTextures() re-establishes the real sampling bindings regardless.
            // The content-version stamp short-circuits before any shape probing: it
            // bumps on every CPU-side pixel mutation, so an unchanged stamp plus an
            // unchanged shape means no level can be dirty. Shape stays a separate
            // compare because a NULL-data glTexImage changes it without touching the
            // content version.
            if (m_isInitialized && stateTextureObject->GetStorageType() == TextureStorageType::Mipmap &&
                m_syncedContentVersion != 0 &&
                m_syncedContentVersion == stateTextureObject->GetContentVersion()) {
                auto* mipmapObject =
                    static_cast<MG_State::GLState::TextureObjectMipmap*>(stateTextureObject.get());
                const auto probeBaseSize = stateTextureObject->GetBaseSize();
                StateTextureBasicInfo probe = {stateTextureObject->GetFormat(),
                                               static_cast<SizeT>(probeBaseSize.x()),
                                               static_cast<SizeT>(probeBaseSize.y()),
                                               static_cast<SizeT>(probeBaseSize.z()),
                                               static_cast<SizeT>(mipmapObject->GetMipmapLevelCount()),
                                               0,
                                               stateTextureObject->GetSamples(),
                                               stateTextureObject->HasFixedSampleLocations()};
                if (probe == m_prevTextureInfo) {
                    MGLOG_D("Texture ID %u already fully synced, skipping scratch bind + upload.",
                            m_backendTextureId);
                    // The probe just proved "fully synced" from the real state, so the cheap
                    // gate may be (re)stamped here: the coarse generation only ever goes stale
                    // from OTHER textures' churn, and this draw re-validated this one.
                    if (MG_State::pGLContext) {
                        m_syncedShapeContextId = MG_State::pGLContext->GetTextureContextId();
                        m_syncedShapeGeneration = MG_State::pGLContext->GetSamplingResolutionGeneration();
                        m_syncedShapeParamsVersion = stateTextureObject->GetTextureParamsVersion();
                    }
                    return;
                }
            }

            Bind(target);
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error: %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });
            const auto baseSize = stateTextureObject->GetBaseSize();
            StateTextureBasicInfo currentTextureInfo = {stateTextureObject->GetFormat(),
                                                        static_cast<SizeT>(baseSize.x()),
                                                        static_cast<SizeT>(baseSize.y()),
                                                        static_cast<SizeT>(baseSize.z()),
                                                        0,
                                                        0,
                                                        stateTextureObject->GetSamples(),
                                                        stateTextureObject->HasFixedSampleLocations()};
            switch (stateTextureObject->GetStorageType()) {
            case TextureStorageType::Mipmap: {
                auto* textureMipmapObject =
                    static_cast<MG_State::GLState::TextureObjectMipmap*>(stateTextureObject.get());
                const auto mipmapCount = textureMipmapObject->GetMipmapLevelCount();
                currentTextureInfo.mipmapLevels = mipmapCount;

                Bool needsRegeneration = !m_isInitialized || (currentTextureInfo != m_prevTextureInfo);
                if (needsRegeneration && m_backendStorageImmutable) {
                    RecreateBackendTexture();
                    Bind(target);
                }

                // Only a texture that is actually image-bound pays for the widening: it doubles
                // or quadruples the storage, and RequireImageBindableStorage is sticky, so a
                // texture that is merely sampled keeps its narrow format for life. See
                // TextureImpl::GetImageBindableStorageWidening for what widens and why.
                const TextureImpl::ImageBindableStorageWidening imageWidening =
                    m_imageBindableStorageRequired
                        ? TextureImpl::GetImageBindableStorageWidening(textureMipmapObject->GetFormat())
                        : TextureImpl::ImageBindableStorageWidening{};

                const Bool canAppendMipmaps =
                    m_isInitialized &&
                    !m_imageBindableStorageRequired &&
                    !stateTextureObject->IsImmutable() &&
                    currentTextureInfo.internalFormat == m_prevTextureInfo.internalFormat &&
                    currentTextureInfo.width == m_prevTextureInfo.width &&
                    currentTextureInfo.height == m_prevTextureInfo.height &&
                    currentTextureInfo.depth == m_prevTextureInfo.depth &&
                    currentTextureInfo.bufferExternalIndex == m_prevTextureInfo.bufferExternalIndex &&
                    currentTextureInfo.samples == m_prevTextureInfo.samples &&
                    currentTextureInfo.fixedSampleLocations == m_prevTextureInfo.fixedSampleLocations &&
                    currentTextureInfo.mipmapLevels > m_prevTextureInfo.mipmapLevels &&
                    !TextureImpl::IsMultisampleTextureTarget(targetInternal);

                MGLOG_D("%s: Got texture info: %dx%dx%d, mips %d, format %s", __func__, baseSize.x(), baseSize.y(),
                        baseSize.z(), mipmapCount,
                        MG_Util::ConvertTextureInternalFormatToString(textureMipmapObject->GetFormat()).c_str());

                if (canAppendMipmaps) {
                    MGLOG_D("Texture mip count increased for backend ID %u, appending levels %zu..%zu",
                            m_backendTextureId, m_prevTextureInfo.mipmapLevels, mipmapCount - 1);

                    GLenum glInternalFormat, glType, glFormat;
                    TextureImpl::GenerateTextureFormatInfo(textureMipmapObject->GetFormat(), &glInternalFormat,
                                                           &glFormat, &glType, targetInternal);

                    const auto& uploadTargets = textureMipmapObject->GetUploadTargets();
                    ScopedDefaultUnpackState unpackState;
                    for (auto& uploadTarget : uploadTargets) {
                        for (SizeT level = m_prevTextureInfo.mipmapLevels; level < mipmapCount; ++level) {
                            auto levelTexelSize = textureMipmapObject->GetMipmapTexelSize(uploadTarget, level);
                            // A level the application never defined reads back as {0, 0, 0}; now that a
                            // sparse chain is synced rather than skipped whole, leave those undefined on
                            // the driver instead of giving the name a 0x0 image at that index.
                            if (levelTexelSize.x() <= 0 || levelTexelSize.y() <= 0 || levelTexelSize.z() <= 0) {
                                textureMipmapObject->MarkStorageDirty(uploadTarget, level, false);
                                continue;
                            }
                            auto levelByteSize = textureMipmapObject->GetMipmapByteSize(uploadTarget, level);
                            bool levelDirty = textureMipmapObject->IsStorageDirty(uploadTarget, level);
                            auto glUploadTarget = ConvertTextureUploadTargetToBackendGLEnum(uploadTarget);
                            auto* pData = (levelDirty && levelByteSize != 0)
                                              ? textureMipmapObject->MapMipmapData(uploadTarget, level)
                                              : nullptr;
                            Vector<Float> convertedUploadData;
                            Vector<Uint8> widenedUploadData;
                            const void* uploadData = PrepareFallbackUpload(
                                textureMipmapObject->GetFormat(), targetInternal, levelTexelSize, pData,
                                levelByteSize, glType, convertedUploadData, widenedUploadData);
                            Vector<Uint8> packedUploadData;
                            uploadData = PreparePackedNormUpload(textureMipmapObject->GetFormat(), levelTexelSize,
                                                                 uploadData, levelByteSize, &glType, packedUploadData);

                            DebugImpl::ErrorLopper::Clear();
                            BufferImpl::BindPixelUnpackBufferId(0); // no-op once the resting 0 state is pinned
                            const IntVec3 uploadSize =
                                GetBackendUploadSize(stateTextureObject->GetTarget(), levelTexelSize);
                            switch (MapToBackendTextureTarget(stateTextureObject->GetTarget())) {
                            case TextureTarget::Texture2D:
                            case TextureTarget::TextureCubeMap:
                                g_GLESFuncs.glTexImage2D(
                                    glUploadTarget, static_cast<GLint>(level), (GLint)glInternalFormat,
                                    static_cast<GLsizei>(uploadSize.x()), static_cast<GLsizei>(uploadSize.y()),
                                    0, glFormat, glType, uploadData);
                                break;
                            case TextureTarget::Texture3D:
                            case TextureTarget::Texture2DArray:
                            // ES 3.2 has GL_TEXTURE_CUBE_MAP_ARRAY natively and it stores exactly
                            // like a 2D array whose depth is 6 * the cube count.
                            case TextureTarget::TextureCubeMapArray:
                                g_GLESFuncs.glTexImage3D(
                                    glUploadTarget, static_cast<GLint>(level), (GLint)glInternalFormat,
                                    static_cast<GLsizei>(uploadSize.x()), static_cast<GLsizei>(uploadSize.y()),
                                    static_cast<GLsizei>(uploadSize.z()), 0, glFormat, glType, uploadData);
                                break;
                            default:
                                MGLOG_E_ONCE("Unhandled texture target %s",
                                        MG_Util::ConvertTextureTargetToString(stateTextureObject->GetTarget()).c_str());
                                break;
                            }
                            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__,
                                                          glUploadTarget, glInternalFormat, glFormat, glType,
                                                          pData](GLenum err) {
                                MGLOG_D("%s(%s:%d) ES error: %s. glTexImage*: target=%s, internalformat=%s, format=%s, "
                                        "type=%s, pixels=%p",
                                        func, file, line, MG_Util::ConvertGLEnumToString(err).c_str(),
                                        MG_Util::ConvertGLEnumToString(glUploadTarget).c_str(),
                                        MG_Util::ConvertGLEnumToString(glInternalFormat).c_str(),
                                        MG_Util::ConvertGLEnumToString(glFormat).c_str(),
                                        MG_Util::ConvertGLEnumToString(glType).c_str(), pData);
                            });
                            textureMipmapObject->MarkStorageDirty(uploadTarget, level, false);
                        }
                    }
                    needsRegeneration = false;
                }

                if (needsRegeneration) {
                    MGLOG_D("Texture state changed significantly or not initialized, regenerating texture with ID: %u",
                            m_backendTextureId);

                    // Regenerate all mipmap levels
                    GLenum glInternalFormat, glType, glFormat;
                    TextureImpl::GenerateTextureFormatInfo(textureMipmapObject->GetFormat(), &glInternalFormat,
                                                           &glFormat, &glType, targetInternal);
                    ApplyImageBindableStorageWidening(imageWidening, &glInternalFormat, &glFormat, &glType);

                    const auto& uploadTargets = textureMipmapObject->GetUploadTargets();
                    if (TextureImpl::IsMultisampleTextureTarget(targetInternal)) {
                        DebugImpl::ErrorLopper::Clear();
                        BufferImpl::BindPixelUnpackBufferId(0); // no-op once the resting 0 state is pinned
                        // The frontend validates against the count MobileGL advertises, which can
                        // exceed what the driver takes for this format (Adreno: GL_MAX_SAMPLES 4,
                        // GL_MAX_INTEGER_SAMPLES 1). Clamp the ES call - and only the ES call:
                        // stateTextureObject keeps the requested count so GL_TEXTURE_SAMPLES and
                        // framebuffer completeness still report what the application asked for.
                        const auto backendSamples = static_cast<GLsizei>(ClampSamplesToBackendSupport(
                            GetFormatCapabilityTargetIndex(targetInternal), textureMipmapObject->GetFormat(),
                            glFormat, static_cast<Int>(stateTextureObject->GetSamples())));
                        // ES 3.1 8.19 requires width/height (and depth, for the array target) >= 1,
                        // so a degenerate size has nothing to allocate and must not reach the
                        // driver. The frontend deallocates such an image rather than defining it
                        // (GL 4.6 core 8.8), so this is belt and braces for any path that still
                        // syncs one.
                        const Bool hasAllocatableSize =
                            baseSize.x() >= 1 && baseSize.y() >= 1 &&
                            (targetInternal != TextureTarget::Texture2DMultisampleArray || baseSize.z() >= 1);
                        if (!hasAllocatableSize) {
                            MGLOG_D("Skipping multisample storage for texture %u: degenerate size (%d, %d, %d)",
                                    m_backendTextureId, baseSize.x(), baseSize.y(), baseSize.z());
                        } else {
                            switch (targetInternal) {
                            case TextureTarget::Texture2DMultisample:
                                g_GLESFuncs.glTexStorage2DMultisample(
                                    target, backendSamples, glInternalFormat,
                                    static_cast<GLsizei>(baseSize.x()), static_cast<GLsizei>(baseSize.y()),
                                    stateTextureObject->HasFixedSampleLocations() ? GL_TRUE : GL_FALSE);
                                break;
                            case TextureTarget::Texture2DMultisampleArray:
                                g_GLESFuncs.glTexStorage3DMultisample(
                                    target, backendSamples, glInternalFormat,
                                    static_cast<GLsizei>(baseSize.x()), static_cast<GLsizei>(baseSize.y()),
                                    static_cast<GLsizei>(baseSize.z()),
                                    stateTextureObject->HasFixedSampleLocations() ? GL_TRUE : GL_FALSE);
                                break;
                            default:
                                MOBILEGL_ASSERT(false, "Unexpected multisample target: %d",
                                                static_cast<Int>(targetInternal));
                                break;
                            }
                            m_backendStorageImmutable = true;
                        }
                        // The one storage branch that cleared the ES error queue without ever
                        // draining it again, so anything this call raised was left for an
                        // unrelated later query to trip over. Paired with its two siblings now.
                        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__, target,
                                                      glInternalFormat, backendSamples](GLenum err) {
                            MGLOG_D("%s(%s:%d) ES error: %s. glTexStorage*Multisample: target=%s, internalformat=%s, "
                                    "samples=%d",
                                    func, file, line, MG_Util::ConvertGLEnumToString(err).c_str(),
                                    MG_Util::ConvertGLEnumToString(target).c_str(),
                                    MG_Util::ConvertGLEnumToString(glInternalFormat).c_str(),
                                    static_cast<Int>(backendSamples));
                        });
                        for (const auto& uploadTarget : uploadTargets) {
                            for (SizeT level = 0; level < mipmapCount; ++level) {
                                textureMipmapObject->MarkStorageDirty(uploadTarget, level, false);
                            }
                        }
                    } else if (stateTextureObject->IsImmutable() || m_imageBindableStorageRequired) {
                        DebugImpl::ErrorLopper::Clear();
                        BufferImpl::BindPixelUnpackBufferId(0); // no-op once the resting 0 state is pinned
                        const IntVec3 storageSize = GetBackendUploadSize(targetInternal, baseSize);
                        switch (MapToBackendTextureTarget(targetInternal)) {
                        case TextureTarget::Texture2D:
                        case TextureTarget::TextureCubeMap:
                            g_GLESFuncs.glTexStorage2D(target, static_cast<GLsizei>(mipmapCount), glInternalFormat,
                                                       static_cast<GLsizei>(storageSize.x()),
                                                       static_cast<GLsizei>(storageSize.y()));
                            break;
                        case TextureTarget::Texture3D:
                        case TextureTarget::Texture2DArray:
                        case TextureTarget::TextureCubeMapArray:
                            g_GLESFuncs.glTexStorage3D(target, static_cast<GLsizei>(mipmapCount), glInternalFormat,
                                                       static_cast<GLsizei>(storageSize.x()),
                                                       static_cast<GLsizei>(storageSize.y()),
                                                       static_cast<GLsizei>(storageSize.z()));
                            break;
                        default:
                            MGLOG_E_ONCE("Unhandled immutable texture target %s",
                                    MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
                            break;
                        }
                        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__, target,
                                                      glInternalFormat](GLenum err) {
                            MGLOG_D("%s(%s:%d) ES error: %s. glTexStorage*: target=%s, internalformat=%s", func,
                                    file, line, MG_Util::ConvertGLEnumToString(err).c_str(),
                                    MG_Util::ConvertGLEnumToString(target).c_str(),
                                    MG_Util::ConvertGLEnumToString(glInternalFormat).c_str());
                        });
                        m_backendStorageImmutable = true;

                        ScopedDefaultUnpackState unpackState;
                        for (auto& uploadTarget : uploadTargets) {
                            for (SizeT level = 0; level < mipmapCount; ++level) {
                                auto levelByteSize = textureMipmapObject->GetMipmapByteSize(uploadTarget, level);
                                const bool levelDirty = textureMipmapObject->IsStorageDirty(uploadTarget, level);
                                if (levelDirty && levelByteSize != 0) {
                                    auto levelTexelSize =
                                        textureMipmapObject->GetMipmapTexelSize(uploadTarget, level);
                                    auto glUploadTarget = ConvertTextureUploadTargetToBackendGLEnum(uploadTarget);
                                    auto* pData = textureMipmapObject->MapMipmapData(uploadTarget, level);
                                    Vector<Float> convertedUploadData;
                                    Vector<Uint8> widenedUploadData;
                                    const void* uploadData = PrepareFallbackUpload(
                                        textureMipmapObject->GetFormat(), targetInternal, levelTexelSize, pData,
                                        levelByteSize, glType, convertedUploadData, widenedUploadData);
                                    Vector<Uint8> packedUploadData;
                                    uploadData =
                                        PreparePackedNormUpload(textureMipmapObject->GetFormat(), levelTexelSize,
                                                                uploadData, levelByteSize, &glType, packedUploadData);
                                    Vector<Uint8> imageWidenedUploadData;
                                    uploadData = PrepareImageWidenedUpload(imageWidening, levelTexelSize, uploadData,
                                                                           levelByteSize, imageWidenedUploadData);

                                    DebugImpl::ErrorLopper::Clear();
                                    BufferImpl::BindPixelUnpackBufferId(0); // no-op once the resting 0 state is pinned
                                    const IntVec3 uploadSize =
                                        GetBackendUploadSize(targetInternal, levelTexelSize);
                                    switch (MapToBackendTextureTarget(targetInternal)) {
                                    case TextureTarget::Texture2D:
                                    case TextureTarget::TextureCubeMap:
                                        g_GLESFuncs.glTexSubImage2D(
                                            glUploadTarget, static_cast<GLint>(level), 0, 0,
                                            static_cast<GLsizei>(uploadSize.x()),
                                            static_cast<GLsizei>(uploadSize.y()), glFormat, glType, uploadData);
                                        break;
                                    case TextureTarget::Texture3D:
                                    case TextureTarget::Texture2DArray:
                                    case TextureTarget::TextureCubeMapArray:
                                        g_GLESFuncs.glTexSubImage3D(
                                            glUploadTarget, static_cast<GLint>(level), 0, 0, 0,
                                            static_cast<GLsizei>(uploadSize.x()),
                                            static_cast<GLsizei>(uploadSize.y()),
                                            static_cast<GLsizei>(uploadSize.z()), glFormat, glType, uploadData);
                                        break;
                                    default:
                                        break;
                                    }
                                    DebugImpl::ErrorLopper::Loop(
                                        [file = __FILE__, line = __LINE__, func = __func__, glUploadTarget,
                                         glFormat, glType, pData](GLenum err) {
                                            MGLOG_D("%s(%s:%d) ES error: %s. glTexSubImage*: target=%s, format=%s, "
                                                    "type=%s, pixels=%p",
                                                    func, file, line, MG_Util::ConvertGLEnumToString(err).c_str(),
                                                    MG_Util::ConvertGLEnumToString(glUploadTarget).c_str(),
                                                    MG_Util::ConvertGLEnumToString(glFormat).c_str(),
                                                    MG_Util::ConvertGLEnumToString(glType).c_str(), pData);
                                        });
                                }
                                textureMipmapObject->MarkStorageDirty(uploadTarget, level, false);
                            }
                        }
                    } else {
                        m_backendStorageImmutable = false;
                        ScopedDefaultUnpackState unpackState;
                        for (auto& uploadTarget : uploadTargets) {
                            for (SizeT level = 0; level < mipmapCount; ++level) {
                                auto levelTexelSize = textureMipmapObject->GetMipmapTexelSize(uploadTarget, level);
                                // See the append-mips loop: an undefined level stays undefined on the
                                // driver rather than becoming a 0x0 image.
                                if (levelTexelSize.x() <= 0 || levelTexelSize.y() <= 0 ||
                                    levelTexelSize.z() <= 0) {
                                    textureMipmapObject->MarkStorageDirty(uploadTarget, level, false);
                                    continue;
                                }
                                auto levelByteSize = textureMipmapObject->GetMipmapByteSize(uploadTarget, level);
                                bool levelDirty = textureMipmapObject->IsStorageDirty(uploadTarget, level);
                                auto glUploadTarget = ConvertTextureUploadTargetToBackendGLEnum(uploadTarget);
                                auto* pData = (levelDirty && levelByteSize != 0)
                                                  ? textureMipmapObject->MapMipmapData(uploadTarget, level)
                                                  : nullptr;
                                Vector<Float> convertedUploadData;
                                Vector<Uint8> widenedUploadData;
                                const void* uploadData = PrepareFallbackUpload(
                                    textureMipmapObject->GetFormat(), targetInternal, levelTexelSize, pData,
                                    levelByteSize, glType, convertedUploadData, widenedUploadData);
                                Vector<Uint8> packedUploadData;
                                uploadData =
                                    PreparePackedNormUpload(textureMipmapObject->GetFormat(), levelTexelSize,
                                                            uploadData, levelByteSize, &glType, packedUploadData);
                                MGLOG_D("%s: target: %s: syncing mip %d: %dx%dx%d, byteSize = %d, pData = %p, "
                                        "levelDirty = %s",
                                        __func__, MG_Util::ConvertTextureUploadTargetToString(uploadTarget).c_str(),
                                        level, levelTexelSize.x(), levelTexelSize.y(), levelTexelSize.z(),
                                        levelByteSize, pData, levelDirty ? "true" : "false");

                                DebugImpl::ErrorLopper::Clear();
                                BufferImpl::BindPixelUnpackBufferId(0); // no-op once the resting 0 state is pinned
                                auto textureTarget = stateTextureObject->GetTarget();
                                const IntVec3 uploadSize = GetBackendUploadSize(textureTarget, levelTexelSize);
                                switch (MapToBackendTextureTarget(textureTarget)) {
                                case TextureTarget::Texture2D:
                                case TextureTarget::TextureCubeMap: {
                                    g_GLESFuncs.glTexImage2D(
                                        glUploadTarget, static_cast<GLint>(level), (GLint)glInternalFormat,
                                        static_cast<GLsizei>(uploadSize.x()),
                                        static_cast<GLsizei>(uploadSize.y()), 0, glFormat, glType, uploadData);
                                    break;
                                }
                                case TextureTarget::Texture3D:
                                case TextureTarget::Texture2DArray:
                                case TextureTarget::TextureCubeMapArray: {
                                    g_GLESFuncs.glTexImage3D(
                                        glUploadTarget, static_cast<GLint>(level), (GLint)glInternalFormat,
                                        static_cast<GLsizei>(uploadSize.x()),
                                        static_cast<GLsizei>(uploadSize.y()),
                                        static_cast<GLsizei>(uploadSize.z()), 0, glFormat, glType, uploadData);
                                    break;
                                }
                                default: {
                                    MGLOG_E_ONCE("Unhandled texture target %s",
                                            MG_Util::ConvertTextureTargetToString(textureTarget).c_str());
                                }
                                }
                                DebugImpl::ErrorLopper::Loop(
                                    [file = __FILE__, line = __LINE__, func = __func__, glUploadTarget,
                                     glInternalFormat, glFormat, glType, pData](GLenum err) {
                                        MGLOG_D("%s(%s:%d) ES error: %s. glTexImage*: target=%s, internalformat=%s, "
                                                "format=%s, type=%s, pixels=%p",
                                                func, file, line, MG_Util::ConvertGLEnumToString(err).c_str(),
                                                MG_Util::ConvertGLEnumToString(glUploadTarget).c_str(),
                                                MG_Util::ConvertGLEnumToString(glInternalFormat).c_str(),
                                                MG_Util::ConvertGLEnumToString(glFormat).c_str(),
                                                MG_Util::ConvertGLEnumToString(glType).c_str(), pData);
                                    });
                                MGLOG_D("Regenerated mipmap level %d for texture with ID: %u", level,
                                        m_backendTextureId);
                                textureMipmapObject->MarkStorageDirty(uploadTarget, level, false);
                            }
                        }
                    }

                    m_isInitialized = true;
                }

                { // Update all dirty mipmap levels
                    if (TextureImpl::IsMultisampleTextureTarget(targetInternal)) {
                        const auto& uploadTargets = textureMipmapObject->GetUploadTargets();
                        for (const auto& uploadTarget : uploadTargets) {
                            for (SizeT level = 0; level < mipmapCount; ++level) {
                                if (textureMipmapObject->IsStorageDirty(uploadTarget, level)) {
                                    textureMipmapObject->MarkStorageDirty(uploadTarget, level, false);
                                }
                            }
                        }
                        break;
                    }

                    const auto mipmapCount = textureMipmapObject->GetMipmapLevelCount();
                    GLenum glInternalFormat, glType, glFormat;
                    TextureImpl::GenerateTextureFormatInfo(textureMipmapObject->GetFormat(), &glInternalFormat,
                                                           &glFormat, &glType, targetInternal);
                    // The storage this level is being written into was widened when it was minted
                    // (see above), so the transfer pair has to describe the carrier here too - ES
                    // requires glTexSubImage's `format` to match the storage's base internal
                    // format, so a GL_RG upload into a GL_RGBA32F image is GL_INVALID_OPERATION.
                    ApplyImageBindableStorageWidening(imageWidening, &glInternalFormat, &glFormat, &glType);
                    const auto& uploadTargets = textureMipmapObject->GetUploadTargets();
                    ScopedDefaultUnpackState unpackState;
                    for (auto& uploadTarget : uploadTargets) {
                        for (SizeT level = 0; level < mipmapCount; ++level) {
                            if (!textureMipmapObject->IsStorageDirty(uploadTarget, level)) {
                                continue;
                            }

                            auto byteSize = textureMipmapObject->GetMipmapByteSize(uploadTarget, level);
                            if (byteSize == 0) {
                                MGLOG_D("Mipmap level %d has no data, skipping update.", level);
                                continue;
                            }

                            if (level > 0)
                                MGLOG_D("%s: Updating dirty mip %d for texture ID %u, size: %dx%d, "
                                        "byteSize: %d",
                                        __func__, level, m_backendTextureId,
                                        textureMipmapObject->GetMipmapTexelSize(uploadTarget, level).x(),
                                        textureMipmapObject->GetMipmapTexelSize(uploadTarget, level).y(), byteSize);

                            auto glUploadTarget = ConvertTextureUploadTargetToBackendGLEnum(uploadTarget);
                            BufferImpl::BindPixelUnpackBufferId(0); // no-op once the resting 0 state is pinned
                            DebugImpl::ErrorLopper::Loop(
                                [file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                                    MGLOG_D("%s(%s:%d) ES error: %s", func, file, line,
                                            MG_Util::ConvertGLEnumToString(err).c_str());
                                });
                            auto texelSize = textureMipmapObject->GetMipmapTexelSize(uploadTarget, level);
                            const void* mipData = textureMipmapObject->MapMipmapData(uploadTarget, level);
                            Vector<Float> convertedUploadData;
                            Vector<Uint8> widenedUploadData;
                            const void* uploadData = PrepareFallbackUpload(
                                textureMipmapObject->GetFormat(), targetInternal, texelSize, mipData, byteSize,
                                glType, convertedUploadData, widenedUploadData);
                            Vector<Uint8> packedUploadData;
                            uploadData = PreparePackedNormUpload(textureMipmapObject->GetFormat(), texelSize,
                                                                 uploadData, byteSize, &glType, packedUploadData);
                            // Leaves `uploadData` pointing at its own buffer when it fires, which
                            // is exactly what takes the sub-rect fast path below out of play: that
                            // path strides into the SHADOW, and the widened texels are four
                            // components wide where the shadow's are one or two.
                            Vector<Uint8> imageWidenedUploadData;
                            uploadData = PrepareImageWidenedUpload(imageWidening, texelSize, uploadData, byteSize,
                                                                   imageWidenedUploadData);
                            const IntVec3 uploadSize =
                                GetBackendUploadSize(stateTextureObject->GetTarget(), texelSize);
                            // Sub-rect upload: when only a region of the level changed (a
                            // 16x16 sprite in a 1024x512 atlas, the per-frame lightmap) and
                            // the shadow bytes go to the driver unconverted, upload just that
                            // region with UNPACK_ROW_LENGTH striding into the level shadow.
                            // Conversion fallbacks rewrite the whole level into a fresh
                            // buffer, so they stay on the full-level path, as do targets
                            // whose backend upload size differs from the shadow's texel size.
                            const auto dirtyRegion = textureMipmapObject->GetStorageDirtyRegion(uploadTarget, level);
                            const SizeT texelCount = static_cast<SizeT>(texelSize.x()) *
                                                     static_cast<SizeT>(texelSize.y()) *
                                                     static_cast<SizeT>(std::max(texelSize.z(), 1));
                            const Bool subRectEligible =
                                uploadData == mipData && !dirtyRegion.Empty() &&
                                !dirtyRegion.CoversWholeLevel(texelSize) && texelCount > 0 &&
                                byteSize % texelCount == 0 && uploadSize.x() == texelSize.x() &&
                                uploadSize.y() == texelSize.y() &&
                                std::max(uploadSize.z(), 1) == std::max(texelSize.z(), 1);
                            const SizeT bpp = subRectEligible ? byteSize / texelCount : 0;
                            const IntVec3 regionSize = {dirtyRegion.hi.x() - dirtyRegion.lo.x(),
                                                        dirtyRegion.hi.y() - dirtyRegion.lo.y(),
                                                        dirtyRegion.hi.z() - dirtyRegion.lo.z()};
                            const SizeT levelRowBytes = static_cast<SizeT>(texelSize.x()) * bpp;
                            const SizeT levelSliceBytes = static_cast<SizeT>(texelSize.y()) * levelRowBytes;
                            const Uint8* regionPtr =
                                static_cast<const Uint8*>(uploadData) +
                                static_cast<SizeT>(dirtyRegion.lo.z()) * levelSliceBytes +
                                static_cast<SizeT>(dirtyRegion.lo.y()) * levelRowBytes +
                                static_cast<SizeT>(dirtyRegion.lo.x()) * bpp;
                            // Scatter refinement behind the union box: ~100 sprite
                            // writes into an atlas leave a box that spans nearly the
                            // whole level while the touched texels are a few percent of
                            // it. The storage's bounded rect list recovers the true
                            // footprint; each rect is uploaded with the same
                            // ROW_LENGTH striding into the level shadow as the box
                            // path. The storage only hands the list out when its
                            // summed area is materially smaller than the box (0
                            // otherwise), so the extra calls always move fewer bytes.
                            MG_State::GLState::MipmapDirtyRegion
                                dirtyRects[MG_State::GLState::MipmapStorage::kMaxDirtyRects];
                            SizeT dirtyRectCount = 0;
                            if (subRectEligible) {
                                dirtyRectCount = textureMipmapObject->GetStorageDirtyRects(
                                    uploadTarget, level, dirtyRects,
                                    MG_State::GLState::MipmapStorage::kMaxDirtyRects);
                                // The scatter refinement pays only on the client-pointer path,
                                // where fewer bytes mean less driver-side copying. Through the
                                // unpack ring every glTexSubImage is a GPU copy job (Mali), so
                                // ~100 sprite rects become ~100 jobs whose fixed cost dwarfs
                                // the union box's extra bytes - measured +6 ms/frame of GPU
                                // time in MC's animated-atlas ticks. One box, one job.
                                if (BufferImpl::UnpackRingAvailable()) {
                                    dirtyRectCount = 0;
                                }
                            }
                            const auto rectShadowPtr = [&](const MG_State::GLState::MipmapDirtyRegion& rect) {
                                return static_cast<const Uint8*>(uploadData) +
                                       static_cast<SizeT>(rect.lo.z()) * levelSliceBytes +
                                       static_cast<SizeT>(rect.lo.y()) * levelRowBytes +
                                       static_cast<SizeT>(rect.lo.x()) * bpp;
                            };
                            // Unpack-ring staging plan, decided ONCE for whichever branch
                            // below runs: either every glTexSubImage of this level sources
                            // from the ring or none does, so the pixel-unpack binding is
                            // toggled exactly once per level. The plan's three shapes line
                            // up 1:1 with the switch's three branches.
                            //
                            // Regions are repacked TIGHTLY (row length = the region's own
                            // width), which is why the ring path issues no glPixelStorei at
                            // all: the surrounding ScopedDefaultUnpackState already holds
                            // ROW_LENGTH/IMAGE_HEIGHT at 0, which is exactly what a tight
                            // block wants. It also stages strictly fewer bytes than the
                            // client-pointer path makes the driver walk, which strides over
                            // the whole level width.
                            UnpackStagingBlock
                                stagingBlocks[MG_State::GLState::MipmapStorage::kMaxDirtyRects];
                            SizeT stagingBlockCount = 0;
                            const Bool ringUsable = BufferImpl::UnpackRingAvailable();
                            if (!ringUsable) {
                                // Nothing to plan: every branch below keeps the client
                                // pointer and its ROW_LENGTH striding, unchanged.
                            } else if (subRectEligible && dirtyRectCount >= 2) {
                                for (SizeT r = 0; r < dirtyRectCount; ++r) {
                                    const auto& rect = dirtyRects[r];
                                    stagingBlocks[r] = {
                                        rectShadowPtr(rect),
                                        static_cast<SizeT>(rect.hi.x() - rect.lo.x()) * bpp,
                                        static_cast<SizeT>(rect.hi.y() - rect.lo.y()),
                                        static_cast<SizeT>(std::max(rect.hi.z() - rect.lo.z(), 1)),
                                        levelRowBytes,
                                        levelSliceBytes,
                                        0};
                                }
                                stagingBlockCount = dirtyRectCount;
                            } else if (subRectEligible) {
                                stagingBlocks[0] = {regionPtr,
                                                    static_cast<SizeT>(regionSize.x()) * bpp,
                                                    static_cast<SizeT>(regionSize.y()),
                                                    static_cast<SizeT>(std::max(regionSize.z(), 1)),
                                                    levelRowBytes,
                                                    levelSliceBytes,
                                                    0};
                                stagingBlockCount = 1;
                            } else if (uploadData == mipData && texelCount > 0 &&
                                       byteSize % texelCount == 0) {
                                // Whole level, shadow bytes verbatim: `byteSize` is exactly
                                // what the driver would have read from the client pointer,
                                // and the shadow is already tightly packed. A CONVERTED
                                // level stays on the pointer path - its buffer's length is
                                // the conversion's, not the shadow's, so nothing here can
                                // size the driver's read of it. The whole-texels check is
                                // the same sanity the sub-rect path applies, and it is what
                                // makes "the shadow's bytes per texel == the transfer's"
                                // hold. (A 1D array's upload size permutes height and depth,
                                // but the texel count and the tight byte order are the same,
                                // so the flat copy still describes what the driver reads.)
                                stagingBlocks[0] = {static_cast<const Uint8*>(uploadData),
                                                    static_cast<SizeT>(byteSize),
                                                    1,
                                                    1,
                                                    static_cast<SizeT>(byteSize),
                                                    static_cast<SizeT>(byteSize),
                                                    0};
                                stagingBlockCount = 1;
                            }
                            const Bool ringStaged =
                                stagingBlockCount > 0 &&
                                StageBlocksIntoUnpackRing(stagingBlocks, stagingBlockCount);
                            if (ringStaged) {
                                BufferImpl::BindPixelUnpackBufferId(BufferImpl::UnpackRingBufferId());
                            }
                            if (MG_Util::PipeStats::Enabled()) {
                                // One emission per (upload target, level) that ships texels;
                                // the switch below turns it into either one union-box job or
                                // dirtyRectCount rect jobs. The box/rect split is counted
                                // separately from the bytes on purpose: SSIM is blind to it
                                // and the +6 ms/frame Mali cliff was a shape regression, not
                                // a byte regression (plan section 7.3).
                                const Bool rectShape = subRectEligible && dirtyRectCount >= 2;
                                Uint64 shippedBytes = 0;
                                if (rectShape) {
                                    for (SizeT r = 0; r < dirtyRectCount; ++r) {
                                        const auto& rect = dirtyRects[r];
                                        shippedBytes += static_cast<Uint64>(rect.hi.x() - rect.lo.x()) *
                                                        static_cast<Uint64>(rect.hi.y() - rect.lo.y()) *
                                                        static_cast<Uint64>(std::max(rect.hi.z() - rect.lo.z(), 1)) *
                                                        static_cast<Uint64>(bpp);
                                    }
                                } else if (subRectEligible) {
                                    shippedBytes = static_cast<Uint64>(regionSize.x()) *
                                                   static_cast<Uint64>(regionSize.y()) *
                                                   static_cast<Uint64>(std::max(regionSize.z(), 1)) *
                                                   static_cast<Uint64>(bpp);
                                } else {
                                    shippedBytes = static_cast<Uint64>(byteSize);
                                }
                                MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageTexture,
                                                             shippedBytes);
                                MG_Util::PipeStats::AddCalls(
                                    MG_Util::PipeStats::CallClass::TextureUploadEmissions, 1);
                                MG_Util::PipeStats::AddCalls(
                                    rectShape ? MG_Util::PipeStats::CallClass::TextureUploadRectEmissions
                                              : MG_Util::PipeStats::CallClass::TextureUploadBoxEmissions,
                                    1);
                                MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::TextureUploadJobs,
                                                             rectShape ? static_cast<Uint64>(dirtyRectCount) : 1u);
                            }
                            switch (MapToBackendTextureTarget(stateTextureObject->GetTarget())) {
                            case TextureTarget::Texture2D:
                            case TextureTarget::TextureCubeMap:
                                if (subRectEligible && dirtyRectCount >= 2) {
                                    if (!ringStaged) g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, texelSize.x());
                                    for (SizeT r = 0; r < dirtyRectCount; ++r) {
                                        const auto& rect = dirtyRects[r];
                                        g_GLESFuncs.glTexSubImage2D(
                                            glUploadTarget, static_cast<GLint>(level), rect.lo.x(),
                                            rect.lo.y(), static_cast<GLsizei>(rect.hi.x() - rect.lo.x()),
                                            static_cast<GLsizei>(rect.hi.y() - rect.lo.y()), glFormat,
                                            glType,
                                            ringStaged ? UnpackRingPixelOffset(stagingBlocks[r].offset)
                                                       : static_cast<const void*>(rectShadowPtr(rect)));
                                    }
                                    // The surrounding ScopedDefaultUnpackState shadow says 0.
                                    if (!ringStaged) g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                                } else if (subRectEligible) {
                                    if (!ringStaged) g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, texelSize.x());
                                    g_GLESFuncs.glTexSubImage2D(
                                        glUploadTarget, static_cast<GLint>(level), dirtyRegion.lo.x(),
                                        dirtyRegion.lo.y(), static_cast<GLsizei>(regionSize.x()),
                                        static_cast<GLsizei>(regionSize.y()), glFormat, glType,
                                        ringStaged ? UnpackRingPixelOffset(stagingBlocks[0].offset)
                                                   : static_cast<const void*>(regionPtr));
                                    // The surrounding ScopedDefaultUnpackState shadow says 0.
                                    if (!ringStaged) g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                                } else {
                                    g_GLESFuncs.glTexSubImage2D(glUploadTarget, static_cast<GLint>(level), 0, 0,
                                                                static_cast<GLsizei>(uploadSize.x()),
                                                                static_cast<GLsizei>(uploadSize.y()), glFormat,
                                                                glType,
                                                                ringStaged
                                                                    ? UnpackRingPixelOffset(stagingBlocks[0].offset)
                                                                    : uploadData);
                                }
                                break;
                            case TextureTarget::Texture3D:
                            case TextureTarget::Texture2DArray:
                            // ES 3.2 has GL_TEXTURE_CUBE_MAP_ARRAY natively and it stores exactly
                            // like a 2D array whose depth is 6 * the cube count.
                            case TextureTarget::TextureCubeMapArray:
                                if (subRectEligible && dirtyRectCount >= 2) {
                                    if (!ringStaged) {
                                        g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, texelSize.x());
                                        g_GLESFuncs.glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, texelSize.y());
                                    }
                                    for (SizeT r = 0; r < dirtyRectCount; ++r) {
                                        const auto& rect = dirtyRects[r];
                                        g_GLESFuncs.glTexSubImage3D(
                                            glUploadTarget, static_cast<GLint>(level), rect.lo.x(),
                                            rect.lo.y(), rect.lo.z(),
                                            static_cast<GLsizei>(rect.hi.x() - rect.lo.x()),
                                            static_cast<GLsizei>(rect.hi.y() - rect.lo.y()),
                                            static_cast<GLsizei>(rect.hi.z() - rect.lo.z()), glFormat,
                                            glType,
                                            ringStaged ? UnpackRingPixelOffset(stagingBlocks[r].offset)
                                                       : static_cast<const void*>(rectShadowPtr(rect)));
                                    }
                                    if (!ringStaged) {
                                        g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                                        g_GLESFuncs.glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
                                    }
                                } else if (subRectEligible) {
                                    if (!ringStaged) {
                                        g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, texelSize.x());
                                        g_GLESFuncs.glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, texelSize.y());
                                    }
                                    g_GLESFuncs.glTexSubImage3D(
                                        glUploadTarget, static_cast<GLint>(level), dirtyRegion.lo.x(),
                                        dirtyRegion.lo.y(), dirtyRegion.lo.z(),
                                        static_cast<GLsizei>(regionSize.x()),
                                        static_cast<GLsizei>(regionSize.y()),
                                        static_cast<GLsizei>(regionSize.z()), glFormat, glType,
                                        ringStaged ? UnpackRingPixelOffset(stagingBlocks[0].offset)
                                                   : static_cast<const void*>(regionPtr));
                                    if (!ringStaged) {
                                        g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                                        g_GLESFuncs.glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
                                    }
                                } else {
                                    g_GLESFuncs.glTexSubImage3D(glUploadTarget, static_cast<GLint>(level), 0, 0, 0,
                                                                static_cast<GLsizei>(uploadSize.x()),
                                                                static_cast<GLsizei>(uploadSize.y()),
                                                                static_cast<GLsizei>(uploadSize.z()), glFormat,
                                                                glType,
                                                                ringStaged
                                                                    ? UnpackRingPixelOffset(stagingBlocks[0].offset)
                                                                    : uploadData);
                                }
                                break;
                            default:
                                MGLOG_E_ONCE("Unhandled texture target %s",
                                        MG_Util::ConvertTextureTargetToString(stateTextureObject->GetTarget()).c_str());
                                break;
                            }
                            if (ringStaged) {
                                // Back to the resting unbound state every other upload site
                                // in this file assumes (their BindPixelUnpackBufferId(0) is
                                // meant to stay a shadow no-op).
                                BufferImpl::BindPixelUnpackBufferId(0);
                            }
                            textureMipmapObject->MarkStorageDirty(uploadTarget, level, false);
                        }
                    }
                }
                break;
            }
            case TextureStorageType::Buffer: {
                auto* textureBufferObject =
                    static_cast<MG_State::GLState::TextureObjectBuffer*>(stateTextureObject.get());
                auto& slot = textureBufferObject->GetBufferBindingSlot();
                auto& buffer = slot.GetBoundObject();
                if (!buffer) {
                    MGLOG_D("Texture buffer object with ID: %u has no bound buffer, skipping sync.",
                            stateTextureObject->GetExternalIndex());
                    return;
                }
                auto bufferIndex = buffer->GetExternalIndex();
                currentTextureInfo.bufferExternalIndex = bufferIndex;

                Bool needsRegeneration = !m_isInitialized || (currentTextureInfo != m_prevTextureInfo);

                // Need to sync texture buffer if not synced yet
                auto* backendBufferResource = BufferImpl::EnsureBufferResource(buffer);
                if (!backendBufferResource || backendBufferResource->id == 0) {
                    MGLOG_E_ONCE("Failed to sync backing buffer for texture buffer with ID: %u",
                            stateTextureObject->GetExternalIndex());
                    return;
                }

                // Bind buffer to texture
                auto backendId = backendBufferResource->id;

                GLenum glInternalFormat, glType, glFormat;
                TextureImpl::GenerateTextureFormatInfo(textureBufferObject->GetFormat(), &glInternalFormat, &glFormat,
                                                       &glType, TextureTarget::TextureBuffer);
                // The view half of the buffer-image SPLIT. A buffer texture has no storage of its
                // own to widen, but the VIEW its format describes can be re-described one
                // component at a time over the same bytes - rg32f over N texels is r32f over 2N -
                // and WidenImageFormatsPass rewrites every access to subscript it that way. Only
                // for a texture that is actually image-bound: a sampled-only buffer texture keeps
                // the format the application asked for (see GetImageBindableBufferSplitFormat).
                //
                // The split goes on a SEPARATE name (m_bufferImageSplitViewId), not on this one.
                // Re-describing the application's own texture also re-describes what a
                // samplerBuffer reading it sees, and the sampler side is not subscript-rewritten -
                // so texelFetch(s, i) started returning component 2i of the base view instead of
                // texel i. rg32f is a legal SAMPLED buffer-texture format in ES 3.2; only the
                // IMAGE binding needs the split, so only the image binding's name carries it.
                const GLenum bufferImageSplitFormat =
                    m_imageBindableStorageRequired
                        ? TextureImpl::GetImageBindableBufferSplitFormat(textureBufferObject->GetFormat())
                        : GL_UNKNOWN_MGL;

                if (needsRegeneration) {
                    // Desktop GL has had buffer textures core since 3.1 and MobileGL advertises a
                    // 4.x context, so glTexBuffer is a legal call the app may make on any driver -
                    // but ES only gained them in 3.2, and g_GLESFuncs.glTexBuffer is simply null
                    // below that without EXT/OES_texture_buffer. Calling it was an unconditional
                    // null dereference. There is no conformant way to refuse the call (it is valid
                    // in the context MobileGL claims), so the texture is left unbacked and the
                    // reason is stated once per object, latched by the flag below. It was parked
                    // at MGLOG_I while the level ordering compiled MGLOG_W out of INFO builds;
                    // W is the correct level and now survives there.
                    if (!AreBufferTexturesSupported()) {
                        if (m_bufferTextureUnsupportedReported) {
                            break;
                        }
                        m_bufferTextureUnsupportedReported = true;
                        MGLOG_W("Texture buffer %u cannot be backed: this ES driver has no buffer "
                                "textures (%s). Every draw sampling it will read zero and every "
                                "shader declaring a samplerBuffer will fail to compile. MobileGL "
                                "still advertises GL_MAX_TEXTURE_BUFFER_SIZE = %d because an "
                                "OpenGL 4.x context may not report 0.",
                                stateTextureObject->GetExternalIndex(), GetBufferTextureTierName(),
                                g_GLESCapabilities.MaxTextureBufferSize);
                        break;
                    }
                    MGLOG_D("Texture state changed significantly or not initialized, regenerating texture buffer with "
                            "ID: %u, buffer ID: %u, buffer size: %zu, format: %s",
                            m_backendTextureId, backendId, buffer->GetSize(),
                            MG_Util::ConvertGLEnumToString(glInternalFormat).c_str());
                    // A texture that names a window of the buffer needs the range form; the
                    // whole-buffer forms report offset 0 and the buffer's current size, which
                    // glTexBuffer expresses more directly (and works where the range entry point
                    // is absent).
                    const SizeT rangeOffset = textureBufferObject->GetBufferRangeOffset();
                    const SizeT rangeSize = textureBufferObject->GetBufferRangeSizeInBytes();
                    // Through CallTexBuffer/CallTexBufferRange rather than g_GLESFuncs directly:
                    // the unsuffixed entry points are the ES 3.2 core spelling, and a driver
                    // whose buffer textures come from EXT/OES_texture_buffer exports the
                    // suffixed ones instead. The dispatchers pick whichever this tier ships.
                    if (rangeOffset == 0 && rangeSize == buffer->GetSize()) {
                        CallTexBuffer(GL_TEXTURE_BUFFER, glInternalFormat, backendId);
                    } else if (!CallTexBufferRange(GL_TEXTURE_BUFFER, glInternalFormat, backendId,
                                                   static_cast<GLintptr>(rangeOffset),
                                                   static_cast<GLsizeiptr>(rangeSize))) {
                        MGLOG_W_ONCE("Texture buffer %u names a sub-range but the driver has no "
                                "glTexBufferRange; binding the whole buffer instead",
                                stateTextureObject->GetExternalIndex());
                        CallTexBuffer(GL_TEXTURE_BUFFER, glInternalFormat, backendId);
                    }
                    DebugImpl::ErrorLopper::Loop(
                        [file = __FILE__, line = __LINE__, func = __func__, glInternalFormat, backendId](GLenum err) {
                            MGLOG_D("%s(%s:%d) glTexBuffer(format=%s, buffer=%u) ES error: %s",
                                    func, file, line, MG_Util::ConvertGLEnumToString(glInternalFormat).c_str(),
                                    backendId, MG_Util::ConvertGLEnumToString(err).c_str());
                        });

                    // The image half of the SPLIT, on its own name over the same buffer. Minted
                    // lazily - only a texture that is both image-bound AND holds a format with no
                    // ESSL image spelling ever gets one - and re-pointed here, in the same
                    // regeneration gate as the view above, so the two never describe different
                    // buffers or different windows of one.
                    if (bufferImageSplitFormat != GL_UNKNOWN_MGL) {
                        if (m_bufferImageSplitViewId == 0) {
                            g_GLESFuncs.glGenTextures(1, &m_bufferImageSplitViewId);
                        }
                        if (m_bufferImageSplitViewId == 0) {
                            MGLOG_E_ONCE("Failed to generate the buffer-image split view for texture %u; "
                                         "its image binding will read the unsplit view.",
                                         stateTextureObject->GetExternalIndex());
                        } else {
                            g_GLESFuncs.glBindTexture(GL_TEXTURE_BUFFER, m_bufferImageSplitViewId);
                            if (rangeOffset == 0 && rangeSize == buffer->GetSize()) {
                                CallTexBuffer(GL_TEXTURE_BUFFER, bufferImageSplitFormat, backendId);
                            } else if (!CallTexBufferRange(GL_TEXTURE_BUFFER, bufferImageSplitFormat, backendId,
                                                           static_cast<GLintptr>(rangeOffset),
                                                           static_cast<GLsizeiptr>(rangeSize))) {
                                CallTexBuffer(GL_TEXTURE_BUFFER, bufferImageSplitFormat, backendId);
                            }
                            // The raw bind above went behind Bind()'s shadow, which tracks objects
                            // rather than names: leaving it claiming THIS object is bound would
                            // make the next Bind(GL_TEXTURE_BUFFER) a no-op and leave the split
                            // view bound in the application texture's place.
                            g_boundTexturesCache[g_activeTextureUnit][static_cast<SizeT>(
                                TextureTarget::TextureBuffer)] = nullptr;
                        }
                    }
                }
                break;
            }
            default:
                // TextureStorageType is {Mipmap, Buffer}, both handled above, so this is a
                // backstop for a state object that grew a new storage kind. Skipping the upload
                // renders wrong; throwing unwinds through the C GL ABI and kills the process.
                MGLOG_E_ONCE("DirectGLES texture sync: no upload path for storage type %d on texture %u; "
                        "skipping this sync",
                        static_cast<int>(stateTextureObject->GetStorageType()),
                        stateTextureObject->GetExternalIndex());
                break;
            }

            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error: %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            m_prevTextureInfo = currentTextureInfo;
            // Everything dirty at entry is uploaded (or provably has no bytes to
            // upload); stamp the version so per-draw re-syncs short-circuit until
            // the next CPU-side mutation.
            m_syncedContentVersion = stateTextureObject->GetContentVersion();
            // Same instant, so the cheap gate's keys describe exactly this synced state.
            // Only Mipmap storage may arm it - the gate refuses other storage types anyway,
            // but a stale trio must not linger on an object that later switches type.
            if (MG_State::pGLContext && stateTextureObject->GetStorageType() == TextureStorageType::Mipmap) {
                m_syncedShapeContextId = MG_State::pGLContext->GetTextureContextId();
                m_syncedShapeGeneration = MG_State::pGLContext->GetSamplingResolutionGeneration();
                m_syncedShapeParamsVersion = stateTextureObject->GetTextureParamsVersion();
            } else {
                m_syncedShapeContextId = 0;
            }
        }

        void BackendTextureObject::SyncBuiltinSamplerToBackend(
            const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif

            if (!stateTextureObject) {
                MGLOG_E_ONCE("State texture object is null, cannot sync to backend.");
                return;
            }

            auto* samplerObject = stateTextureObject->GetSamplerObject().get();
            Uint currentSamplerVersion = samplerObject->GetVersion();
            if (m_syncedSamplerVersion == currentSamplerVersion && !m_forceSamplerResync) {
                MGLOG_D("Sampler parameters have not changed for texture ID: %u, skipping sync.", m_backendTextureId);
                return;
            }

            m_syncedSamplerVersion = currentSamplerVersion;
            m_forceSamplerResync = false;

            MGLOG_D("Syncing texture built-in sampler with backend ID %u to backend for state ID %u",
                    m_backendTextureId, stateTextureObject->GetExternalIndex());

            GLenum target = ConvertTextureTargetToBackendGLEnum(stateTextureObject->GetTarget());
            auto targetInternal = stateTextureObject->GetTarget();
            MGLOG_D("    Texture target for syncing is %s",
                    MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
            if (!IsSupportedTextureTarget(targetInternal)) {
                MGLOG_E_ONCE("    Texture target %s is not supported, skipping.",
                        MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
                return;
            }

            const auto& samplerParams = samplerObject->GetAllSamplerParameters();
            if (TextureImpl::IsMultisampleTextureTarget(targetInternal)) {
                m_cacheSamplerParameters = samplerParams;
                return;
            }

            Bind(target);
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error: %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            // Update built-in sampler parameters
            MGLOG_D("Updating sampler parameters for texture with ID: %u", m_backendTextureId);

#define SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(internalName, glName, type)                                                  \
    if (m_cacheSamplerParameters.internalName != samplerParams.internalName) {                                         \
        g_GLESFuncs.glTexParameteri(target, glName,                                                                    \
                                    MG_Util::ConvertSampler##type##ToGLEnum(samplerParams.internalName));              \
        m_cacheSamplerParameters.internalName = samplerParams.internalName;                                            \
        DebugImpl::ErrorLopper::Loop(                                                                                  \
            [file = __FILE__, line = __LINE__, func = __func__,                                                        \
             t = MG_Util::ConvertSampler##type##ToGLEnum(samplerParams.internalName)](GLenum err) {                    \
                MGLOG_D("%s(%s:%d) ES error %s, GL_TEXTURE_MIN_FILTER = %s", func, file, line,                         \
                        MG_Util::ConvertGLEnumToString(err).c_str(), MG_Util::ConvertGLEnumToString(t).c_str());       \
            });                                                                                                        \
    }

            if (m_cacheSamplerParameters.minFilter != samplerParams.minFilter ||
                m_cacheSamplerParameters.mipmapMode != samplerParams.mipmapMode) {
                g_GLESFuncs.glTexParameteri(target, GL_TEXTURE_MIN_FILTER,
                                            (GLint)ResolveBackendMinFilter(samplerParams, IsAngleLlvmpipeRenderer()));
                m_cacheSamplerParameters.minFilter = samplerParams.minFilter;
                m_cacheSamplerParameters.mipmapMode = samplerParams.mipmapMode;
            }
            if (m_cacheSamplerParameters.magFilter != samplerParams.magFilter) {
                g_GLESFuncs.glTexParameteri(
                    target, GL_TEXTURE_MAG_FILTER,
                    (GLint)MG_Util::ConvertSamplerFilterModeToGLEnum(samplerParams.magFilter, SamplerMipmapMode::None));
                m_cacheSamplerParameters.magFilter = samplerParams.magFilter;
            }
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(wrapS, GL_TEXTURE_WRAP_S, WrapMode)
            SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(wrapT, GL_TEXTURE_WRAP_T, WrapMode)
            if (SupportsWrapR(targetInternal)) {
                SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(wrapR, GL_TEXTURE_WRAP_R, WrapMode)
            } else {
                m_cacheSamplerParameters.wrapR = samplerParams.wrapR;
            }
            SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(compareFunc, GL_TEXTURE_COMPARE_FUNC, CompareFunc)
            SYNC_TEX_SAMPLER_PARAM_IF_CHANGED(compareMode, GL_TEXTURE_COMPARE_MODE, CompareMode)
            if (m_cacheSamplerParameters.minLod != samplerParams.minLod) {
                g_GLESFuncs.glTexParameterf(target, GL_TEXTURE_MIN_LOD, samplerParams.minLod);
                m_cacheSamplerParameters.minLod = samplerParams.minLod;
            }
            if (m_cacheSamplerParameters.maxLod != samplerParams.maxLod) {
                g_GLESFuncs.glTexParameterf(target, GL_TEXTURE_MAX_LOD, samplerParams.maxLod);
                m_cacheSamplerParameters.maxLod = samplerParams.maxLod;
            }
            if (m_cacheSamplerParameters.maxAnisotropy != samplerParams.maxAnisotropy) {
                if (g_GLESCapabilities.SupportsTextureFilterAnisotropy) {
                    g_GLESFuncs.glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                                                samplerParams.maxAnisotropy);
                }
                // Unsupported GLES backends intentionally treat anisotropy as a
                // frontend-only no-op; remember the observed value so the cache
                // remains coherent without issuing an illegal enum every sync.
                m_cacheSamplerParameters.maxAnisotropy = samplerParams.maxAnisotropy;
            }
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });
#undef SYNC_TEX_SAMPLER_PARAM_IF_CHANGED
        }

        void BackendTextureObject::SyncTextureParamsToBackend(
            const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif

            if (!stateTextureObject) {
                MGLOG_E_ONCE("State texture object is null, cannot sync to backend.");
                return;
            }

            Uint16 currentTextureParamsVersion = stateTextureObject->GetTextureParamsVersion();
            if (m_syncedTextureParamsVersion == currentTextureParamsVersion && !m_forceTextureParamsResync) {
                MGLOG_D("Texture parameters have not changed for texture ID: %u, skipping sync.", m_backendTextureId);
                return;
            }
            m_syncedTextureParamsVersion = currentTextureParamsVersion;
            m_forceTextureParamsResync = false;

            MGLOG_D("Syncing texture params with backend ID %u to backend for state ID %u", m_backendTextureId,
                    stateTextureObject->GetExternalIndex());

            GLenum target = ConvertTextureTargetToBackendGLEnum(stateTextureObject->GetTarget());
            auto targetInternal = stateTextureObject->GetTarget();
            MGLOG_D("    Texture target for syncing is %s",
                    MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
            if (!IsSupportedTextureTarget(targetInternal)) {
                MGLOG_E_ONCE("    Texture target %s is not supported, skipping.",
                        MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
                return;
            }

            // Multisample targets reject the *sampler* parameters (LOD range, border color) but
            // GL_TEXTURE_SWIZZLE_* is texture state, not sampler state, and ES accepts it on them.
            // Bailing out entirely used to drop every swizzle write on the floor, which is what the
            // frontend already assumes is legal (see GL_Texture.cpp's MS-invalid pname list, which
            // deliberately omits the swizzle enums). Note the caches for the skipped parameters are
            // still refreshed so they never look stale, but m_cacheSwizzleParams must NOT be, or the
            // change detection below would swallow the very writes we came here to emit.
            const Bool isMultisampleTarget = TextureImpl::IsMultisampleTextureTarget(targetInternal);
            if (isMultisampleTarget) {
                m_cacheLodRange = stateTextureObject->GetLevelRange();
                m_cacheBorderColor = stateTextureObject->GetBorderColor();
            }

            Bind(target);
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error: %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            // Update texture parameters
            MGLOG_D("Updating texture parameters for texture with ID: %u", m_backendTextureId);

            const auto& levelRange = stateTextureObject->GetLevelRange();

            if (!isMultisampleTarget && m_cacheLodRange.x() != levelRange.x()) {
                g_GLESFuncs.glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, static_cast<GLint>(levelRange.x()));
                m_cacheLodRange.x() = levelRange.x();
            }
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });
            if (!isMultisampleTarget && m_cacheLodRange.y() != levelRange.y()) {
                g_GLESFuncs.glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(levelRange.y()));
                m_cacheLodRange.y() = levelRange.y();
            }
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            // A three-channel format widened to four to keep the image colour-renderable (see
            // NormalizePixelFormat) gains an alpha channel the frontend format does not have, and
            // whatever the draw that filled it wrote there is not what GL would report: a format
            // without alpha reads back as 1.0. Answer the ALPHA swizzle source with ONE so the
            // promotion stays invisible, composed with the swizzle the application asked for.
            Vec4<TextureSwizzleParam> swizzleParams = stateTextureObject->GetAllSwizzleParams();
            if (TextureImpl::BackendTextureFormatAddsAlpha(stateTextureObject->GetFormat(), targetInternal)) {
                for (SizeT channel = 0; channel < 4; ++channel) {
                    if (swizzleParams[channel] == TextureSwizzleParam::Alpha) {
                        swizzleParams[channel] = TextureSwizzleParam::One;
                    }
                }
            }
            // The same composition for the image-format widening, which can add TWO or THREE
            // channels rather than one (GL_R8UI carried in a GL_RGBA8UI). GL reads a channel the
            // format does not have as 0, except alpha, which reads as 1 - so a sampler must see
            // those constants and not whatever the widened storage holds. The upload and the
            // shader's own store mask already keep them at exactly these values; this covers
            // storage nothing has written yet (glTexStorage with no upload), whose surplus
            // channels are undefined. Composed with the application's own swizzle for the same
            // reason as the alpha case above: GL_TEXTURE_SWIZZLE names a SOURCE channel of the
            // logical texel, so it is the source that is substituted, never the destination.
            if (const auto imageWidening =
                    m_imageBindableStorageRequired
                        ? TextureImpl::GetImageBindableStorageWidening(stateTextureObject->GetFormat())
                        : TextureImpl::ImageBindableStorageWidening{}) {
                for (SizeT channel = 0; channel < 4; ++channel) {
                    switch (swizzleParams[channel]) {
                    case TextureSwizzleParam::Green:
                        if (imageWidening.SourceChannels < 2) swizzleParams[channel] = TextureSwizzleParam::Zero;
                        break;
                    case TextureSwizzleParam::Blue:
                        if (imageWidening.SourceChannels < 3) swizzleParams[channel] = TextureSwizzleParam::Zero;
                        break;
                    case TextureSwizzleParam::Alpha:
                        if (imageWidening.SourceChannels < 4) swizzleParams[channel] = TextureSwizzleParam::One;
                        break;
                    default:
                        break;
                    }
                }
            }
            if (swizzleParams != m_cacheSwizzleParams) {
#define SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED(func, glEnum)                                                                \
    if (m_cacheSwizzleParams.func != swizzleParams.func) {                                                             \
        g_GLESFuncs.glTexParameteri(target, glEnum, MG_Util::ConvertTextureSwizzleParamToGLEnum(swizzleParams.func));  \
        m_cacheSwizzleParams.func = swizzleParams.func;                                                                \
    }
                SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED(r(), GL_TEXTURE_SWIZZLE_R);
                SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED(g(), GL_TEXTURE_SWIZZLE_G);
                SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED(b(), GL_TEXTURE_SWIZZLE_B);
                SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED(a(), GL_TEXTURE_SWIZZLE_A);
#undef SYNC_TEX_SWIZZLE_PARAM_IF_CHANGED
                m_cacheSwizzleParams = swizzleParams;
                DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                    MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
                });
            }

            // GL_TEXTURE_BORDER_COLOR needs ES 3.2 or EXT/OES_texture_border_clamp; on a driver
            // without it every such call is INVALID_ENUM, so the parameter is simply not synced.
            //
            // The FORM has to be forwarded along with the value. A border colour set through
            // glTexParameterIiv/Iuiv is an integer one, and an isampler2D/usampler2D fetch of the
            // border returns whatever the driver's integer border register holds - so pushing it
            // through glTexParameterfv handed the driver float 255.0 and the shader read back
            // 1132396544, the bit pattern of that float. glTexParameterIiv/Iuiv are ES 3.2 core
            // beside GL_TEXTURE_BORDER_COLOR itself, so they sit behind the same capability gate;
            // the entry-point null check covers a driver that advertises the extension without them.
            // The redundancy filter has to look at the AUTHORITATIVE representation, not just the
            // float one: two integer borders that differ above 2^24 (16777216 and 16777217, say)
            // collapse onto the same float, so a float-only comparison would skip the second sync and
            // leave the driver holding the first value forever.
            const auto borderColorForm = stateTextureObject->GetBorderColorForm();
            if (!isMultisampleTarget && g_GLESCapabilities.SupportsTextureBorderClamp &&
                (m_cacheBorderColor != stateTextureObject->GetBorderColor() ||
                 m_cacheBorderColorI != stateTextureObject->GetBorderColorI() ||
                 m_cacheBorderColorUI != stateTextureObject->GetBorderColorUI() ||
                 m_cacheBorderColorForm != borderColorForm)) {
                if (borderColorForm == BorderColorForm::Int && g_GLESFuncs.glTexParameterIiv) {
                    const auto& borderColorI = stateTextureObject->GetBorderColorI();
                    const GLint borderColorArray[4] = {borderColorI.x(), borderColorI.y(), borderColorI.z(),
                                                       borderColorI.w()};
                    g_GLESFuncs.glTexParameterIiv(target, GL_TEXTURE_BORDER_COLOR, borderColorArray);
                } else if (borderColorForm == BorderColorForm::Uint && g_GLESFuncs.glTexParameterIuiv) {
                    const auto& borderColorUI = stateTextureObject->GetBorderColorUI();
                    const GLuint borderColorArray[4] = {borderColorUI.x(), borderColorUI.y(), borderColorUI.z(),
                                                        borderColorUI.w()};
                    g_GLESFuncs.glTexParameterIuiv(target, GL_TEXTURE_BORDER_COLOR, borderColorArray);
                } else {
                    const auto& borderColor = stateTextureObject->GetBorderColor();
                    const GLfloat borderColorArray[4] = {borderColor.x(), borderColor.y(), borderColor.z(),
                                                         borderColor.w()};
                    g_GLESFuncs.glTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, borderColorArray);
                }
                m_cacheBorderColor = stateTextureObject->GetBorderColor();
                m_cacheBorderColorI = stateTextureObject->GetBorderColorI();
                m_cacheBorderColorUI = stateTextureObject->GetBorderColorUI();
                m_cacheBorderColorForm = borderColorForm;
                DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                    MGLOG_D("%s(%s:%d) ES error %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
                });
            }

            // GL_DEPTH_STENCIL_TEXTURE_MODE (GL_ARB_stencil_texturing / ES 3.1 core): which aspect
            // of a packed depth/stencil image a sampler reads. Until this was forwarded the
            // frontend kept the mode as a pure shadow - glGetTexParameter answered it, sampling
            // ignored it - so a usampler2D bound to a D24S8 texture in STENCIL_INDEX mode read the
            // depth aspect. Texture state rather than sampler state, so multisample targets take
            // it too (ES 3.1 8.10 lists it among the three pnames they accept). It is only sent
            // when it has moved, which for the overwhelming majority of textures is never.
            const Bool supportsStencilTextureMode =
                g_GLESCapabilities.GLESVersion.Major > 3 ||
                (g_GLESCapabilities.GLESVersion.Major == 3 && g_GLESCapabilities.GLESVersion.Minor >= 1);
            if (supportsStencilTextureMode) {
                const GLenum depthStencilTextureMode = stateTextureObject->GetDepthStencilTextureMode();
                if (m_cacheDepthStencilTextureMode != depthStencilTextureMode) {
                    g_GLESFuncs.glTexParameteri(target, GL_DEPTH_STENCIL_TEXTURE_MODE,
                                                static_cast<GLint>(depthStencilTextureMode));
                    m_cacheDepthStencilTextureMode = depthStencilTextureMode;
                    DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                        MGLOG_D("%s(%s:%d) ES error %s", func, file, line,
                                MG_Util::ConvertGLEnumToString(err).c_str());
                    });
                }
            }
        }

        void ActivateTextureUnit(Uint unit) {
            if (unit == g_activeTextureUnit) {
                return;
            }
            g_GLESFuncs.glActiveTexture(GL_TEXTURE0 + unit);
            g_activeTextureUnit = unit;
        }

        void UnbindTexture(Uint unit, GLenum target) { // Activates `unit` when an unbind is issued
            auto targetN = static_cast<SizeT>(MG_Util::ConvertGLEnumToTextureTarget(target));
            if (g_boundTexturesCache[unit][targetN] == nullptr) return;

            ActivateTextureUnit(unit);
            g_GLESFuncs.glBindTexture(target, 0);
            g_boundTexturesCache[unit][targetN] = nullptr;
        }

        Uint g_activeTextureUnit = 0;
        Array<Array<BackendTextureObject*, (SizeT)TextureTarget::TextureTargetCount>,
              MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS>
            g_boundTexturesCache;
        StateBackendObjectRegistry<MG_State::GLState::ITextureObject, BackendTextureObject> g_backendTextureObjects;
    } // namespace TextureImpl

    namespace FramebufferImpl {
        BackendFramebufferObject::BackendFramebufferObject() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            // Identity until a non-identity draw-buffer array forces a relocation. A framebuffer
            // that is never draw-bound never runs the recompute, so the table has to start out
            // matching what the attachment loop will physically do.
            for (Uint i = 0; i < MAX_COLOR_ATTACHMENT_SLOTS; ++i) {
                m_backendColorSlots[i] = GL_COLOR_ATTACHMENT0 + i;
            }
            g_GLESFuncs.glGenFramebuffers(1, &m_backendFBOId);
            m_contextGeneration = g_backendContextGeneration;
            if (m_backendFBOId == 0) {
                MGLOG_E_ONCE("Failed to generate framebuffer object.");
                MGLOG_E_ONCE("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            } else {
                MGLOG_D("Generated framebuffer object with ID: %u.", m_backendFBOId);
            }
        }

        BackendFramebufferObject::~BackendFramebufferObject() {
            if (InProcessTeardown()) {
                return; // see InProcessTeardown(): the driver may be unloaded already
            }
            if (m_backendFBOId == 0) {
                return;
            }
            // Scrub the binding shadow whether or not the id can still be deleted: a
            // recycled name must never satisfy the shadow's dedup.
            NoteFramebufferIdDeleted(m_backendFBOId);
            if (m_contextGeneration == g_backendContextGeneration && g_GLESFuncs.glDeleteFramebuffers) {
                g_GLESFuncs.glDeleteFramebuffers(1, &m_backendFBOId);
            }
            m_backendFBOId = 0;
        }

        void BackendFramebufferObject::Bind(FramebufferTarget target) const {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (target == FramebufferTarget::Read)
                BindFramebufferId(GL_READ_FRAMEBUFFER, m_backendFBOId);
            else
                BindFramebufferId(GL_DRAW_FRAMEBUFFER, m_backendFBOId);
        }

        namespace {
            // Driver-level framebuffer-binding shadow (see Managers.h). Indexed by
            // FramebufferTarget {Draw, Read}.
            Array<Uint, SizeT(FramebufferTarget::FramebufferTargetCount)> g_driverFBOBindings = {0, 0};
            Array<Bool, SizeT(FramebufferTarget::FramebufferTargetCount)> g_driverFBOBindingKnown = {false, false};
        } // namespace

        void BindFramebufferId(GLenum fbTarget, Uint id) {
            const Bool bindsDraw = fbTarget == GL_DRAW_FRAMEBUFFER || fbTarget == GL_FRAMEBUFFER;
            const Bool bindsRead = fbTarget == GL_READ_FRAMEBUFFER || fbTarget == GL_FRAMEBUFFER;
            const SizeT drawIdx = SizeT(FramebufferTarget::Draw);
            const SizeT readIdx = SizeT(FramebufferTarget::Read);
            const Bool drawMatches =
                !bindsDraw || (g_driverFBOBindingKnown[drawIdx] && g_driverFBOBindings[drawIdx] == id);
            const Bool readMatches =
                !bindsRead || (g_driverFBOBindingKnown[readIdx] && g_driverFBOBindings[readIdx] == id);
            if (drawMatches && readMatches) {
                return;
            }
            g_GLESFuncs.glBindFramebuffer(fbTarget, id);
            if (bindsDraw) {
                g_driverFBOBindings[drawIdx] = id;
                g_driverFBOBindingKnown[drawIdx] = true;
            }
            if (bindsRead) {
                g_driverFBOBindings[readIdx] = id;
                g_driverFBOBindingKnown[readIdx] = true;
            }
        }

        Uint CurrentFramebufferBinding(FramebufferTarget target) {
            const SizeT idx = SizeT(target);
            if (!g_driverFBOBindingKnown[idx]) {
                // Cold path: pin the shadow from the driver once (init probes and
                // pre-shadow code bind raw but restore what they found).
                GLint binding = 0;
                g_GLESFuncs.glGetIntegerv(
                    target == FramebufferTarget::Read ? GL_READ_FRAMEBUFFER_BINDING : GL_DRAW_FRAMEBUFFER_BINDING,
                    &binding);
                g_driverFBOBindings[idx] = static_cast<Uint>(binding);
                g_driverFBOBindingKnown[idx] = true;
            }
            return g_driverFBOBindings[idx];
        }

        void NoteFramebufferIdDeleted(Uint id) {
            if (id == 0) {
                return;
            }
            for (SizeT idx = 0; idx < g_driverFBOBindings.size(); ++idx) {
                if (g_driverFBOBindingKnown[idx] && g_driverFBOBindings[idx] == id) {
                    g_driverFBOBindings[idx] = 0; // glDeleteFramebuffers reverts a bound FBO to 0
                }
            }
        }

        void InvalidateFramebufferBindingCache() {
            g_driverFBOBindings = {0, 0};
            g_driverFBOBindingKnown = {false, false};
            // The per-target "already synced" memo describes work pushed into the ES context
            // that is being left or replaced. Both callers (MakeCurrent, DestroyEGLContext)
            // mean the context may have been reset under us, so claim nothing is synced:
            // a null object never matches a real binding, so SyncCurrentFBO re-pushes.
            g_fboSyncedSlotVersions = {0};
            g_fboSyncedObjectVersions = {0};
            g_fboSyncedObjects = {};
        }

        void BackendFramebufferObject::InvalidateSyncedState() {
            std::fill(std::begin(m_frontendDrawBuffers), std::end(m_frontendDrawBuffers),
                      FramebufferAttachmentType::Unknown);
            std::fill(std::begin(m_backendDrawBuffers), std::end(m_backendDrawBuffers), GL_NONE);
            // NOTE: this does NOT empty the backend ES framebuffer - m_backendFBOId keeps every
            // attachment it had, possibly under a non-identity permutation. Declaring the table
            // identity here is safe only because every attachment version below is invalidated too,
            // so the next sync re-attaches all non-empty attachments at their identity points AND
            // (see SyncToBackend's attachment loop) detaches any colour point whose frontend owner
            // is empty. Without that detach a stale image would survive under a point the table now
            // claims for a different, empty attachment.
            for (Uint i = 0; i < MAX_COLOR_ATTACHMENT_SLOTS; ++i) {
                m_backendColorSlots[i] = GL_COLOR_ATTACHMENT0 + i;
            }
            m_frontendReadBuffer = FramebufferAttachmentType::Unknown;
            m_backendReadBuffer = GL_NONE;
            std::fill(m_syncedFrontendAttachmentVersions.begin(), m_syncedFrontendAttachmentVersions.end(),
                      static_cast<Uint16>(~0u));
            // Every attachment version is invalidated above, so the next walk re-attaches
            // everything regardless; stamp the generation so it does not re-arm twice.
            m_syncedBackendIdGeneration = g_attachmentBackendIdGeneration;
        }

        static Bool SyncAttachmentObject(GLenum glFBOTarget,
                                         const MG_State::GLState::FramebufferAttachmentObject& attachmentObject,
                                         GLenum glBackendAttachment) {
            if (attachmentObject.IsTexture()) {
                const auto& textureObject = attachmentObject.GetTexture();
                SharedPtr<TextureImpl::BackendTextureObject> backendTextureObject;
                if (auto* backendTextureSlot = TextureImpl::g_backendTextureObjects.Find(textureObject.get())) {
                    backendTextureObject = *backendTextureSlot;
                } else {
                    auto& newTextureSlot = TextureImpl::g_backendTextureObjects.GetOrCreate(textureObject);
                    if (!newTextureSlot) {
                        newTextureSlot = MakeShared<TextureImpl::BackendTextureObject>();
                    }
                    backendTextureObject = newTextureSlot;
                }
                if (!backendTextureObject) {
                    MGLOG_E_ONCE("%s: No backend texture found for FBO attachment, cannot bind texture.", __func__);
                    return false;
                }
                backendTextureObject->SyncMipmapsToBackend(textureObject);
                if (attachmentObject.IsLayered()) {
                    g_GLESFuncs.glFramebufferTexture(glFBOTarget, glBackendAttachment,
                                                     backendTextureObject->GetBackendTextureId(),
                                                     static_cast<GLint>(attachmentObject.GetTextureLevel()));
                } else if (const auto uploadTarget = attachmentObject.GetTextureUploadTarget();
                           uploadTarget == TextureUploadTarget::Texture3D ||
                           uploadTarget == TextureUploadTarget::Texture2DArray ||
                           uploadTarget == TextureUploadTarget::Texture1DArray ||
                           uploadTarget == TextureUploadTarget::CubeMapArray ||
                           uploadTarget == TextureUploadTarget::Texture2DMultisampleArray) {
                    // Single slice/layer of a 3D or array texture: ES has no
                    // glFramebufferTexture3D, layers attach via glFramebufferTextureLayer.
                    g_GLESFuncs.glFramebufferTextureLayer(glFBOTarget, glBackendAttachment,
                                                          backendTextureObject->GetBackendTextureId(),
                                                          static_cast<GLint>(attachmentObject.GetTextureLevel()),
                                                          static_cast<GLint>(attachmentObject.GetTextureLayer()));
                } else {
                    auto glTextureTarget = TextureImpl::ConvertTextureUploadTargetToBackendGLEnum(
                        attachmentObject.GetTextureUploadTarget());
                    if (glTextureTarget == GL_UNKNOWN_MGL) {
                        glTextureTarget = TextureImpl::ConvertTextureTargetToBackendGLEnum(textureObject->GetTarget());
                    }
                    // glBindTexture rejects cube-face enums (INVALID_ENUM with no
                    // bind, while Bind() would still record the cube-map cache slot
                    // as bound): bind via the owning cube target; the attach below
                    // keeps the face target.
                    const Bool isCubeFace = glTextureTarget >= GL_TEXTURE_CUBE_MAP_POSITIVE_X &&
                                            glTextureTarget <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
                    backendTextureObject->Bind(isCubeFace ? GL_TEXTURE_CUBE_MAP : glTextureTarget);
                    g_GLESFuncs.glFramebufferTexture2D(glFBOTarget, glBackendAttachment, glTextureTarget,
                                                       backendTextureObject->GetBackendTextureId(),
                                                       static_cast<GLint>(attachmentObject.GetTextureLevel()));
                }
            } else if (attachmentObject.IsRenderbuffer()) {
                const auto& renderbufferObject = attachmentObject.GetRenderbuffer();
                SharedPtr<RenderbufferImpl::BackendRenderbufferObject> backendRenderbufferObject;
                if (auto* backendRenderbufferSlot =
                        RenderbufferImpl::g_backendRenderbufferObjects.Find(renderbufferObject.get())) {
                    backendRenderbufferObject = *backendRenderbufferSlot;
                } else {
                    auto& newRenderbufferSlot =
                        RenderbufferImpl::g_backendRenderbufferObjects.GetOrCreate(renderbufferObject);
                    if (!newRenderbufferSlot) {
                        newRenderbufferSlot = MakeShared<RenderbufferImpl::BackendRenderbufferObject>();
                    }
                    backendRenderbufferObject = newRenderbufferSlot;
                }

                backendRenderbufferObject->SyncToBackend(renderbufferObject);
                backendRenderbufferObject->Bind();
                g_GLESFuncs.glFramebufferRenderbuffer(glFBOTarget, glBackendAttachment, GL_RENDERBUFFER,
                                                      backendRenderbufferObject->GetBackendRenderbufferId());
            }
            return true;
        }

        static Bool IsSnormFormat(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGBA8Snorm:
            case TextureInternalFormat::R16Snorm:
            case TextureInternalFormat::RG16Snorm:
            case TextureInternalFormat::RGB16Snorm:
            case TextureInternalFormat::RGBA16Snorm:
                return true;
            default:
                return false;
            }
        }

        static Bool IsUnormFormat(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R16:
            case TextureInternalFormat::RG16:
            case TextureInternalFormat::RGB16:
            case TextureInternalFormat::RGBA16:
                return true;
            default:
                return false;
            }
        }

        static Bool IsSnormFallbackAttachment(
            const MG_State::GLState::FramebufferAttachmentObject& attachmentObject) {
            if (attachmentObject.IsTexture()) {
                const auto& textureObject = attachmentObject.GetTexture();
                return textureObject && IsSnormFormat(textureObject->GetFormat()) &&
                       TextureImpl::ShouldUseCaveatTextureFormat(textureObject->GetFormat(), textureObject->GetTarget());
            }
            if (attachmentObject.IsRenderbuffer()) {
                const auto& renderbufferObject = attachmentObject.GetRenderbuffer();
                return renderbufferObject &&
                       IsSnormFormat(renderbufferObject->GetInternalFormat()) &&
                       TextureImpl::ShouldUseCaveatRenderbufferFormat(renderbufferObject->GetInternalFormat());
            }
            return false;
        }

        static Bool IsUnormFallbackAttachment(
            const MG_State::GLState::FramebufferAttachmentObject& attachmentObject) {
            if (attachmentObject.IsTexture()) {
                const auto& textureObject = attachmentObject.GetTexture();
                return textureObject && IsUnormFormat(textureObject->GetFormat()) &&
                       TextureImpl::ShouldUseCaveatTextureFormat(textureObject->GetFormat(), textureObject->GetTarget());
            }
            if (attachmentObject.IsRenderbuffer()) {
                const auto& renderbufferObject = attachmentObject.GetRenderbuffer();
                return renderbufferObject &&
                       IsUnormFormat(renderbufferObject->GetInternalFormat()) &&
                       TextureImpl::ShouldUseCaveatRenderbufferFormat(renderbufferObject->GetInternalFormat());
            }
            return false;
        }

        // The colour attachment glReadPixels/glGetTexImage would read from, or nullptr when the
        // read buffer names no colour attachment at all.
        static const MG_State::GLState::FramebufferAttachmentObject* GetReadColorAttachment() {
            const auto& readFBO =
                MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();
            if (!readFBO) {
                return nullptr;
            }
            const auto readBuffer = readFBO->GetReadBuffer();
            if (readBuffer < FramebufferAttachmentType::Color0 || readBuffer > FramebufferAttachmentType::Color31) {
                return nullptr;
            }
            return &readFBO->GetAttachment(readBuffer);
        }

        Bool IsAlphaWidenedColorAttachment(
            const MG_State::GLState::FramebufferAttachmentObject& attachmentObject) {
            if (attachmentObject.IsTexture()) {
                const auto& textureObject = attachmentObject.GetTexture();
                return textureObject && TextureImpl::BackendTextureFormatAddsAlpha(textureObject->GetFormat(),
                                                                                   textureObject->GetTarget());
            }
            if (attachmentObject.IsRenderbuffer()) {
                const auto& renderbufferObject = attachmentObject.GetRenderbuffer();
                return renderbufferObject &&
                       TextureImpl::BackendRenderbufferFormatAddsAlpha(renderbufferObject->GetInternalFormat());
            }
            return false;
        }

        Uint32 g_alphaWidenedDrawBufferMask = 0;
        Uint32 g_integerColorDrawBufferMask = 0;

        static Bool IsIntegerColorFormat(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R8I:
            case TextureInternalFormat::R8UI:
            case TextureInternalFormat::R16I:
            case TextureInternalFormat::R16UI:
            case TextureInternalFormat::R32I:
            case TextureInternalFormat::R32UI:
            case TextureInternalFormat::RG8I:
            case TextureInternalFormat::RG8UI:
            case TextureInternalFormat::RG16I:
            case TextureInternalFormat::RG16UI:
            case TextureInternalFormat::RG32I:
            case TextureInternalFormat::RG32UI:
            case TextureInternalFormat::RGB8I:
            case TextureInternalFormat::RGB8UI:
            case TextureInternalFormat::RGB16I:
            case TextureInternalFormat::RGB16UI:
            case TextureInternalFormat::RGB32I:
            case TextureInternalFormat::RGB32UI:
            case TextureInternalFormat::RGBA8I:
            case TextureInternalFormat::RGBA8UI:
            case TextureInternalFormat::RGBA16I:
            case TextureInternalFormat::RGBA16UI:
            case TextureInternalFormat::RGBA32I:
            case TextureInternalFormat::RGBA32UI:
            case TextureInternalFormat::RGB10A2UI:
                return true;
            default:
                return false;
            }
        }

        static Bool IsIntegerColorAttachment(
            const MG_State::GLState::FramebufferAttachmentObject& attachmentObject) {
            if (attachmentObject.IsTexture()) {
                const auto& textureObject = attachmentObject.GetTexture();
                return textureObject && IsIntegerColorFormat(textureObject->GetFormat());
            }
            if (attachmentObject.IsRenderbuffer()) {
                const auto& renderbufferObject = attachmentObject.GetRenderbuffer();
                return renderbufferObject && IsIntegerColorFormat(renderbufferObject->GetInternalFormat());
            }
            return false;
        }

        Uint32 ComputeAlphaWidenedDrawBufferMask(const MG_State::GLState::FramebufferObject& fbo) {
            using FBO = MG_State::GLState::FramebufferObject;
            const auto& drawBuffers = fbo.GetDrawBuffers();
            Uint32 mask = 0;
            for (Uint i = 0; i < FBO::MAX_DRAW_BUFFERS && i < 32; ++i) {
                const auto frontendBuf = drawBuffers[i];
                if (frontendBuf < FramebufferAttachmentType::Color0 ||
                    frontendBuf > FramebufferAttachmentType::Color31) {
                    continue;
                }
                if (IsAlphaWidenedColorAttachment(fbo.GetAttachment(frontendBuf))) {
                    mask |= (1u << i);
                }
            }
            return mask;
        }

        // The read attachment's storage carries an alpha channel its frontend format does not
        // (the three-channel colour-renderable widening). GL answers such a read with 1.0, but
        // the storage holds whatever the draw wrote there, so the readback has to overwrite it.
        Bool IsAlphaWidenedFallbackReadAttachment() {
            const auto* attachmentObject = GetReadColorAttachment();
            if (attachmentObject == nullptr) {
                return false;
            }
            return IsAlphaWidenedColorAttachment(*attachmentObject);
        }

        Bool IsFixedPointFallbackReadAttachment() {
            const auto& readFBO =
                MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();
            if (!readFBO) {
                return false;
            }
            const auto readBuffer = readFBO->GetReadBuffer();
            if (readBuffer < FramebufferAttachmentType::Color0 || readBuffer > FramebufferAttachmentType::Color31) {
                return false;
            }
            // Any signed-normalized attachment, not just the ones currently substituted:
            // ES has no GL_CLAMP_READ_COLOR at all, so even a natively stored SNORM buffer
            // hands back the negative half that desktop GL clamps away.
            const auto& attachmentObject = readFBO->GetAttachment(readBuffer);
            if (attachmentObject.IsTexture()) {
                const auto& textureObject = attachmentObject.GetTexture();
                return textureObject && IsSnormFormat(textureObject->GetFormat());
            }
            if (attachmentObject.IsRenderbuffer()) {
                const auto& renderbufferObject = attachmentObject.GetRenderbuffer();
                return renderbufferObject && IsSnormFormat(renderbufferObject->GetInternalFormat());
            }
            return false;
        }

        void BackendFramebufferObject::SyncReadBufferToBackend(
            const SharedPtr<MG_State::GLState::FramebufferObject>& stateFBOObject) {
            if (!stateFBOObject) {
                return;
            }
            auto frontendReadBuf = stateFBOObject->GetReadBuffer();
            if (frontendReadBuf == m_frontendReadBuffer) {
                return;
            }
            m_frontendReadBuffer = frontendReadBuf;

            GLenum glBackendReadBuffer = GetBackendAttachmentType(frontendReadBuf);
            if (m_backendReadBuffer != glBackendReadBuffer) {
                m_backendReadBuffer = glBackendReadBuffer;
                // glReadBuffer targets whatever FBO is bound to GL_READ_FRAMEBUFFER. When this is
                // reached from SyncCurrentFBO's "same FBO as draw" skip path the backend FBO was
                // only bound as DRAW, so bind it as READ first to route the read buffer correctly.
                Bind(FramebufferTarget::Read);
                g_GLESFuncs.glReadBuffer(glBackendReadBuffer);
            }
        }

        Bool BackendFramebufferObject::RecomputeBackendColorSlots(
            const FramebufferObject::FramebufferAttachmentArray& stateDrawBuffers) {
            // Only the first GL_MAX_COLOR_ATTACHMENTS points exist in the backend. The frontend's own
            // limit (ValidateColorAttachmentInRange, which reads the clamped
            // GetDynamicParameters().MaxColorAttachments) is never larger than this raw ES cap, so an
            // index the frontend accepted is always < slotCount. Indices at or above it can never own
            // an image and stay on their identity point - never touched, never a GL error.
            const Uint slotCount =
                std::min<Uint>(MAX_COLOR_ATTACHMENT_SLOTS,
                               static_cast<Uint>(std::max<Int>(g_GLESCapabilities.MaxColorAttachments, 1)));

            GLenum newSlots[MAX_COLOR_ATTACHMENT_SLOTS];
            for (Uint i = 0; i < MAX_COLOR_ATTACHMENT_SLOTS; ++i) {
                newSlots[i] = GL_COLOR_ATTACHMENT0 + i;
            }
            Bool assigned[MAX_COLOR_ATTACHMENT_SLOTS] = {false};
            Bool slotTaken[MAX_COLOR_ATTACHMENT_SLOTS] = {false};

            // 1. ES pins draw-buffer slot s to GL_COLOR_ATTACHMENTs, so an attachment named by draw
            //    buffer slot s has no choice: its image must sit at backend point s. This has to
            //    agree with the compaction the caller just pushed through glDrawBuffers.
            for (Uint s = 0; s < FramebufferObject::MAX_DRAW_BUFFERS && s < slotCount; ++s) {
                const auto frontendBuf = stateDrawBuffers[s];
                if (frontendBuf < FramebufferAttachmentType::Color0 ||
                    frontendBuf > FramebufferAttachmentType::Color31) {
                    continue; // GL_NONE, or a default-framebuffer FRONT/BACK token: never relocated.
                }
                const Uint a =
                    static_cast<Uint>(frontendBuf) - static_cast<Uint>(FramebufferAttachmentType::Color0);
                // Neither guard may ever fire: a duplicate draw buffer is already INVALID_OPERATION
                // and an out-of-range one is rejected by ValidateColorAttachmentInRange. If one did
                // fire the table would disagree with the glDrawBuffers the caller already issued,
                // which is the exact non-injectivity this table exists to remove.
                MOBILEGL_ASSERT(a < slotCount && !assigned[a],
                                "Draw buffer %u names colour attachment %u which is out of range or duplicated.", s,
                                a);
                if (a >= slotCount || assigned[a]) {
                    continue;
                }
                newSlots[a] = GL_COLOR_ATTACHMENT0 + s;
                assigned[a] = true;
                slotTaken[s] = true;
            }

            // 2. Everything else keeps its identity point when that point survived step 1. This is
            //    what makes the ordinary drawBuffers[s] == COLOR_ATTACHMENTs case a strict no-op:
            //    the table stays identity, nothing moves, no attachment is re-issued.
            for (Uint a = 0; a < slotCount; ++a) {
                if (assigned[a] || slotTaken[a]) {
                    continue;
                }
                newSlots[a] = GL_COLOR_ATTACHMENT0 + a;
                assigned[a] = true;
                slotTaken[a] = true;
            }

            // 3. What is left are attachments whose identity point step 1 took away. Park them on the
            //    lowest free point. They are not draw buffers, so nothing is rendered through them;
            //    they only have to stay addressable for glReadBuffer and blits, and the map has to
            //    stay injective so reading one of them cannot land on another's image.
            for (Uint a = 0; a < slotCount; ++a) {
                if (assigned[a]) {
                    continue;
                }
                for (Uint s = 0; s < slotCount; ++s) {
                    if (!slotTaken[s]) {
                        newSlots[a] = GL_COLOR_ATTACHMENT0 + s;
                        assigned[a] = true;
                        slotTaken[s] = true;
                        break;
                    }
                }
            }

            Bool moved = false;
            for (Uint a = 0; a < MAX_COLOR_ATTACHMENT_SLOTS; ++a) {
                if (m_backendColorSlots[a] == newSlots[a]) {
                    continue;
                }
                m_backendColorSlots[a] = newSlots[a];
                moved = true;
                // This attachment's image now belongs at a different backend point. Its frontend
                // version has not changed, so the attachment loop would skip it; force it.
                m_syncedFrontendAttachmentVersions[static_cast<SizeT>(FramebufferAttachmentType::Color0) + a] =
                    static_cast<Uint16>(~0u);
            }
            return moved;
        }

        void BackendFramebufferObject::SyncToBackend(
            const SharedPtr<MG_State::GLState::FramebufferObject>& stateFBOObject, FramebufferTarget asTarget) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!stateFBOObject) {
                MGLOG_E_ONCE("State FBO object is null, cannot sync to backend.");
                return;
            }
            MGLOG_D("Syncing FBO with backend ID %u to backend for state ID %u, as %s FBO", m_backendFBOId,
                    stateFBOObject->GetExternalIndex(), (asTarget == FramebufferTarget::Draw ? "DRAW" : "READ"));
            GLenum glFBOTarget = MG_Util::ConvertFramebufferTargetToGLEnum(asTarget);
            Bind(asTarget);

            // -------------------- Connect attachments (set buffers) -----------------------
            // 1. Remap draw buffers
            auto& stateDrawBuffers = stateFBOObject->GetDrawBuffers();
            Bool drawBufferClean = false;
            if (memcmp(m_frontendDrawBuffers, stateDrawBuffers.data(),
                       FramebufferObject::MAX_DRAW_BUFFERS * sizeof(FramebufferAttachmentType)) == 0) {
                drawBufferClean = true;
            }

            // glDrawBuffers writes the state of the FBO bound to GL_DRAW_FRAMEBUFFER.
            // When this object is only bound as the READ target the call would land on
            // whatever framebuffer is draw-bound AND falsely stamp this object's memo,
            // so the later draw-target sync skips as "clean" while the real state is
            // stale (Minecraft 26.x OIT: the scratch clear-FBO kept draw buffers NONE
            // from its blit-destination configuration, silently dropping every
            // offscreen color clear).
            if (!drawBufferClean && asTarget == FramebufferTarget::Draw) {
                memcpy(m_frontendDrawBuffers, stateDrawBuffers.data(),
                       FramebufferObject::MAX_DRAW_BUFFERS * sizeof(FramebufferAttachmentType));
                std::fill(m_backendDrawBuffers, m_backendDrawBuffers + FramebufferObject::MAX_DRAW_BUFFERS, GL_NONE);
                int nEffectiveBuffers = 0;
                for (GLint i = 0; i < FramebufferObject::MAX_DRAW_BUFFERS; ++i) {
                    auto& frontendBuf = stateDrawBuffers[i];
                    if (frontendBuf == FramebufferAttachmentType::None) {
                        m_backendDrawBuffers[i] = GL_NONE;
                        continue;
                    }

                    // Create compacted mapping
                    if (frontendBuf == FramebufferAttachmentType::FrontLeft ||
                        frontendBuf == FramebufferAttachmentType::FrontRight ||
                        frontendBuf == FramebufferAttachmentType::BackLeft ||
                        frontendBuf == FramebufferAttachmentType::BackRight) {
                        MGLOG_D("%s: frontend buf token found for default fbo, shouldn't remap", __func__);
                        m_backendDrawBuffers[i] = MG_Util::ConvertFramebufferAttachmentTypeToGLEnum(frontendBuf);
                    } else {
                        m_backendDrawBuffers[i] = GL_COLOR_ATTACHMENT0 + i;
                    }
                    nEffectiveBuffers = i + 1;
                }
                g_GLESFuncs.glDrawBuffers(nEffectiveBuffers, m_backendDrawBuffers);
                // The line above pinned backend point s to draw-buffer slot s, so the images have to
                // be moved under those points. Rebuild the whole colour map and, when anything moved,
                // also drop the read-buffer memo: SyncReadBufferToBackend keys it on the frontend
                // enum alone, which does not change when the point under it does.
                if (RecomputeBackendColorSlots(stateDrawBuffers)) {
                    m_frontendReadBuffer = FramebufferAttachmentType::Unknown;
                }
                MGLOG_D("DBAPPLY beFbo=%u target=%d n=%d db0=0x%x feDb0=%d", m_backendFBOId, (int)asTarget,
                        nEffectiveBuffers, m_backendDrawBuffers[0], (int)stateDrawBuffers[0]);
            }

            if (asTarget == FramebufferTarget::Draw) {
                Uint32 snormClampOutputMask = 0;
                Uint32 unormClampOutputMask = 0;
                Uint32 alphaWidenedMask = 0;
                Uint32 integerColorMask = 0;
                for (Uint i = 0; i < FramebufferObject::MAX_DRAW_BUFFERS && i < 32; ++i) {
                    const auto frontendBuf = stateDrawBuffers[i];
                    if (frontendBuf < FramebufferAttachmentType::Color0 ||
                        frontendBuf > FramebufferAttachmentType::Color31) {
                        continue;
                    }
                    const auto& attachmentObject = stateFBOObject->GetAttachment(frontendBuf);
                    if (IsSnormFallbackAttachment(attachmentObject)) {
                        snormClampOutputMask |= (1u << i);
                    } else if (IsUnormFallbackAttachment(attachmentObject)) {
                        unormClampOutputMask |= (1u << i);
                    }
                    // Independent of the two above: a widened attachment can be SNORM
                    // (GL_RGB8_SNORM -> GL_RGBA16F, which also clamps) or not (GL_SRGB8 ->
                    // GL_SRGB8_ALPHA8, which does not), so it gets its own bit rather than an
                    // `else if` branch of theirs.
                    if (IsAlphaWidenedColorAttachment(attachmentObject)) {
                        alphaWidenedMask |= (1u << i);
                    }
                    if (IsIntegerColorAttachment(attachmentObject)) {
                        integerColorMask |= (1u << i);
                    }
                }
                PrgramImpl::g_snormFallbackClampOutputMask = snormClampOutputMask;
                PrgramImpl::g_unormFallbackClampOutputMask = unormClampOutputMask;
                g_alphaWidenedDrawBufferMask = alphaWidenedMask;
                g_integerColorDrawBufferMask = integerColorMask;
            }

            // 2. Remap read buffer. glReadBuffer writes the READ-bound FBO's state, so
            // only apply (and stamp the memo) when this object is bound as READ.
            if (asTarget == FramebufferTarget::Read) {
                SyncReadBufferToBackend(stateFBOObject);
            }

            // -------------------- Attach texture to backend FBO -----------------------
            // A backend texture id was re-minted since this twin's last walk
            // (RecreateBackendTexture): any point here may still hold the dead id while
            // its frontend attachment version is unchanged, so the memo below would skip
            // exactly the attachment that needs repair. Re-arm every point first.
            if (m_syncedBackendIdGeneration != g_attachmentBackendIdGeneration) {
                std::fill(m_syncedFrontendAttachmentVersions.begin(), m_syncedFrontendAttachmentVersions.end(),
                          static_cast<Uint16>(~0u));
                m_syncedBackendIdGeneration = g_attachmentBackendIdGeneration;
            }
            const auto& attachments = stateFBOObject->GetAllAttachmentObjects();
            const auto& attachmentVersions = stateFBOObject->GetAllFramebufferAttachmentVersions();
            for (SizeT i = 0; i < attachments.size(); ++i) {
                const auto& attachmentObject = attachments[i];
                auto frontendType = static_cast<FramebufferAttachmentType>(i);
                GLenum glBackendAttachment = GL_NONE;
                if (frontendType >= FramebufferAttachmentType::Color0 &&
                    frontendType <= FramebufferAttachmentType::Color31)
                    glBackendAttachment = GetBackendAttachmentType(frontendType);
                else
                    glBackendAttachment = MG_Util::ConvertFramebufferAttachmentTypeToGLEnum(frontendType);

                // relevant FRONTEND!!! version should be checked and updated
                if (m_syncedFrontendAttachmentVersions[i] != attachmentVersions[i]) {
                    // SyncAttachmentObject only ever attaches: for an empty frontend attachment it
                    // returns true and issues nothing, so the point keeps whatever was there. That is
                    // what makes m_backendColorSlots a permutation of the PHYSICAL layout rather than
                    // a claim about one - a point handed to an attachment with no image would
                    // otherwise still hold the previous owner's image and glReadBuffer would return
                    // it. Bounded by GL_MAX_COLOR_ATTACHMENTS because GL_COLOR_ATTACHMENTn above the
                    // driver's limit is INVALID_ENUM, and restricted to colour points because
                    // FRONT_LEFT/BACK_LEFT and co. are not ES attachment points at all.
                    const Bool isColorPoint =
                        frontendType >= FramebufferAttachmentType::Color0 &&
                        frontendType <= FramebufferAttachmentType::Color31 &&
                        (static_cast<Int>(frontendType) - static_cast<Int>(FramebufferAttachmentType::Color0)) <
                            g_GLESCapabilities.MaxColorAttachments;
                    if (isColorPoint && attachmentObject.IsEmpty() && glBackendAttachment != GL_NONE) {
                        g_GLESFuncs.glFramebufferRenderbuffer(glFBOTarget, glBackendAttachment, GL_RENDERBUFFER, 0);
                    }
                    if (SyncAttachmentObject(glFBOTarget, attachmentObject, glBackendAttachment)) {
                        m_syncedFrontendAttachmentVersions[i] = attachmentVersions[i];
                    }
                }
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
                else {
                    MGLOG_D("%s: Skipped SyncAttachmentObject(target=%s, frontendObj=(%dx%dx%d, %s), backendAtt=%s), "
                            "version = %u",
                            __func__, MG_Util::ConvertGLEnumToString(glFBOTarget).c_str(),
                            attachmentObject.GetSize().x(), attachmentObject.GetSize().y(),
                            attachmentObject.GetSize().z(),
                            MG_Util::ConvertFramebufferAttachmentTypeToString(frontendType).c_str(),
                            MG_Util::ConvertGLEnumToString(glBackendAttachment).c_str(),
                            m_syncedFrontendAttachmentVersions[i]);
                    if (!attachmentObject.IsTexture() && !attachmentObject.IsRenderbuffer()) {
                        continue;
                    }
                    GLint objectType = GL_NONE;
                    g_GLESFuncs.glGetFramebufferAttachmentParameteriv(
                        glFBOTarget, glBackendAttachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &objectType);
                    MOBILEGL_ASSERT((objectType == GL_NONE) ||
                                        (attachmentObject.IsTexture() && objectType == GL_TEXTURE) ||
                                        (attachmentObject.IsRenderbuffer() && objectType == GL_RENDERBUFFER),
                                    "Attachment type not match!");
                    GLint objectName = 0;
                    g_GLESFuncs.glGetFramebufferAttachmentParameteriv(
                        glFBOTarget, glBackendAttachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &objectName);
                    // Verify that the backend object's name and parameters match the frontend attachment state
                    if (attachmentObject.IsTexture()) {
                        const auto& textureObject = attachmentObject.GetTexture();
                        auto* backendTextureSlot = TextureImpl::g_backendTextureObjects.Find(textureObject.get());
                        MOBILEGL_ASSERT(backendTextureSlot != nullptr && *backendTextureSlot != nullptr,
                                        "No backend texture found while framebuffer reports texture attachment.");
                        GLuint backendTexId = (*backendTextureSlot)->GetBackendTextureId();
                        MOBILEGL_ASSERT(static_cast<GLint>(backendTexId) == objectName,
                                        "Attachment texture name mismatch between GLES (%d) and backend texture object "
                                        "(%d), frontend texture object ID=%d.",
                                        objectName, backendTexId, textureObject->GetExternalIndex());

                        GLint texLevel = 0;
                        g_GLESFuncs.glGetFramebufferAttachmentParameteriv(
                            glFBOTarget, glBackendAttachment, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &texLevel);
                        MOBILEGL_ASSERT(texLevel == static_cast<GLint>(attachmentObject.GetTextureLevel()),
                                        "Attachment texture level mismatch between GLES and state object.");
                    } else if (attachmentObject.IsRenderbuffer()) {
                        const auto& renderbufferObject = attachmentObject.GetRenderbuffer();
                        auto* backendRboSlot =
                            RenderbufferImpl::g_backendRenderbufferObjects.Find(renderbufferObject.get());
                        MOBILEGL_ASSERT(
                            backendRboSlot != nullptr && *backendRboSlot != nullptr,
                            "No backend renderbuffer found while framebuffer reports renderbuffer attachment.");
                        GLuint backendRboId = (*backendRboSlot)->GetBackendRenderbufferId();
                        MOBILEGL_ASSERT(static_cast<GLint>(backendRboId) == objectName,
                                        "Attachment renderbuffer name mismatch between GLES and state object.");
                    }
                }
#endif
            }

            // The walk itself can re-mint an id (SyncAttachmentObject ->
            // SyncMipmapsToBackend -> RecreateBackendTexture), invalidating points this
            // walk already attached or version-skipped - e.g. one texture attached at two
            // points. Re-enter until the generation is quiescent: every pass syncs each
            // dirty texture clean, so each repeat finds strictly fewer re-mints and the
            // common case (no re-mint) never takes a second pass. The head's draw/read-
            // buffer syncs are memoized against their own shadows, so a repeat re-walks
            // only the attachments.
            if (m_syncedBackendIdGeneration != g_attachmentBackendIdGeneration) {
                SyncToBackend(stateFBOObject, asTarget);
            }
        }

        GLenum BackendFramebufferObject::GetBackendAttachmentType(FramebufferAttachmentType frontendAtt) const {
            // Only colour attachments are ever relocated; depth/stencil, the default framebuffer's
            // FRONT/BACK names and None map straight through.
            if (frontendAtt < FramebufferAttachmentType::Color0 || frontendAtt > FramebufferAttachmentType::Color31) {
                return MG_Util::ConvertFramebufferAttachmentTypeToGLEnum(frontendAtt);
            }
            // The table is a permutation of the backend colour points, so this is the one point that
            // owns this attachment. Searching the draw-buffer array instead returned the identity
            // point for every attachment that was not a draw buffer - which is exactly the point a
            // relocated draw buffer had just taken over, so COLOR_ATTACHMENT0 read back the image of
            // whatever attachment was last made the draw buffer.
            const Uint index = static_cast<Uint>(frontendAtt) - static_cast<Uint>(FramebufferAttachmentType::Color0);
            return m_backendColorSlots[index];
        }

        StateBackendObjectRegistry<MG_State::GLState::FramebufferObject, BackendFramebufferObject>
            g_backendFramebufferObjects;
        Array<Uint16, SizeT(FramebufferTarget::FramebufferTargetCount)> g_fboSyncedSlotVersions = {0};
        // Tracks the bound FBO's object version (bumped on any attachment/drawbuffer change)
        // per target: re-attaching textures or changing draw buffers on an already-bound FBO
        // must re-sync it even when the binding-slot version has not moved.
        Array<Uint16, SizeT(FramebufferTarget::FramebufferTargetCount)> g_fboSyncedObjectVersions = {0};
        Array<MG_State::GLState::FramebufferObject*, SizeT(FramebufferTarget::FramebufferTargetCount)>
            g_fboSyncedObjects = {};
        Uint64 g_attachmentBackendIdGeneration = 0;
        Array<Uint64, SizeT(FramebufferTarget::FramebufferTargetCount)> g_fboSyncedBackendIdGenerations = {0};
    } // namespace FramebufferImpl

    namespace ScratchFBOImpl {
        namespace {
            ScratchFramebuffer g_tempFramebuffer;
            ScratchFramebuffer g_blitReadFramebuffer;
            ScratchFramebuffer g_blitDrawFramebuffer;
            Uint g_completeTinyFBOId = 0;
            Uint g_completeTinyRBOId = 0;

            // Detach every point the shadow no longer vouches for. Used when the
            // shadow is unknown (context reset, texture id deleted while attached).
            void ScrubAllAttachments(ScratchFramebuffer& fb, GLenum fbTarget) {
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                fb.colorTex = 0;
                fb.colorTarget = 0;
                fb.colorLevel = 0;
                fb.colorLayer = -1;
                fb.depthTex = 0;
                fb.depthTarget = 0;
                fb.depthLevel = 0;
                fb.depthHasStencil = false;
                fb.attachmentsKnown = true;
            }

            void PrepareForUse(ScratchFramebuffer& fb, GLenum fbTarget) {
                if (!fb.attachmentsKnown) {
                    ScrubAllAttachments(fb, fbTarget);
                }
            }

            // The post-attach glGetError probe below must not misread an error some
            // earlier operation left queued; drain before attaching (rare path -
            // only runs when the attachment actually changes).
            void DrainPendingGLErrors() {
                while (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                }
            }

            // Record the color point as detached when the shadow said something was
            // there; the actual detach call is the caller's (it may be replaced by
            // the new attach directly when the point is being overwritten).
            void RecordNoColor(ScratchFramebuffer& fb) {
                fb.colorTex = 0;
                fb.colorTarget = 0;
                fb.colorLevel = 0;
                fb.colorLayer = -1;
            }

            void RecordNoDepth(ScratchFramebuffer& fb) {
                fb.depthTex = 0;
                fb.depthTarget = 0;
                fb.depthLevel = 0;
                fb.depthHasStencil = false;
            }
        } // namespace

        ScratchFramebuffer& TempFramebuffer() {
            return g_tempFramebuffer;
        }
        ScratchFramebuffer& BlitReadFramebuffer() {
            return g_blitReadFramebuffer;
        }
        ScratchFramebuffer& BlitDrawFramebuffer() {
            return g_blitDrawFramebuffer;
        }

        Uint EnsureId(ScratchFramebuffer& fb) {
            if (fb.id == 0) {
                g_GLESFuncs.glGenFramebuffers(1, &fb.id);
                // A fresh FBO has nothing attached and COLOR_ATTACHMENT0 read/draw
                // buffers (the ES defaults for a non-default framebuffer).
                fb.attachmentsKnown = true;
                RecordNoColor(fb);
                RecordNoDepth(fb);
                fb.readBuffer = GL_COLOR_ATTACHMENT0;
                fb.drawBuffer = GL_COLOR_ATTACHMENT0;
            }
            return fb.id;
        }

        void EnsureColorAttachment2D(ScratchFramebuffer& fb, GLenum fbTarget, Uint tex, GLenum texTarget,
                                     GLint level) {
            PrepareForUse(fb, fbTarget);
            if (fb.depthTex != 0) {
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                RecordNoDepth(fb);
            }
            if (fb.colorTex == tex && fb.colorTarget == texTarget && fb.colorLevel == level && fb.colorLayer < 0) {
                return;
            }
            if (fb.colorTex != 0) {
                // Detach first: if the new attach fails, the point must read as
                // missing (incomplete FBO), not silently keep the old texture.
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            }
            DrainPendingGLErrors();
            g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_COLOR_ATTACHMENT0, texTarget, tex, level);
            if (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                RecordNoColor(fb);
                return;
            }
            fb.colorTex = tex;
            fb.colorTarget = texTarget;
            fb.colorLevel = level;
            fb.colorLayer = -1;
        }

        void EnsureColorAttachmentLayer(ScratchFramebuffer& fb, GLenum fbTarget, Uint tex, GLint level, GLint layer) {
            PrepareForUse(fb, fbTarget);
            if (fb.depthTex != 0) {
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                RecordNoDepth(fb);
            }
            if (fb.colorTex == tex && fb.colorTarget == 0 && fb.colorLevel == level && fb.colorLayer == layer) {
                return;
            }
            if (fb.colorTex != 0) {
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            }
            DrainPendingGLErrors();
            g_GLESFuncs.glFramebufferTextureLayer(fbTarget, GL_COLOR_ATTACHMENT0, tex, level, layer);
            if (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                RecordNoColor(fb);
                return;
            }
            fb.colorTex = tex;
            fb.colorTarget = 0;
            fb.colorLevel = level;
            fb.colorLayer = layer;
        }

        void EnsureDepthAttachment2D(ScratchFramebuffer& fb, GLenum fbTarget, Uint tex, GLenum texTarget, GLint level,
                                     Bool withStencil) {
            PrepareForUse(fb, fbTarget);
            if (fb.colorTex != 0) {
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                RecordNoColor(fb);
            }
            if (fb.depthTex == tex && fb.depthTarget == texTarget && fb.depthLevel == level &&
                fb.depthHasStencil == withStencil) {
                return;
            }
            if (fb.depthTex != 0) {
                // One call clears both depth and stencil points regardless of how
                // the previous attachment was made.
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            }
            DrainPendingGLErrors();
            g_GLESFuncs.glFramebufferTexture2D(fbTarget,
                                               withStencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                                               texTarget, tex, level);
            if (g_GLESFuncs.glGetError() != GL_NO_ERROR) {
                RecordNoDepth(fb);
                return;
            }
            fb.depthTex = tex;
            fb.depthTarget = texTarget;
            fb.depthLevel = level;
            fb.depthHasStencil = withStencil;
        }

        void EnsureNoColorAttachment(ScratchFramebuffer& fb, GLenum fbTarget) {
            PrepareForUse(fb, fbTarget);
            if (fb.colorTex != 0) {
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                RecordNoColor(fb);
            }
        }

        void EnsureNoDepthAttachment(ScratchFramebuffer& fb, GLenum fbTarget) {
            PrepareForUse(fb, fbTarget);
            if (fb.depthTex != 0) {
                g_GLESFuncs.glFramebufferTexture2D(fbTarget, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                RecordNoDepth(fb);
            }
        }

        void EnsureReadBuffer(ScratchFramebuffer& fb, GLenum readBuffer) {
            if (fb.readBuffer == readBuffer) {
                return;
            }
            g_GLESFuncs.glReadBuffer(readBuffer);
            fb.readBuffer = readBuffer;
        }

        void EnsureDrawBuffer(ScratchFramebuffer& fb, GLenum drawBuffer) {
            if (fb.drawBuffer == drawBuffer) {
                return;
            }
            g_GLESFuncs.glDrawBuffers(1, &drawBuffer);
            fb.drawBuffer = drawBuffer;
        }

        Uint EnsureCompleteTinyFramebufferId() {
            if (g_completeTinyFBOId != 0) {
                return g_completeTinyFBOId;
            }
            // One-time creation: the renderbuffer binding is context state with no
            // shadow, so save/restore it by query here (cold path only).
            GLint prevRenderbuffer = 0;
            g_GLESFuncs.glGetIntegerv(GL_RENDERBUFFER_BINDING, &prevRenderbuffer);
            g_GLESFuncs.glGenFramebuffers(1, &g_completeTinyFBOId);
            g_GLESFuncs.glGenRenderbuffers(1, &g_completeTinyRBOId);
            FramebufferImpl::BindFramebufferId(GL_FRAMEBUFFER, g_completeTinyFBOId);
            g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, g_completeTinyRBOId);
            g_GLESFuncs.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 1, 1);
            g_GLESFuncs.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                                  g_completeTinyRBOId);
            const GLenum drawBuffer = GL_COLOR_ATTACHMENT0;
            g_GLESFuncs.glDrawBuffers(1, &drawBuffer);
            g_GLESFuncs.glReadBuffer(GL_COLOR_ATTACHMENT0);
            MOBILEGL_ASSERT(g_GLESFuncs.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
                            "Scratch 1x1 framebuffer is incomplete.");
            g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, static_cast<Uint>(prevRenderbuffer));
            return g_completeTinyFBOId;
        }

        void NoteTextureIdDeleted(Uint textureId) {
            if (textureId == 0) {
                return;
            }
            for (ScratchFramebuffer* fb : {&g_tempFramebuffer, &g_blitReadFramebuffer, &g_blitDrawFramebuffer}) {
                if (fb->colorTex == textureId || fb->depthTex == textureId) {
                    fb->attachmentsKnown = false;
                }
            }
        }

        void OnBackendContextDestroyed() {
            g_tempFramebuffer = {};
            g_blitReadFramebuffer = {};
            g_blitDrawFramebuffer = {};
            g_completeTinyFBOId = 0;
            g_completeTinyRBOId = 0;
        }
    } // namespace ScratchFBOImpl

    namespace PixelStoreImpl {
        namespace {
            PackState g_packState;
            Bool g_packStateKnown = false;

            void PinPackState(const PackState& value) {
                g_GLESFuncs.glPixelStorei(GL_PACK_ALIGNMENT, value.Alignment);
                g_GLESFuncs.glPixelStorei(GL_PACK_ROW_LENGTH, value.RowLength);
                g_GLESFuncs.glPixelStorei(GL_PACK_SKIP_ROWS, value.SkipRows);
                g_GLESFuncs.glPixelStorei(GL_PACK_SKIP_PIXELS, value.SkipPixels);
                g_packState = value;
                g_packStateKnown = true;
            }
        } // namespace

        void ApplyPackState(const PackState& desired) {
            if (!g_packStateKnown) {
                PinPackState(desired);
                return;
            }
            if (desired.Alignment != g_packState.Alignment) {
                g_GLESFuncs.glPixelStorei(GL_PACK_ALIGNMENT, desired.Alignment);
                g_packState.Alignment = desired.Alignment;
            }
            if (desired.RowLength != g_packState.RowLength) {
                g_GLESFuncs.glPixelStorei(GL_PACK_ROW_LENGTH, desired.RowLength);
                g_packState.RowLength = desired.RowLength;
            }
            if (desired.SkipRows != g_packState.SkipRows) {
                g_GLESFuncs.glPixelStorei(GL_PACK_SKIP_ROWS, desired.SkipRows);
                g_packState.SkipRows = desired.SkipRows;
            }
            if (desired.SkipPixels != g_packState.SkipPixels) {
                g_GLESFuncs.glPixelStorei(GL_PACK_SKIP_PIXELS, desired.SkipPixels);
                g_packState.SkipPixels = desired.SkipPixels;
            }
        }

        PackState CurrentPackState() {
            if (!g_packStateKnown) {
                // Fresh/unknown context: pin to the GL defaults (what a new context
                // starts with; writing them makes the shadow authoritative either way).
                PinPackState(PackState{});
            }
            return g_packState;
        }

        void InvalidatePackStateCache() {
            g_packStateKnown = false;
        }
    } // namespace PixelStoreImpl

    namespace PrgramImpl {
        Uint32 g_snormFallbackClampOutputMask = 0;
        Uint g_fragColorBroadcastCount = 1;
        Uint32 g_unormFallbackClampOutputMask = 0;
        Uint g_lastUsedBackendProgramId = 0;
        // Every error-queue drain in the program build path is bounded by this: a lost
        // context never answers GL_NO_ERROR, and the build runs on the thread that would
        // then spin forever.
        constexpr Int kMaxDrainedProgramErrors = 32;
        StateBackendObjectRegistry<MG_State::GLState::ProgramObject, BackendProgramObjectImpl> g_backendProgramObjects;

        BackendProgramObjectImpl::BackendProgramObjectImpl() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            m_backendProgramId = g_GLESFuncs.glCreateProgram();
            if (m_backendProgramId == 0) {
                MGLOG_E_ONCE("Failed to create program object in backend.");
                MGLOG_E_ONCE("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());

            } else {
                MGLOG_D("Created backend program object with ID: %u", m_backendProgramId);
            }
        }

        BackendProgramObjectImpl::~BackendProgramObjectImpl() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (InProcessTeardown()) {
                return; // see InProcessTeardown(): the driver may be unloaded already
            }
            if (m_backendProgramId != 0) {
                MGLOG_D("Deleting backend program object with ID: %u", m_backendProgramId);
                g_GLESFuncs.glDeleteProgram(m_backendProgramId);
                // The driver may recycle this GL name for a future program; a stale
                // guard entry would then wrongly skip the glUseProgram for it.
                if (g_lastUsedBackendProgramId == m_backendProgramId) {
                    g_lastUsedBackendProgramId = 0;
                }
            }
        }

        Bool ApplyShaderStorageBlockBinding(Uint backendProgramId, const String& blockName, Uint binding) {
            if (backendProgramId == 0 || blockName.empty()) return false;
            if (!g_GLESFuncs.glGetProgramResourceIndex || !g_GLESFuncs.glShaderStorageBlockBinding) return false;
            GLuint driverIndex =
                g_GLESFuncs.glGetProgramResourceIndex(backendProgramId, GL_SHADER_STORAGE_BLOCK, blockName.c_str());
            if (driverIndex == GL_INVALID_INDEX) {
                // An arrayed block is enumerated per element by GL but declared once; the
                // generated ESSL carries the bare block name.
                const auto bracket = blockName.rfind('[');
                if (bracket == String::npos || blockName.back() != ']') return false;
                driverIndex = g_GLESFuncs.glGetProgramResourceIndex(backendProgramId, GL_SHADER_STORAGE_BLOCK,
                                                                    blockName.substr(0, bracket).c_str());
                if (driverIndex == GL_INVALID_INDEX) return false;
            }
            g_GLESFuncs.glShaderStorageBlockBinding(backendProgramId, driverIndex, binding);
            return true;
        }

        void ReseedShaderStorageBlockBindings(Uint backendProgramId,
                                              const MG_State::GLState::ProgramObject& stateProgramObject) {
            const auto& overrides = stateProgramObject.GetShaderStorageBlockBindingOverrides();
            if (overrides.empty()) return; // the overwhelming majority of programs
            for (const auto& [blockName, binding] : overrides) {
                if (binding < 0) continue;
                ApplyShaderStorageBlockBinding(backendProgramId, blockName, static_cast<Uint>(binding));
            }
        }

        Uint64 ComputeShaderStorageBlockBindingSignature(
            const MG_State::GLState::ProgramObject& stateProgramObject) {
            const auto& overrides = stateProgramObject.GetShaderStorageBlockBindingOverrides();
            if (overrides.empty()) return 0; // the overwhelming majority of programs
            // Order-independent on purpose: the source is an UnorderedMap, so any signature that
            // depended on iteration order would differ between two identical override sets and
            // rebuild the program for nothing.
            //
            // Built from the VALUES, not from a change counter, so re-setting a block to the
            // binding it already carries produces the same signature and forces no rebuild - an
            // application that calls glShaderStorageBlockBinding every frame with unchanged
            // arguments must not retranspile every frame.
            Uint64 signature = 0;
            for (const auto& [blockName, binding] : overrides) {
                if (binding < 0) continue; // never rebound; the declared qualifier still stands
                Uint64 entry = std::hash<String>{}(blockName);
                // Mixed rather than merely summed with the name hash: name and binding must not
                // be able to trade places between two entries and cancel out.
                entry ^= (static_cast<Uint64>(static_cast<Uint32>(binding)) + 0x9e3779b97f4a7c15ull +
                          (entry << 6) + (entry >> 2));
                signature += entry; // commutative combine
            }
            return signature;
        }

        namespace {
            // The GL internal format bound to an image unit right now. GL_NONE for a unit
            // outside the frontend's array, which cannot be addressed at all.
            Uint BoundImageUnitFormat(Int unit) {
                if (unit < 0 || unit >= MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS) return 0;
                return static_cast<Uint>(MG_State::pGLContext->GetImageTextureBinding(unit).Format);
            }

            // Combines one (unit, format) pair into a running digest. Commutative, so the order
            // the uniforms are walked in cannot change the answer, and mixed rather than summed
            // so a unit and a format cannot trade places between two pairs and cancel out.
            Uint64 MixImageUnitFormat(Uint64 signature, Int unit, Uint format) {
                Uint64 entry = static_cast<Uint64>(static_cast<Uint32>(unit)) + 0x9e3779b97f4a7c15ull;
                entry ^= static_cast<Uint64>(format) + 0xbf58476d1ce4e5b9ull + (entry << 6) + (entry >> 2);
                return signature + entry;
            }

            // Pipeline position of a shader stage. Names the PRODUCER of an inter-stage
            // interface block: a block one stage consumes was written by the stage before it.
            // ShaderStage is declared in pipeline order, so the enum value IS the position;
            // compute has no inter-stage interface at all and is reported as -1.
            Int InterStagePipelineIndex(ShaderStage stage) {
                switch (stage) {
                case ShaderStage::Vertex:
                case ShaderStage::TessControl:
                case ShaderStage::TessEval:
                case ShaderStage::Geometry:
                case ShaderStage::Fragment:
                    return static_cast<Int>(stage);
                default:
                    return -1;
                }
            }

            // Whether a stage can declare interface blocks in BOTH directions at once, i.e.
            // whether one block name can name two different blocks inside it. A vertex INPUT
            // and a fragment OUTPUT cannot be blocks and compute has neither, so only these
            // three can. This is what keeps the module probe off every program without
            // tessellation or geometry - which is every program Minecraft and its shader packs
            // build.
            Bool CanDeclareBlocksInBothDirections(ShaderStage stage) {
                return stage == ShaderStage::TessControl || stage == ShaderStage::TessEval ||
                       stage == ShaderStage::Geometry;
            }

            // Reflection names an array uniform after its first element ("g_image[0]") at every
            // location it spans; SPIR-V names the variable once, without the subscript. This is
            // the name both sides agree on.
            String ImageUniformBaseName(const String& reflectionName) {
                if (reflectionName.size() >= 3 && reflectionName.compare(reflectionName.size() - 3, 3, "[0]") == 0) {
                    return reflectionName.substr(0, reflectionName.size() - 3);
                }
                return reflectionName;
            }

            // GL_MAX_<stage>_IMAGE_UNIFORMS as the ES driver reports it, which is also exactly what
            // MobileGL advertises for it (GL_Getter answers from the same DynamicBackendParameters).
            // -1 for a stage the ES side has no such limit for, which is the "cannot say" answer
            // the diagnostic that reads it prints rather than a made-up number. [[maybe_unused]]
            // because its only caller is an MGLOG_E argument, and MGLOG_E compiles to nothing in a
            // build whose MOBILEGL_LOG_ACTIVE_LEVEL is above ERROR.
            [[maybe_unused]] Int AdvertisedStageImageUniformLimit(ShaderStage stage) {
                switch (stage) {
                case ShaderStage::Vertex: return g_GLESCapabilities.MaxVertexImageUniforms;
                case ShaderStage::Geometry: return g_GLESCapabilities.MaxGeometryImageUniforms;
                case ShaderStage::Fragment: return g_GLESCapabilities.MaxFragmentImageUniforms;
                case ShaderStage::Compute: return g_GLESCapabilities.MaxComputeImageUniforms;
                default: return -1;
                }
            }

            // Whether a glslang layout format is one GLSL ES has in core; the rest reach ES only
            // through GL_NV_image_formats. Asked of DECLARED formats, which this backend passes
            // through untouched - the emitted ESSL still has to be legal for the driver.
            Bool IsCoreEsslLayoutFormat(glslang::TLayoutFormat format) {
                switch (format) {
                case glslang::ElfRgba32f:
                case glslang::ElfRgba16f:
                case glslang::ElfR32f:
                case glslang::ElfRgba8:
                case glslang::ElfRgba8Snorm:
                case glslang::ElfRgba32i:
                case glslang::ElfRgba16i:
                case glslang::ElfRgba8i:
                case glslang::ElfR32i:
                case glslang::ElfRgba32ui:
                case glslang::ElfRgba16ui:
                case glslang::ElfRgba8ui:
                case glslang::ElfR32ui:
                    return true;
                default:
                    return false;
                }
            }

            // The GL internal format a glslang layout format names, for the eighteen non-core
            // formats WidenImageFormatsForEssl carries losslessly plus nothing else: the only
            // question asked of it is "does this DECLARED format widen", and answering 0 for
            // everything else is the same "no" a non-widenable format gets. Kept as its own
            // switch rather than routed through the frontend's enum converters because a
            // TLayoutFormat is a glslang value and the reflection snapshot stores it raw.
            //
            // IT MUST LIST EXACTLY WHAT WideningOfSpirvImageFormat DOES. This table is what arms
            // the pass (ImageFormatWillBeWidened -> declaresWidenableImageFormat), so a format the
            // pass would carry but this switch answers 0 for never gets the chance: the module
            // reaches SPIRV-Cross with its original qualifier, the throw takes the stage, and the
            // only visible symptom is the "no GLSL ES spelling" diagnostic for a format that has
            // one. That is exactly what r11f_g11f_b10f did until it was added here.
            Uint GLInternalFormatOfLayoutFormat(glslang::TLayoutFormat format) {
                switch (format) {
                case glslang::ElfRg32f: return 0x8230;    // GL_RG32F
                case glslang::ElfRg16f: return 0x822F;    // GL_RG16F
                case glslang::ElfR16f: return 0x822D;     // GL_R16F
                case glslang::ElfRg8: return 0x822B;      // GL_RG8
                case glslang::ElfR8: return 0x8229;       // GL_R8
                case glslang::ElfRg8Snorm: return 0x8F95; // GL_RG8_SNORM
                case glslang::ElfR8Snorm: return 0x8F94;  // GL_R8_SNORM
                case glslang::ElfRg32i: return 0x823B;    // GL_RG32I
                case glslang::ElfRg16i: return 0x8239;    // GL_RG16I
                case glslang::ElfR16i: return 0x8233;     // GL_R16I
                case glslang::ElfRg8i: return 0x8237;     // GL_RG8I
                case glslang::ElfR8i: return 0x8231;      // GL_R8I
                case glslang::ElfRg32ui: return 0x823C;   // GL_RG32UI
                case glslang::ElfRg16ui: return 0x823A;   // GL_RG16UI
                case glslang::ElfR16ui: return 0x8234;    // GL_R16UI
                case glslang::ElfRg8ui: return 0x8238;    // GL_RG8UI
                case glslang::ElfR8ui: return 0x8232;     // GL_R8UI
                // Not a channel widening but a lossless re-encoding into rgba16f - the one entry
                // here whose carrier has a different per-channel layout. See
                // WidenImageFormatsPass.h.
                case glslang::ElfR11fG11fB10f: return 0x8C3A; // GL_R11F_G11F_B10F
                // 10/10/10/2 unsigned INTEGER channels in an rgba16ui: same component type, same
                // channel count, every value representable. Only the transfer is re-encoded.
                case glslang::ElfRgb10a2ui: return 0x906F; // GL_RGB10_A2UI
                // The seven NORMALIZED formats, carried in an rgba16ui as their own channel CODES.
                // These are the entries whose carrier changes the shader-visible type as well as
                // the qualifier (image2D becomes uimage2D), so every access through them is
                // wrapped in the GL 4.6 2.3.5 conversion - see WidenImageFormatsPass.h.
                case glslang::ElfRgba16: return 0x805B;      // GL_RGBA16
                case glslang::ElfRg16: return 0x822C;        // GL_RG16
                case glslang::ElfR16: return 0x822A;         // GL_R16
                case glslang::ElfRgb10A2: return 0x8059;     // GL_RGB10_A2
                case glslang::ElfRgba16Snorm: return 0x8F9B; // GL_RGBA16_SNORM
                case glslang::ElfRg16Snorm: return 0x8F99;   // GL_RG16_SNORM
                case glslang::ElfR16Snorm: return 0x8F98;    // GL_R16_SNORM
                default:
                    return 0;
                }
            }

            // Whether the ESSL chain will re-declare an image of this format in a core carrier
            // and mask its accesses (WidenImageFormatsForEssl). The same rule
            // TextureImpl::GetImageBindableStorageWidening applies to the storage and the bind -
            // the three layers move together or the shader addresses a texel size the storage
            // does not have.
            //
            // Without GL_NV_image_formats there is no legal spelling for any non-core format, so
            // everything carriable widens. WITH the extension only the formats SPIRV-Cross
            // refuses to print do: it throws for its is_desktop_only_format set instead of
            // emitting a token, and the throw loses the stage however willing the driver was.
            Bool ImageFormatWillBeWidened(Uint glInternalFormat) {
                if (glInternalFormat == 0) return false;
                if (MG_Util::ShaderTranspiler::ShaderCompiler::WidenedCoreEsslImageFormat(glInternalFormat) == 0) {
                    return false;
                }
                return !g_GLESCapabilities.SupportsExtendedImageFormats ||
                       !MG_Util::ShaderTranspiler::ShaderCompiler::SpirvCrossCanPrintEsslImageFormat(
                           glInternalFormat);
            }
        } // namespace

        // What the format bake needs from the frontend, collected in one walk of the uniform
        // reflection: which image uniforms declared NO format (the only ones a bake may touch -
        // a declared format is authoritative and stays), what the units they address currently
        // hold, and whether any format in play - declared or baked - is outside the ES core set.
        ImageFormatBakeInputs CollectImageFormatBakeInputs(
            const MG_State::GLState::ProgramObject& stateProgramObject) {
            ImageFormatBakeInputs inputs;
            // A format GLSL ES cannot spell on a driver with no GL_NV_image_formats to spell it
            // with. There is no legal ESSL for such a shader at all, so the stage will not
            // compile and the program is lost - a failure that used to leave nothing behind but
            // a draw that rendered nothing. Recorded and reported ONCE per program build rather
            // than per uniform: an image array reaches this decision once per element.
            String unspellableUniform;
            String unspellableFormat;
            Uint unspellableCount = 0;
            const auto recordUnspellableFormat = [&](const String& uniformName, String formatSpelling) {
                if (unspellableCount == 0) {
                    unspellableUniform = uniformName;
                    unspellableFormat = Move(formatSpelling);
                }
                ++unspellableCount;
            };

            const Uint maxUniformLoc = stateProgramObject.GetMaxUniformLocation();
            for (Uint loc = 0; loc <= maxUniformLoc; ++loc) {
                const auto& name = stateProgramObject.GetUniformName(loc);
                if (name.empty()) continue;
                if (!IsImageUniformType(stateProgramObject.GetUniformType(loc))) continue;
                const auto& type = stateProgramObject.GetUniformTypeFacts(loc);
                if (type.hasFormat) {
                    // Declared, and therefore never overridden by the BAKE - but a non-core
                    // spelling still has to become legal ESSL somehow.
                    const auto declaredFormat = static_cast<glslang::TLayoutFormat>(type.layoutFormat);
                    if (!IsCoreEsslLayoutFormat(declaredFormat)) {
                        // Eighteen of the twenty-six non-core formats are re-declared in a core
                        // format that carries them losslessly, with every access masked back to
                        // the channels GL says they have (WidenImageFormatsForEssl, and the
                        // matching storage/bind widening in TextureImpl). Those need neither the
                        // extension nor the diagnostic: there IS a legal spelling for them now.
                        if (ImageFormatWillBeWidened(GLInternalFormatOfLayoutFormat(declaredFormat))) {
                            inputs.declaresWidenableImageFormat = true;
                        } else {
                            inputs.needsExtendedImageFormats = true;
                            if (!g_GLESCapabilities.SupportsExtendedImageFormats) {
                                // From the OWNED TypeFacts, not from a live TType: the reflection
                                // snapshot already carries the declared layout format, and there
                                // is no glslang object to ask on a translation-cache L1 hit.
                                recordUnspellableFormat(
                                    name, glslang::TQualifier::getLayoutFormatString(declaredFormat));
                            }
                        }
                    }
                    continue;
                }
                const Int unit = stateProgramObject.GetUniformSamplerOrImageUnitIndex(loc);
                if (unit < 0 || unit >= MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS) continue;
                const Uint boundFormat = BoundImageUnitFormat(unit);

                // Every format-less uniform contributes to the rebuild key, including one whose
                // unit holds nothing yet: an image bound for the first time AFTER the link has
                // to move the key, or the program built against "nothing bound" would never be
                // rebuilt against the real format.
                inputs.units.push_back(unit);
                inputs.signature = MixImageUnitFormat(inputs.signature, unit, boundFormat);

                if (boundFormat == 0) continue;
                if (!MG_Util::ShaderTranspiler::ShaderCompiler::GLInternalFormatIsCoreEsslImageFormat(boundFormat)) {
                    // The same three-way split the DECLARED branch above makes, and it has to be
                    // the same one: a format-less image is baked with the bound format, so from
                    // WidenImageFormatsForEssl's point of view the two routes hand it identical
                    // modules and must arm it identically.
                    if (ImageFormatWillBeWidened(boundFormat)) {
                        // The bake writes this format INTO the module, so the widening that runs
                        // straight after has to be armed for it even though nothing DECLARED it -
                        // and armed WHETHER OR NOT the driver has GL_NV_image_formats. SPIRV-Cross
                        // throws for its is_desktop_only_format set the moment it targets ESSL,
                        // however willing the driver was, so the extension decides HOW MUCH gets
                        // widened (widenOnlyUnprintableImageFormats) and never WHETHER. Arming
                        // this only on the no-extension path left the shader half of the widening
                        // switched off while TextureImpl's storage/bind half - which keys on
                        // SpirvCrossCanPrintEsslImageFormat, not on the driver bit - still ran:
                        // the stage threw, the program linked without it, and every dispatch
                        // silently did nothing. That is the whole of
                        // KHR-GL43.stencil_texturing.functional's compute half, whose uni_image is
                        // a format-less uimage2D bound to an R8UI texture.
                        inputs.declaresWidenableImageFormat = true;
                    } else if (!g_GLESCapabilities.SupportsExtendedImageFormats) {
                        // Outside the GLSL ES core set, with no GL_NV_image_formats to spell it
                        // and no core format that carries it exactly: there is no legal ESSL for
                        // this stage at all. Leaving the image format-LESS is NOT a softer
                        // failure: all three test devices reject a format-less image declaration
                        // outright ("all images have to define layout format"), readonly and
                        // writeonly alike, so it trades one hard compile error for another. The
                        // unit stays in the rebuild key either way, so a rebind to a spellable
                        // format still rebuilds and works.
                        MGLOG_D("Image uniform '%s' has no declared format and its unit %d holds 0x%x, which "
                                "GLSL ES core cannot spell, this driver has no GL_NV_image_formats for, and "
                                "no core format carries exactly.",
                                name.c_str(), unit, boundFormat);
                        recordUnspellableFormat(
                            name, MG_Util::ShaderTranspiler::ShaderCompiler::EsslImageFormatSpelling(boundFormat));
                        continue;
                    } else {
                        inputs.needsExtendedImageFormats = true;
                    }
                }
                const String baseName = ImageUniformBaseName(name);
                const auto existing = inputs.glFormatByUniformName.find(baseName);
                if (existing == inputs.glFormatByUniformName.end()) {
                    inputs.glFormatByUniformName.emplace(baseName, boundFormat);
                } else if (existing->second != boundFormat) {
                    // An ARRAY whose elements were pointed at units holding different formats.
                    // One declaration carries one qualifier, so there is no spelling for it, and
                    // the uniform is left format-less rather than given a format that is wrong
                    // for all but one element. Marked in place with GL_NONE and swept below -
                    // never by erasing here, because the entry is reached again by the array's
                    // remaining elements and a flat hash map must not be mutated structurally
                    // while an iterator into it is live.
                    existing->second = 0;
                }
            }
            for (const auto& entry : inputs.glFormatByUniformName) {
                if (entry.second == 0) inputs.conflictedNames.push_back(entry.first);
            }
            for (const auto& conflicted : inputs.conflictedNames) {
                inputs.glFormatByUniformName.erase(conflicted);
            }
            // Split off the ones SPIRV-Cross will not print. They cannot go through the module -
            // it throws for them when targeting ESSL, and the stage is lost - so they are spelled
            // into the emitted text instead. Collected first, erased after, because a flat hash
            // map must not be restructured while it is being walked.
            Vector<String> textCompleted;
            for (const auto& entry : inputs.glFormatByUniformName) {
                if (MG_Util::ShaderTranspiler::ShaderCompiler::SpirvCrossCanPrintEsslImageFormat(entry.second)) {
                    continue;
                }
                // A format the widening carries stays on the module route even though SPIRV-Cross
                // would not print it: by the time SPIRV-Cross sees the declaration it names the
                // core carrier, which it does print. Writing the narrow spelling into the text
                // instead would put back exactly the token the driver rejects.
                if (ImageFormatWillBeWidened(entry.second)) {
                    continue;
                }
                String spelling = MG_Util::ShaderTranspiler::ShaderCompiler::EsslImageFormatSpelling(entry.second);
                if (spelling.empty()) continue; // no image-format spelling at all; nothing to write
                inputs.esslFormatQualifierByUniformName.emplace(entry.first, Move(spelling));
                textCompleted.push_back(entry.first);
            }
            for (const auto& name : textCompleted) {
                inputs.glFormatByUniformName.erase(name);
            }
            // Unlatched MGLOG_E, like the transpile- and link-failure diagnostics in SyncToBackend:
            // one line per failing program build, and naming the uniform and the format is the
            // whole diagnostic value. Left as a log rather than a link failure on purpose - the
            // frontend has already reported LINK_STATUS = true and GL cannot retract it, and the
            // program stays queryable exactly as the "linked but not drawable" exit leaves it.
            if (unspellableCount != 0) {
                MGLOG_E("Image format '%s' on uniform '%s' has no GLSL ES spelling and this driver does not expose "
                        "GL_NV_image_formats%s; the stage using it cannot compile and the program will draw "
                        "nothing.",
                        unspellableFormat.empty() ? "(none)" : unspellableFormat.c_str(), unspellableUniform.c_str(),
                        unspellableCount > 1 ? " (and it is not the only image uniform affected)" : "");
            }
            return inputs;
        }

        // Every image ARRAY whose elements the application did NOT leave on units consecutive from
        // element zero - the only shape ESSL can spell, since an image unit there comes solely
        // from the one layout(binding=N) an array declaration carries. Desktop GL assigns them
        // per element with glUniform1i, which ES makes an INVALID_OPERATION on an image uniform,
        // so there is nothing to fix at the API end and the emitted text has to carry it
        // (RemapImageArrayElementUnits). Empty for every program that does not do this, which is
        // very nearly all of them - one walk of the reflection and no allocation in that case.
        Vector<ImageArrayUnitPlan> CollectNonConsecutiveImageArrayPlans(
            const MG_State::GLState::ProgramObject& stateProgramObject) {
            Vector<ImageArrayUnitPlan> plans;
            const Uint maxUniformLoc = stateProgramObject.GetMaxUniformLocation();
            for (Uint loc = 0; loc <= maxUniformLoc; ++loc) {
                const auto& name = stateProgramObject.GetUniformName(loc);
                if (name.empty()) continue;
                if (!IsImageUniformType(stateProgramObject.GetUniformType(loc))) continue;
                // Reflection repeats the array's "g_image[0]" spelling at EVERY location the array
                // spans, so only the location that name resolves back to is the array itself.
                if (stateProgramObject.GetUniformLocation(name) != static_cast<Int>(loc)) continue;
                const String baseName = ImageUniformBaseName(name);
                if (baseName == name) continue; // a scalar image: one binding says it all

                ImageArrayUnitPlan plan;
                plan.name = baseName;
                for (Uint element = loc; element <= maxUniformLoc &&
                                         stateProgramObject.UniformLocationsAliasSameUniform(
                                             static_cast<Int>(loc), static_cast<Int>(element));
                     ++element) {
                    plan.units.push_back(stateProgramObject.GetUniformSamplerOrImageUnitIndex(element));
                }
                if (plan.units.size() < 2) continue;

                Bool consecutive = true;
                for (SizeT element = 0; element < plan.units.size(); ++element) {
                    if (plan.units[element] != plan.units[0] + static_cast<Int>(element)) {
                        consecutive = false;
                        break;
                    }
                }
                // What ESSL does unaided is already right; leaving these out is what keeps the
                // emitted text of every ordinary image shader byte-identical to before.
                if (consecutive) continue;
                plans.push_back(Move(plan));
            }
            return plans;
        }

        Uint64 BackendProgramObjectImpl::ComputeImageUnitFormatSignature() const {
            if (m_formatlessImageUnits.empty()) return 0; // all but a handful of programs
            Uint64 signature = 0;
            for (const Int unit : m_formatlessImageUnits) {
                signature = MixImageUnitFormat(signature, unit, BoundImageUnitFormat(unit));
            }
            return signature;
        }

        Bool BackendProgramObjectImpl::ImageUnitFormatsStillMatch() const {
            if (m_formatlessImageUnits.empty()) return m_imageUnitFormatSignature == 0;
            return ComputeImageUnitFormatSignature() == m_imageUnitFormatSignature;
        }


        // ===== THE MEMOIZED SEGMENT (shader translation memo, level 2) =====
        //
        // One stage's sanitized SPIR-V turned into the ESSL SPIRV-Cross emits, through the
        // DirectGLES-specific pass chain. Extracted out of SyncToBackend's loop so that the
        // boundary the L2 memo keys on is a function signature rather than a comment: every
        // input this reads is either an argument below or a process-global capability bit,
        // and EVERY ONE OF THEM IS IN EsslTranslationKeyInputs. If you add a read here, add
        // it to BuildEsslTranslationKey too - an under-specified key here is a silently
        // miscompiled shader.
        //
        // Reads (audited): the arguments; g_GLESCapabilities.{SupportsViewportArray,
        // MaxSamples, MaxColorTextureSamples, MaxIntegerSamples, MaxDepthTextureSamples,
        // SupportsNoperspectiveInterpolation, SupportsExtendedImageFormats, GLESVersion}
        // (the last via ResolveBackendEsslVersion); and m_backendProgramId, for a log line only.
        //
        // Deliberately NOT in here, and therefore NOT in the key: the text-level passes that
        // follow in SyncToBackend. They are cheap string work and they read a long tail of
        // live per-program state (RebindImageUniformsToFrontendUnits walks the ProgramObject
        // reflection, the norm-clamp masks and the fragColor broadcast count are live
        // globals, the buffer-texture tier retargets an #extension line) whose inclusion
        // would make the key both enormous and fragile for no measurable saving.
        //
        // Returns false when SPIRV-Cross refused the module; `outError` then holds its
        // message and nothing is memoized.
        Bool BackendProgramObjectImpl::TranspileSpirvToEssl(
            const Vector<unsigned int>& spirvCode, const GLenum glShaderType,
            const std::set<String>& xfbCaptureBlockNames, const ImageFormatBakeInputs& imageFormatBake,
            const UnorderedMap<String, Int>& storageBlockBindingOverrides,
            const std::map<String, String>& inputBlockRenames,
            const std::map<String, String>& outputBlockRenames, const Bool stripInputBlockLocations,
            const Bool stripOutputBlockLocations,
            const Int atomicCounterEsslBindingTop, const Bool enableSpirvValidation, String& outSource,
            std::set<String>& outFlattenedXfbBlockNames, Vector<Int>& outAtomicCounterGlBindings,
            String& outError) const {
            // ESSL cannot express gl_DrawID/gl_BaseInstance/gl_BaseVertex; demote them to
            // plain globals (mg_*) before handing the module to SPIRV-Cross.
            Vector<unsigned int> loweredSpirv;
            const Vector<unsigned int>* effectiveSpirv = &spirvCode;
            if (glShaderType == GL_VERTEX_SHADER &&
                MG_Util::ShaderTranspiler::ShaderCompiler::LowerDrawParametersForEssl(spirvCode, loweredSpirv, enableSpirvValidation) &&
                !loweredSpirv.empty()) {
                effectiveSpirv = &loweredSpirv;
            }

            // ESSL cannot express gl_ViewportIndex either, but unlike the draw parameters
            // there IS an extension that provides it - so this runs only when the driver does
            // NOT advertise GL_OES_viewport_array. A driver that does keeps the builtin and
            // gets the `#extension` request added to the decompiled source below instead.
            // Demoting the builtin costs the multi-viewport routing (every invocation lands in
            // viewport 0), which is the degradation ViewportArrayScenario already documents
            // for this backend; NOT demoting it costs the whole program, because the stage
            // fails to compile and every draw made with it silently renders nothing.
            // Gated on the module actually declaring the output, so no other stage pays an
            // optimizer round trip for it.
            // One parse of the module answers every armed pass gate below. The per-gate
            // Declares* probes each cost a BuildModule per stage, and on a driver where both
            // gates are armed (Mali: no GL_OES_viewport_array AND integer multisample
            // squeezed to 1) the doubled parse made compile-heavy workloads ~10% slower.
            // Probing the pre-lowering module is sound for both gates: demoting
            // gl_ViewportIndex neither adds nor removes multisampled image types.
            // Recomputed here rather than calling GL_Getter's GetAdvertisedMaxSamples():
            // this is backend code and must not reach into the GL frontend. 4 is that
            // translation unit's kFrontendMaxSamples, which is the source of truth -
            // keep the two in step.
            const Int advertisedMaxSamples =
                std::max(g_GLESCapabilities.MaxSamples, kFrontendMaxSamples);
            // Armed by the EMULATION as well as by the missing extension, and the emulation is on
            // by default (MOBILEGL_ESPRYT_FORCE_VIEWPORT_ARRAY_EMULATION). Having the extension is not a
            // reason to keep the builtin: it only ever gave the SHADER a compilable name, while
            // the driver's INDEXED viewport state was never programmed by anything in MobileGL
            // (SyncRenderState pushes index 0 and stops), so an extension-capable driver
            // rasterized every index as index 0 exactly like a driver without it. Lowering here
            // is what lets the ESSL passes downstream turn the builtin into the flat varying the
            // replay gates on.
            //
            // Restricted to the three stages GL lets WRITE the builtin (4.1 core gives it to the
            // geometry stage, ARB_shader_viewport_layer_array adds vertex and tessellation
            // evaluation). A fragment stage's gl_ViewportIndex is an INPUT, which the pass
            // declines anyway, and a compute stage has none - so arming those two only ever
            // bought them the shared probe's BuildModule for nothing.
            const Bool stageCanWriteViewportIndex = glShaderType == GL_VERTEX_SHADER ||
                                                    glShaderType == GL_TESS_EVALUATION_SHADER ||
                                                    glShaderType == GL_GEOMETRY_SHADER;
            const Bool viewportLoweringArmed =
                stageCanWriteViewportIndex &&
                (ViewportArrayEmulationEnabled() || !g_GLESCapabilities.SupportsViewportArray);
            const Bool sampleClampArmed =
                g_GLESCapabilities.MaxColorTextureSamples < advertisedMaxSamples ||
                g_GLESCapabilities.MaxIntegerSamples < advertisedMaxSamples ||
                g_GLESCapabilities.MaxDepthTextureSamples < advertisedMaxSamples;
            // The image-format widening is armed on EVERY driver, so its probe has to ride the
            // shared parse rather than add one: it is asked of every stage of every program, and
            // a BuildModule per stage per gate is exactly what cost compile-heavy CTS cases ~10%
            // before this struct existed. What differs per driver is only HOW MUCH it widens -
            // everything carriable where there is no GL_NV_image_formats to spell the narrow
            // format, and only the formats SPIRV-Cross refuses to print where there is.
            const Bool widenOnlyUnprintableImageFormats = g_GLESCapabilities.SupportsExtendedImageFormats;
            MG_Util::ShaderTranspiler::ShaderCompiler::SpirvGateFeatures spirvGates;
            if (viewportLoweringArmed || sampleClampArmed) {
                spirvGates = MG_Util::ShaderTranspiler::ShaderCompiler::ProbeSpirvGateFeatures(
                    *effectiveSpirv);
            }

            Vector<unsigned int> loweredViewportSpirv;
            if (viewportLoweringArmed && spirvGates.WritesViewportIndexOutput &&
                MG_Util::ShaderTranspiler::ShaderCompiler::LowerViewportIndexForEssl(
                    *effectiveSpirv, loweredViewportSpirv, enableSpirvValidation) &&
                !loweredViewportSpirv.empty()) {
                effectiveSpirv = &loweredViewportSpirv;
                MGLOG_D("Program %u stage %s writes gl_ViewportIndex, which ESSL has no core "
                        "spelling for. The builtin was demoted to a plain global; %s.",
                        m_backendProgramId,
                        MG_Util::ConvertGLEnumToString(glShaderType).c_str(),
                        ViewportArrayEmulationEnabled()
                            ? "the ESSL passes below promote it to a routing varying"
                            : "every invocation renders into viewport 0");
            }

            // GL 4.6 core table 23.53 requires GL_MAX_SAMPLES >= 4, so every multisample
            // ceiling MobileGL advertises is floored to 4 no matter what the ES driver
            // reports - but the realised allocation cannot be, and
            // ClampSamplesToBackendSupport quietly gives an integer or depth multisample
            // texture the ONE sample Adreno and Mali actually support for it. A shader
            // written against the advertised ceiling then fetches a sample that storage does
            // not have and reads garbage; KHR-GL33/40/41.texture_swizzle.functional_* and
            // KHR-GLxx.texture_size_promotion.functional bake exactly that literal in. Clamp
            // the Sample operand to the backend-real per-category maximum so the fetch lands
            // inside the allocation. Gated on some category actually being squeezed AND the
            // module actually declaring a multisampled image, so no other stage pays an
            // optimizer round trip for it. DirectVulkan is deliberately not given this: it
            // allocates the sample count it was asked for, so its modules are already right.
            Vector<unsigned int> clampedSampleSpirv;
            if (sampleClampArmed && spirvGates.DeclaresMultisampledImage &&
                MG_Util::ShaderTranspiler::ShaderCompiler::ClampMultisampleFetchesForEssl(
                    *effectiveSpirv, clampedSampleSpirv,
                    g_GLESCapabilities.MaxColorTextureSamples,
                    g_GLESCapabilities.MaxIntegerSamples,
                    g_GLESCapabilities.MaxDepthTextureSamples, advertisedMaxSamples,
                    enableSpirvValidation) &&
                !clampedSampleSpirv.empty()) {
                effectiveSpirv = &clampedSampleSpirv;
            }

            // GLSL ES has no ARRAY vertex inputs, and SPIRV-Cross refuses the whole module
            // rather than emulating them, so this has to happen before it sees the binary.
            Vector<unsigned int> splitArrayInputSpirv;
            if (glShaderType == GL_VERTEX_SHADER &&
                MG_Util::ShaderTranspiler::ShaderCompiler::SplitArrayVertexInputsForEssl(
                    *effectiveSpirv, splitArrayInputSpirv, enableSpirvValidation) &&
                !splitArrayInputSpirv.empty() && splitArrayInputSpirv != *effectiveSpirv) {
                // Only when the pass ACTUALLY split something. The optimizer hands back a
                // re-serialised copy either way, and adopting that copy for every vertex
                // shader would put every one of them through a round trip they do not need
                // - which is not free: it cost the create-indirect retrace 0.15 SSIM the
                // first time this gate was missing.
                effectiveSpirv = &splitArrayInputSpirv;
            }

            // Adopt the rewritten module only when THIS stage actually had one of the
            // blocks - the optimizer hands back a re-serialised copy either way, and taking
            // that copy for a module it did not rewrite is not free (it cost the
            // create-indirect retrace 0.15 SSIM when the array-input split first missed
            // this gate). The report has to be per stage, not cumulative: a fragment shader
            // consuming the same block reports a name the vertex stage already reported,
            // and its own rewrite must still be taken or the two stages stop matching.
            Vector<unsigned int> flattenedXfbSpirv;
            if (!xfbCaptureBlockNames.empty()) {
                // Reported into a local first, and published only if the module is really
                // adopted. The caller unions the published set unconditionally (so that a
                // cache HIT contributes its names too), so publishing a name for a rewrite
                // that was declined would rename a capture the emitted ESSL never renamed.
                std::set<String> flattenedNames;
                if (MG_Util::ShaderTranspiler::ShaderCompiler::FlattenXfbInterfaceBlocksForEssl(
                        *effectiveSpirv, xfbCaptureBlockNames, flattenedNames, flattenedXfbSpirv,
                        enableSpirvValidation) &&
                    !flattenedXfbSpirv.empty() && !flattenedNames.empty()) {
                    effectiveSpirv = &flattenedXfbSpirv;
                    outFlattenedXfbBlockNames = Move(flattenedNames);
                }
            }

            // The producer-keyed interface-block rename, planned program-wide by the caller and
            // applied to this stage: the blocks it CONSUMES are spelled after the previous stage
            // present in the program and the ones it PRODUCES after itself, so a tessellation
            // evaluation stage's two TCSOutputBlocks stop being one name and every other stage
            // still agrees with it. See UniquifyIoBlockNamesPass for why Mali needs it.
            //
            // INSIDE THE MEMOIZED SEGMENT, and at exactly the position it was written in - after
            // the XFB flatten, before the UBO precision strip. Both halves of that matter:
            //   * INSIDE, because it rewrites the MODULE and the emitted ESSL carries the result.
            //     Left outside, a second program sharing this stage's key would be served ESSL
            //     with the blocks un-renamed and the repair would silently stop working - the
            //     same trap SetAtomicCounterBlockBindings sets one screen down.
            //   * AT THIS POSITION, because moving a SPIR-V pass in a chain is a behavioural
            //     change, and this one arrived device-verified on Mali. Hoisting it above the
            //     cache probe instead would have needed no key material at all (the rename would
            //     already be in the module bytes the key hashes) and was rejected for that
            //     reason: it reorders the chain, and it would re-serialise the module on every
            //     build including the ones the memo is there to make free.
            // Its two rename maps are therefore KEY MATERIAL - see the caller.
            Vector<unsigned int> uniquifiedIoBlockSpirv;
            if (!inputBlockRenames.empty() || !outputBlockRenames.empty()) {
                std::set<String> stageRenamedIoBlockNames;
                if (MG_Util::ShaderTranspiler::ShaderCompiler::UniquifyIoBlockNamesForEssl(
                        *effectiveSpirv, inputBlockRenames, outputBlockRenames,
                        stageRenamedIoBlockNames, uniquifiedIoBlockSpirv, enableSpirvValidation) &&
                    !uniquifiedIoBlockSpirv.empty() && !stageRenamedIoBlockNames.empty()) {
                    effectiveSpirv = &uniquifiedIoBlockSpirv;
                    MGLOG_D("Program %u stage %s: %zu inter-stage interface block(s) renamed per "
                            "producing stage, because some stage of this program declares the same "
                            "block name in both directions and the ES driver may alias the two.",
                            m_backendProgramId, MG_Util::ConvertGLEnumToString(glShaderType).c_str(),
                            stageRenamedIoBlockNames.size());
                }
            }

            // ESSL stage-matches uniform blocks by member precision, but SPIRV-Cross prints
            // a RelaxedPrecision member as explicit "mediump" in the vertex stage and as
            // UNQUALIFIED (mediump-by-default) in the fragment stage; after
            // ForceSupporterOutput swaps the fragment header to highp, that member reads
            // back as highp and the ES driver refuses to link ("definitions of uniform
            // block ... do not match"). Strip the hint from block structs so both stages
            // declare the member highp; nothing else about emission changes.
            Vector<unsigned int> uboPrecisionSpirv;
            if (MG_Util::ShaderTranspiler::ShaderCompiler::StripUboMemberRelaxedPrecisionForEssl(
                    *effectiveSpirv, uboPrecisionSpirv, enableSpirvValidation) &&
                !uboPrecisionSpirv.empty()) {
                effectiveSpirv = &uboPrecisionSpirv;
            }

            // noperspective is core desktop GLSL and reaches here as the SPIR-V NoPerspective
            // decoration. SPIRV-Cross renders it as ESSL `noperspective` + `#extension
            // GL_NV_shader_noperspective_interpolation : require`; a driver without that extension
            // rejects the require. So on such devices emulate screen-linear interpolation instead
            // (pre-multiply outputs by gl_Position.w, recover inputs via gl_FragCoord.w) and drop
            // the decoration - exact, extension-free. Devices that have the extension keep the
            // decoration and let the hardware do it natively.
            Vector<unsigned int> noperspectiveSpirv;
            if (!g_GLESCapabilities.SupportsNoperspectiveInterpolation &&
                MG_Util::ShaderTranspiler::ShaderCompiler::EmulateNoPerspectiveForEssl(
                    *effectiveSpirv, noperspectiveSpirv, enableSpirvValidation) &&
                !noperspectiveSpirv.empty()) {
                effectiveSpirv = &noperspectiveSpirv;
            }

            // ES has no rectangle sampler, and SPIRV-Cross refuses the whole module rather
            // than approximating one. The shared pass turns the type into the 2D one and
            // divides the coordinate of every normalized-coordinate lookup by the texture
            // size, which is the whole of the difference between the two.
            Vector<unsigned int> rectLoweredSpirv;
            if (MG_Util::ShaderTranspiler::ShaderCompiler::LowerRectImages(*effectiveSpirv, rectLoweredSpirv, enableSpirvValidation) &&
                !rectLoweredSpirv.empty()) {
                effectiveSpirv = &rectLoweredSpirv;
            }

            // ES has no 1D texture at all, so a 1D ARRAY is stored as a 2D array with height
            // 1 (MapToBackendTextureTarget / GetBackendUploadSize). SPIRV-Cross emulates 1D
            // as 2D for images without ever asking whether the type is arrayed, so a
            // 1D-array image comes out as ivec2(ivec2(u, layer), 0) - three components in a
            // two-component constructor, which every driver rejects, taking the whole
            // program with it. The pass does the conversion properly - type to 2D array,
            // coordinate to (u, 0, layer) - before SPIRV-Cross can apply its own.
            Vector<unsigned int> arrayImageSpirv;
            if (MG_Util::ShaderTranspiler::ShaderCompiler::Lower1DArrayImagesForEssl(*effectiveSpirv,
                                                                                      arrayImageSpirv, enableSpirvValidation) &&
                !arrayImageSpirv.empty()) {
                effectiveSpirv = &arrayImageSpirv;
            }

            // The SAMPLER half of the same 1D story, and a defect one layer deeper than the one
            // above. SPIRV-Cross DOES widen a 1D sampler's coordinate for ES - it just prints the
            // OFFSET and the two GRADIENT operands with the arity the desktop shader spelled, so
            // textureLodOffset(sampler1DArray, vec2, float, int) is emitted against a
            // sampler2DArray and the driver answers "no matching overloaded function found",
            // losing the stage and silently no-oping every dispatch that used it. Widening the
            // operands alone would be an INVALID module (the validator derives the required arity
            // from the image's own Dim), so the pass moves the type to 2D and widens coordinate,
            // offset and gradients together.
            //
            // NO KEY MATERIAL, by the same test LegalizeResourceArrayIndexingForEssl passes:
            // it takes the module and nothing else, no capability bit arms it, and it self-gates
            // on the module's own content (BinaryHasOffsetOrGrad1DSampledImage). The module is
            // already the largest thing in the L2 key, so it is covered completely.
            Vector<unsigned int> sampled1DSpirv;
            if (MG_Util::ShaderTranspiler::ShaderCompiler::Lower1DSampledImagesForEssl(
                    *effectiveSpirv, sampled1DSpirv, enableSpirvValidation) &&
                !sampled1DSpirv.empty()) {
                effectiveSpirv = &sampled1DSpirv;
            }

            // GLSL ES has no format-less image: `writeonly uniform uimage2D` is legal desktop
            // GLSL 4.2 and an Adreno ES compile error ("all images have to define layout
            // format"), which loses the whole program. Give each such image the format the
            // application bound to its unit - the one GL's format-class rules make correct -
            // so SPIRV-Cross prints a qualifier. AFTER the 1D-array lowering above, which
            // also rewrites image types, so this one is looking at the final shapes.
            //
            // Gated on the module actually declaring one: the map is empty for every program
            // whose images all declare formats, and the cheap probe keeps a program that has
            // an unbound format-less image from paying an optimizer round trip per stage.
            Vector<unsigned int> imageFormatSpirv;
            if (!imageFormatBake.glFormatByUniformName.empty() &&
                MG_Util::ShaderTranspiler::ShaderCompiler::DeclaresFormatlessStorageImage(*effectiveSpirv) &&
                MG_Util::ShaderTranspiler::ShaderCompiler::BakeImageFormatsForEssl(
                    *effectiveSpirv, imageFormatBake.glFormatByUniformName, imageFormatSpirv,
                    enableSpirvValidation) &&
                !imageFormatSpirv.empty()) {
                effectiveSpirv = &imageFormatSpirv;
            }

            // GL has forty image formats and GLSL ES core has thirteen; the other twenty-seven
            // reach ES only through GL_NV_image_formats, which no tested driver advertises. A
            // shader declaring one of them has NO legal ESSL spelling at all - SPIRV-Cross throws
            // for some of them and the driver rejects the token for the rest ("'rg32f' : not a
            // legal layout qualifier id"), and dropping the qualifier is refused too ("all images
            // have to define layout format") - so the stage is lost and every draw with the
            // program silently renders nothing. Seventeen of them widen EXACTLY into a core format
            // of the same per-channel width, and this rewrites those declarations to the carrier
            // and masks every access back to the channels GL says the format has. The other nine
            // have no exact carrier and keep the honest diagnostic
            // CollectImageFormatBakeInputs emits.
            //
            // A driver that HAS GL_NV_image_formats still needs part of this. SPIRV-Cross throws
            // for its is_desktop_only_format set when it targets ESSL rather than printing a
            // token, and the throw loses the stage however willing the driver was - Mesa
            // advertises the extension and `layout(r8ui) uimage2D` lost its whole program there
            // until the widening ran for it too. So the driver bit decides HOW MUCH is widened,
            // never WHETHER.
            //
            // AFTER the bake above, deliberately: a format-less image whose unit holds a non-core
            // format is baked with that format and widened here, so both routes end in the same
            // place and there is no second widening rule for baked declarations.
            //
            // KEY MATERIAL: g_GLESCapabilities.SupportsExtendedImageFormats, which selects the
            // mode - see EsslTranslationKeyInputs::supportsExtendedImageFormats. The pass takes
            // no other input: what it rewrites is a pure function of the module's own declared
            // formats and that mode, and the module is already the largest thing in the L2 key.
            // The ARMING flag is deliberately NOT key material: it only decides whether the pass
            // runs, and the module below is adopted only when the pass actually changed the bytes
            // - so a program-wide flag that over-arms a stage costs an optimizer round trip and
            // changes no output.
            //
            // DirectVulkan is deliberately not given this: it takes the declared format natively
            // and resolves the descriptor's view format from the same bind state.
            Vector<unsigned int> widenedImageFormatSpirv;
            if (imageFormatBake.declaresWidenableImageFormat &&
                MG_Util::ShaderTranspiler::ShaderCompiler::WidenImageFormatsForEssl(
                    *effectiveSpirv, widenedImageFormatSpirv, widenOnlyUnprintableImageFormats,
                    enableSpirvValidation) &&
                !widenedImageFormatSpirv.empty() && widenedImageFormatSpirv != *effectiveSpirv) {
                effectiveSpirv = &widenedImageFormatSpirv;
            }

            // GLSL ES demands a constant integral expression to index a fragment output
            // array; SPIR-V does not, so a shader that writes coeff[i] from a loop
            // reaches SPIRV-Cross intact and comes out as ESSL a strict driver rejects
            // outright ("array indexes for fragment outputs must be constant integral
            // expressions"), linking no program and silently no-oping every draw that
            // uses it. Mesa accepts it, ANGLE does not - which is the whole of the
            // improved-transparency-minecraft-26.3 failure. Fold or lower the index here,
            // on the ESSL path only: the same module is legal for DirectVulkan.
            Vector<unsigned int> outputIndexSpirv;
            if (glShaderType == GL_FRAGMENT_SHADER &&
                MG_Util::ShaderTranspiler::ShaderCompiler::LegalizeFragmentOutputIndexingForEssl(
                    *effectiveSpirv, outputIndexSpirv, enableSpirvValidation) &&
                !outputIndexSpirv.empty()) {
                effectiveSpirv = &outputIndexSpirv;
            }

            // Same rule, two more resources, every stage: desktop GL lets an array of
            // storage blocks and an array of image uniforms be indexed with any
            // dynamically-uniform expression, GLSL ES keeps the ES 3.1
            // constant-expression rule for both, and the drivers enforce it - Qualcomm
            // ("indexing into an SSBO array using a non-constant expression is not
            // permitted"), Mesa ("image arrays indexed with non-constant expressions are
            // forbidden in GLSL ES") - losing the stage, the program, and every draw or
            // dispatch that used it, while the frontend keeps reporting the link glslang
            // performed. Fold or lower the index here, on the ESSL path only: the same
            // module is legal for DirectVulkan, which binds the array as one descriptor
            // array.
            //
            // The image half is also what makes RemapImageArrayElementUnits below possible
            // at all: that pass rewrites `g_image[k]` into a per-element declaration, and it
            // can only do that once every k the emitted ESSL spells is a literal.
            //
            // NO KEY MATERIAL, and that is a conclusion rather than an omission: this takes the
            // module and nothing else - no capability bit arms it, no per-program plan steers
            // it - and it self-gates on the module's own content
            // (BinaryHasDynamicResourceArrayIndexing). The module is already the largest
            // thing in the L2 key, so it is fully covered. Contrast LowerViewportIndexForEssl,
            // whose signature is equally module-only but which SupportsViewportArray ARMS -
            // that bit is in the key precisely because of it.
            Vector<unsigned int> blockArrayIndexSpirv;
            if (MG_Util::ShaderTranspiler::ShaderCompiler::LegalizeResourceArrayIndexingForEssl(
                    *effectiveSpirv, blockArrayIndexSpirv, enableSpirvValidation) &&
                !blockArrayIndexSpirv.empty()) {
                effectiveSpirv = &blockArrayIndexSpirv;
            }

            // glslang kept the application's layout(offset = N) on the atomic counters it
            // lowered onto gl_AtomicCounterBlock_<N>, and no std140/std430 layout can put
            // member 0 anywhere but offset 0 - so SPIRV-Cross throws ("cannot be expressed as
            // neither std430 nor std140") and the stage never reaches the driver. Collapse the
            // block into one uint array at offset 0 and re-index each counter to the element
            // that used to be at its byte offset; the buffer then stays bound whole, which it
            // has to (GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT is 64 on Adreno 830 and 32 or
            // more everywhere else, so an 8-byte bind offset is not expressible on any of them).
            // BEFORE SetAtomicCounterBlockBindings
            // below, which only moves the block's BINDING and needs the block intact.
            //
            // NO KEY MATERIAL either, for the same reason - and note where the application's
            // layout(offset = N) values live: glslang already baked them into the module as
            // member Offset decorations, so they are in the key as MODULE BYTES. There is no
            // separate offset input to carry.
            Vector<unsigned int> atomicCounterSpirv;
            if (MG_Util::ShaderTranspiler::ShaderCompiler::FlattenAtomicCounterBlockOffsetsForEssl(
                    *effectiveSpirv, atomicCounterSpirv, enableSpirvValidation) &&
                !atomicCounterSpirv.empty()) {
                effectiveSpirv = &atomicCounterSpirv;
            }

            // The second half of the inter-stage interface-block repair, and the one that
            // actually closes the 420pack group: this driver drops the payload of a block that
            // carries an explicit layout(location=) whenever a tessellation or geometry stage
            // is in the pipeline, so the qualifier comes off and ES matches the block by name
            // and member sequence instead. The names those two sides agree on are the ones the
            // rename above just fixed, which is why this runs AFTER it and not before.
            //
            // The caller arms the two directions; both are false unless the driver POST
            // measured the defect AND this program has a stage that can hit it. Adopted only
            // when this stage really had a located block, for the reason the array-input split
            // documents: the optimizer hands back a re-serialised copy either way.
            //
            // LAST IN THE CHAIN, and that position is load-bearing. Vulkan SPIR-V REQUIRES a
            // Location on every user-defined Input/Output variable
            // ([VUID-StandaloneSpirv-Location-04915]), so the module this produces is
            // deliberately no longer valid Vulkan SPIR-V - it is an ESSL-emission intermediate
            // that goes straight into SPIRV-Cross and reaches no driver as SPIR-V. Running it
            // here means no later pass validates what it produced; the pass itself skips
            // validation for the same reason (see StripIoBlockLocationsForEssl). Anywhere
            // earlier and every remaining pass would latch a validation failure on a module
            // that is doing exactly what it was asked to.
            Vector<unsigned int> strippedIoBlockLocationSpirv;
            if (stripInputBlockLocations || stripOutputBlockLocations) {
                Bool strippedAny = false;
                if (MG_Util::ShaderTranspiler::ShaderCompiler::StripIoBlockLocationsForEssl(
                        *effectiveSpirv, stripInputBlockLocations, stripOutputBlockLocations,
                        strippedAny, strippedIoBlockLocationSpirv, enableSpirvValidation) &&
                    !strippedIoBlockLocationSpirv.empty() && strippedAny) {
                    effectiveSpirv = &strippedIoBlockLocationSpirv;
                    // THE ARMING SIGNAL, and it is INFO on purpose: the per-stage line below is
                    // MGLOG_D, which is compiled out of every build CI and the device runs, so
                    // nothing outside a debug build could tell an armed repair from a silently
                    // un-armed one. Latched, so it costs one line per process rather than one
                    // per stage of every program. The integration lane that pins the emulation
                    // on asserts on exactly this line - see UnlocatedIoBlockScenario.
                    MGLOG_I_ONCE("DirectGLES is emitting inter-stage interface blocks WITHOUT their "
                                 "layout(location) qualifier, because this driver loses a located "
                                 "block's payload across a tessellation or geometry boundary.");
                    MGLOG_D("Program %u stage %s: interface-block location qualifiers dropped "
                            "(%s), because this driver loses a located block's payload across a "
                            "tessellation or geometry boundary.",
                            m_backendProgramId, MG_Util::ConvertGLEnumToString(glShaderType).c_str(),
                            stripInputBlockLocations
                                ? (stripOutputBlockLocations ? "consumed and produced" : "consumed")
                                : "produced");
                }
            }

            MG_Util::ShaderTranspiler::SpvcSession spvcSession(*effectiveSpirv,
                MG_Util::ShaderTranspiler::SessionUsageBit::Transpile);

            spvc_compiler_options options;
            spvcSession.CreateOptions(&options);

            spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION,
                                           ResolveBackendEsslVersion());
            spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE);
            spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_FALSE);

            spvcSession.SetOptions(options);

            // ES fixes a storage block's binding at link from its layout(binding=) qualifier
            // and has no glShaderStorageBlockBinding to move it afterwards, so a rebinding
            // can only be honoured by printing it INTO the qualifier. Rewriting the Binding
            // decoration before SPIRV-Cross emits is what does that; RemoveLayoutBinding
            // then deliberately preserves the qualifier for `buffer` declarations.
            if (!storageBlockBindingOverrides.empty()) { // empty for almost every program
                spvcSession.SetShaderStorageBlockBinding(storageBlockBindingOverrides);
            }

            // Atomic counters, same mechanism for the same reason. glslang already turned
            // every atomic_uint into a member of gl_AtomicCounterBlock_<N> and let the IO
            // mapper pick that block's binding, which has no relation to the GL binding point
            // N the application bound its counter buffer to - and can alias an SSBO the
            // application binds itself. Move each block to its reserved slot and record N, so
            // the draw path knows which GL_ATOMIC_COUNTER_BUFFER points to re-issue as
            // storage-buffer bindings.
            //
            // BOTH HALVES ARE MEMO STATE. `atomicCounterEsslBindingTop` decides the binding
            // this prints into the ESSL, so it is in the L2 key; `outAtomicCounterGlBindings`
            // is an OUTPUT this stage produces and the draw path consumes, so it is in the L2
            // payload. A hit that replayed only the text would leave the bindings empty and
            // every counter buffer unbound - the same class of silent loss the flattened XFB
            // block names would have been.
            spvcSession.SetAtomicCounterBlockBindings(atomicCounterEsslBindingTop,
                                                      outAtomicCounterGlBindings);

            // `layout(index = 0)` is the GL default spelled out loud, and GLSL ES has no such
            // qualifier in core - a stage that prints it is refused with "index layout
            // qualifier requires EXT_blend_func_extended" and the whole program then draws
            // nothing. Drop the decoration when it carries the default; a REAL dual-source
            // index (1) is left alone, because that one genuinely needs the extension and the
            // driver has to see it. Fragment stage only: no other stage can carry it.
            if (glShaderType == GL_FRAGMENT_SHADER) {
                spvcSession.DropDefaultFragmentOutputColorIndex();
            }

            // `readonly writeonly` together says the buffer variable can only be asked its
            // .length(), which the frontend has already enforced - so the pair is inert, and
            // printing it is not. Mesa's ES compiler refuses a block spelled that way and the
            // stage never reaches the program.
            spvcSession.RelaxReadWriteExclusiveStorageBuffers();

            const char* result = nullptr;
            spvcSession.Compile(&result);

            if (!result) {
                // The caller owns the diagnostic: it is the one that knows the
                // frontend program id, and a failed transpile must NOT be memoized -
                // the message names the stage and is worth re-emitting every time.
                const char* lastError = spvcSession.GetLastErrorString();
                outError = lastError ? lastError : "";
                return false;
            }

            outSource = result;
            return true;
        }

        // GL 4.6 core 11.2.2 lets a program have a tessellation EVALUATION shader and no CONTROL
        // shader: the input patch is passed through unmodified and the levels come from the
        // PATCH_DEFAULT_OUTER_LEVEL / PATCH_DEFAULT_INNER_LEVEL state. OpenGL ES 3.2 has no such
        // state and no such allowance - it rejects the program at link, and with an EMPTY info
        // log, which was verified on an Adreno 830 with no MobileGL in the process (TES-only:
        // link=0, log empty; the same shaders plus any TCS: link=1, with or without the SSBO and
        // atomic counter the failing conformance case also declares). The frontend's own glslang
        // link succeeds, so GL_LINK_STATUS reads TRUE, program 0 is bound in its place, and every
        // draw silently renders nothing - a black framebuffer, an atomic counter still at 0 and
        // an untouched SSBO, with no error anywhere.
        //
        // So the missing stage is synthesized and attached here, alongside the program's own.
        // DirectVulkan already does exactly this for the same structural reason
        // (ProgramFactory::BuildPassthroughTessControlSource), so this completes the pair rather
        // than inventing an approach.
        //
        // Nothing that works today can be harmed by it: it fires ONLY for a program that has an
        // evaluation stage and no control stage, and every such program fails to link on ES right
        // now. The worst case is that the synthesized stage fails to compile or link, which leaves
        // the program exactly as dead as it already was - but with a driver log that says why,
        // where today there is an empty one.
        void BackendProgramObjectImpl::AttachPassthroughTessControlStage(
            const MG_State::GLState::ProgramObject& stateProgramObject, const Int tessEvalShaderIndex,
            const Vector<Vector<unsigned int>>& shaderSpirvs, const String& vertexStageEssl,
            const String& tessEvalStageEssl) {
            // PATCH_VERTICES is dynamic state, and it decides the synthesized stage's output
            // patch size - so a program built for one value is stale for another. Recorded here
            // and compared on the draw path (SyncCurrentProgram), the same shape as the
            // storage-block and image-format signatures next to it.
            const Uint patchVertices = MG_State::pGLContext != nullptr
                                           ? MG_State::pGLContext->GetPatchVertices()
                                           : 3u;
            m_passthroughTessControlPatchVertices = static_cast<Int>(patchVertices);
            // PATCH_DEFAULT_{OUTER,INNER}_LEVEL are the same kind of dynamic state and are baked
            // into the same stage (ES has no such state and no entry point to forward them to), so
            // they are recorded and compared alongside the patch size - the two move together, as
            // BuildPassthroughTessControlEssl's contract says.
            m_passthroughTessControlOuterLevel = MG_State::pGLContext != nullptr
                                                     ? MG_State::pGLContext->GetPatchDefaultOuterLevel()
                                                     : FloatVec4(1.0f, 1.0f, 1.0f, 1.0f);
            m_passthroughTessControlInnerLevel = MG_State::pGLContext != nullptr
                                                     ? MG_State::pGLContext->GetPatchDefaultInnerLevel()
                                                     : FloatVec2(1.0f, 1.0f);

            if (tessEvalShaderIndex < 0 ||
                static_cast<SizeT>(tessEvalShaderIndex) >= shaderSpirvs.size()) {
                MGLOG_E("Program %u has a tessellation evaluation stage with no control stage, but no "
                        "SPIR-V for it; the pass-through control stage GL describes cannot be checked, so "
                        "the program is left to fail its ES link.",
                        stateProgramObject.GetExternalIndex());
                m_backendProgramUsable = false;
                return;
            }

            // The one shape the pass-through cannot stand in for. It forwards gl_Position and
            // nothing else, so an evaluation stage that reads a user-defined varying or a
            // per-patch input - both of which carry a Location, where every built-in it needs
            // does not - would start reading undefined values the moment a control stage sat
            // between it and the vertex stage. Declining keeps that from being silent; it is the
            // identical rule DirectVulkan applies in ReflectPassthroughTessControlNeed.
            if (MG_Util::ShaderTranspiler::ShaderCompiler::ModuleReadsLocatedInput(
                    shaderSpirvs[static_cast<SizeT>(tessEvalShaderIndex)])) {
                MGLOG_E("Program %u has a tessellation evaluation stage with no control stage AND reads a "
                        "user-defined input through it; a synthesized pass-through control stage cannot "
                        "forward that, so the program is declined rather than fed an undefined varying.",
                        stateProgramObject.GetExternalIndex());
                m_backendProgramUsable = false;
                return;
            }

            // Mirrored from the neighbours rather than fixed: whether SPIRV-Cross redeclares
            // gl_PerVertex, and with which members, depends on what the application's shaders
            // touched, and a synthesized stage that redeclares a DIFFERENT shape than the stage
            // it feeds is an ES link error against a program with no other problem. gl_in copies
            // the vertex stage's OUT block (that is what arrives) and gl_out the evaluation
            // stage's IN block (that is what is expected). A neighbour that redeclared nothing
            // yields an empty list, which leaves the driver's own built-in declaration in place -
            // which is exactly what matching it requires.
            const String inMembers =
                ExtractPerVertexBlockMembers(vertexStageEssl, /*input=*/false).value_or(String());
            const String outMembers =
                ExtractPerVertexBlockMembers(tessEvalStageEssl, /*input=*/true).value_or(String());

            String source = BuildPassthroughTessControlEssl(ResolveBackendEsslVersion(), patchVertices,
                                                            inMembers, outMembers,
                                                            m_passthroughTessControlOuterLevel,
                                                            m_passthroughTessControlInnerLevel);
            // The mirrored member lists can carry gl_PointSize - the neighbour stage declared it,
            // so matching it is the whole point - and a redeclaration is exactly as illegal as a
            // reference in ESSL without the extension. Same directive, same never-speculative
            // rule as the per-stage loop; a driver with neither spelling gets nothing added and
            // fails below with its own message, which is the honest outcome for a shape it
            // cannot express.
            const char* passthroughPointSizeExtension =
                source.find("gl_PointSize") != String::npos
                    ? PointSizeExtensionName(g_GLESCapabilities.TessellationPointSizeSupport, /*tessellation=*/true)
                    : nullptr;
            source = RequestPointSizeExtension(Move(source), passthroughPointSizeExtension);

            const GLuint backendShaderId = g_GLESFuncs.glCreateShader(GL_TESS_CONTROL_SHADER);
            if (backendShaderId == 0) {
                MGLOG_E("Failed to create the synthesized pass-through tessellation control shader for "
                        "program %u.",
                        stateProgramObject.GetExternalIndex());
                m_backendProgramUsable = false;
                return;
            }

            const char* sourceCStr = source.c_str();
            MGLOG_D("Synthesized pass-through tessellation control stage for program %u (patch vertices "
                    "%u):\n%s",
                    stateProgramObject.GetExternalIndex(), patchVertices, sourceCStr);
            g_GLESFuncs.glShaderSource(backendShaderId, 1, &sourceCStr, nullptr);
            g_GLESFuncs.glCompileShader(backendShaderId);

            // GL_FALSE, not GL_TRUE, for the reason the per-stage loop states: an unwritten
            // out-param must read as "compile failed" and never as a silent success.
            GLint compileStatus = GL_FALSE;
            g_GLESFuncs.glGetShaderiv(backendShaderId, GL_COMPILE_STATUS, &compileStatus);
            if (compileStatus == GL_FALSE) {
                GLint logLength = 0;
                g_GLESFuncs.glGetShaderiv(backendShaderId, GL_INFO_LOG_LENGTH, &logLength);
                if (logLength < 0) logLength = 0;
                Vector<GLchar> log(static_cast<SizeT>(logLength) + 1, '\0');
                g_GLESFuncs.glGetShaderInfoLog(backendShaderId, logLength, nullptr, log.data());
                log.back() = '\0';
                MGLOG_E("The synthesized pass-through tessellation control stage failed to compile for "
                        "program %u. Driver log: %s\nSource:\n%s",
                        stateProgramObject.GetExternalIndex(), log.data(), sourceCStr);
                m_backendProgramUsable = false;
                g_GLESFuncs.glDeleteShader(backendShaderId);
                return;
            }

            g_GLESFuncs.glAttachShader(m_backendProgramId, backendShaderId);
            // Same ownership handover as every other stage: glDeleteShader only FLAGS, so this is
            // what makes the program own it and what keeps a relink from leaking it.
            g_GLESFuncs.glDeleteShader(backendShaderId);
        }

        void BackendProgramObjectImpl::SyncToBackend(
            const SharedPtr<MG_State::GLState::ProgramObject>& stateProgramObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!stateProgramObject) {
                MGLOG_E_ONCE("State program object is null, skipping backend sync.");
                return;
            }
            // Recorded before either early return below, so Use() can always name the GL
            // program a no-op draw belongs to - including the "linked but not drawable" exit.
            m_frontendProgramId = stateProgramObject->GetExternalIndex();

            // GetSpirvStatus() as well as GetLinkStatus(): a program whose phase-B job was
            // cancelled (teardown) or whose optimizer run failed is fully linked and fully
            // queryable, but has no SPIR-V to build a driver program out of. GL cannot retract
            // a LINK_STATUS it already reported true, so "linked but not drawable" is the
            // answer, and this is where the ES backend expresses it.
            if (!stateProgramObject->GetLinkStatus() || !stateProgramObject->GetSpirvStatus()) {
                MGLOG_E_ONCE("Program object is not linked or has no generated SPIR-V, skipping backend sync. State "
                        "program ID: %u",
                        stateProgramObject->GetExternalIndex());
                return;
            }

            MGLOG_D("Syncing program to backend. State program ID: %u, Backend ID: %u",
                    stateProgramObject->GetExternalIndex(), m_backendProgramId);
            // Every link-derived cache below (incl. m_samplerUniformBindings and its
            // lastAssignedUnit/lastAssignedLodBias program-state mirrors) is rebuilt;
            // the sampler-pass memo keyed on them must not survive.
            m_samplerPassMemo.valid = false;
            m_backendProgramUsable = true;
            m_snormFallbackClampOutputMask = g_snormFallbackClampOutputMask;
            m_unormFallbackClampOutputMask = g_unormFallbackClampOutputMask;
            m_fragColorBroadcastCount = g_fragColorBroadcastCount;
            // The generated ESSL bakes these in (see the SetShaderStorageBlockBinding call in the
            // transpile loop below), so the set they were generated against is part of what makes
            // this build current - the draw path compares the signature and rebuilds on a change.
            const auto& storageBlockBindingOverrides = stateProgramObject->GetShaderStorageBlockBindingOverrides();
            m_shaderStorageBlockBindingSignature = ComputeShaderStorageBlockBindingSignature(*stateProgramObject);
            // Rebuilt by the transpile loop below, one entry per atomic-counter block it finds.
            // The top is snapshotted here so every stage of this program - and the draw path
            // reading it afterwards - resolves the same slot for the same GL binding.
            m_atomicCounterGlBindings.clear();
            m_atomicCounterEsslBindingTop = AtomicCounterEsslBindingTop();
            // Re-established by AttachPassthroughTessControlStage below when this program needs
            // one; cleared first so a program that stops needing one (a relink that now attaches
            // a real control stage) does not keep comparing against a stale patch size. The
            // default levels are re-established from the same call and gated on the same -1.
            m_passthroughTessControlPatchVertices = -1;
            m_passthroughTessControlOuterLevel = FloatVec4(1.0f, 1.0f, 1.0f, 1.0f);
            m_passthroughTessControlInnerLevel = FloatVec2(1.0f, 1.0f);
            // The same shape again for image FORMATS: what a format-less image declaration
            // compiles to depends on live glBindImageTexture state, so the pairs it was built
            // against are recorded here and compared per draw (ImageUnitFormatsStillMatch).
            // Taken BEFORE the transpile loop so both the bake and the key see one snapshot.
            const ImageFormatBakeInputs imageFormatBake = CollectImageFormatBakeInputs(*stateProgramObject);
            m_formatlessImageUnits = imageFormatBake.units;
            m_imageUnitFormatSignature = imageFormatBake.signature;
            for (const auto& conflicted : imageFormatBake.conflictedNames) {
                MGLOG_D("Image uniform '%s' of program %u declares no format and its elements address units with "
                        "different bound formats; left format-less.",
                        conflicted.c_str(), stateProgramObject->GetExternalIndex());
            }
            // ...and once more for image ARRAYS whose per-element units are not consecutive, which
            // ESSL has no way to express in one declaration. Program-wide, like the bake, and read
            // from the same snapshot of the reflection; the per-stage rewrite happens below.
            const Vector<ImageArrayUnitPlan> nonConsecutiveImageArrays =
                CollectNonConsecutiveImageArrayPlans(*stateProgramObject);

            // Detach all existing shaders
            GLint attachedCount = 0;
            g_GLESFuncs.glGetProgramiv(m_backendProgramId, GL_ATTACHED_SHADERS, &attachedCount);
            MGLOG_D("Currently attached shaders count: %d", attachedCount);

            if (attachedCount > 0) {
                Vector<GLuint> attachedShaders(attachedCount);
                // Every GL out-param in this function is pre-initialized and every count is
                // re-clamped after the query. A driver that returns without writing the
                // out-param (no current context, a lost context, a stubbed entry point) would
                // otherwise leak an uninitialized stack value straight into a container size
                // or a loop bound - which is exactly how this path used to throw
                // length_error out of a Vector fill-ctor.
                GLsizei actualCount = 0;
                g_GLESFuncs.glGetAttachedShaders(m_backendProgramId, attachedCount, &actualCount,
                                                 attachedShaders.data());
                actualCount = std::clamp<GLsizei>(actualCount, 0, static_cast<GLsizei>(attachedShaders.size()));
                MGLOG_D("Detaching %d existing shaders from program %u", actualCount, m_backendProgramId);

                for (GLsizei i = 0; i < actualCount; ++i) {
                    MGLOG_D("Detaching shader ID: %u from program %u", attachedShaders[i], m_backendProgramId);
                    g_GLESFuncs.glDetachShader(m_backendProgramId, attachedShaders[i]);
                }
            }

            // Attach current shaders.
            //
            // The EXECUTABLE's stage list, not GetAttachedShaders(): this loop indexes
            // shaderSpirvs by the same running index, and the generated SPIR-V is a link
            // artifact while the attach list is live. GL 4.6 core 7.3 makes glAttachShader take
            // effect only at the next link, so a program that is attached to after it linked has
            // MORE entries in the attach list than there are modules - and pairing the two read
            // straight off the end of shaderSpirvs (a std::vector copy from garbage, which is
            // how this crashed). GetLinkedShaderStages() is the list the modules were generated
            // from, one entry per module, in module order.
            const Vector<ShaderStage> linkedStages = stateProgramObject->GetLinkedShaderStages();
            auto& shaderSpirvs = stateProgramObject->GetGeneratedSpirv();
            // Both come from the same Link(), so they agree by construction. If they ever did
            // not there would be no index this function could safely use for EITHER array, so
            // this refuses the build instead of picking one and hoping.
            if (linkedStages.size() != shaderSpirvs.size()) {
                MGLOG_E_ONCE("Program %u: %zu linked stage(s) but %zu generated SPIR-V module(s); refusing to "
                             "build a backend program from mismatched link artifacts.",
                             stateProgramObject->GetExternalIndex(), linkedStages.size(), shaderSpirvs.size());
                m_backendProgramUsable = false;
                return;
            }
            if (stateProgramObject->PointSizeDemoted()) {
                // THE ARMING SIGNAL, INFO on purpose and latched: the integration lane that
                // pins MOBILEGL_POINT_SIZE_DEMOTION=1 asserts on exactly this line, because
                // every rendering assertion stays green on a healthy driver whether the
                // demotion ran or was silently disarmed. See PointSizeDemotionScenario.
                MGLOG_I_ONCE("DirectGLES is building programs whose tessellation/geometry gl_PointSize was "
                             "demoted to an ordinary varying, because this driver cannot host the built-in "
                             "in those stages.");
            }
            MGLOG_D("Attaching %zu shaders to program %u", linkedStages.size(), m_backendProgramId);
            for (const auto& ref : stateProgramObject->GetLinkedShaderSnapshot()) {
                if (!ref.shader) continue;
                const auto& stage =
                    MG_Util::ConvertGLEnumToString(MG_Util::ConvertShaderStageToGLEnum(ref.shader->GetShaderStage()));
                // The source THIS link consumed, which a later glShaderSource does not disturb.
                const String& src = ref.source ? *ref.source : ref.shader->GetShaderSource();
                MGLOG_D("Original src @ %s: \n", stage.c_str());
                MGLOG_D("%s:", src.empty() ? "" : src.c_str());
            }
            const Bool enableSpirvValidation = stateProgramObject->GetSpirvValidationEnabled();

            // Blocks a transform-feedback capture request names a member of ("StageData" of
            // "StageData.attrib[0]"). The Adreno ES driver accepts such a request, links, and
            // then captures nothing at all for it, so those blocks - and ONLY those - get
            // flattened into per-member variables below, in EVERY stage, so a producer and its
            // consumer keep matching. gl_PerVertex members ("gl_Position") carry no block
            // prefix and so never enter this set.
            std::set<String> xfbCaptureBlockNames;
            for (const auto& xfbVarying : stateProgramObject->GetTransformFeedbackVaryings()) {
                const SizeT dot = xfbVarying.name.find('.');
                if (dot != String::npos && dot > 0) {
                    xfbCaptureBlockNames.insert(xfbVarying.name.substr(0, dot));
                }
            }
            std::set<String> flattenedXfbBlockNames;
            // Stages whose ESSL had a read+write image declaration doubled into a coherent
            // read/write pair, and by how many. Empty for every program but a handful; consulted
            // ONLY when the link then fails, because the doubling spends the driver's per-stage
            // GL_MAX_*_IMAGE_UNIFORMS budget that MobileGL keeps advertising unadjusted (halving
            // the advertised value would fail basic-api and NotSupported-out cases that never pay
            // the doubling, so the limit must stay honest and the connection has to be made here
            // instead). See the budget note on SplitReadWriteImageUniforms.
            struct SplitImageUniformStage {
                ShaderStage stage;
                Uint splitCount;
            };
            Vector<SplitImageUniformStage> splitImageUniformStages;

            // Desktop GLSL keeps SEPARATE name namespaces for input and output interface
            // blocks, so ONE stage may legally declare `in FOO {...}` and `out FOO {...}` at
            // the same time - which the tessellation evaluation stage of both interface-block
            // tests in KHR-GL42/43.shading_language_420pack does ("in TCSOutputBlock ... out
            // TCSOutputBlock"). SPIRV-Cross keeps the same split (block_input_names vs
            // block_output_names) and re-emits BOTH under the name FOO, so the generated ESSL
            // declares two different blocks called FOO in one shader. Adreno's ES compiler
            // keeps them apart; Mali's does not - the stage compiles, the program links, and
            // the output block's payload never reaches the next stage. All 22 of that group's
            // Mali failures are exactly the two tests that write this shape, and every one of
            // them passes on Adreno and on DirectVulkan.
            //
            // The repair is a rename keyed on the PRODUCING stage, planned here and applied
            // per stage below so a producer and its consumer keep naming the same block.
            // Gated twice over, because a re-serialised module is not free (it cost the
            // create-indirect retrace 0.15 SSIM the first time the array-input split missed
            // its gate): only a tessellation or geometry stage can declare blocks in both
            // directions at all, and even then the probe has to FIND a collision before any
            // stage is rewritten.
            std::set<String> collidingIoBlockNames;
            std::set<String> declaredIoBlockNames;
            Vector<Int> stagePipelineIndices(linkedStages.size(), -1);
            Bool anyStageCanDeclareBlocksInBothDirections = false;
            for (SizeT index = 0; index < linkedStages.size(); ++index) {
                const ShaderStage stage = linkedStages[index];
                stagePipelineIndices[index] = InterStagePipelineIndex(stage);
                if (CanDeclareBlocksInBothDirections(stage)) anyStageCanDeclareBlocksInBothDirections = true;
            }
            // A SECOND, INDEPENDENT interface-block repair riding the same gate, because it
            // needs the same question answered: "does this program have a stage where an
            // inter-stage block can go wrong?". CanDeclareBlocksInBothDirections is true for
            // exactly the tessellation and geometry stages, which is also exactly the set of
            // stages whose presence makes this driver drop a LOCATED block's payload (a
            // vertex-to-fragment located block is fine on the same driver, measured). The two
            // repairs are otherwise unrelated: the rename fixes a name collision inside ONE
            // stage, this drops a qualifier from EVERY block of the program - so it does not
            // wait for the collision probe to find anything.
            const Bool ioBlockLocationStripArmed =
                !g_GLESCapabilities.SupportsLocatedInterStageIoBlocks &&
                anyStageCanDeclareBlocksInBothDirections;
            if (anyStageCanDeclareBlocksInBothDirections) {
                for (SizeT index = 0; index < shaderSpirvs.size(); ++index) {
                    MG_Util::ShaderTranspiler::ShaderCompiler::ProbeIoBlockNamesForEssl(
                        shaderSpirvs[index], collidingIoBlockNames, declaredIoBlockNames);
                }
                // A block a capture request names is resolved BY NAME at
                // glTransformFeedbackVaryings time - and flattened away entirely by the pass
                // below - so renaming one would ask the driver for a block the request does
                // not spell.
                for (const auto& xfbCaptureBlockName : xfbCaptureBlockNames) {
                    collidingIoBlockNames.erase(xfbCaptureBlockName);
                }
            }
            // The one spelling every stage of THIS program agrees on for `blockName` as written
            // by pipeline stage `producerPipelineIndex`. "__" is reserved in GLSL, so a name
            // already ending in '_' does not get another one, and the digit-suffix loop steps
            // off any name the program already spells.
            const auto uniqueIoBlockName = [&declaredIoBlockNames](const String& blockName,
                                                                   Int producerPipelineIndex) {
                const char* separator = (!blockName.empty() && blockName.back() == '_') ? "" : "_";
                String candidate = blockName + separator + "mgio" + std::to_string(producerPipelineIndex);
                while (declaredIoBlockNames.find(candidate) != declaredIoBlockNames.end()) {
                    candidate += "0";
                }
                return candidate;
            };

            // Desktop GL makes the tessellation CONTROL stage optional; OpenGL ES 3.2 rejects a
            // program that has an evaluation stage without one, with an empty info log. When that
            // is this program's shape, one is synthesized below - and it has to be spelled to
            // MATCH the two stages it sits between, so their emitted ESSL is kept here as it is
            // produced. Empty for every program that has a control stage of its own, which is
            // all but a handful.
            Bool hasTessEvalStage = false;
            Bool hasTessControlStage = false;
            Int tessEvalShaderIndex = -1;
            String vertexStageEssl;
            String tessEvalStageEssl;
            //
            // Asked of the executable for the same reason the loop below indexes it: a
            // tessellation evaluation shader merely ATTACHED to a linked vertex+fragment program
            // is not part of what this program runs, and synthesizing a control stage for it
            // would build a tessellating driver program for an executable that does not
            // tessellate (and would take tessEvalShaderIndex past the end of shaderSpirvs).
            for (SizeT index = 0; index < linkedStages.size(); ++index) {
                const ShaderStage stage = linkedStages[index];
                if (stage == ShaderStage::TessControl) hasTessControlStage = true;
                if (stage == ShaderStage::TessEval) {
                    hasTessEvalStage = true;
                    tessEvalShaderIndex = static_cast<Int>(index);
                }
            }
            const Bool needsPassthroughTessControl = hasTessEvalStage && !hasTessControlStage;

            // The stage order the loop below walks, with every FRAGMENT stage moved to the end.
            // The viewport-routing gate is the reason: whether a fragment stage needs one is a
            // question about the OTHER stages ("does any of them still write gl_ViewportIndex?"),
            // and the honest, free answer to it is the promotion the producing stage's own text
            // pass just performed. Answering it any other way costs a BuildModule per
            // pre-rasterization stage of every program - the parse the shared SpirvGateFeatures
            // probe exists to avoid. Nothing else in the loop is order-sensitive: the two
            // passthrough-tessellation sources it captures are a vertex and an evaluation stage,
            // and the three sets it accumulates are unions.
            Vector<SizeT> stageOrder;
            stageOrder.reserve(linkedStages.size());
            for (SizeT index = 0; index < linkedStages.size(); ++index) {
                if (linkedStages[index] != ShaderStage::Fragment) stageOrder.push_back(index);
            }
            for (SizeT index = 0; index < linkedStages.size(); ++index) {
                if (linkedStages[index] == ShaderStage::Fragment) stageOrder.push_back(index);
            }
            // Set by whichever pre-rasterization stage's demoted mg_ViewportIndex global the text
            // pass turned into a varying; read by the fragment stage to decide whether to inject
            // the gate that consumes it.
            Bool programRoutesViewportIndex = false;
            // No fragment stage, no gate - and without a gate the promotion below would only add
            // an output nothing can read. That is not merely useless: in a separable program
            // pipeline the fragment stage lives in a DIFFERENT program, which never saw this
            // build and cannot be given a gate, so promoting there would hang an unmatched
            // varying off a program to buy nothing. Both cases keep the pre-emulation behaviour,
            // which is what a program with no fragment stage had anyway.
            const Bool programHasFragmentStage =
                std::find(linkedStages.begin(), linkedStages.end(), ShaderStage::Fragment) !=
                linkedStages.end();
            const Bool viewportEmulationForThisProgram =
                ViewportArrayEmulationEnabled() && programHasFragmentStage;

            for (const SizeT index : stageOrder) {
                GLenum glShaderType = MG_Util::ConvertShaderStageToGLEnum(linkedStages[index]);
                GLuint backendShaderId = g_GLESFuncs.glCreateShader(glShaderType);

                if (backendShaderId == 0) {
                    MGLOG_E_ONCE("Failed to create backend shader for attachment.");
                    continue;
                }
                String source;
                auto& spirvCode = shaderSpirvs[index];

                // A samplerBuffer is core in the OpenGL 3.1+ context MobileGL advertises but needs
                // ES 3.2 or EXT/OES_texture_buffer on the host. Without it SPIRV-Cross emits
                // `#extension GL_EXT_texture_buffer : require` and the driver rejects both that
                // and the isamplerBuffer keyword - the program never links and every draw using it
                // becomes a silent no-op. Say so here, naming the stage. Deliberately unlatched:
                // this is bounded by program count, and which stage failed is the whole point.
                // Gated on the capability so the module walk never runs on a healthy driver.
                if (!AreBufferTexturesSupported() &&
                    MG_Util::ShaderTranspiler::ShaderCompiler::ModuleDeclaresBufferTextureSampler(spirvCode)) {
                    MGLOG_E("Program %u stage %s samples a buffer texture, which this ES driver "
                            "cannot provide (%s). The shader will not compile and the program will "
                            "not link; every draw using it is a no-op.",
                            m_backendProgramId,
                            MG_Util::ConvertGLEnumToString(glShaderType).c_str(),
                            GetBufferTextureTierName());
                    m_backendProgramUsable = false;
                    g_GLESFuncs.glDeleteShader(backendShaderId);
                    continue;
                }

                // ---- L2 of the shader translation memo -------------------------------
                // The whole DirectGLES SPIR-V pass chain plus SPIRV-Cross for this stage,
                // memoized on the module bytes and on every capability bit and per-program
                // input that steers them. See TranslationCache.h for the key inventory and
                // for why the text-level passes below stay outside the boundary.
                MG_Util::ShaderTranspiler::EsslTranslationKeyInputs esslKeyInputs;
                esslKeyInputs.spirv = &spirvCode;
                esslKeyInputs.shaderType = glShaderType;
                // The EFFECTIVE arming, computed the same way TranspileSpirvToEssl computes it.
                // Duplicated rather than shared because the two live on opposite sides of the
                // memo boundary - and a key that disagrees with the pass it is keying is the one
                // failure mode of this cache that renders wrong pixels instead of being slow.
                esslKeyInputs.viewportIndexLoweringArmed =
                    (glShaderType == GL_VERTEX_SHADER || glShaderType == GL_TESS_EVALUATION_SHADER ||
                     glShaderType == GL_GEOMETRY_SHADER) &&
                    (ViewportArrayEmulationEnabled() || !g_GLESCapabilities.SupportsViewportArray);
                esslKeyInputs.supportsNoperspectiveInterpolation =
                    g_GLESCapabilities.SupportsNoperspectiveInterpolation;
                esslKeyInputs.supportsExtendedImageFormats =
                    g_GLESCapabilities.SupportsExtendedImageFormats;
                esslKeyInputs.maxColorTextureSamples = g_GLESCapabilities.MaxColorTextureSamples;
                esslKeyInputs.maxIntegerSamples = g_GLESCapabilities.MaxIntegerSamples;
                esslKeyInputs.maxDepthTextureSamples = g_GLESCapabilities.MaxDepthTextureSamples;
                esslKeyInputs.advertisedMaxSamples =
                    std::max(g_GLESCapabilities.MaxSamples, kFrontendMaxSamples);
                esslKeyInputs.xfbCaptureBlockNames = &xfbCaptureBlockNames;
                esslKeyInputs.glFormatByUniformName = &imageFormatBake.glFormatByUniformName;
                esslKeyInputs.storageBlockBindingOverrides = &storageBlockBindingOverrides;
                esslKeyInputs.esslVersion = ResolveBackendEsslVersion();
                esslKeyInputs.atomicCounterEsslBindingTop = m_atomicCounterEsslBindingTop;

                // THIS STAGE's share of the program-wide interface-block rename plan built above
                // the loop. Resolved here, outside the memoized segment, because it is planning
                // and not translation - exactly like the image-format bake map - and because that
                // makes the two maps a plain function argument the L2 key can carry.
                //
                // KEYING ON THE RESOLVED MAPS rather than on what they were derived from
                // (collidingIoBlockNames, declaredIoBlockNames, stagePipelineIndices, this
                // stage's index) is deliberate: the maps ARE the pass's arguments, so they are
                // exactly as fine as the pass's behaviour and no finer. Two programs whose
                // collision plans differ but whose maps for THIS stage come out identical really
                // do produce the same ESSL and should share the entry.
                //
                // A block whose other end is NOT in this program is deliberately left out of the
                // plan: in a separate-shader-objects pipeline the interface it matches across
                // lives in another program that never saw this plan, and renaming one side of
                // THAT would break a program pipeline to repair a driver quirk. That is what the
                // producer/consumer presence tests below are for - in a monolithic program both
                // are trivially satisfied for every interface the collision can touch.
                std::map<String, String> inputBlockRenames;
                std::map<String, String> outputBlockRenames;
                if (!collidingIoBlockNames.empty() && stagePipelineIndices[index] >= 0) {
                    const Int myPipelineIndex = stagePipelineIndices[index];
                    Int producerPipelineIndex = -1;
                    Bool hasConsumerStage = false;
                    for (const Int otherPipelineIndex : stagePipelineIndices) {
                        if (otherPipelineIndex < 0) continue;
                        if (otherPipelineIndex < myPipelineIndex &&
                            otherPipelineIndex > producerPipelineIndex) {
                            producerPipelineIndex = otherPipelineIndex;
                        }
                        if (otherPipelineIndex > myPipelineIndex) hasConsumerStage = true;
                    }
                    for (const auto& collidingBlockName : collidingIoBlockNames) {
                        if (producerPipelineIndex >= 0) {
                            inputBlockRenames[collidingBlockName] =
                                uniqueIoBlockName(collidingBlockName, producerPipelineIndex);
                        }
                        if (hasConsumerStage) {
                            outputBlockRenames[collidingBlockName] =
                                uniqueIoBlockName(collidingBlockName, myPipelineIndex);
                        }
                    }
                }
                esslKeyInputs.inputBlockRenames = &inputBlockRenames;
                esslKeyInputs.outputBlockRenames = &outputBlockRenames;

                // ...and THIS STAGE's share of the interface-block LOCATION strip, planned the
                // same way and for the same reason. The gate has three parts, all of which have
                // to hold before a single block loses its qualifier:
                //   * the driver POST measured the defect (never a renderer-string quirk list);
                //   * this program has a stage that can hit it - a located block between a
                //     vertex and a fragment stage works on the affected driver, so a program
                //     with neither tessellation nor geometry keeps its ESSL byte for byte;
                //   * for THIS stage and THIS direction, this program HAS a stage on that side
                //     of it. That is the same test the rename plan above makes, and the same
                //     approximation: it asks "is some stage of this program earlier/later than
                //     me", not "is the exact partner of every one of my blocks here". The two
                //     coincide for every program MobileGL builds, because a separable pipeline
                //     is flattened into one composite carrying every stage that has a shader
                //     (GLContext::GetProgramForDraw) and a program bound with glUseProgram has
                //     no partner program at all - so a stage set with a gap in it does not
                //     arise. Should one ever arise, this must become the nearest-stage
                //     resolution the rename plan computes, or the two ends of the gap would
                //     disagree about the qualifier.
                // The direction tests deliberately mirror that plan rather than inventing a
                // second rule for the same question.
                Bool stripInputBlockLocations = false;
                Bool stripOutputBlockLocations = false;
                if (ioBlockLocationStripArmed && stagePipelineIndices[index] >= 0) {
                    const Int myPipelineIndex = stagePipelineIndices[index];
                    for (const Int otherPipelineIndex : stagePipelineIndices) {
                        if (otherPipelineIndex < 0) continue;
                        if (otherPipelineIndex < myPipelineIndex) stripInputBlockLocations = true;
                        if (otherPipelineIndex > myPipelineIndex) stripOutputBlockLocations = true;
                    }
                }
                esslKeyInputs.stripInputBlockLocations = stripInputBlockLocations;
                esslKeyInputs.stripOutputBlockLocations = stripOutputBlockLocations;
                esslKeyInputs.enableSpirvValidation = enableSpirvValidation;

                auto& esslCache = MG_Util::ShaderTranspiler::GetEsslTranslationCache();
                MG_Util::ShaderTranspiler::TranslationCacheKey esslCacheKey;
                if (MG_Util::ShaderTranspiler::ShaderTranslationCacheEnabled()) {
                    esslCacheKey = MG_Util::ShaderTranspiler::BuildEsslTranslationKey(esslKeyInputs);
                }

                std::set<String> stageFlattenedXfbBlockNames;
                // Per stage, and NOT m_atomicCounterGlBindings directly: on a miss the
                // transpile appends to this, on a hit the payload supplies it, and only then
                // is it folded into the program-wide vector. Pointing the transpile straight
                // at the member would have made the miss path and the hit path disagree about
                // who owns the append.
                Vector<Int> stageAtomicCounterGlBindings;
                const MG_Util::ShaderTranspiler::EsslTranslationResultPtr esslHit =
                    esslCacheKey.Valid() ? esslCache.Find(esslCacheKey) : nullptr;
                if (esslHit) {
                    source = esslHit->essl;
                    stageFlattenedXfbBlockNames = esslHit->flattenedXfbBlockNames;
                    stageAtomicCounterGlBindings = esslHit->atomicCounterGlBindings;
                } else {
                    String transpileError;
                    if (!TranspileSpirvToEssl(spirvCode, glShaderType, xfbCaptureBlockNames,
                                              imageFormatBake, storageBlockBindingOverrides,
                                              inputBlockRenames, outputBlockRenames,
                                              stripInputBlockLocations, stripOutputBlockLocations,
                                              m_atomicCounterEsslBindingTop,
                                              enableSpirvValidation, source,
                                              stageFlattenedXfbBlockNames,
                                              stageAtomicCounterGlBindings, transpileError)) {
                        // MGLOG_E, unlatched, like the compile- and link-failure diagnostics
                        // below: one line per failing stage is bounded by program count and
                        // naming the stage is the entire diagnostic value. A stage that never
                        // reaches the driver leaves the program short of that stage, so the link
                        // fails with an EMPTY driver info log - the least debuggable failure
                        // MobileGL can produce, and what hid the whole
                        // KHR-GL43.vertex_attrib_binding family behind "the draw captured zeros".
                        MGLOG_E("Shader transpilation to ESSL failed. State program ID: %u, stage: %s, "
                                "SPIRV-Cross error: %s",
                                stateProgramObject->GetExternalIndex(),
                                MG_Util::ConvertGLEnumToString(glShaderType).c_str(),
                                transpileError.c_str());
                        m_backendProgramUsable = false;
                        continue;
                    }
                    if (esslCacheKey.Valid()) {
                        auto payload = MakeShared<MG_Util::ShaderTranspiler::EsslTranslationResult>();
                        payload->essl = source;
                        payload->flattenedXfbBlockNames = stageFlattenedXfbBlockNames;
                        payload->atomicCounterGlBindings = stageAtomicCounterGlBindings;
                        const SizeT payloadBytes =
                            MG_Util::ShaderTranspiler::EsslTranslationResultBytes(*payload);
                        esslCache.Insert(
                            esslCacheKey,
                            MG_Util::ShaderTranspiler::EsslTranslationResultPtr(Move(payload)),
                            payloadBytes);
                    }
                }
                // Per stage, never cumulative: a fragment shader consuming the same block
                // reports a name the vertex stage already reported, and its own rewrite must
                // still be taken or the two stages stop matching. Done here rather than inside
                // the transpile so a cache HIT contributes its names too.
                flattenedXfbBlockNames.insert(stageFlattenedXfbBlockNames.begin(),
                                              stageFlattenedXfbBlockNames.end());
                // Same rule for the atomic-counter bindings this stage declared, and for the
                // same reason: the loop below de-duplicates across stages, so a hit that
                // contributed nothing would silently drop a counter buffer the draw path has
                // to bind.
                m_atomicCounterGlBindings.insert(m_atomicCounterGlBindings.end(),
                                                 stageAtomicCounterGlBindings.begin(),
                                                 stageAtomicCounterGlBindings.end());

                // Position in the chain is arbitrary: this is the only header-level rewrite, it
                // edits #extension directives and never the body, and the replacement is the
                // same length and stays an #extension line - so it commutes with every pass
                // below, including ForceSupporterOutput's scan for the last directive. First,
                // because a header concern reads better before the body ones.
                source = RetargetTextureBufferExtension(std::move(source),
                                                        g_GLESCapabilities.TextureBufferSupport);
                // The other header-level rewrite, and next to that one for the same reason. The
                // formats it covers are both the ones the bake above put into the module and the
                // ones the application declared itself - either can be outside the thirteen GLSL
                // ES has in core, and neither reaches the driver without this directive.
                source = RequestExtendedImageFormats(std::move(source),
                                                     imageFormatBake.needsExtendedImageFormats &&
                                                         g_GLESCapabilities.SupportsExtendedImageFormats);
                // The third header-level rewrite, for the builtin SPIRV-Cross prints bare:
                // gl_ViewportIndex is in no version of ESSL core, so without this directive the
                // stage does not compile and the whole program - not just its viewport routing -
                // is lost. The token probe keeps the line off every other program and the
                // capability gate keeps it off drivers that would hard-error on an unadvertised
                // name; a driver without the extension took the LowerViewportIndexPass fallback
                // above and its source no longer names the builtin at all, so the two are mutually
                // exclusive by construction. Read `source` BEFORE it is moved from.
                // The routing emulation is the third way this can be reached and the only one
                // that needs no directive: it renames the fragment stage's read onto the varying
                // the producing stage now writes, a few passes below.
                const Bool needsViewportArrayExtension =
                    g_GLESCapabilities.SupportsViewportArray &&
                    !(ViewportArrayEmulationEnabled() && programRoutesViewportIndex) &&
                    source.find("gl_ViewportIndex") != String::npos;
                source = RequestViewportArrayExtension(std::move(source), needsViewportArrayExtension);

                // The fourth header-level rewrite, and the same shape as the third: ESSL has no
                // gl_PointSize in a tessellation or geometry stage at ANY version - 320 makes the
                // stages core and still leaves the built-in behind EXT/OES_..._point_size - while
                // SPIRV-Cross prints it bare. Without the directive the stage fails to compile
                // with "`gl_PointSize' undeclared", which takes the whole program to program 0:
                // the draw renders nothing AND glBeginTransformFeedback is rejected, so a capture
                // of anything at all off that program silently comes back empty. The token probe
                // keeps the line off every other program and PointSizeExtensionName returns
                // nullptr - i.e. nothing is emitted - on a driver advertising neither spelling.
                if (source.find("gl_PointSize") != String::npos) {
                    const Bool tessellationStage = glShaderType == GL_TESS_CONTROL_SHADER ||
                                                   glShaderType == GL_TESS_EVALUATION_SHADER;
                    if (tessellationStage || glShaderType == GL_GEOMETRY_SHADER) {
                        const auto tier = tessellationStage ? g_GLESCapabilities.TessellationPointSizeSupport
                                                            : g_GLESCapabilities.GeometryPointSizeSupport;
                        const char* pointSizeExtension = PointSizeExtensionName(tier, tessellationStage);
                        if (pointSizeExtension == nullptr) {
                            // Latched, and an ERROR rather than a warning: what follows is a
                            // driver compile failure whose text names a built-in the application
                            // never mis-spelled, and the reason is a missing driver capability
                            // rather than anything in the shader. Saying so here is the whole
                            // difference between a legible skip and an unexplained black draw.
                            MGLOG_E_ONCE("This driver advertises neither the EXT nor the OES %s_point_size "
                                         "extension, so its ESSL has no gl_PointSize in a %s stage; program %u "
                                         "will fail to compile. Point size from a non-vertex stage is not "
                                         "available on this device.",
                                         tessellationStage ? "tessellation" : "geometry",
                                         tessellationStage ? "tessellation" : "geometry",
                                         stateProgramObject->GetExternalIndex());
                        }
                        source = RequestPointSizeExtension(std::move(source), pointSizeExtension);
                    }
                }

                source = RebindImageUniformsToFrontendUnits(std::move(source), stateProgramObject);
                // The completion half of the format bake, for the formats SPIRV-Cross throws on
                // rather than prints (r8ui and the rest of its desktop-only set). Empty for every
                // program whose format-less images bound a format the module could carry, which
                // is the normal case - those were baked into the SPIR-V above and this pass finds
                // their declarations already qualified. AFTER the rebind, so the layout qualifier
                // it edits is the one that already exists; BEFORE the split and the binding
                // strip, so both halves of a split image inherit the format.
                source = BakeImageFormatQualifiers(std::move(source),
                                                   imageFormatBake.esslFormatQualifierByUniformName);
                // An image ARRAY whose elements do not sit on consecutive units cannot be spelled
                // by the single layout(binding=N) the rebind above stamped: ESSL gives element k
                // the unit N+k and there is no glUniform1i to correct it with. Split the array
                // into one scalar declaration per element, each with its own binding. AFTER the
                // rebind and the format bake, both of which look the array up by its GL uniform
                // name and need the binding already there; BEFORE the read+write split, so an
                // element that is both read and written is split with its own binding on it.
                if (!nonConsecutiveImageArrays.empty()) {
                    Vector<String> declinedImageArrays;
                    source = RemapImageArrayElementUnits(source, nonConsecutiveImageArrays,
                                                         &declinedImageArrays);
                    for (const auto& declined : declinedImageArrays) {
                        // MGLOG_E, unlatched, like the transpile- and compile-failure diagnostics
                        // around it: this is the "linked, drew, produced wrong numbers, said
                        // nothing" shape that cost earlier waves whole days, and one line per
                        // declined array is bounded by program count. There is no honest GL answer
                        // to give instead - the frontend has already reported LINK_STATUS = true.
                        MGLOG_E("Image array %s. Its elements address image units GLSL ES cannot be made to reach "
                                "from one declaration, so this stage will read and write the WRONG units. State "
                                "program ID: %u, stage: %s.",
                                declined.c_str(), stateProgramObject->GetExternalIndex(),
                                MG_Util::ConvertGLEnumToString(glShaderType).c_str());
                    }
                }
                // Wedged between those two on purpose:
                //  * AFTER RebindImageUniformsToFrontendUnits, so the binding it copies onto
                //    both halves of a split image is already the frontend texture unit (and so
                //    that pass never has to reason about the alias it introduces);
                //  * BEFORE RemoveLayoutBinding, whose keepBindingRegex recognises an image
                //    declaration and preserves its binding - an image unit cannot be set from
                //    the API in ES, so the qualifier is the only binding mechanism there is,
                //    and both halves of the pair have to still be carrying theirs when it runs.
                // Takes no stage: the qualifier it adds is a decision about THIS text's accesses
                // and the rename that keeps two stages from declaring one image uniform
                // differently is keyed on that same decision, so two stages that agree still
                // share one uniform (see the location-budget note on the pass).
                Uint splitImageUniformCount = 0;
                source = SplitReadWriteImageUniforms(source, &splitImageUniformCount);
                if (splitImageUniformCount != 0) {
                    splitImageUniformStages.push_back({linkedStages[index], splitImageUniformCount});
                }
                source = RemoveLayoutBinding(source);
                source = ProcessOutColorLocations(source);
                source = ForceFlatIntegerVaryings(source, glShaderType);
                source = BroadcastLegacyFragColor(std::move(source), glShaderType, m_fragColorBroadcastCount);
                source = EmulateTextureLodBias(source, ShouldAvoidExplicitLodBiasOnAngleLlvmpipe());
                source = EmulateBaseInstanceInVertexShader(std::move(source), glShaderType);
                source = PromoteDrawParameterGlobalsToUniforms(std::move(source), glShaderType);
                // The two halves of the gl_ViewportIndex routing emulation, next to the draw-
                // parameter promotion because they are the same shape: a builtin ESSL cannot
                // spell, demoted to a plain global by a SPIR-V pass, given a real interface here.
                // BEFORE ForceSupporterOutput, so the `precision highp` statements it hoists to
                // the top land above the declarations these inject; AFTER
                // ForceFlatIntegerVaryings, which matches only declarations carrying a
                // layout(...) qualifier and so cannot touch either of them.
                if (viewportEmulationForThisProgram) {
                    if (glShaderType == GL_FRAGMENT_SHADER) {
                        if (programRoutesViewportIndex && !InjectViewportIndexPassGate(source)) {
                            // MGLOG_E, unlatched, like the transpile- and compile-failure
                            // diagnostics around it: the program still links and still draws, so
                            // nothing else in the process will ever say that its viewport routing
                            // silently collapsed back to one rectangle.
                            MGLOG_E("Program %u routes gl_ViewportIndex but its fragment stage has no "
                                    "entry point to gate, so the routing cannot be emulated: every "
                                    "index will rasterize against viewport 0. State program ID: %u.",
                                    m_backendProgramId, stateProgramObject->GetExternalIndex());
                        }
                    } else if (PromoteViewportIndexGlobalToVarying(source)) {
                        programRoutesViewportIndex = true;
                    }
                }
                source = ForceSupporterOutput(source);
                source = ClampNormFallbackOutputs(std::move(source), glShaderType,
                                                  m_snormFallbackClampOutputMask,
                                                  m_unormFallbackClampOutputMask);

                // Patch for Photon compiler precision issue
                String findStr = "1000000.0";
                String replaceStr = "65500.0";
                auto pos = source.find(findStr);
                while (pos != String::npos) {
                    MGLOG_D("Applying patch #2 to Photon...");
                    source.replace(pos, findStr.length(), replaceStr);
                    pos = source.find(findStr, pos);
                }

                const char* sourceCStr = source.c_str();
                MGLOG_D("Setting shader source for backend shader ID: %u\nsrc:\n%s", backendShaderId, sourceCStr);
                g_GLESFuncs.glShaderSource(backendShaderId, 1, &sourceCStr, nullptr);
                g_GLESFuncs.glCompileShader(backendShaderId);

                // GL_FALSE, not GL_TRUE: an unwritten out-param must read as "compile failed"
                // and take the diagnostic path, never as a silent success that attaches an
                // uncompiled shader.
                GLint compileStatus = GL_FALSE;
                g_GLESFuncs.glGetShaderiv(backendShaderId, GL_COMPILE_STATUS, &compileStatus);
                if (compileStatus == GL_FALSE) {
                    GLint logLength = 0;
                    g_GLESFuncs.glGetShaderiv(backendShaderId, GL_INFO_LOG_LENGTH, &logLength);
                    if (logLength < 0) logLength = 0;
                    // +1 and zero-filled: GL_INFO_LOG_LENGTH already counts the terminator,
                    // but a driver that reports 0 (or fails the query) must still leave
                    // log.data() a readable empty C string for the %s below.
                    Vector<GLchar> log(static_cast<SizeT>(logLength) + 1, '\0');
                    g_GLESFuncs.glGetShaderInfoLog(backendShaderId, logLength, nullptr, log.data());
                    log.back() = '\0';
                    // MGLOG_E, unlatched. This was parked at MGLOG_I while the level ordering
                    // compiled E and W out of every INFO build: the Android retrace artifact
                    // carried 294 INFO lines and zero ERROR lines while two generated shaders
                    // were being rejected outright, and the lane could not say why it was
                    // rendering an empty translucent layer. A shader the driver refuses is
                    // never noise.
                    //
                    // A BOUNDED EXCERPT of the source goes with it. The driver log names a line
                    // and a column in text that exists nowhere but here, so without any source at
                    // all the only way to read "`gl_PointSize' undeclared" is to rebuild the whole
                    // library at DEBUG - but the full dump cannot go at E either. This is not
                    // "one line per refused shader": SyncToBackend's rebuild gate keys on
                    // per-draw state (the enabled-draw-buffer count among it), so a program used
                    // across passes with different draw-buffer counts re-transpiles, re-compiles
                    // and re-fails on every alternation, i.e. per frame. At E - live at the
                    // production INFO level - each of those records would push the whole
                    // post-SPIRV-Cross ESSL through the global log mutex with a forced flush onto
                    // /sdcard/MG/latest.log, the file users are asked to share. The excerpt keeps
                    // the record O(1); the full text is still there at D, printed against this
                    // same backend shader id by the "Setting shader source" line above, so
                    // nothing needs to be dumped twice.
                    constexpr SizeT kMaxLoggedSourceBytes = 2048;
                    String truncatedSource;
                    const char* sourceForLog = source.c_str();
                    if (source.size() > kMaxLoggedSourceBytes) {
                        // Back up to a line boundary when there is one inside the window, so the
                        // excerpt ends on a whole statement rather than mid-token. Built only on
                        // this branch: a stage that fits keeps its own buffer and is not copied.
                        SizeT cut = kMaxLoggedSourceBytes;
                        if (const SizeT lastNewline = source.rfind('\n', cut);
                            lastNewline != String::npos && lastNewline > 0) {
                            cut = lastNewline + 1;
                        }
                        truncatedSource = source.substr(0, cut);
                        truncatedSource += "... [" + std::to_string(source.size() - cut) +
                                           " more bytes; the whole stage is printed at the DEBUG level]\n";
                        sourceForLog = truncatedSource.c_str();
                    }
                    MGLOG_E("Shader compilation failed. State program ID: %u, stage: %s, backend shader ID: "
                            "%u, driver log: %s\nSource:\n%s",
                            stateProgramObject->GetExternalIndex(),
                            MG_Util::ConvertGLEnumToString(glShaderType).c_str(), backendShaderId,
                            log.data(), sourceForLog);
                    m_backendProgramUsable = false;
                    // Nothing will ever attach this one, so nothing else can free it.
                    g_GLESFuncs.glDeleteShader(backendShaderId);
                    continue;
                }

                MGLOG_D("Attaching shader ID: %u to program %u", backendShaderId, m_backendProgramId);
                g_GLESFuncs.glAttachShader(m_backendProgramId, backendShaderId);
                // Hand the shader's lifetime to the program, immediately and unconditionally.
                //
                // glDeleteShader only FLAGS a shader; the driver frees it when it is attached to
                // nothing. Flagging it here is what makes the program own it, so deleting the
                // program (or the detach loop above, on a relink) is what actually frees it.
                // Without this call every program build leaked its shader objects for the process
                // lifetime, and a relink leaked them twice - the detach loop above dropped the
                // program's reference to shaders nothing had flagged, so they became unreachable
                // AND undeletable. The GL swizzle conformance test builds 1,296 programs per case,
                // so a handful of cases left tens of thousands of live driver shaders behind and
                // the driver started mis-serving them (KHR-GL33/GL40.texture_swizzle.smoke_*).
                // Same class of defect as the missing framebuffer/renderbuffer/sampler destructors
                // fixed in Wave 1, and the last of that family: this is the one backend GL object
                // MobileGL creates without an owning wrapper to destroy it.
                g_GLESFuncs.glDeleteShader(backendShaderId);

                // Kept AFTER every text-level pass, so what the synthesized control stage mirrors
                // is the text the driver actually sees, not an intermediate form.
                if (needsPassthroughTessControl) {
                    if (glShaderType == GL_VERTEX_SHADER) {
                        vertexStageEssl = source;
                    } else if (glShaderType == GL_TESS_EVALUATION_SHADER) {
                        tessEvalStageEssl = source;
                    }
                }

                MGLOG_D("Processed shader source length: %zu", source.length());
            }

            if (needsPassthroughTessControl) {
                AttachPassthroughTessControlStage(*stateProgramObject, tessEvalShaderIndex, shaderSpirvs,
                                                  vertexStageEssl, tessEvalStageEssl);
            }

            // A counter buffer declared by several stages was recorded once per stage; the draw
            // path binds per GL binding point, so collapse the duplicates here rather than
            // re-issuing the same glBindBufferBase two or three times every draw.
            if (!m_atomicCounterGlBindings.empty()) {
                std::sort(m_atomicCounterGlBindings.begin(), m_atomicCounterGlBindings.end());
                m_atomicCounterGlBindings.erase(
                    std::unique(m_atomicCounterGlBindings.begin(), m_atomicCounterGlBindings.end()),
                    m_atomicCounterGlBindings.end());
            }

            // Transform feedback capture runs on the real driver (see XfbImpl in
            // DirectGLES.cpp), so the capture set has to be declared on the backend
            // program before it links. SPIRV-Cross keeps user output names verbatim in
            // the transpiled ESSL (`out vec4 result_0;` stays `result_0`), so the
            // frontend's requested names carry over unchanged.
            SizeT declaredXfbVaryingCount = 0;
            if (stateProgramObject->GetTransformFeedbackVaryingCount() > 0 &&
                g_GLESFuncs.glTransformFeedbackVaryings != nullptr) {
                const auto& xfbVaryings = stateProgramObject->GetTransformFeedbackVaryings();
                Vector<const GLchar*> xfbNames;
                xfbNames.reserve(xfbVaryings.size());
                // A block this build flattened no longer HAS the member the application asked
                // for; it has the variable that replaced it. Everything else - including a
                // member of a block that was left alone - keeps the application's spelling.
                // Storage first, pointers after: xfbNames holds pointers into these strings.
                //
                // Same rule for a demoted gl_PointSize: the capture stage's ESSL no longer
                // spells the built-in at all - the value lives in the carrier the demotion
                // named - so the driver-side request has to follow it there. Only when the
                // capture stage IS a demoted one (geometry, else evaluation): a program whose
                // capture stage is the vertex shader keeps the built-in and its spelling,
                // whatever happened to a control stage behind it.
                Bool captureStageDemoted = false;
                if (stateProgramObject->PointSizeDemoted()) {
                    for (const ShaderStage linkedStage : linkedStages) {
                        if (linkedStage == ShaderStage::TessEval || linkedStage == ShaderStage::Geometry) {
                            captureStageDemoted = true;
                            break;
                        }
                    }
                }
                Vector<String> rewrittenXfbNames(xfbVaryings.size());
                for (SizeT nameIndex = 0; nameIndex < xfbVaryings.size(); ++nameIndex) {
                    String flatName;
                    if (!flattenedXfbBlockNames.empty() &&
                        MG_Util::ShaderTranspiler::ShaderCompiler::RewriteXfbCaptureNameForFlattenedBlock(
                            xfbVaryings[nameIndex].name, flattenedXfbBlockNames, flatName)) {
                        rewrittenXfbNames[nameIndex] = std::move(flatName);
                    } else if (captureStageDemoted && xfbVaryings[nameIndex].name == "gl_PointSize") {
                        rewrittenXfbNames[nameIndex] =
                            MG_Util::ShaderTranspiler::ShaderCompiler::POINT_SIZE_CAPTURE_CARRIER_NAME;
                    } else {
                        rewrittenXfbNames[nameIndex] = xfbVaryings[nameIndex].name;
                    }
                }
                for (const auto& xfbName : rewrittenXfbNames) {
                    xfbNames.push_back(xfbName.c_str());
                }
                MGLOG_D("Declaring %zu transform feedback varyings on program %u", xfbNames.size(),
                        m_backendProgramId);
                // Bounded: a lost context never answers GL_NO_ERROR, and this runs on the
                // thread that would then spin forever.
                for (Int i = 0; i < kMaxDrainedProgramErrors && g_GLESFuncs.glGetError() != GL_NO_ERROR; ++i) {
                }
                g_GLESFuncs.glTransformFeedbackVaryings(m_backendProgramId, static_cast<GLsizei>(xfbNames.size()),
                                                        xfbNames.data(),
                                                        stateProgramObject->GetTransformFeedbackBufferMode());
                // Unchecked before. A rejected capture set leaves the program linking happily
                // with NO capture set at all, and then every draw of every span records
                // nothing while the application reads its buffer's pre-draw bytes and
                // GL_NO_ERROR - the signature four conformance families were stuck on.
                if (const GLenum xfbError = g_GLESFuncs.glGetError(); xfbError != GL_NO_ERROR) {
                    String declared;
                    for (const auto& xfbName : rewrittenXfbNames) {
                        if (!declared.empty()) declared += ", ";
                        declared += xfbName;
                    }
                    MGLOG_E("The ES driver REJECTED the transform feedback capture set for backend program %u with "
                            "%s (mode %s): [%s]. Every capture made with GL program %u will record nothing.",
                            m_backendProgramId, MG_Util::ConvertGLEnumToString(xfbError).c_str(),
                            MG_Util::ConvertGLEnumToString(
                                stateProgramObject->GetTransformFeedbackBufferMode()).c_str(),
                            declared.c_str(), stateProgramObject->GetExternalIndex());
                }
                declaredXfbVaryingCount = xfbNames.size();
            }

            // Link program
            MGLOG_D("Linking program %u", m_backendProgramId);
            g_GLESFuncs.glLinkProgram(m_backendProgramId);

            GLint linkStatus = GL_FALSE;
            g_GLESFuncs.glGetProgramiv(m_backendProgramId, GL_LINK_STATUS, &linkStatus);
            m_backendProgramUsable = m_backendProgramUsable && linkStatus == GL_TRUE;
            if (linkStatus != GL_TRUE) {
                GLint logLength = 0;
                g_GLESFuncs.glGetProgramiv(m_backendProgramId, GL_INFO_LOG_LENGTH, &logLength);
                if (logLength < 0) logLength = 0;
                Vector<GLchar> log(static_cast<SizeT>(logLength) + 1, '\0');
                g_GLESFuncs.glGetProgramInfoLog(m_backendProgramId, logLength, nullptr, log.data());
                log.back() = '\0';
                // MGLOG_I for the same reason as the compile failure above: a program that
                // links nothing no-ops every draw that uses it, and that has to be readable
                // in an INFO-level artifact.
                MGLOG_E("Program linking failed. State program ID: %u, backend program ID: %u, driver log: %s",
                        stateProgramObject->GetExternalIndex(), m_backendProgramId, log.data());
                // The one link failure MobileGL can name a cause for that the driver's log never
                // will: ESSL has no legal single declaration for a read+write image outside
                // r32f/r32i/r32ui, so those are split into a coherent pair and the stage ends up
                // declaring more image uniforms than the application did - against a
                // GL_MAX_*_IMAGE_UNIFORMS that is still the driver's raw number, because lowering
                // it would fail basic-api and NotSupported-out every case that only ever uses
                // readonly/writeonly images. A shader declaring more than half a stage's budget in
                // read+write images therefore links here and nowhere else, and without this line
                // the next reader has only a generic driver message to go on.
                for (const SplitImageUniformStage& split : splitImageUniformStages) {
                    MGLOG_E("...and %u read+write image uniform(s) in stage %s were split into coherent "
                            "read/write pairs, so that stage declares %u image uniform(s) more than the "
                            "program did; GL_MAX_*_IMAGE_UNIFORMS for it is %d. If the driver log names "
                            "image uniforms, that is the cause.",
                            split.splitCount,
                            MG_Util::ConvertGLEnumToString(
                                MG_Util::ConvertShaderStageToGLEnum(split.stage)).c_str(),
                            split.splitCount, AdvertisedStageImageUniformLimit(split.stage));
                }
            } else {
                MGLOG_D("Program linked successfully. ID: %u", m_backendProgramId);
                // A link that SUCCEEDS can still have dropped the capture set: ESSL rejects a
                // requested name the transpiled shader does not actually declare by simply not
                // capturing it, and a program whose last vertex-processing stage was rewritten
                // by a SPIR-V pass (viewport-index lowering, gl_PerVertex handling, the
                // synthesized pass-through tessellation control stage) can end up spelling its
                // outputs differently from the frontend's request. Asking the driver what it
                // ACTUALLY linked is the only way to tell that apart from a driver that just
                // captures nothing - which is the whole ambiguity the empty-capture failures
                // across geometry_shader / tessellation_shader / gpu_shader5 / DSA sat on.
                if (declaredXfbVaryingCount > 0) {
                    GLint linkedXfbVaryings = 0;
                    GLint linkedXfbBufferMode = 0;
                    g_GLESFuncs.glGetProgramiv(m_backendProgramId, GL_TRANSFORM_FEEDBACK_VARYINGS,
                                               &linkedXfbVaryings);
                    g_GLESFuncs.glGetProgramiv(m_backendProgramId, GL_TRANSFORM_FEEDBACK_BUFFER_MODE,
                                               &linkedXfbBufferMode);
                    for (Int i = 0; i < kMaxDrainedProgramErrors && g_GLESFuncs.glGetError() != GL_NO_ERROR; ++i) {
                    }
                    const GLenum requestedMode = stateProgramObject->GetTransformFeedbackBufferMode();
                    if (static_cast<SizeT>(std::max(linkedXfbVaryings, 0)) != declaredXfbVaryingCount ||
                        static_cast<GLenum>(linkedXfbBufferMode) != requestedMode) {
                        MGLOG_E("Backend program %u (GL program %u) linked with a capture set the driver does not "
                                "agree with: asked for %zu varying(s) in mode %s, the driver reports %d varying(s) "
                                "in mode %s. Captures made with it will be empty or wrongly laid out.",
                                m_backendProgramId, stateProgramObject->GetExternalIndex(), declaredXfbVaryingCount,
                                MG_Util::ConvertGLEnumToString(requestedMode).c_str(), linkedXfbVaryings,
                                MG_Util::ConvertGLEnumToString(
                                    static_cast<GLenum>(linkedXfbBufferMode)).c_str());
                    } else {
                        MGLOG_D("Backend program %u capture set confirmed by the driver: %d varying(s), mode %s",
                                m_backendProgramId, linkedXfbVaryings,
                                MG_Util::ConvertGLEnumToString(
                                    static_cast<GLenum>(linkedXfbBufferMode)).c_str());
                    }
                }
            }
            // The driver program was relinked IN PLACE, so its GL name no longer identifies
            // the executable behind it - and that name is exactly what Use()'s
            // g_lastUsedBackendProgramId early-out treats as identifying it. Without this,
            // a rebuild of the program that is already bound issues no glUseProgram at all
            // and the driver keeps running whatever the last one installed.
            //
            // GL 4.6 core 7.3 does promise that a successful re-link of a program in use
            // installs the new executable - but only "for all shader stages where the program
            // is active", and a stage the previous link did not produce is not active for
            // anything. So a relink that ADDS a stage is precisely the case the promise does
            // not cover. Verified with no MobileGL in the process (bare EGL + GLES 3.2, Mesa
            // 26.1.4 llvmpipe): vertex+fragment linked, used and drawn renders; a geometry
            // shader attached and relinked reports LINK_STATUS true with an empty info log,
            // and the next draw renders NOTHING and raises no error - while the same draw
            // after a fresh glUseProgram of the same name renders again.
            //
            // A flag rather than zeroing the guard: 0 is also the id Use() binds for a build
            // that did NOT come out usable, and a zeroed guard would make it skip that
            // glUseProgram(0) and leave the failed program's previous executable running -
            // the silent wrong-shader draw Use() exists to prevent.
            m_rebindAfterRelink = true;
            m_baseInstanceUniformLocation = g_GLESFuncs.glGetUniformLocation(m_backendProgramId,
                                                                             BASE_INSTANCE_UNIFORM_NAME);
            m_drawIdUniformLocation = g_GLESFuncs.glGetUniformLocation(m_backendProgramId, DRAW_ID_UNIFORM_NAME);
            m_baseVertexUniformLocation = g_GLESFuncs.glGetUniformLocation(m_backendProgramId,
                                                                           BASE_VERTEX_UNIFORM_NAME);
            m_baseInstanceWordIndexUniformLocation =
                g_GLESFuncs.glGetUniformLocation(m_backendProgramId, BASE_INSTANCE_WORD_INDEX_UNIFORM_NAME);
            // Asked of the DRIVER rather than remembered from the injection, deliberately: the
            // gate is only real if the uniform survived compilation and linking, and this is the
            // one question whose answer covers both. A gate the driver optimized away would
            // otherwise leave the draw path replaying passes whose mask reaches nothing, which
            // renders every index's primitives in every pass.
            m_viewportPassMaskUniformLocation =
                g_GLESFuncs.glGetUniformLocation(m_backendProgramId, VIEWPORT_PASS_MASK_UNIFORM_NAME);
            if (m_viewportPassMaskUniformLocation >= 0) {
                // Sticky, and never cleared on a relink: it only ever short-circuits a per-draw
                // check, so being late to go false costs a pointer compare and being late to go
                // true would cost correctness.
                g_anyProgramRoutesViewportIndex = true;
            }
            // The mg_IndirectParams block binding is baked into the ESSL (ES cannot rebind
            // SSBO blocks after compile); record it so draws bind the indirect buffer there.
            m_indirectParamsBinding = -1;
            if (m_baseInstanceWordIndexUniformLocation >= 0 && g_GLESFuncs.glGetProgramResourceIndex) {
                const GLuint blockIndex = g_GLESFuncs.glGetProgramResourceIndex(
                    m_backendProgramId, GL_SHADER_STORAGE_BLOCK, INDIRECT_PARAMS_BLOCK_NAME);
                if (blockIndex != GL_INVALID_INDEX && g_GLESCapabilities.MaxShaderStorageBufferBindings > 0) {
                    m_indirectParamsBinding = g_GLESCapabilities.MaxShaderStorageBufferBindings - 1;
                }
            }

            // Create global UBO
            if (stateProgramObject->GetUBOSize() > 0) {
                g_GLESFuncs.glGenBuffers(1, &m_backendGlobalUBOId);
                g_GLESFuncs.glBindBuffer(GL_UNIFORM_BUFFER, m_backendGlobalUBOId);
                g_GLESFuncs.glBufferData(GL_UNIFORM_BUFFER, stateProgramObject->GetUBOSize(), nullptr, GL_STREAM_DRAW);
                g_GLESFuncs.glBindBuffer(GL_UNIFORM_BUFFER, 0);
            } else {
                m_backendGlobalUBOId = 0;
            }

            CacheResourceLocations(stateProgramObject);
            // NOT the mechanism that makes a rebinding work - the transpiled qualifier above is.
            // glShaderStorageBlockBinding is a GL 4.3 entry point that no real ES driver exposes,
            // so this replay is a no-op almost everywhere; it stays because it is still correct
            // (and cheaper than a rebuild) on a driver that does expose it, e.g. a desktop GL
            // driver used as the ES backend. AFTER the link either way, because it needs the
            // driver's linked interface.
            ReseedShaderStorageBlockBindings(m_backendProgramId, *stateProgramObject);
            m_syncedLinkVersion = stateProgramObject->GetLinkVersion();
            m_syncedImageUnitVersion = stateProgramObject->GetImageUnitVersion();

            m_isInitialized = true;
            MGLOG_D("Program sync completed. backend ID %u", m_backendProgramId);
        }

        namespace {
            // The GL name of the array element that lives at `location`, given the reflection
            // name reported for it. Reflection reports one name per UNIFORM ("goku[0]") but
            // one location per ELEMENT, so a caller walking locations sees the same name
            // repeatedly; this turns it back into "goku[k]". Anything that is not an array
            // (or whose base location cannot be resolved) comes back unchanged, so the only
            // behaviour that moves is the array case.
            String SubscriptUniformNameForElement(const MG_State::GLState::ProgramObject& program, const String& name,
                                                  Uint location) {
                if (name.size() < 3 || name.compare(name.size() - 3, 3, "[0]") != 0) return name;
                const Int base = program.GetUniformLocation(name);
                if (base < 0 || static_cast<Uint>(base) > location) return name;
                const Uint element = location - static_cast<Uint>(base);
                if (element == 0) return name;
                return name.substr(0, name.size() - 3) + "[" + std::to_string(element) + "]";
            }
        } // namespace

        // Resolves every name-based resource lookup once per link so the per-draw path
        // (BindCurrentProgramWithResources) never issues glGetUniformBlockIndex /
        // glGetUniformLocation string queries; block-to-binding-point assignments are
        // program state and only need to be established here.
        void BackendProgramObjectImpl::CacheResourceLocations(
            const SharedPtr<MG_State::GLState::ProgramObject>& stateProgramObject) {
            m_globalUboBackendBlockIndex = -1;
            m_globalUboBackendBlockSize = 0;
            m_lastUploadedGlobalUboVersion = ~0u;
            m_globalUboRingAllocation = {};
            if (stateProgramObject->GetUBOSize() > 0) {
                const Uint blockIndex =
                    g_GLESFuncs.glGetUniformBlockIndex(m_backendProgramId, MG_Util::ShaderTranspiler::GLOBAL_UBO_NAME);
                if (blockIndex != GL_INVALID_INDEX) {
                    m_globalUboBackendBlockIndex = static_cast<Int>(blockIndex);
                    g_GLESFuncs.glUniformBlockBinding(m_backendProgramId, blockIndex, 0);
                    // Ring bindings are ranges and must span the block as the backend
                    // compiled it (its std140 padding may exceed the frontend's
                    // SPIR-V-reflected size).
                    if (g_GLESFuncs.glGetActiveUniformBlockiv) {
                        GLint blockDataSize = 0;
                        g_GLESFuncs.glGetActiveUniformBlockiv(m_backendProgramId, blockIndex,
                                                              GL_UNIFORM_BLOCK_DATA_SIZE, &blockDataSize);
                        m_globalUboBackendBlockSize = static_cast<Int>(blockDataSize);
                    }
                } else {
                    MGLOG_W_ONCE("Program %u has frontend global UBO storage, but backend has no %s block.",
                            stateProgramObject->GetExternalIndex(), MG_Util::ShaderTranspiler::GLOBAL_UBO_NAME);
                }
            }

            const Int uboCount = stateProgramObject->GetActiveUniformBlocksCount();
            m_uniformBlockBackendIndices.assign(static_cast<SizeT>(std::max(uboCount, 0)), -1);
            Uint lastUBOBinding = 0; // binding 0 is reserved for the global UBO
            for (Int i = 0; i < uboCount; ++i) {
                ++lastUBOBinding;
                const auto& name = stateProgramObject->GetUniformBlockName(static_cast<Uint>(i));
                const GLuint backendBlkIdx = g_GLESFuncs.glGetUniformBlockIndex(m_backendProgramId, name.c_str());
                if (backendBlkIdx == GL_INVALID_INDEX) {
                    // Either eliminated as unused, or an SSBO block (frontend reflection
                    // lists those among uniform blocks); SSBO bindings are baked into the ESSL.
                    continue;
                }
                m_uniformBlockBackendIndices[static_cast<SizeT>(i)] = static_cast<Int>(backendBlkIdx);
                g_GLESFuncs.glUniformBlockBinding(m_backendProgramId, backendBlkIdx, lastUBOBinding);
                MGLOG_D("CACHE prog=%u beProg=%u blk[%d]='%s' beIdx=%u -> bePoint=%u",
                        stateProgramObject->GetExternalIndex(), m_backendProgramId, i, name.c_str(), backendBlkIdx,
                        lastUBOBinding);
            }

            m_samplerUniformBindings.clear();
            const Uint maxUniformLoc = stateProgramObject->GetMaxUniformLocation();
            for (Uint loc = 0; loc <= maxUniformLoc; ++loc) {
                const auto& name = stateProgramObject->GetUniformName(loc);
                if (name.empty()) continue;
                const GLenum uniformType = stateProgramObject->GetUniformType(loc);
                if (IsImageUniformType(uniformType)) {
                    // ES image units come exclusively from the layout(binding=N) qualifier
                    // (preserved in the transpiled ESSL); glUniform1i on an image uniform
                    // is an INVALID_OPERATION.
                    continue;
                }
                // Reflection names an array uniform after its FIRST element ("goku[0]") at
                // every location the array spans, so asking the driver for that one name
                // once per location hands back the same backend location N times. The
                // per-draw pass then issues N glUniform1i calls against it and only the
                // last element's unit survives - "layout(binding = 1) uniform sampler2D
                // goku[7]" ended up with goku[0] on unit 7 and goku[1..6] still on 0.
                // Address each element by its own name instead; the frontend already
                // reserves one location per element, so the element index is the distance
                // from the array's base location.
                const String elementName = SubscriptUniformNameForElement(*stateProgramObject, name, loc);
                const Int backendLoc = g_GLESFuncs.glGetUniformLocation(m_backendProgramId, elementName.c_str());
                if (backendLoc < 0) continue;
                SamplerUniformBinding binding;
                binding.frontendLocation = loc;
                binding.backendLocation = backendLoc;
                binding.uniformType = uniformType;
                binding.lastAssignedUnit = -1;
                // Present only for the samplers EmulateTextureLodBias actually rewrote; the
                // pass names it after the sampler, which SPIRV-Cross preserves verbatim.
                binding.lodBiasLocation = g_GLESFuncs.glGetUniformLocation(
                    m_backendProgramId, (String(LOD_BIAS_UNIFORM_PREFIX) + elementName).c_str());
                binding.lastAssignedLodBias = 0.0f;
                m_samplerUniformBindings.push_back(binding);
            }
        }

        void BackendProgramObjectImpl::Use() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            // glUseProgram on a program that did not link is an INVALID_OPERATION and
            // leaves the *previous* program current, so the draw would silently render
            // with an unrelated shader (KHR-GL3x.texture_size_promotion read another
            // test case's alpha that way once a sampler2DRect stage failed to
            // transpile). Bind nothing instead: the draw is then a visible no-op.
            const Uint programToBind = m_backendProgramUsable ? m_backendProgramId : 0;
            // ...unless SyncToBackend relinked this program since the last bind, in which case
            // the id matching proves nothing about the executable behind it.
            if (g_lastUsedBackendProgramId == programToBind && !m_rebindAfterRelink) {
                return;
            }
            m_rebindAfterRelink = false;
            if (!m_backendProgramUsable) {
                // Every draw made with this program renders nothing and raises no GL error, so
                // without this line the only symptom is a framebuffer that kept its clear
                // colour. Latched: the early return above only dedupes CONSECUTIVE binds, so an
                // app alternating a healthy and a broken program would otherwise log every
                // single draw. Parked at MGLOG_I until the level ordering was fixed.
                MGLOG_E_ONCE("Backend program for GL program %u is unusable (a shader failed to transpile, "
                        "compile or link); binding program 0 - draws with it will render nothing",
                        m_frontendProgramId);
            }
            MGLOG_D("Using program %u", programToBind);
            g_GLESFuncs.glUseProgram(programToBind);
            g_lastUsedBackendProgramId = programToBind;
        }

        void BackendProgramObjectImpl::SetBaseInstance(Uint32 baseInstance) const {
            if (m_baseInstanceUniformLocation >= 0) {
                g_GLESFuncs.glUniform1i(m_baseInstanceUniformLocation, static_cast<GLint>(baseInstance));
            }
            // A direct value disables the indirect-command-buffer read.
            SetBaseInstanceWordIndex(-1);
        }

        // The uniform is written one-based so that its GLSL initial value, zero, already reads
        // as "no indirect command" - see PromoteDrawParameterGlobalsToUniforms.
        void BackendProgramObjectImpl::SetBaseInstanceWordIndex(Int32 wordIndex) const {
            if (m_baseInstanceWordIndexUniformLocation >= 0) {
                g_GLESFuncs.glUniform1i(m_baseInstanceWordIndexUniformLocation,
                                        wordIndex < 0 ? 0 : wordIndex + 1);
            }
        }

        void BackendProgramObjectImpl::SetBaseVertex(Int32 baseVertex) const {
            if (m_baseVertexUniformLocation >= 0) {
                g_GLESFuncs.glUniform1i(m_baseVertexUniformLocation, baseVertex);
            }
        }

        void BackendProgramObjectImpl::SetDrawID(Uint32 drawId) const {
            if (m_drawIdUniformLocation < 0) {
                return;
            }
            g_GLESFuncs.glUniform1i(m_drawIdUniformLocation, static_cast<GLint>(drawId));
        }

        void BackendProgramObjectImpl::SetViewportPassMask(Uint32 indexMask) const {
            if (m_viewportPassMaskUniformLocation < 0) {
                return;
            }
            g_GLESFuncs.glUniform1i(m_viewportPassMaskUniformLocation, static_cast<GLint>(indexMask));
        }
    } // namespace PrgramImpl

    namespace SamplerImpl {
        BackendSamplerObject::BackendSamplerObject() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_GLESFuncs.glGenSamplers(1, &m_backendSamplerId);
            m_contextGeneration = g_backendContextGeneration;
            if (m_backendSamplerId == 0) {
                MGLOG_E_ONCE("Failed to generate sampler object.");
                MGLOG_E_ONCE("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            } else {
                MGLOG_D("Generated sampler object with ID: %u.", m_backendSamplerId);
            }
        }

        BackendSamplerObject::~BackendSamplerObject() {
            if (InProcessTeardown()) {
                return; // see InProcessTeardown(): the driver may be unloaded already
            }
            if (m_backendSamplerId == 0) {
                return;
            }
            // Scrub the unit shadow whether or not the id can still be deleted - the next
            // twin can land on this heap address and would otherwise false-skip its Bind.
            for (auto& boundSampler : g_boundSamplersCache) {
                if (boundSampler == this) {
                    boundSampler = nullptr; // glDeleteSamplers unbinds from every unit
                }
            }
            if (m_contextGeneration == g_backendContextGeneration && g_GLESFuncs.glDeleteSamplers) {
                g_GLESFuncs.glDeleteSamplers(1, &m_backendSamplerId);
            }
            m_backendSamplerId = 0;
        }

        void BackendSamplerObject::SyncToBackend(
            const SharedPtr<MG_State::GLState::SamplerObject>& stateSamplerObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!stateSamplerObject) {
                MGLOG_E_ONCE("State sampler object is null, cannot sync to backend.");
                return;
            }

            Uint currentSamplerVersion = stateSamplerObject->GetVersion();
            if (m_isInitialized && m_syncedSamplerVersion == currentSamplerVersion) {
                MGLOG_D("Sampler parameters have not changed for sampler ID: %u, skipping sync.",
                        stateSamplerObject->GetExternalIndex());
                return;
            }

            m_syncedSamplerVersion = currentSamplerVersion;

            MGLOG_D("Syncing sampler with backend ID %u to backend for state ID %u", m_backendSamplerId,
                    stateSamplerObject->GetExternalIndex());

            const auto& samplerParams = stateSamplerObject->GetAllSamplerParameters();

#define SYNC_SAMPLER_PARAM_IF_CHANGED(internalName, glName, type)                                                      \
    if (m_cacheSamplerParameters.internalName != samplerParams.internalName) {                                         \
        g_GLESFuncs.glSamplerParameteri(m_backendSamplerId, glName,                                                    \
                                        (GLint)MG_Util::ConvertSampler##type##ToGLEnum(samplerParams.internalName));   \
        m_cacheSamplerParameters.internalName = samplerParams.internalName;                                            \
    }

            if (m_cacheSamplerParameters.minFilter != samplerParams.minFilter ||
                m_cacheSamplerParameters.mipmapMode != samplerParams.mipmapMode) {
                g_GLESFuncs.glSamplerParameteri(m_backendSamplerId, GL_TEXTURE_MIN_FILTER,
                                                (GLint)ResolveBackendMinFilter(
                                                    samplerParams,
                                                    ShouldAvoidSamplerMipmapMinFilterOnAngleLlvmpipe()));
                m_cacheSamplerParameters.minFilter = samplerParams.minFilter;
                m_cacheSamplerParameters.mipmapMode = samplerParams.mipmapMode;
            }
            if (m_cacheSamplerParameters.magFilter != samplerParams.magFilter) {
                g_GLESFuncs.glSamplerParameteri(
                    m_backendSamplerId, GL_TEXTURE_MAG_FILTER,
                    (GLint)MG_Util::ConvertSamplerFilterModeToGLEnum(samplerParams.magFilter, SamplerMipmapMode::None));
                m_cacheSamplerParameters.magFilter = samplerParams.magFilter;
            }

            SYNC_SAMPLER_PARAM_IF_CHANGED(wrapS, GL_TEXTURE_WRAP_S, WrapMode)
            SYNC_SAMPLER_PARAM_IF_CHANGED(wrapT, GL_TEXTURE_WRAP_T, WrapMode)
            SYNC_SAMPLER_PARAM_IF_CHANGED(wrapR, GL_TEXTURE_WRAP_R, WrapMode)
            SYNC_SAMPLER_PARAM_IF_CHANGED(compareFunc, GL_TEXTURE_COMPARE_FUNC, CompareFunc)
            SYNC_SAMPLER_PARAM_IF_CHANGED(compareMode, GL_TEXTURE_COMPARE_MODE, CompareMode)
            if (m_cacheSamplerParameters.minLod != samplerParams.minLod) {
                g_GLESFuncs.glSamplerParameterf(m_backendSamplerId, GL_TEXTURE_MIN_LOD, samplerParams.minLod);
                m_cacheSamplerParameters.minLod = samplerParams.minLod;
            }
            if (m_cacheSamplerParameters.maxLod != samplerParams.maxLod) {
                g_GLESFuncs.glSamplerParameterf(m_backendSamplerId, GL_TEXTURE_MAX_LOD, samplerParams.maxLod);
                m_cacheSamplerParameters.maxLod = samplerParams.maxLod;
            }
            if (m_cacheSamplerParameters.maxAnisotropy != samplerParams.maxAnisotropy) {
                if (g_GLESCapabilities.SupportsTextureFilterAnisotropy) {
                    g_GLESFuncs.glSamplerParameterf(m_backendSamplerId, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                                                    samplerParams.maxAnisotropy);
                }
                m_cacheSamplerParameters.maxAnisotropy = samplerParams.maxAnisotropy;
            }
            if (m_cacheSamplerParameters.borderColor != samplerParams.borderColor ||
                m_cacheSamplerParameters.borderColorI != samplerParams.borderColorI ||
                m_cacheSamplerParameters.borderColorUI != samplerParams.borderColorUI ||
                m_cacheSamplerParameters.borderColorForm != samplerParams.borderColorForm) {
                // Same gate as the texture-side border colour above, and the same reason for
                // branching on the form: an integer border colour must reach the driver through
                // glSamplerParameterIiv/Iuiv or an integer sampler reads the float's bit pattern
                // back instead of the value.
                if (g_GLESCapabilities.SupportsTextureBorderClamp) {
                    if (samplerParams.borderColorForm == BorderColorForm::Int &&
                        g_GLESFuncs.glSamplerParameterIiv) {
                        const GLint borderColorArray[4] = {
                            samplerParams.borderColorI.x(), samplerParams.borderColorI.y(),
                            samplerParams.borderColorI.z(), samplerParams.borderColorI.w()};
                        g_GLESFuncs.glSamplerParameterIiv(m_backendSamplerId, GL_TEXTURE_BORDER_COLOR,
                                                          borderColorArray);
                    } else if (samplerParams.borderColorForm == BorderColorForm::Uint &&
                               g_GLESFuncs.glSamplerParameterIuiv) {
                        const GLuint borderColorArray[4] = {
                            samplerParams.borderColorUI.x(), samplerParams.borderColorUI.y(),
                            samplerParams.borderColorUI.z(), samplerParams.borderColorUI.w()};
                        g_GLESFuncs.glSamplerParameterIuiv(m_backendSamplerId, GL_TEXTURE_BORDER_COLOR,
                                                           borderColorArray);
                    } else if (g_GLESFuncs.glSamplerParameterfv) {
                        const GLfloat borderColorArray[4] = {
                            samplerParams.borderColor.x(), samplerParams.borderColor.y(),
                            samplerParams.borderColor.z(), samplerParams.borderColor.w()};
                        g_GLESFuncs.glSamplerParameterfv(m_backendSamplerId, GL_TEXTURE_BORDER_COLOR,
                                                         borderColorArray);
                    }
                }
                m_cacheSamplerParameters.borderColor = samplerParams.borderColor;
                m_cacheSamplerParameters.borderColorI = samplerParams.borderColorI;
                m_cacheSamplerParameters.borderColorUI = samplerParams.borderColorUI;
                m_cacheSamplerParameters.borderColorForm = samplerParams.borderColorForm;
            }
#undef SYNC_SAMPLER_PARAM_IF_CHANGED
            m_isInitialized = true;
        }

        void BackendSamplerObject::Bind(Uint unit) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (g_boundSamplersCache[unit] == this) return;

            g_GLESFuncs.glBindSampler(static_cast<GLenum>(unit), m_backendSamplerId);
            g_boundSamplersCache[unit] = this;
        }

        Uint BackendSamplerObject::GetBackendSamplerId() const {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            return m_backendSamplerId;
        }

        void UnbindSampler(Uint unit) {
            if (g_boundSamplersCache[unit] == nullptr) return;

            g_GLESFuncs.glBindSampler(static_cast<GLenum>(unit), 0);
            g_boundSamplersCache[unit] = nullptr;
        }

        Array<BackendSamplerObject*, MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS> g_boundSamplersCache;
        StateBackendObjectRegistry<MG_State::GLState::SamplerObject, BackendSamplerObject> g_backendSamplerObjects;
    } // namespace SamplerImpl

    namespace RenderbufferImpl {
        BackendRenderbufferObject::BackendRenderbufferObject() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_GLESFuncs.glGenRenderbuffers(1, &m_backendRBOId);
            m_contextGeneration = g_backendContextGeneration;
            if (m_backendRBOId == 0) {
                MGLOG_E_ONCE("Failed to generate renderbuffer object.");
                MGLOG_E_ONCE("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            }
        }

        BackendRenderbufferObject::~BackendRenderbufferObject() {
            if (InProcessTeardown()) {
                return; // see InProcessTeardown(): the driver may be unloaded already
            }
            if (m_backendRBOId == 0) {
                return;
            }
            // No driver-level renderbuffer-binding shadow exists (Bind() always issues the
            // call), so there is nothing to scrub here - only the id to release.
            if (m_contextGeneration == g_backendContextGeneration && g_GLESFuncs.glDeleteRenderbuffers) {
                g_GLESFuncs.glDeleteRenderbuffers(1, &m_backendRBOId);
            }
            m_backendRBOId = 0;
        }

        void BackendRenderbufferObject::Bind() const {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, m_backendRBOId);
        }

        void BackendRenderbufferObject::SyncToBackend(
            const SharedPtr<MG_State::GLState::RenderbufferObject>& stateRBOObject) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!stateRBOObject) {
                MGLOG_E_ONCE("State RBO object is null, cannot sync to backend.");
                return;
            }

            MGLOG_D("Syncing RBO with backend ID %u to backend for state ID %u", m_backendRBOId,
                    stateRBOObject->GetExternalIndex());

            if (m_isInitialized && m_cacheInternalFormat == stateRBOObject->GetInternalFormat() &&
                m_cacheWidth == stateRBOObject->GetWidth() && m_cacheHeight == stateRBOObject->GetHeight() &&
                m_cacheSamples == stateRBOObject->GetSamples()) {
                MGLOG_D("RBO %u already initialized with matching parameters, skipping re-allocation.",
                        stateRBOObject->GetExternalIndex());
                return;
            }

            Bind();

            // Allocate storage
            TextureInternalFormat internalFormat = stateRBOObject->GetInternalFormat();
            Int width = static_cast<Int>(stateRBOObject->GetWidth());
            Int height = static_cast<Int>(stateRBOObject->GetHeight());
            Int samples = static_cast<Int>(stateRBOObject->GetSamples());
            GLenum glInternalFormat, glType, glFormat;
            TextureImpl::GenerateRenderbufferFormatInfo(internalFormat, &glInternalFormat, &glFormat, &glType);

            // The allocation is deferred to here, so an ES driver that refuses it (a
            // multi-gigabyte renderbuffer is refused routinely) used to leave m_isInitialized
            // true over a renderbuffer with no storage and say nothing at all: the attachment
            // then rendered nowhere. Drain first so the check cannot pick up an unrelated stale
            // flag, and report GL_OUT_OF_MEMORY to the application. The error lands on whatever
            // entry point triggered the sync rather than on glRenderbufferStorage itself, which
            // is where the deferred model puts it - still far better than silence.
            DebugImpl::ErrorLopper::Clear();
            if (samples > 0) {
                // Same clamp as the multisample texture path: the frontend accepts the count it
                // advertised, the driver only takes the count it supports for this format, and
                // the state object keeps reporting the requested one.
                const auto backendSamples = static_cast<GLsizei>(ClampSamplesToBackendSupport(
                    GetRenderbufferFormatCapabilityTargetIndex(), internalFormat, glFormat, samples));
                g_GLESFuncs.glRenderbufferStorageMultisample(GL_RENDERBUFFER, backendSamples, glInternalFormat,
                                                             static_cast<GLsizei>(width),
                                                             static_cast<GLsizei>(height));
            } else {
                g_GLESFuncs.glRenderbufferStorage(GL_RENDERBUFFER, glInternalFormat, static_cast<GLsizei>(width),
                                                  static_cast<GLsizei>(height));
            }
            if (g_GLESFuncs.glGetError() == GL_OUT_OF_MEMORY) {
                MGLOG_E_ONCE("Renderbuffer %u storage allocation ran out of memory: %dx%d, samples=%d, format=%s",
                             stateRBOObject->GetExternalIndex(), width, height, samples,
                             MG_Util::ConvertGLEnumToString(glInternalFormat).c_str());
                if (MG_State::pGLContext) {
                    MG_State::pGLContext->RecordError(
                        ErrorCode::OutOfMemory,
                        MakeUnique<GenericErrorInfo>("DirectGLES", "BackendRenderbufferObject::SyncToBackend",
                                                     "The ES driver could not allocate the renderbuffer storage."));
                }
            }
            DebugImpl::ErrorLopper::Clear();

            m_cacheInternalFormat = internalFormat;
            m_cacheWidth = width;
            m_cacheHeight = height;
            m_cacheSamples = samples;

            m_isInitialized = true;
            MGLOG_D("RBO %u sync completed. backend ID %u", stateRBOObject->GetExternalIndex(), m_backendRBOId);
        }

        StateBackendObjectRegistry<MG_State::GLState::RenderbufferObject, BackendRenderbufferObject>
            g_backendRenderbufferObjects;
    } // namespace RenderbufferImpl
} // namespace MobileGL::MG_Backend::DirectGLES
