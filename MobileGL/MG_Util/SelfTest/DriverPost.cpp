// MobileGL - MobileGL/MG_Util/SelfTest/DriverPost.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DriverPost.h"
#include "MG_Util/BackendLoaders/OpenGL/Loader.h"
#include <Config.h>
#include <MGGitHash.h>
#include <MG_Backend/DirectGLES/BackendObject_DirectGLES.h>
#include <MG_Backend/DirectGLES/MultiDraw.h>
#include <MG_Backend/DirectVulkan/BackendObject_DirectVulkan.h>
// Only for the compile-time MAX_VERTEX_ATTRIBS constant asserted below. The POST still executes no
// MG_State code: it runs standalone, before MG_State::Init().
#include <MG_State/GLState/VertexArrayState/VertexArrayObject.h>
#include <MG_Backend/DirectGLES/Utils.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToStr/GLExtensionConverter.h>
#include <MG_Util/Converters/MGToStr/TextureEnumConverter.h>
#include <MG_Util/Texture/TextureFormatProcessor.h>
#include <MG_Util/Async/ShaderCompilePool.h>
#include <chrono>
#include <thread>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace MobileGL::MG_Util::SelfTest {
    namespace {
        // Display ranks for PostCheck::displayRank: within one backend section, FAIL
        // rows render first, then WARN, PASS, INFO, then the device-driver identity
        // strings, and always last (regardless of status) the strings MobileGL itself
        // reports to applications. Rows are stable-sorted, so relative order within a
        // rank is preserved. Purely cosmetic: the verdict computation is unaffected.
        enum DisplayRank : Int {
            RankFail = 0,
            RankWarn = 1,
            RankPass = 2,
            RankInfo = 3,
            RankDriverReported = 4,
            RankMobileGLReported = 5,
        };

        // Both backends' fp64 rows end the same way, and the sentence they end with depends on
        // a config flag rather than on anything either backend probes: the demotion is what
        // makes doubles work, but GL_ARB_gpu_shader_fp64 promises the PRECISION the demotion
        // cannot deliver, so the string is opt-in and the row has to say which way it went.
        String AppendFp64AdvertisementNote(String detail) {
            if (MG_Config::Features.AdvertiseFp64) {
                return Move(detail) +
                       ". GL_ARB_gpu_shader_fp64 IS advertised (MOBILEGL_ADVERTISE_FP64): an application "
                       "that checks the string will believe it has 64-bit precision, and it does not";
            }
            return Move(detail) +
                   ". GL_ARB_gpu_shader_fp64 is not advertised, because the precision it promises is the "
                   "one thing the demotion cannot provide; set MOBILEGL_ADVERTISE_FP64=1 to advertise it "
                   "anyway";
        }

        struct ReportBuilder {
            BackendPostReport report;
            Bool fatalFailed = false;
            Bool warnUnmet = false;

            void Pass(String name, String detail) {
                report.checks.push_back({Move(name), "PASS", Move(detail), RankPass});
            }

            void Fail(String name, String detail) {
                fatalFailed = true;
                report.checks.push_back({Move(name), "FAIL", Move(detail), RankFail});
            }

            void Warn(String name, String detail) {
                warnUnmet = true;
                report.checks.push_back({Move(name), "WARN", Move(detail), RankWarn});
            }

            void Info(String name, String detail) {
                report.checks.push_back({Move(name), "INFO", Move(detail), RankInfo});
            }

            // A "Backend driver reported ..." identity string straight from the device
            // driver; rendered after the regular rows.
            void DriverReported(String name, String detail) {
                report.checks.push_back({Move(name), "INFO", Move(detail), RankDriverReported});
            }

            // A "MobileGL reported ..." string: what MobileGL itself reports to
            // applications on this backend; always rendered at the very bottom.
            void MobileGLReported(String name, String detail) {
                report.checks.push_back({Move(name), "INFO", Move(detail), RankMobileGLReported});
            }

            void Finalize() {
                report.verdict = fatalFailed ? "UNSUPPORTED" : (warnUnmet ? "DEGRADED" : "OK");
                std::stable_sort(report.checks.begin(), report.checks.end(),
                                 [](const PostCheck& a, const PostCheck& b) { return a.displayRank < b.displayRank; });
            }
        };

        // ---- "MobileGL reported ..." row assembly -------------------------------
        // The vendor/version/renderer strings mirror GL_Getter.cpp's GL_VENDOR /
        // GL_VERSION / GL_RENDERER cases; the backend API version string and the
        // extension list come from the per-backend single-source-of-truth helpers
        // (GetRendererIdentity / FormatBackendAPIVersionString /
        // BuildAdvertisedExtensions) shared with the real backends.

        // Mirrors GL_Getter.cpp's GL_VENDOR case.
        String BuildReportedGLVendor(const RendererInfo& identity) {
            if (identity.ExtraVendor.has_value()) {
                return format("{}{}", MG_Config::CoreVendor, identity.ExtraVendor.value());
            }
            return MG_Config::CoreVendor;
        }

        // Mirrors GL_Getter.cpp's GL_VERSION case.
        String BuildReportedGLVersion(const RendererInfo& identity) {
            return format("{} {} {}, {} Backend, GIT@" GIT_COMMIT_HASH_SHORT,
                          identity.RendererGLInfo.TargetGLVersion.toString(), MG_Config::ProjectName,
                          MG_Config::CoreVersion.toFormattedString(MG_Config::DefaultVersionStringFormatAttrib),
                          identity.BackendName);
        }

        // Mirrors GL_Getter.cpp's GL_RENDERER case.
        String BuildReportedGLRenderer(const RendererInfo& identity, const String& backendApiVersionString) {
            return format("{} ({}) ({})", identity.RendererName, MG_Config::CoreName, backendApiVersionString);
        }

        // Mirrors GL_Getter.cpp's GL_EXTENSIONS case (space-separated).
        String JoinAdvertisedExtensions(const Vector<GLExtension>& extensions) {
            String result;
            for (const auto& extension : extensions) {
                if (!result.empty()) {
                    result += " ";
                }
                result += ConvertGLExtToString(extension);
            }
            return result;
        }

        // ---- Asynchronous shader compilation ------------------------------------
        // MobileGL's OWN capability row, appended for both backends: nothing about it comes
        // from the device driver, so it is the same fact on Espryt and on Magma. The POST
        // rule ("every new capability gets a row") applies to frontend capabilities too -
        // and this one especially, because it is the capability that changes what
        // applications DO, not just what they can do: with the extension advertised, Iris
        // and Sodium batch their pipeline compiles and poll GL_COMPLETION_STATUS_KHR.
        //
        // PASS when it is on (the intended configuration once the default flips), INFO when
        // it is off - "off" is a supported configuration, not a degradation, so it must not
        // colour the verdict. Either way the row names MOBILEGL_ASYNC_SHADER_COMPILE, so a
        // user reading a POST page can tell which side of the switch they are on and how to
        // change it.
        void AppendAsyncShaderCompileRow(ReportBuilder& builder) {
            constexpr const char* rowName = "Asynchronous shader compilation";
            if (!MG_Util::Async::AsyncShaderCompileEnabled()) {
                builder.Info(rowName,
                             "off; glCompileShader and glLinkProgram run on the calling thread and "
                             "GL_KHR_parallel_shader_compile is not advertised (set environment variable "
                             "MOBILEGL_ASYNC_SHADER_COMPILE=1 to enable it)");
                return;
            }
            const Uint threads = MG_Util::Async::DetectShaderCompileThreadCount();
            builder.Pass(rowName,
                         format("on with {} compiler thread{}; GL_KHR_parallel_shader_compile is advertised "
                                "and GL_MAX_SHADER_COMPILER_THREADS_KHR = {} (set environment variable "
                                "MOBILEGL_ASYNC_SHADER_COMPILE=0 to disable it, or "
                                "MOBILEGL_ASYNC_SHADER_COMPILE_THREADS=n to change the count)",
                                threads, threads == 1 ? "" : "s", threads));
        }

        // Appends the four "MobileGL reported ..." rows for one backend section.
        // GL_VENDOR and GL_VERSION only depend on the backend's static identity, so
        // they are always concrete; GL_RENDERER and GL_EXTENSIONS need data from the
        // device probe and degrade to an explanatory detail when it failed.
        void AppendMobileGLReportedRows(ReportBuilder& builder, const RendererInfo& identity,
                                        const Optional<String>& backendApiVersionString,
                                        const Optional<String>& advertisedExtensions) {
            static const String Unavailable = "unavailable (backend probe failed)";
            // Frontend capability, not a probe result, so it is appended on every path -
            // including one where the device probe failed outright.
            AppendAsyncShaderCompileRow(builder);
            builder.MobileGLReported("MobileGL reported GL_VENDOR", BuildReportedGLVendor(identity));
            builder.MobileGLReported("MobileGL reported GL_VERSION", BuildReportedGLVersion(identity));
            builder.MobileGLReported("MobileGL reported GL_RENDERER",
                                     backendApiVersionString.has_value()
                                         ? BuildReportedGLRenderer(identity, backendApiVersionString.value())
                                         : Unavailable);
            builder.MobileGLReported("MobileGL reported GL_EXTENSIONS",
                                     advertisedExtensions.has_value() ? advertisedExtensions.value() : Unavailable);
        }

        // Runs a callable when the enclosing scope exits, so driver teardown still happens
        // even if a String/format allocation throws while report rows are being built.
        template <typename Callable>
        struct ScopeGuard {
            explicit ScopeGuard(Callable callable) : onExit(Move(callable)) {}
            ScopeGuard(const ScopeGuard&) = delete;
            ScopeGuard& operator=(const ScopeGuard&) = delete;
            ~ScopeGuard() { onExit(); }

        private:
            Callable onExit;
        };

        String EGLErrorSuffix(const MG_External::EGLFunctionsTable& eglFuncs) {
            if (!eglFuncs.eglGetError) {
                return "";
            }
            return format(" (EGL error 0x{:x})", eglFuncs.eglGetError());
        }

        // Suffix folded into each backend's single "Timer queries" row when the user
        // disabled timer queries; the note rides along with whatever combined verdict
        // the row carries instead of being a standalone INFO row, and spells out the
        // cause (the environment variable) and its consequence explicitly.
        String TimerQueryDisabledNote() {
            return MG_Config::Features.DisableTimerQuery
                       ? "; environment variable MOBILEGL_DISABLE_TIMERQUERY is set, disabling timer "
                         "queries as a result"
                       : "";
        }

        // ---- Vertex attribute limit --------------------------------------------
        // GL 3.3 Core mandates GL_MAX_VERTEX_ATTRIBS >= 16 (spec table 6.32); a driver below
        // that cannot back a conformant core context at all.
        constexpr Int kGL33MinVertexAttribs = 16;

        // The capacity of the per-context current-vertex-attribute array, which is also the width of
        // the Uint32 attribute masks the backends pass around. Pinned to the state layer's constant so
        // the two can never drift: a mismatch between them is precisely the defect this row guards.
        constexpr Int kMobileGLMaxVertexAttribs = MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS;
        static_assert(kMobileGLMaxVertexAttribs <= 32, "Vertex attribute masks are Uint32");
        static_assert(kMobileGLMaxVertexAttribs >= kGL33MinVertexAttribs,
                      "MobileGL cannot advertise a conformant GL 3.3 Core GL_MAX_VERTEX_ATTRIBS");

        // Both backends index a fixed-size, per-context array of current generic vertex attribute
        // values by shader input location, and both clamp the GL_MAX_VERTEX_ATTRIBS they advertise
        // to that array's capacity. A driver reporting more attributes than the array can hold used
        // to make the DirectVulkan draw path walk locations past the end of it -- an out-of-bounds
        // read in release builds, and a MOBILEGL_ASSERT abort in debug builds -- as soon as a shader
        // declared a vertex input at a high location whose array was disabled. The clamp closes that
        // hole, so this row exists to make the underlying driver/host mismatch visible rather than
        // silently swallowed.
        void EvaluateVertexAttribLimit(ReportBuilder& builder, Int deviceLimit, const char* rowName,
                                       const char* driverLimitName) {
            if (deviceLimit < kGL33MinVertexAttribs) {
                builder.Fail(rowName,
                             format("{} = {} (< {}); OpenGL 3.3 Core requires at least {} generic vertex "
                                    "attributes, so this driver cannot back a conformant core context",
                                    driverLimitName, deviceLimit, kGL33MinVertexAttribs, kGL33MinVertexAttribs));
                return;
            }
            if (deviceLimit > kMobileGLMaxVertexAttribs) {
                builder.Warn(rowName,
                             format("{} = {} (> {}); MobileGL clamps GL_MAX_VERTEX_ATTRIBS to {} because its "
                                    "current-vertex-attribute storage and its Uint32 attribute masks hold {} "
                                    "locations, so the driver's extra attributes stay unusable",
                                    driverLimitName, deviceLimit, kMobileGLMaxVertexAttribs,
                                    kMobileGLMaxVertexAttribs, kMobileGLMaxVertexAttribs));
                return;
            }
            builder.Pass(rowName, format("{} = {}; MobileGL advertises GL_MAX_VERTEX_ATTRIBS = {}",
                                         driverLimitName, deviceLimit, deviceLimit));
        }

        void EvaluateGlesChecklist(ReportBuilder& builder, const MG_External::GLESCapabilities& caps,
                                   const MG_External::GLESFunctionsTable& glesFuncs) {
            const Int major = caps.GLESVersion.Major;
            const Int minor = caps.GLESVersion.Minor;
            const Bool es31 = major > 3 || (major == 3 && minor >= 1);
            const Bool es32 = major > 3 || (major == 3 && minor >= 2);
            const String versionLabel = format("OpenGL ES {}.{}", major, minor);
            if (es32) {
                builder.Pass("OpenGL ES version", versionLabel + " (>= 3.2, full native feature set)");
            } else if (es31) {
                builder.Warn("OpenGL ES version",
                             versionLabel +
                                 " (compute shaders and native indirect draws available; ES 3.2 is recommended)");
            } else {
                builder.Fail("OpenGL ES version",
                             versionLabel + " (< 3.1: no compute shaders or native indirect draws)");
            }

            EvaluateVertexAttribLimit(builder, caps.MaxVertexAttribs, "Vertex attributes",
                                      "GL_MAX_VERTEX_ATTRIBS");

            if (caps.SupportsPolygonMode) {
                builder.Pass("Polygon mode",
                             "glPolygonMode GL_LINE/GL_POINT available via GL_NV/ANGLE_polygon_mode");
            } else {
                builder.Warn("Polygon mode",
                             "no GL_NV/ANGLE_polygon_mode; glPolygonMode GL_LINE/GL_POINT falls back to GL_FILL");
            }
            if (caps.SupportsIndexedColorMask) {
                builder.Pass("Indexed color mask",
                             "per-draw-buffer glColorMaski available (ES 3.2 core or draw_buffers_indexed)");
            } else {
                builder.Warn("Indexed color mask",
                             "no indexed glColorMaski; per-draw-buffer color masks fall back to draw buffer 0");
            }
            if (caps.SupportsDualSourceBlend) {
                builder.Pass("Dual-source blend",
                             "GL_SRC1_* dual-source blend factors available via GL_EXT_blend_func_extended");
            } else {
                builder.Warn("Dual-source blend",
                             "no GL_EXT_blend_func_extended; GL_SRC1_* dual-source blend factors hard-fail at draw");
            }

            if (es31) {
                GLint maxVertexSsboBlocks = 0;
                glesFuncs.glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &maxVertexSsboBlocks);
                while (glesFuncs.glGetError && glesFuncs.glGetError() != GL_NO_ERROR) {
                }
                if (maxVertexSsboBlocks >= 1) {
                    builder.Pass("Vertex shader storage blocks",
                                 format("GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS = {}", maxVertexSsboBlocks));
                } else {
                    builder.Warn("Vertex shader storage blocks",
                                 format("GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS = {}; the Flywheel/Create indirect draw "
                                        "machinery cannot read indirect command buffers from the vertex stage",
                                        maxVertexSsboBlocks));
                }

                if (caps.MaxShaderStorageBufferBindings >= 8) {
                    builder.Pass("Shader storage buffer bindings",
                                 format("GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS = {} (the last binding is reserved "
                                        "for mg_IndirectParams)",
                                        caps.MaxShaderStorageBufferBindings));
                } else {
                    builder.Warn("Shader storage buffer bindings",
                                 format("GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS = {} (< 8); reserving the last "
                                        "binding for mg_IndirectParams leaves little room for app SSBOs",
                                        caps.MaxShaderStorageBufferBindings));
                }
            }

            if (caps.SupportsPersistentMapping) {
                builder.Pass("GL_EXT_buffer_storage", "supported (persistent buffer mapping)");
            } else {
                builder.Info("GL_EXT_buffer_storage",
                             "not supported; no impact today: the frontend fully emulates persistent "
                             "mapping regardless of this extension");
            }
            if (caps.SupportsBaseInstance) {
                builder.Pass("GL_EXT_base_instance", "supported (native baseInstance draws)");
            } else {
                builder.Info("GL_EXT_base_instance",
                             "not supported; direct baseInstance draws are emulated by shifting the "
                             "instanced arrays' attribute offsets, and gl_BaseInstance by a uniform. "
                             "The one gap is an INDIRECT draw whose command carries a non-zero "
                             "baseInstance and is executed natively: its vertex fetch is not shifted");
            }
            // Both multi-draw rows gate on the capability flags, not the entry-point pointers:
            // eglGetProcAddress may hand back a non-NULL stub for these on drivers without the
            // extension (NVIDIA ES does, and its glMultiDrawElementsBaseVertexEXT stub silently
            // drops every draw), so the pointers prove nothing. Absence is INFO in both cases
            // because MobileGL falls back to an equivalent per-draw loop.
            if (caps.SupportsMultiDrawIndirect) {
                builder.Pass("Multi-draw indirect",
                             "glMultiDrawArrays/ElementsIndirectEXT available via GL_EXT_multi_draw_indirect");
            } else {
                builder.Info("Multi-draw indirect",
                             "GL_EXT_multi_draw_indirect not supported; no impact today: multi-draw "
                             "indirect is decomposed into per-command indirect draws regardless");
            }
            if (caps.SupportsMultiDrawElementsBaseVertex) {
                builder.Pass("Multi-draw base vertex",
                             "glMultiDrawElementsBaseVertexEXT available (EXT/OES_draw_elements_base_vertex "
                             "with GL_EXT_multi_draw_arrays); glMultiDrawElementsBaseVertex batches into one "
                             "driver call");
            } else {
                builder.Info("Multi-draw base vertex",
                             "glMultiDrawElementsBaseVertexEXT not supported (needs EXT/OES_"
                             "draw_elements_base_vertex plus GL_EXT_multi_draw_arrays); the batch "
                             "takes the next emulation tier instead, with identical output - see "
                             "\"Multi-draw elements tier\" below for the one that will run");
            }
            // glMultiDrawElements(BaseVertex) has no ES counterpart at all, so DirectGLES
            // emulates it; these rows say which emulation the driver leaves available and
            // which one will run. The two capabilities each tier leans on come first.
            if (caps.SupportsDrawElementsBaseVertex) {
                builder.Pass("Draw elements base vertex",
                             "glDrawElementsBaseVertex available (ES 3.2 core or EXT/OES_draw_elements_base_"
                             "vertex); a multi-draw batch can replay its sub-draws with their own base "
                             "vertices");
            } else {
                builder.Warn("Draw elements base vertex",
                             "glDrawElementsBaseVertex not supported (pre-ES 3.2 without EXT/OES_draw_"
                             "elements_base_vertex); every base-vertex draw has to be emulated by rewriting "
                             "the index stream on the CPU, which costs an upload per batch");
            }
            if (caps.SupportsComputeShader) {
                builder.Pass("Compute shaders",
                             "available (ES 3.1 core); the opt-in \"compute\" multi-draw tier can flatten a "
                             "whole batch into one draw");
            } else {
                builder.Info("Compute shaders",
                             "not available (pre-ES 3.1); no impact on the default multi-draw tiers, which "
                             "never use compute");
            }
            {
                // The same resolution the backend runs, over the capabilities probed here.
                // Like the Magma tier row, the preference comes from MG_Config::Features,
                // which is only populated once MobileGL::Initialize() has parsed the
                // environment - a POST executed standalone before that reports the
                // unclamped choice, so the row names the variable rather than implying it
                // was consulted.
                using MG_Backend::DirectGLES::MultiDrawImpl::ResolveTier;
                String resolution;
                ResolveTier(caps, glesFuncs, MG_Config::Features.EsprytMultiDrawMode, &resolution);
                builder.Info("Multi-draw elements tier",
                             "glMultiDrawElements(BaseVertex) emulation: " + resolution +
                                 "; override with MOBILEGL_ESPRYT_MULTIDRAW_MODE");
            }
            if (caps.SupportsTextureBorderClamp) {
                builder.Pass("Texture border clamp",
                             "supported (GL_TEXTURE_BORDER_COLOR reaches the driver, so "
                             "GL_CLAMP_TO_BORDER samples the colour the application set)");
            } else {
                builder.Warn("Texture border clamp",
                             "not supported (pre-ES 3.2 without GL_EXT/OES_texture_border_clamp); "
                             "GL_TEXTURE_BORDER_COLOR is not synced to the driver at all, so anything "
                             "sampling outside a GL_CLAMP_TO_BORDER texture reads the driver's default "
                             "border instead of the requested colour");
            }
            if (caps.SupportsTextureCubeMapArray) {
                builder.Pass("Texture cube map array",
                             "supported (GL_TEXTURE_CUBE_MAP_ARRAY textures get real storage and can be "
                             "attached to a framebuffer)");
            } else {
                builder.Warn("Texture cube map array",
                             "not supported (pre-ES 3.2 without GL_EXT/OES_texture_cube_map_array); a cube "
                             "map array texture gets no driver storage at all, so sampling one reads nothing "
                             "and rendering to one does not reach the screen");
            }
            // WARN, not FAIL, and the choice is deliberate. The consequence is severe - buffer
            // textures are CORE in OpenGL 3.1 and MobileGL advertises a 4.x context, so an
            // application may use one without asking, and nothing degrades gracefully: the
            // texture gets no driver storage, and every shader declaring a samplerBuffer fails
            // to compile outright, because SPIRV-Cross emits `#extension GL_EXT_texture_buffer :
            // require` for it below ESSL 320, so the program never links and every draw using it
            // silently draws nothing. That is how Minecraft 26.3, whose cloud layer is built
            // entirely from gl_VertexID plus texelFetch on a GL_R8I buffer texture, loses its
            // clouds. But FAIL means "this backend cannot run on this driver", and that is not
            // true: such a device runs everything that does not touch a buffer texture. It is
            // also exactly the shape of the "Texture cube map array" row above, which loses its
            // shaders to the same SPIRV-Cross `: require` mechanism and is a WARN - two adjacent
            // rows with one consequence must not carry two severities.
            // The limit is stated on every tier because it is the one number an application can
            // read, and on the None tier it is knowingly a fiction (see below).
            {
                using Tier = MG_External::GLESCapabilities::TextureBufferTier;
                const Int advertisedLimit = caps.MaxTextureBufferSize;
                // A supported tier that then refused GL_MAX_TEXTURE_BUFFER_SIZE is a driver bug;
                // the row must not call MobileGL's floor "the driver's own answer" there.
                const char* limitProvenance =
                    caps.MaxTextureBufferSizeIsDriverReported
                        ? "the driver's own answer"
                        : "MobileGL's floor - this driver claims buffer textures but rejected the query";
                switch (caps.TextureBufferSupport) {
                case Tier::CoreEs32:
                    builder.Pass("Buffer textures",
                                 format("core in ES 3.2; GL_MAX_TEXTURE_BUFFER_SIZE = {} is {}, and "
                                        "ESSL 320 needs no #extension directive to declare a "
                                        "samplerBuffer",
                                        advertisedLimit, limitProvenance));
                    break;
                case Tier::ExtensionEXT:
                    builder.Pass("Buffer textures",
                                 format("GL_EXT_texture_buffer; GL_MAX_TEXTURE_BUFFER_SIZE = {} is {}, "
                                        "and the directive SPIRV-Cross emits "
                                        "(GL_EXT_texture_buffer) is the one this driver wants",
                                        advertisedLimit, limitProvenance));
                    break;
                case Tier::ExtensionOES:
                    builder.Pass("Buffer textures",
                                 format("GL_OES_texture_buffer; GL_MAX_TEXTURE_BUFFER_SIZE = {} is {}. "
                                        "SPIRV-Cross hardcodes the EXT spelling, so MobileGL "
                                        "retargets the emitted #extension directive to the OES one "
                                        "this driver advertises",
                                        advertisedLimit, limitProvenance));
                    break;
                case Tier::None:
                default:
                    builder.Warn("Buffer textures",
                                 format("not supported (pre-ES 3.2 without GL_EXT/OES_texture_buffer); "
                                        "glTexBuffer does not exist, so a buffer texture gets no storage, "
                                        "and any shader declaring a samplerBuffer fails to compile and "
                                        "leaves its program unlinked - every draw using it is a silent "
                                        "no-op. MobileGL still reports GL_MAX_TEXTURE_BUFFER_SIZE = {}: "
                                        "the value is a floor it cannot honour, kept because an OpenGL "
                                        "4.x context may not answer 0 and GL has no way to say that a "
                                        "core feature is missing",
                                        advertisedLimit));
                    break;
                }
            }
            // Both rows are reported rather than probed: neither can come out any other way.
            // ESSL has no 64-bit float type at all, so no driver and no extension could change
            // either answer, and the rows exist so the two halves of the loss are named at
            // startup instead of discovered as a shader that will not compile or an
            // unexplained GL_INVALID_OPERATION at draw setup.
            builder.Pass("fp64", AppendFp64AdvertisementNote(
                                     "demoted to fp32 - ESSL has no 64-bit float type, so every double / "
                                     "dvec / dmat in a shader is narrowed to 32 bits before transpilation "
                                     "(DemoteFloat64Pass). Such shaders COMPILE AND RUN, at single "
                                     "precision; a block containing a double is re-laid-out for the "
                                     "narrowed members, so an application that hard-codes std140 offsets "
                                     "computed for doubles must query them instead"));
            builder.Warn("64-bit vertex attributes",
                         "not supported (ES has no GL_DOUBLE vertex format, and after the fp64 demotion "
                         "above there is no 64-bit shader input left to feed either); "
                         "glVertexAttribLFormat / glVertexArrayAttribLFormat report "
                         "GL_INVALID_OPERATION - feed the attribute with glVertexAttribPointer(GL_FLOAT), "
                         "which a demoted dvec input reads correctly");
            if (glesFuncs.glPatchParameteri != nullptr) {
                builder.Pass("Tessellation patch parameters",
                             "glPatchParameteri present (GL_PATCH_VERTICES reaches the driver)");
            } else {
                builder.Warn("Tessellation patch parameters",
                             "glPatchParameteri missing (pre-ES 3.2 without GL_EXT_tessellation_shader); "
                             "GL_PATCH_VERTICES stays at the driver default of 3 and a patch draw of any "
                             "other size renders nothing");
            }
            if (glesFuncs.glGenTransformFeedbacks != nullptr && glesFuncs.glBindTransformFeedback != nullptr &&
                glesFuncs.glPauseTransformFeedback != nullptr && glesFuncs.glResumeTransformFeedback != nullptr) {
                builder.Pass("Transform feedback objects",
                             "supported (each GL transform feedback object gets one of the driver's, so "
                             "several can hold a paused capture at once)");
            } else {
                builder.Warn("Transform feedback objects",
                             "entry points missing; every GL transform feedback object shares the driver's "
                             "default one, so a second object cannot open a capture while the first is paused");
            }
            if (caps.SupportsNorm16Texture) {
                builder.Pass("GL_EXT_texture_norm16", "supported");
            } else {
                builder.Warn("GL_EXT_texture_norm16",
                             "not supported; 16-bit normalized texture formats need emulation");
            }
            if (caps.SupportsRenderSnorm) {
                builder.Pass("GL_EXT_render_snorm",
                             "supported (signed-normalized formats are colour-renderable, so an "
                             "SNORM render target keeps its own encoding instead of a float substitute)");
            } else {
                builder.Warn("GL_EXT_render_snorm",
                             "not supported; signed-normalized formats are texture-only, so every SNORM "
                             "render target is stored as a float (GL_RGBA8_SNORM/GL_RGB8_SNORM -> "
                             "GL_RGBA16F) and its fragment outputs are clamped to [-1,1] in software");
            }
            // FAIL, not WARN: ES 3.x core makes every float format texture-only, and every Iris
            // shaderpack renders into at least GL_R11F_G11F_B10F (Complementary's colortex0, BSL's
            // colortex0). Without this extension there is no substitute format left - a half float
            // is not renderable either - so shaderpacks cannot work at all on such a driver.
            if (caps.SupportsColorBufferFloat) {
                builder.Pass("GL_EXT_color_buffer_float",
                             "supported (GL_R11F_G11F_B10F / GL_RGBA16F / GL_RGBA32F are "
                             "colour-renderable, which is what every shaderpack renders into)");
            } else if (caps.SupportsColorBufferHalfFloat) {
                builder.Warn("GL_EXT_color_buffer_float",
                             "not supported, but GL_EXT_color_buffer_half_float is; 16-bit float render "
                             "targets work, 32-bit float ones (GL_RGBA32F, and the GL_RGBA16 fallback "
                             "that lands on it) do not");
            } else {
                builder.Fail("GL_EXT_color_buffer_float",
                             "not supported, and neither is GL_EXT_color_buffer_half_float; no floating-point "
                             "format is colour-renderable on this driver, so no shaderpack can create its "
                             "render targets (Iris reports GL_FRAMEBUFFER_UNSUPPORTED and refuses to load)");
            }

            // INFO, never WARN: this is the HOST driver's ability to compile its own ESSL on
            // its own threads, and MobileGL's asynchronous compilation does not depend on it
            // in the slightest - the pool parallelises GLSL -> SPIR-V -> ESSL translation,
            // which is where a shaderpack load actually spends its time, and it does that on
            // a driver that has never heard of the extension. The row exists so that the day
            // the driver-side half is overlapped too, the POST already says which devices can.
            builder.Info("Driver GL_KHR_parallel_shader_compile",
                         caps.SupportsParallelShaderCompile
                             ? "supported; the device driver can also compile the translated ESSL off-thread"
                             : "not supported; the device driver compiles the translated ESSL on the calling "
                               "thread (MobileGL's own compile pool is unaffected)");

            builder.Info("Indirect gl_InstanceID semantics",
                         caps.IndirectDrawInstanceIdIncludesBaseInstance
                             ? "includes baseInstance (ANGLE-style; MobileGL's shader rewrite keeps gl_InstanceID "
                               "zero-based)"
                             : "conforming (zero-based)");

            builder.DriverReported("Backend driver reported GL_VENDOR", caps.GLESVendorString);
            builder.DriverReported("Backend driver reported GL_RENDERER", caps.GLESRendererString);
            builder.DriverReported("Backend driver reported GL_VERSION", caps.GLESVersionString);
        }

        // Single "Timer queries" row: GL_EXT_disjoint_timer_query presence and a real
        // GL_TIME_ELAPSED_EXT span around a trivial workload on the probe context fold
        // into one combined verdict (WARN when absent, PASS when the probe works, FAIL
        // naming the step that broke). Requires the probe context to still be current.
        void ProbeGlesTimerQuery(ReportBuilder& builder, const MG_External::GLESCapabilities& caps,
                                 const MG_External::GLESFunctionsTable& glesFuncs) {
            const String disabledNote = TimerQueryDisabledNote();
            if (!caps.SupportsDisjointTimerQuery) {
                builder.Warn("Timer queries",
                             "GL_EXT_disjoint_timer_query not supported; timer queries unavailable; "
                             "Minecraft F3 GPU% will not show" +
                                 disabledNote);
                return;
            }
            // Every emit carries the extension-presence fact the old standalone
            // GL_EXT_disjoint_timer_query row showed, plus the probe outcome.
            const String extensionPresent = "GL_EXT_disjoint_timer_query extension present";
            const auto fail = [&](const String& detail) {
                builder.Fail("Timer queries", extensionPresent + "; but " + detail + disabledNote);
            };

            if (!glesFuncs.glGenQueries || !glesFuncs.glDeleteQueries || !glesFuncs.glBeginQuery ||
                !glesFuncs.glEndQuery || !glesFuncs.glGetQueryObjectuiv || !glesFuncs.glGetQueryObjectui64vEXT ||
                !glesFuncs.glClearColor || !glesFuncs.glClear || !glesFuncs.glFlush || !glesFuncs.glFinish ||
                !glesFuncs.glGetError) {
                fail("the query entry points did not resolve through eglGetProcAddress");
                return;
            }

            // Drain stale errors so probe failures are attributable to the probe itself.
            while (glesFuncs.glGetError() != GL_NO_ERROR) {
            }

            GLuint queryId = 0;
            glesFuncs.glGenQueries(1, &queryId);
            if (queryId == 0) {
                fail("glGenQueries did not return a query object");
                return;
            }
            const ScopeGuard deleteQuery([&]() { glesFuncs.glDeleteQueries(1, &queryId); });

            glesFuncs.glBeginQuery(GL_TIME_ELAPSED_EXT, queryId);
            // Trivial workload inside the span: clear the 1x1 probe pbuffer and flush.
            glesFuncs.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glesFuncs.glClear(GL_COLOR_BUFFER_BIT);
            glesFuncs.glFlush();
            glesFuncs.glEndQuery(GL_TIME_ELAPSED_EXT);
            glesFuncs.glFinish();

            const GLenum spanError = glesFuncs.glGetError();
            if (spanError != GL_NO_ERROR) {
                fail(format("GL error 0x{:x} while recording the GL_TIME_ELAPSED_EXT span", spanError));
                return;
            }

            // glFinish already drained the GPU, so a conforming driver reports the
            // result available immediately; the bounded loop only covers drivers
            // that latch availability lazily. Paced at ~100us per poll to match
            // the runtime GetQueryResult64 wait loop, bounding the worst case
            // at ~100ms so a broken driver cannot stall the POST.
            GLuint available = 0;
            for (Int attempt = 0; attempt < 1000 && available == 0; ++attempt) {
                glesFuncs.glGetQueryObjectuiv(queryId, GL_QUERY_RESULT_AVAILABLE, &available);
                if (available == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }
            if (available == 0) {
                fail("GL_QUERY_RESULT_AVAILABLE never became true after glFinish "
                     "(1000 polls over ~100ms)");
                return;
            }

            GLuint64 elapsedNs = 0;
            glesFuncs.glGetQueryObjectui64vEXT(queryId, GL_QUERY_RESULT, &elapsedNs);
            const GLenum resultError = glesFuncs.glGetError();
            if (resultError != GL_NO_ERROR) {
                fail(format("GL error 0x{:x} while reading GL_QUERY_RESULT", resultError));
                return;
            }
            builder.Pass("Timer queries",
                         extensionPresent + format("; timer query functional (probe observed {} ns)", elapsedNs) +
                             disabledNote);
        }

        // Compiles + links a two-stage program on the probe context. Returns 0 on failure and writes a
        // human-readable reason into |detail|.
        GLuint CompileLinkProgram(const MG_External::GLESFunctionsTable& g, const char* vs, const char* fs,
                                  String& detail) {
            const auto compile = [&](GLenum stage, const char* src, GLuint& out) -> bool {
                out = g.glCreateShader(stage);
                if (out == 0) {
                    detail = "glCreateShader returned 0";
                    return false;
                }
                g.glShaderSource(out, 1, &src, nullptr);
                g.glCompileShader(out);
                GLint ok = GL_FALSE;
                g.glGetShaderiv(out, GL_COMPILE_STATUS, &ok);
                if (ok != GL_TRUE) {
                    GLchar log[512] = {};
                    GLsizei len = 0;
                    g.glGetShaderInfoLog(out, static_cast<GLsizei>(sizeof(log) - 1), &len, log);
                    detail = format("{} shader compile failed: {}",
                                    stage == GL_VERTEX_SHADER ? "vertex" : "fragment",
                                    len > 0 ? log : "(no info log)");
                    return false;
                }
                return true;
            };
            GLuint v = 0, f = 0;
            const ScopeGuard delV([&]() { if (v) g.glDeleteShader(v); });
            const ScopeGuard delF([&]() { if (f) g.glDeleteShader(f); });
            if (!compile(GL_VERTEX_SHADER, vs, v) || !compile(GL_FRAGMENT_SHADER, fs, f)) {
                return 0;
            }
            const GLuint prog = g.glCreateProgram();
            if (prog == 0) {
                detail = "glCreateProgram returned 0";
                return 0;
            }
            g.glAttachShader(prog, v);
            g.glAttachShader(prog, f);
            g.glLinkProgram(prog);
            GLint linked = GL_FALSE;
            g.glGetProgramiv(prog, GL_LINK_STATUS, &linked);
            if (linked != GL_TRUE) {
                detail = "program link failed";
                g.glDeleteProgram(prog);
                return 0;
            }
            return prog;
        }

        // "noperspective interpolation" row - a real correctness render, not just a compile. A viewport-
        // filling quad is drawn with strong perspective (left clip-w 1, right clip-w 8) and a varying that
        // runs 0..1 across it. At the screen centre screen-linear interpolation gives 0.5 while perspective-
        // correct gives 1/(w+1) ~= 0.11, so reading the centre texel tells the two apart. The varying is
        // carried either through the native `noperspective` qualifier (extension present) or through the
        // exact gl_Position.w / gl_FragCoord.w rewrite MobileGL applies when it is absent. Verdict:
        //   PASS  - extension present and the native noperspective result is screen-linear;
        //   WARN  - extension absent but the gl_Position.w/gl_FragCoord.w emulation renders screen-linear
        //           (correct, just the fallback path shipping shader packs hit on such devices);
        //   FAIL  - either path renders perspective-correct / wrong (noperspective does not actually work),
        //           or the program will not compile/link, or the render errors.
        // Requires the probe context to still be current.
        void ProbeGlesNoperspective(ReportBuilder& builder, const MG_External::GLESCapabilities& caps,
                                    const MG_External::GLESFunctionsTable& g) {
            const Bool native = caps.SupportsNoperspectiveInterpolation;
            const String pathNote = native ? "GL_NV_shader_noperspective_interpolation present (native path)"
                                           : "GL_NV_shader_noperspective_interpolation absent (gl_Position.w / "
                                             "gl_FragCoord.w emulation path)";
            const auto fail = [&](const String& detail) {
                builder.Fail("noperspective interpolation", pathNote + "; " + detail);
            };

            if (!g.glCreateShader || !g.glShaderSource || !g.glCompileShader || !g.glGetShaderiv ||
                !g.glGetShaderInfoLog || !g.glDeleteShader || !g.glCreateProgram || !g.glAttachShader ||
                !g.glLinkProgram || !g.glGetProgramiv || !g.glUseProgram || !g.glDeleteProgram ||
                !g.glGenFramebuffers || !g.glBindFramebuffer || !g.glDeleteFramebuffers ||
                !g.glGenRenderbuffers || !g.glBindRenderbuffer || !g.glRenderbufferStorage ||
                !g.glFramebufferRenderbuffer || !g.glDeleteRenderbuffers || !g.glCheckFramebufferStatus ||
                !g.glGenBuffers || !g.glBindBuffer || !g.glBufferData || !g.glDeleteBuffers ||
                !g.glGetAttribLocation || !g.glVertexAttribPointer || !g.glEnableVertexAttribArray ||
                !g.glViewport || !g.glClearColor || !g.glClear || !g.glDrawArrays || !g.glReadPixels ||
                !g.glFinish || !g.glGetError) {
                fail("the render entry points did not resolve through eglGetProcAddress");
                return;
            }

            // Match MobileGL's own ESSL target (the device's version). At #version 300 es some drivers
            // (Adreno) still treat `noperspective` as reserved even with the extension enabled; the ES 3.2
            // form the backend actually emits compiles. Emulated shaders are version-agnostic but use the
            // same header for consistency.
            const Int esslVer = caps.GLESVersion.Major * 100 + caps.GLESVersion.Minor * 10;
            const String header = format("#version {} es\n", esslVer >= 300 ? esslVer : 300);
            static const char* const kVsNativeBody =
                "#extension GL_NV_shader_noperspective_interpolation : require\n"
                "in vec4 a_pos;\n"
                "in float a_v;\n"
                "noperspective out highp float v_out;\n"
                "void main() { gl_Position = a_pos; v_out = a_v; }\n";
            static const char* const kFsNativeBody =
                "#extension GL_NV_shader_noperspective_interpolation : require\n"
                "precision highp float;\n"
                "noperspective in highp float v_out;\n"
                "out vec4 fragColor;\n"
                "void main() { fragColor = vec4(v_out, 0.0, 0.0, 1.0); }\n";
            // Exactly MobileGL's emulation (verified against EmulateNoPerspectivePass output): pre-multiply
            // the varying by clip-w in the vertex stage, recover with gl_FragCoord.w in the fragment stage,
            // no noperspective qualifier (so the driver interpolates it perspective-correct).
            static const char* const kVsEmuBody =
                "in vec4 a_pos;\n"
                "in float a_v;\n"
                "out highp float v_out;\n"
                "void main() { gl_Position = a_pos; v_out = a_v * gl_Position.w; }\n";
            static const char* const kFsEmuBody =
                "precision highp float;\n"
                "in highp float v_out;\n"
                "out vec4 fragColor;\n"
                "void main() { fragColor = vec4(v_out * gl_FragCoord.w, 0.0, 0.0, 1.0); }\n";

            while (g.glGetError() != GL_NO_ERROR) {
            }

            const String vsSrc = header + (native ? kVsNativeBody : kVsEmuBody);
            const String fsSrc = header + (native ? kFsNativeBody : kFsEmuBody);
            String linkDetail;
            const GLuint prog = CompileLinkProgram(g, vsSrc.c_str(), fsSrc.c_str(), linkDetail);
            if (prog == 0) {
                fail(native ? "a noperspective program failed to build though the extension is advertised: " +
                                  linkDetail
                            : "the emulation program failed to build: " + linkDetail);
                return;
            }
            const ScopeGuard delProg([&]() { g.glDeleteProgram(prog); });

            // 9x9 so the centre texel (4,4) sits exactly at NDC (0,0).
            constexpr GLsizei kDim = 9;
            GLuint rbo = 0, fbo = 0, vbo = 0;
            g.glGenRenderbuffers(1, &rbo);
            const ScopeGuard delRbo([&]() { if (rbo) g.glDeleteRenderbuffers(1, &rbo); });
            g.glBindRenderbuffer(GL_RENDERBUFFER, rbo);
            g.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kDim, kDim);
            g.glGenFramebuffers(1, &fbo);
            const ScopeGuard delFbo([&]() {
                if (fbo) {
                    g.glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    g.glDeleteFramebuffers(1, &fbo);
                }
            });
            g.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            g.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
            if (g.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                fail("the probe framebuffer is incomplete");
                return;
            }

            // Interleaved [vec4 clip-pos, float v]. Left w=1, right w=8; x/y pre-multiplied by w so the quad
            // still fills NDC after the perspective divide.
            const GLfloat verts[] = {
                -1.f, -1.f, 0.f, 1.f, 0.f, //
                8.f,  -8.f, 0.f, 8.f, 1.f, //
                -1.f, 1.f,  0.f, 1.f, 0.f, //
                8.f,  8.f,  0.f, 8.f, 1.f, //
            };
            g.glGenBuffers(1, &vbo);
            const ScopeGuard delVbo([&]() { if (vbo) g.glDeleteBuffers(1, &vbo); });
            g.glBindBuffer(GL_ARRAY_BUFFER, vbo);
            g.glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

            g.glUseProgram(prog);
            const GLint posLoc = g.glGetAttribLocation(prog, "a_pos");
            const GLint vLoc = g.glGetAttribLocation(prog, "a_v");
            if (posLoc < 0 || vLoc < 0) {
                fail("the probe vertex attributes did not resolve");
                return;
            }
            g.glEnableVertexAttribArray(static_cast<GLuint>(posLoc));
            g.glVertexAttribPointer(static_cast<GLuint>(posLoc), 4, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
                                    reinterpret_cast<const void*>(0));
            g.glEnableVertexAttribArray(static_cast<GLuint>(vLoc));
            g.glVertexAttribPointer(static_cast<GLuint>(vLoc), 1, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
                                    reinterpret_cast<const void*>(4 * sizeof(GLfloat)));

            g.glViewport(0, 0, kDim, kDim);
            g.glClearColor(0.f, 0.f, 0.f, 1.f);
            g.glClear(GL_COLOR_BUFFER_BIT);
            g.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            g.glFinish();

            const GLenum drawError = g.glGetError();
            if (drawError != GL_NO_ERROR) {
                fail(format("GL error 0x{:x} while rendering the probe quad", drawError));
                return;
            }

            GLubyte center[4] = {};
            g.glReadPixels(kDim / 2, kDim / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
            const GLenum readError = g.glGetError();
            if (readError != GL_NO_ERROR) {
                fail(format("GL error 0x{:x} while reading the probe pixel back", readError));
                return;
            }

            // At the centre: screen-linear -> 0.5 (~128); perspective-correct -> 1/(8+1) ~= 0.111 (~28).
            const float observed = static_cast<float>(center[0]) / 255.0f;
            const int observedByte = center[0];
            constexpr float kScreenLinear = 0.5f;
            const bool screenLinear = observed > 0.5f * (kScreenLinear + 1.0f / 9.0f); // midpoint ~= 0.306
            if (!screenLinear) {
                fail(format("the centre texel read {} (~{:.3f}); expected the screen-linear ~0.5 - "
                            "interpolation came out perspective-correct, so noperspective does not work here",
                            observedByte, observed));
                return;
            }
            if (native) {
                builder.Pass("noperspective interpolation",
                             pathNote + format("; native noperspective renders screen-linear (centre {} ~= 0.5)",
                                               observedByte));
            } else {
                builder.Warn("noperspective interpolation",
                             pathNote +
                                 format("; the emulation renders screen-linear correctly (centre {} ~= 0.5), "
                                        "but this is the fallback path with less driver coverage",
                                        observedByte));
            }
        }

        // No real ES driver renders to a three-channel image, but desktop GL applications ask for
        // one constantly - Complementary Reimagined's colortex1 is GL_RGB8_SNORM and its colortex2
        // is GL_RGB16F, and Iris refuses to load when a framebuffer built from them is not
        // COMPLETE. DirectGLES substitutes the four-channel sibling, and this row names the
        // outcome per format so the failure mode is a five-second read instead of an
        // investigation. Answered from the capability cache that was just probed on this very
        // driver, so it costs no extra GL work.
        void ReportThreeChannelColorAttachments(ReportBuilder& builder, const MG_External::GLESCapabilities& caps,
                                                const MG_Backend::FormatCapabilityCache& cache) {
            // GL_RGB8 is the control: it is ES-core renderable, and it is exactly why BSL loads on
            // the same driver where Complementary does not. The rest are one representative of
            // each widening class - signed-normalized, half float, 32-bit float, sRGB, integer -
            // so the row says which CLASS of shaderpack target a device cannot serve rather than
            // just "three-channel formats".
            constexpr TextureInternalFormat kProbedFormats[] = {
                TextureInternalFormat::RGB8,   TextureInternalFormat::RGB8Snorm, TextureInternalFormat::RGB16F,
                TextureInternalFormat::RGB32F, TextureInternalFormat::SRGB8,     TextureInternalFormat::RGB8UI};
            const SizeT targetIndex = MG_Backend::GetFormatCapabilityTargetIndex(TextureTarget::Texture2D);
            const Flags<PixelFormatNormalizeOptionBit> renderTargetOptions =
                MG_Backend::DirectGLES::TextureImpl::GetRenderTargetNormalizeOptions(caps, targetIndex);

            String nativeList;
            String widenedList;
            String unusableList;
            // GL_RGB8 is colour-renderable in ES 3.0 CORE. A driver that answers no to it is
            // broken (or the probe itself is), and that is the ONLY three-channel verdict that
            // deserves a FAIL on its own - see the verdict block below.
            Bool controlFormatBroken = false;
            const auto append = [](String& list, const String& entry) {
                if (!list.empty()) list += ", ";
                list += entry;
            };

            for (const TextureInternalFormat probedFormat : kProbedFormats) {
                const SizeT formatIndex = static_cast<SizeT>(probedFormat);
                const String name = MG_Util::ConvertTextureInternalFormatToString(probedFormat);
                if (MG_Backend::HasFormatCapability(cache.FullCaps[targetIndex][formatIndex],
                                                    MG_Backend::FormatCapability::FramebufferRenderable)) {
                    append(nativeList, name);
                    continue;
                }
                if (probedFormat == TextureInternalFormat::RGB8) {
                    controlFormatBroken = true;
                }
                if (MG_Backend::HasFormatCapability(cache.CaveatCaps[targetIndex][formatIndex],
                                                    MG_Backend::FormatCapability::FramebufferRenderable)) {
                    GLenum widenedInternalFormat = GL_UNKNOWN_MGL;
                    MG_Util::TextureFormatProcessor::NormalizePixelFormat(
                        MG_Util::ConvertTextureInternalFormatToGLEnum(probedFormat), renderTargetOptions,
                        &widenedInternalFormat, nullptr, nullptr);
                    append(widenedList, name + " -> " + MG_Util::ConvertGLEnumToString(widenedInternalFormat));
                    continue;
                }
                append(unusableList, name);
            }

            String detail;
            if (!nativeList.empty()) detail += "renderable natively: " + nativeList;
            if (!widenedList.empty()) {
                if (!detail.empty()) detail += "; ";
                detail += "widened to stay renderable: " + widenedList;
            }
            if (!unusableList.empty()) {
                if (!detail.empty()) detail += "; ";
                detail += "NOT renderable and not substitutable: " + unusableList;
            }

            // The verdict deliberately does NOT track "every probed format came out usable".
            //
            // GL_RGB32F widens to GL_RGBA32F, and GL_RGBA32F is colour-renderable only under
            // GL_EXT_color_buffer_float. A perfectly healthy half-float-only driver (the common
            // mobile shape: EXT_color_buffer_half_float and nothing more) therefore reports
            // GL_RGB32F as unusable while every format a shaderpack actually renders into works.
            // FAILing that device would make the POST's hardest verdict fire on a configuration
            // MobileGL runs fine on, which is exactly how a report stops being read.
            //
            // So FAIL is reserved for the two answers that really are broken:
            //   * the ES-core control (GL_RGB8) is not renderable - the probe or the driver is
            //     wrong about something much more basic than three-channel widening; and
            //   * a widenable format has no usable fallback ON A DRIVER THAT ADVERTISES
            //     GL_EXT_color_buffer_float - the extension promises the widened float targets
            //     are renderable, so a gap here is a real, unexplained refusal.
            // Everything else is a WARN carrying the exact per-format status, which is what the
            // row is for. The "no float render targets at all" case is already a FAIL of its own
            // on the GL_EXT_color_buffer_float row above; repeating it here would only double-count.
            if (controlFormatBroken) {
                builder.Fail("Three-channel colour attachments",
                             detail + " - GL_RGB8 is colour-renderable in OpenGL ES 3.0 core, so a driver "
                                      "that refuses it cannot render to ANY three-channel attachment and the "
                                      "capability probe itself is suspect");
            } else if (!unusableList.empty() && caps.SupportsColorBufferFloat) {
                builder.Fail("Three-channel colour attachments",
                             detail + " - GL_EXT_color_buffer_float is supported, so the widened "
                                      "four-channel float targets are required to be renderable; a framebuffer "
                                      "using one of the formats above still reports GL_FRAMEBUFFER_UNSUPPORTED, "
                                      "which Iris turns into a hard load failure");
            } else if (!unusableList.empty()) {
                builder.Warn("Three-channel colour attachments",
                             detail + " - without GL_EXT_color_buffer_float the 32-bit float widening has no "
                                      "renderable target left, so a shaderpack asking for one of the formats "
                                      "above gets GL_FRAMEBUFFER_UNSUPPORTED; the half-float and fixed-point "
                                      "ones above still work");
            } else if (!widenedList.empty()) {
                builder.Warn("Three-channel colour attachments",
                             detail + " - the substitution costs the extra alpha channel's memory and is "
                                      "hidden from the application by an ALPHA->ONE swizzle");
            } else {
                builder.Pass("Three-channel colour attachments", detail);
            }
        }

        // Everything the "MobileGL reported ..." rows need from the GLES device probe.
        struct GlesProbeSummary {
            Bool capsValid = false;
            MG_External::GLESCapabilities caps{};
        };
    } // namespace

    // The GLES device probe proper. Split out of RunGlesDriverPost so that the
    // "MobileGL reported ..." rows are appended on every path (including early
    // probe failures) before the report is finalized.
    //
    // The whole EGL bring-up chain (library load, display init, API bind, config,
    // pbuffer surface, context) is one "ES3 context" row. The detail accumulates one
    // completed-stage description per stage so no sub-fact of the old per-stage rows
    // is lost: PASS enumerates every stage's result, FAIL lists the stages that
    // completed and then names the exact stage that broke with its detail string.
    static void ProbeGlesDriver(ReportBuilder& builder, GlesProbeSummary& summary) {
        String chain;
        const auto stageDone = [&](const String& description) {
            if (!chain.empty()) {
                chain += "; ";
            }
            chain += description;
        };
        const auto failStage = [&](const String& stage, const String& detail) {
            builder.Fail("ES3 context", (chain.empty() ? "" : chain + "; but ") + stage + ": " + detail);
        };

        MG_External::EGLFunctionsTable eglFuncs{};
        BackendLoader::AcquireEGLFunctions(eglFuncs);
        const Bool eglLoaded = eglFuncs.eglGetDisplay && eglFuncs.eglInitialize && eglFuncs.eglBindAPI &&
                               eglFuncs.eglChooseConfig && eglFuncs.eglCreatePbufferSurface &&
                               eglFuncs.eglCreateContext && eglFuncs.eglMakeCurrent && eglFuncs.eglDestroySurface &&
                               eglFuncs.eglDestroyContext && eglFuncs.eglTerminate && eglFuncs.eglGetProcAddress;
        if (!eglLoaded) {
            failStage("EGL library", "libEGL.so or one of its required entry points is missing");
            return;
        }
        stageDone("libEGL.so loaded with all required entry points");

        EGLDisplay display = eglFuncs.eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display == EGL_NO_DISPLAY) {
            failStage("EGL display", "eglGetDisplay returned EGL_NO_DISPLAY");
            return;
        }
        EGLint eglMajor = 0;
        EGLint eglMinor = 0;
        if (!eglFuncs.eglInitialize(display, &eglMajor, &eglMinor)) {
            failStage("EGL display", "eglInitialize failed on the default display" + EGLErrorSuffix(eglFuncs));
            return;
        }
        stageDone(format("EGL {}.{} initialized on the default display", eglMajor, eglMinor));
        builder.report.available = true;

        EGLSurface surface = EGL_NO_SURFACE;
        EGLContext context = EGL_NO_CONTEXT;
        const ScopeGuard eglTeardown([&]() {
            eglFuncs.eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (surface != EGL_NO_SURFACE) {
                eglFuncs.eglDestroySurface(display, surface);
            }
            if (context != EGL_NO_CONTEXT) {
                eglFuncs.eglDestroyContext(display, context);
            }
            // eglTerminate is deliberately not called: the probe shares EGL_DEFAULT_DISPLAY with
            // the process UI renderer (HWUI), and terminating it can invalidate the UI's EGL
            // objects on pre-refcounting Android builds. Unbinding and destroying our own
            // surface/context is sufficient cleanup.
        });
        do {
            if (!eglFuncs.eglBindAPI(EGL_OPENGL_ES_API)) {
                failStage("OpenGL ES API bind", "eglBindAPI(EGL_OPENGL_ES_API) failed" + EGLErrorSuffix(eglFuncs));
                break;
            }
            stageDone("eglBindAPI(EGL_OPENGL_ES_API) succeeded");

            const EGLint configAttribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                            EGL_RED_SIZE,     8,               EGL_GREEN_SIZE,      8,
                                            EGL_BLUE_SIZE,    8,               EGL_ALPHA_SIZE,      8,
                                            EGL_NONE};
            EGLConfig config = nullptr;
            EGLint numConfigs = 0;
            if (!eglFuncs.eglChooseConfig(display, configAttribs, &config, 1, &numConfigs)) {
                failStage("ES3 RGBA8888 pbuffer config", "eglChooseConfig failed" + EGLErrorSuffix(eglFuncs));
                break;
            }
            if (numConfigs < 1) {
                // No EGL error suffix here: eglChooseConfig succeeded, so it would read EGL_SUCCESS.
                failStage("ES3 RGBA8888 pbuffer config", "no ES3-capable RGBA8888 pbuffer config");
                break;
            }
            stageDone("ES3-renderable RGBA8888 pbuffer config found");

            const EGLint surfaceAttribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
            surface = eglFuncs.eglCreatePbufferSurface(display, config, surfaceAttribs);
            if (surface == EGL_NO_SURFACE) {
                failStage("1x1 pbuffer surface", "eglCreatePbufferSurface failed" + EGLErrorSuffix(eglFuncs));
                break;
            }
            stageDone("1x1 probe surface created");

            const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
            context = eglFuncs.eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
            if (context == EGL_NO_CONTEXT) {
                failStage("OpenGL ES 3 context", "eglCreateContext failed" + EGLErrorSuffix(eglFuncs));
                break;
            }
            if (!eglFuncs.eglMakeCurrent(display, surface, surface, context)) {
                failStage("OpenGL ES 3 context", "eglMakeCurrent failed" + EGLErrorSuffix(eglFuncs));
                break;
            }
            stageDone("ES 3 context created and made current");
            builder.Pass("ES3 context", chain);

            MG_External::GLESFunctionsTable glesFuncs{};
            BackendLoader::AcquireGLESFunctions(glesFuncs, eglFuncs.eglGetProcAddress);
            if (!BackendLoader::FillInGLESCapabilities(summary.caps, glesFuncs)) {
                builder.Fail("GLES capability query",
                             "required GLES entry points could not be resolved through eglGetProcAddress");
                break;
            }
            summary.capsValid = true;
            const MG_External::GLESCapabilities& caps = summary.caps;
            builder.report.rendererInfo = format("{} ({})", caps.GLESRendererString, caps.GLESVersionString);
            EvaluateGlesChecklist(builder, caps, glesFuncs);
            ProbeGlesTimerQuery(builder, caps, glesFuncs);
            ProbeGlesNoperspective(builder, caps, glesFuncs);
            builder.report.formatCapabilities.emplace();
            MG_Backend::DirectGLES::PopulateFormatCapabilities(
                glesFuncs, caps, builder.report.formatCapabilities.value());
            ReportThreeChannelColorAttachments(builder, caps, builder.report.formatCapabilities.value());
        } while (false);
    }

    BackendPostReport RunGlesDriverPost() {
        MGLOG_I("Driver POST: probing the device GLES driver");
        ReportBuilder builder;
        GlesProbeSummary summary;
        ProbeGlesDriver(builder, summary);

        // "MobileGL reported ..." rows: what applications running on the DirectGLES
        // backend (Espryt) would see. The backend API version string and the extension
        // list are built from the probe's own capability data through the same helpers
        // the real backend uses, so they cannot drift.
        Optional<String> backendApiVersionString;
        Optional<String> advertisedExtensions;
        if (summary.capsValid) {
            backendApiVersionString = MG_Backend::DirectGLES::FormatBackendAPIVersionString(
                summary.caps.GLESRendererString, summary.caps.GLESVersion.Major, summary.caps.GLESVersion.Minor);
            advertisedExtensions = JoinAdvertisedExtensions(MG_Backend::DirectGLES::BuildAdvertisedExtensions(
                summary.caps.SupportsDisjointTimerQuery, summary.caps.SupportsTextureFilterAnisotropy));
        }
        AppendMobileGLReportedRows(builder, MG_Backend::DirectGLES::GetRendererIdentity(), backendApiVersionString,
                                   advertisedExtensions);

        builder.Finalize();
        MGLOG_I("Driver POST: GLES verdict = %s", builder.report.verdict.c_str());
        return builder.report;
    }

    namespace {
        // The Vulkan loader is bootstrapped through dlopen + vkGetInstanceProcAddr instead of
        // static linking so the POST also works in build configurations that do not link a
        // Vulkan loader (and degrades gracefully when the device ships none). The library
        // handle is intentionally never closed: Android Vulkan ICDs may register threads and
        // state that do not survive unloading, and the loader stays resident for the real
        // backend anyway.
        void* OpenVulkanLoaderLibrary() {
#if defined(_WIN32)
            return reinterpret_cast<void*>(LoadLibraryA("vulkan-1.dll"));
#else
            static const char* const LoaderNames[] = {
#if defined(__APPLE__)
                "libvulkan.dylib",
                "libvulkan.1.dylib",
                "libMoltenVK.dylib",
#else
                "libvulkan.so.1",
                "libvulkan.so",
#endif
            };
            for (const char* name : LoaderNames) {
                if (void* library = dlopen(name, RTLD_LOCAL | RTLD_NOW)) {
                    MGLOG_I("Driver POST: loaded Vulkan loader library: %s", name);
                    return library;
                }
            }
            return nullptr;
#endif
        }

        void* VulkanLoaderSymbol(void* library, const char* name) {
#if defined(_WIN32)
            return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(library), name));
#else
            return dlsym(library, name);
#endif
        }

        Bool HasVkExtension(const Vector<VkExtensionProperties>& extensions, const char* name) {
            return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties& extension) {
                return std::strcmp(extension.extensionName, name) == 0;
            });
        }

        String VkApiVersionToString(Uint32 version) {
            return format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                          VK_VERSION_PATCH(version));
        }

        // Real timestamp-query probe, emitting the backend's single "Timer queries" row:
        // a throwaway logical device records two vkCmdWriteTimestamp(BOTTOM_OF_PIPE)
        // queries and reads them back. Both outcomes state the validBits and period
        // values (the facts of the old standalone rows): PASS adds the observed span,
        // FAIL names the step (and VkResult) that broke. Every created object is torn
        // down from a scope guard before the caller's instance guard runs.
        void ProbeVulkanTimerQuery(ReportBuilder& builder, PFN_vkGetInstanceProcAddr getInstanceProcAddr,
                                   VkInstance instance, VkPhysicalDevice physicalDevice,
                                   Uint32 graphicsQueueFamilyIndex, Uint32 timestampValidBits,
                                   Float timestampPeriod) {
            const String disabledNote = TimerQueryDisabledNote();
            const String timestampFacts =
                format("timestampValidBits = {} on the graphics queue family; timestampPeriod = {} ns per tick",
                       timestampValidBits, timestampPeriod);
            const auto fail = [&](const String& detail) {
                builder.Fail("Timer queries", timestampFacts + "; but " + detail + disabledNote);
            };
            const auto vkCreateDeviceFn =
                reinterpret_cast<PFN_vkCreateDevice>(getInstanceProcAddr(instance, "vkCreateDevice"));
            const auto vkDestroyDeviceFn =
                reinterpret_cast<PFN_vkDestroyDevice>(getInstanceProcAddr(instance, "vkDestroyDevice"));
            const auto vkGetDeviceQueueFn =
                reinterpret_cast<PFN_vkGetDeviceQueue>(getInstanceProcAddr(instance, "vkGetDeviceQueue"));
            const auto vkCreateCommandPoolFn =
                reinterpret_cast<PFN_vkCreateCommandPool>(getInstanceProcAddr(instance, "vkCreateCommandPool"));
            const auto vkDestroyCommandPoolFn =
                reinterpret_cast<PFN_vkDestroyCommandPool>(getInstanceProcAddr(instance, "vkDestroyCommandPool"));
            const auto vkAllocateCommandBuffersFn = reinterpret_cast<PFN_vkAllocateCommandBuffers>(
                getInstanceProcAddr(instance, "vkAllocateCommandBuffers"));
            const auto vkBeginCommandBufferFn =
                reinterpret_cast<PFN_vkBeginCommandBuffer>(getInstanceProcAddr(instance, "vkBeginCommandBuffer"));
            const auto vkEndCommandBufferFn =
                reinterpret_cast<PFN_vkEndCommandBuffer>(getInstanceProcAddr(instance, "vkEndCommandBuffer"));
            const auto vkCreateQueryPoolFn =
                reinterpret_cast<PFN_vkCreateQueryPool>(getInstanceProcAddr(instance, "vkCreateQueryPool"));
            const auto vkDestroyQueryPoolFn =
                reinterpret_cast<PFN_vkDestroyQueryPool>(getInstanceProcAddr(instance, "vkDestroyQueryPool"));
            const auto vkCmdResetQueryPoolFn =
                reinterpret_cast<PFN_vkCmdResetQueryPool>(getInstanceProcAddr(instance, "vkCmdResetQueryPool"));
            const auto vkCmdWriteTimestampFn =
                reinterpret_cast<PFN_vkCmdWriteTimestamp>(getInstanceProcAddr(instance, "vkCmdWriteTimestamp"));
            const auto vkCreateFenceFn =
                reinterpret_cast<PFN_vkCreateFence>(getInstanceProcAddr(instance, "vkCreateFence"));
            const auto vkDestroyFenceFn =
                reinterpret_cast<PFN_vkDestroyFence>(getInstanceProcAddr(instance, "vkDestroyFence"));
            const auto vkWaitForFencesFn =
                reinterpret_cast<PFN_vkWaitForFences>(getInstanceProcAddr(instance, "vkWaitForFences"));
            const auto vkQueueSubmitFn =
                reinterpret_cast<PFN_vkQueueSubmit>(getInstanceProcAddr(instance, "vkQueueSubmit"));
            const auto vkGetQueryPoolResultsFn = reinterpret_cast<PFN_vkGetQueryPoolResults>(
                getInstanceProcAddr(instance, "vkGetQueryPoolResults"));
            const auto vkDeviceWaitIdleFn =
                reinterpret_cast<PFN_vkDeviceWaitIdle>(getInstanceProcAddr(instance, "vkDeviceWaitIdle"));

            if (vkCreateDeviceFn == nullptr || vkDestroyDeviceFn == nullptr || vkGetDeviceQueueFn == nullptr ||
                vkCreateCommandPoolFn == nullptr || vkDestroyCommandPoolFn == nullptr ||
                vkAllocateCommandBuffersFn == nullptr || vkBeginCommandBufferFn == nullptr ||
                vkEndCommandBufferFn == nullptr || vkCreateQueryPoolFn == nullptr ||
                vkDestroyQueryPoolFn == nullptr || vkCmdResetQueryPoolFn == nullptr ||
                vkCmdWriteTimestampFn == nullptr || vkCreateFenceFn == nullptr || vkDestroyFenceFn == nullptr ||
                vkWaitForFencesFn == nullptr || vkQueueSubmitFn == nullptr || vkGetQueryPoolResultsFn == nullptr ||
                vkDeviceWaitIdleFn == nullptr) {
                fail("vkGetInstanceProcAddr could not resolve the entry points required for the "
                     "timestamp probe");
                return;
            }

            const Float queuePriority = 1.0f;
            VkDeviceQueueCreateInfo queueInfo{};
            queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.queueFamilyIndex = graphicsQueueFamilyIndex;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &queuePriority;

            VkDeviceCreateInfo deviceInfo{};
            deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            deviceInfo.queueCreateInfoCount = 1;
            deviceInfo.pQueueCreateInfos = &queueInfo;

            VkDevice device = VK_NULL_HANDLE;
            VkResult result = vkCreateDeviceFn(physicalDevice, &deviceInfo, nullptr, &device);
            if (result != VK_SUCCESS || device == VK_NULL_HANDLE) {
                fail(format("vkCreateDevice failed (VkResult = {})", static_cast<Int>(result)));
                return;
            }

            VkCommandPool commandPool = VK_NULL_HANDLE;
            VkQueryPool queryPool = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
            Bool fenceWaitTimedOut = false;
            // Same teardown-on-every-path style as the caller's instance guard; runs
            // before that guard, so device objects die before the instance does. The
            // idle wait keeps an in-flight submission from racing object destruction.
            const ScopeGuard destroyDeviceObjects([&]() {
                if (fenceWaitTimedOut) {
                    // The probe fence never signaled within its timeout, so the
                    // submission may still be executing - or the GPU is hung.
                    // vkDeviceWaitIdle could then block forever and destroying
                    // in-flight objects is undefined, so the probe deliberately
                    // leaks the device objects (device, pools, fence): a hung
                    // GPU must not hang the POST.
                    return;
                }
                vkDeviceWaitIdleFn(device);
                if (fence != VK_NULL_HANDLE) {
                    vkDestroyFenceFn(device, fence, nullptr);
                }
                if (queryPool != VK_NULL_HANDLE) {
                    vkDestroyQueryPoolFn(device, queryPool, nullptr);
                }
                if (commandPool != VK_NULL_HANDLE) {
                    vkDestroyCommandPoolFn(device, commandPool, nullptr);
                }
                vkDestroyDeviceFn(device, nullptr);
            });

            VkQueue queue = VK_NULL_HANDLE;
            vkGetDeviceQueueFn(device, graphicsQueueFamilyIndex, 0, &queue);
            if (queue == VK_NULL_HANDLE) {
                fail("vkGetDeviceQueue returned a null graphics queue");
                return;
            }

            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.queueFamilyIndex = graphicsQueueFamilyIndex;
            result = vkCreateCommandPoolFn(device, &poolInfo, nullptr, &commandPool);
            if (result != VK_SUCCESS) {
                fail(format("vkCreateCommandPool failed (VkResult = {})", static_cast<Int>(result)));
                return;
            }

            VkCommandBufferAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.commandPool = commandPool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
            result = vkAllocateCommandBuffersFn(device, &allocInfo, &commandBuffer);
            if (result != VK_SUCCESS) {
                fail(format("vkAllocateCommandBuffers failed (VkResult = {})", static_cast<Int>(result)));
                return;
            }

            VkQueryPoolCreateInfo queryPoolInfo{};
            queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryPoolInfo.queryCount = 2;
            result = vkCreateQueryPoolFn(device, &queryPoolInfo, nullptr, &queryPool);
            if (result != VK_SUCCESS) {
                fail(format("vkCreateQueryPool failed (VkResult = {})", static_cast<Int>(result)));
                return;
            }

            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            result = vkBeginCommandBufferFn(commandBuffer, &beginInfo);
            if (result != VK_SUCCESS) {
                fail(format("vkBeginCommandBuffer failed (VkResult = {})", static_cast<Int>(result)));
                return;
            }
            vkCmdResetQueryPoolFn(commandBuffer, queryPool, 0, 2);
            vkCmdWriteTimestampFn(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 0);
            vkCmdWriteTimestampFn(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);
            result = vkEndCommandBufferFn(commandBuffer);
            if (result != VK_SUCCESS) {
                fail(format("vkEndCommandBuffer failed (VkResult = {})", static_cast<Int>(result)));
                return;
            }

            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            result = vkCreateFenceFn(device, &fenceInfo, nullptr, &fence);
            if (result != VK_SUCCESS) {
                fail(format("vkCreateFence failed (VkResult = {})", static_cast<Int>(result)));
                return;
            }

            VkSubmitInfo submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &commandBuffer;
            result = vkQueueSubmitFn(queue, 1, &submitInfo, fence);
            if (result != VK_SUCCESS) {
                fail(format("vkQueueSubmit failed (VkResult = {})", static_cast<Int>(result)));
                return;
            }

            constexpr Uint64 FenceTimeoutNs = 5'000'000'000ull; // 5 s: a POST must never hang the launcher
            result = vkWaitForFencesFn(device, 1, &fence, VK_TRUE, FenceTimeoutNs);
            if (result != VK_SUCCESS) {
                // Skip the teardown idle wait too (see the scope guard): the
                // submission is still pending on a possibly-hung GPU.
                fenceWaitTimedOut = true;
                fail(format("vkWaitForFences did not signal within 5 s (VkResult = {})",
                            static_cast<Int>(result)));
                return;
            }

            Uint64 timestamps[2] = {0, 0};
            result = vkGetQueryPoolResultsFn(device, queryPool, 0, 2, sizeof(timestamps), timestamps,
                                             sizeof(Uint64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            if (result != VK_SUCCESS) {
                fail(format("vkGetQueryPoolResults failed (VkResult = {})", static_cast<Int>(result)));
                return;
            }

            const Uint64 validMask =
                timestampValidBits >= 64 ? ~0ull : ((1ull << timestampValidBits) - 1ull);
            const Uint64 t0 = timestamps[0] & validMask;
            const Uint64 t1 = timestamps[1] & validMask;
            if (t1 < t0) {
                fail(format("timestamps are not monotonic (t0 = {}, t1 = {})", t0, t1));
                return;
            }
            const Uint64 elapsedNs =
                static_cast<Uint64>(static_cast<Double>(t1 - t0) * static_cast<Double>(timestampPeriod));
            builder.Pass("Timer queries",
                         timestampFacts +
                             format("; timer query functional (t1 >= t0, probe observed {} ns)", elapsedNs) +
                             disabledNote);
        }

        // Everything the "MobileGL reported ..." rows need from the Vulkan device probe.
        struct VulkanProbeSummary {
            Bool devicePropsValid = false;
            String deviceName;
            String apiVersionString;
            String driverVersionString; // raw hex, vendor-encoded (see RunVulkanDriverPost)
            Bool shaderSubgroupUsable = false;
            Bool timerQueriesSupported = false;
            Bool samplerAnisotropySupported = false;
        };
    } // namespace

    // The Vulkan device probe proper. Split out of RunVulkanDriverPost so that the
    // "MobileGL reported ..." rows are appended on every path (including early
    // probe failures) before the report is finalized.
    //
    // The loader bring-up chain (dlopen, instance API version, vkCreateInstance) is one
    // "Vulkan instance" row, and the two required surface instance extensions are one
    // "Surface extensions" row. Details carry every sub-fact of the old per-stage rows:
    // PASS enumerates each stage's result (and each extension's presence), FAIL lists
    // the stages that completed and then names the exact stage that broke (or states
    // per extension whether it is present or missing) with the stage detail strings.
    static void ProbeVulkanDriver(ReportBuilder& builder, VulkanProbeSummary& summary) {
        String instanceChain;
        const auto instanceStageDone = [&](const String& description) {
            if (!instanceChain.empty()) {
                instanceChain += "; ";
            }
            instanceChain += description;
        };
        const auto failInstanceStage = [&](const String& stage, const String& detail) {
            builder.Fail("Vulkan instance",
                         (instanceChain.empty() ? "" : instanceChain + "; but ") + stage + ": " + detail);
        };

        void* loaderLibrary = OpenVulkanLoaderLibrary();
        if (loaderLibrary == nullptr) {
            failInstanceStage("Vulkan loader", "libvulkan.so could not be loaded; no Vulkan loader on this device");
            return;
        }
        const auto getInstanceProcAddr =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(VulkanLoaderSymbol(loaderLibrary, "vkGetInstanceProcAddr"));
        if (getInstanceProcAddr == nullptr) {
            failInstanceStage("Vulkan loader", "vkGetInstanceProcAddr is missing from the Vulkan loader library");
            return;
        }
        instanceStageDone("Vulkan loader library loaded and vkGetInstanceProcAddr resolved");

        const auto vkCreateInstanceFn =
            reinterpret_cast<PFN_vkCreateInstance>(getInstanceProcAddr(nullptr, "vkCreateInstance"));
        const auto vkEnumerateInstanceVersionFn = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            getInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
        const auto vkEnumerateInstanceExtensionPropertiesFn =
            reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
                getInstanceProcAddr(nullptr, "vkEnumerateInstanceExtensionProperties"));

        Uint32 instanceApiVersion = VK_API_VERSION_1_0;
        if (vkEnumerateInstanceVersionFn != nullptr) {
            vkEnumerateInstanceVersionFn(&instanceApiVersion);
        }
        if (vkCreateInstanceFn == nullptr || vkEnumerateInstanceVersionFn == nullptr ||
            instanceApiVersion < VK_API_VERSION_1_1) {
            failInstanceStage("Instance API version",
                              format("instance API {} (< 1.1); the DirectVulkan backend requires a Vulkan 1.1 "
                                     "instance",
                                     VkApiVersionToString(instanceApiVersion)));
            return;
        }
        instanceStageDone(format("instance API {}", VkApiVersionToString(instanceApiVersion)));

        Vector<VkExtensionProperties> instanceExtensions;
        if (vkEnumerateInstanceExtensionPropertiesFn != nullptr) {
            Uint32 extensionCount = 0;
            if (vkEnumerateInstanceExtensionPropertiesFn(nullptr, &extensionCount, nullptr) == VK_SUCCESS &&
                extensionCount > 0) {
                instanceExtensions.resize(extensionCount);
                if (vkEnumerateInstanceExtensionPropertiesFn(nullptr, &extensionCount, instanceExtensions.data()) ==
                    VK_SUCCESS) {
                    instanceExtensions.resize(extensionCount);
                } else {
                    instanceExtensions.clear();
                }
            }
        }
        // One row for the required surface instance extensions; the detail states each
        // extension's presence individually, and a missing one carries the "required
        // instance extension" fact plus its consequence from the old per-extension rows.
        {
            String surfaceDetail;
            Bool anySurfaceExtensionMissing = false;
            const auto recordExtension = [&](const char* name, const char* consequence) {
                if (!surfaceDetail.empty()) {
                    surfaceDetail += "; ";
                }
                if (HasVkExtension(instanceExtensions, name)) {
                    surfaceDetail += format("{} instance extension present", name);
                } else {
                    anySurfaceExtensionMissing = true;
                    surfaceDetail += format("{} missing (required instance extension; {})", name, consequence);
                }
            };
            recordExtension(VK_KHR_SURFACE_EXTENSION_NAME, "on-screen rendering is impossible");
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
            recordExtension(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, "ANativeWindow surfaces cannot be created");
#endif
            if (anySurfaceExtensionMissing) {
                builder.Fail("Surface extensions", surfaceDetail);
            } else {
                builder.Pass("Surface extensions", surfaceDetail);
            }
        }

        // Windowless (EGL pbuffer) contexts want a headless surface. Almost no mobile
        // ICD provides one - Mali r32p1 does not - so its absence is not fatal: the
        // renderer hands the WSI an AImageReader window instead. Reported because the
        // fallback costs a buffer queue the headless path does not need, and because
        // this used to abort the process instead.
        if (HasVkExtension(instanceExtensions, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME)) {
            builder.Pass("Headless surface",
                         format("{} present; windowless contexts get a real headless surface",
                                VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME));
        } else {
            builder.Warn("Headless surface",
                         format("{} absent; a windowless (pbuffer) context falls back to an AImageReader "
                                "ANativeWindow, which needs libmediandk.so and an extra buffer queue",
                                VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME));
        }

        // The probe never creates a surface, so the instance is created without extensions.
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "MobileGL Driver POST";
        appInfo.pEngineName = "MobileGL";
        appInfo.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &appInfo;

        VkInstance instance = VK_NULL_HANDLE;
        const VkResult createResult = vkCreateInstanceFn(&instanceInfo, nullptr, &instance);
        if (createResult != VK_SUCCESS || instance == VK_NULL_HANDLE) {
            failInstanceStage("Vulkan instance creation",
                              format("vkCreateInstance failed (VkResult = {})", static_cast<Int>(createResult)));
            return;
        }
        instanceStageDone("Vulkan 1.1 instance created");
        builder.Pass("Vulkan instance", instanceChain);

        const auto vkDestroyInstanceFn =
            reinterpret_cast<PFN_vkDestroyInstance>(getInstanceProcAddr(instance, "vkDestroyInstance"));
        const auto vkEnumeratePhysicalDevicesFn = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
            getInstanceProcAddr(instance, "vkEnumeratePhysicalDevices"));
        const auto vkGetPhysicalDevicePropertiesFn = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties"));
        const auto vkGetPhysicalDeviceQueueFamilyPropertiesFn =
            reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
                getInstanceProcAddr(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
        const auto vkGetPhysicalDeviceFeaturesFn = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(
            getInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures"));
        const auto vkEnumerateDeviceExtensionPropertiesFn =
            reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                getInstanceProcAddr(instance, "vkEnumerateDeviceExtensionProperties"));
        const auto vkGetPhysicalDeviceFeatures2Fn = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            getInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
        const auto vkGetPhysicalDeviceProperties2Fn = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));
        const auto vkGetPhysicalDeviceFormatPropertiesFn =
            reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
                getInstanceProcAddr(instance, "vkGetPhysicalDeviceFormatProperties"));

        // The instance is destroyed from a scope guard so it is released on every early-return
        // path and even if a String/format allocation throws while report rows are being built.
        const ScopeGuard destroyInstance([&]() {
            if (vkDestroyInstanceFn != nullptr) {
                vkDestroyInstanceFn(instance, nullptr);
            }
        });

        if (vkEnumeratePhysicalDevicesFn == nullptr || vkGetPhysicalDevicePropertiesFn == nullptr ||
            vkGetPhysicalDeviceQueueFamilyPropertiesFn == nullptr || vkGetPhysicalDeviceFeaturesFn == nullptr ||
            vkEnumerateDeviceExtensionPropertiesFn == nullptr) {
            builder.Fail("Vulkan core entry points",
                         "vkGetInstanceProcAddr could not resolve required Vulkan 1.0 functions");
            return;
        }

        // Device discovery (physical device enumeration, graphics queue selection, device
        // API version) is one "Graphics device" row; FAIL names the failing stage.
        Uint32 deviceCount = 0;
        const VkResult countResult = vkEnumeratePhysicalDevicesFn(instance, &deviceCount, nullptr);
        if (countResult != VK_SUCCESS) {
            builder.Fail("Graphics device", format("vkEnumeratePhysicalDevices failed (VkResult = {})",
                                                   static_cast<Int>(countResult)));
            return;
        }
        if (deviceCount == 0) {
            builder.Fail("Graphics device", "no Vulkan physical devices found");
            return;
        }
        builder.report.available = true;
        Vector<VkPhysicalDevice> devices(deviceCount);
        const VkResult enumerateResult = vkEnumeratePhysicalDevicesFn(instance, &deviceCount, devices.data());
        if (enumerateResult != VK_SUCCESS) {
            builder.Fail("Graphics device", format("vkEnumeratePhysicalDevices failed (VkResult = {})",
                                                   static_cast<Int>(enumerateResult)));
            return;
        }
        devices.resize(deviceCount);

        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        Uint32 graphicsQueueFamilyIndex = 0;
        Uint32 graphicsQueueTimestampValidBits = 0;
        for (VkPhysicalDevice candidate : devices) {
            Uint32 queueFamilyCount = 0;
            vkGetPhysicalDeviceQueueFamilyPropertiesFn(candidate, &queueFamilyCount, nullptr);
            Vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
            vkGetPhysicalDeviceQueueFamilyPropertiesFn(candidate, &queueFamilyCount, queueFamilies.data());
            for (Uint32 familyIndex = 0; familyIndex < queueFamilyCount; ++familyIndex) {
                const VkQueueFamilyProperties& family = queueFamilies[familyIndex];
                if (family.queueCount > 0 && (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                    physicalDevice = candidate;
                    graphicsQueueFamilyIndex = familyIndex;
                    graphicsQueueTimestampValidBits = family.timestampValidBits;
                    break;
                }
            }
            if (physicalDevice != VK_NULL_HANDLE) {
                break;
            }
        }
        if (physicalDevice == VK_NULL_HANDLE) {
            builder.Fail("Graphics device",
                         format("none of the {} physical device(s) exposes a graphics queue family", deviceCount));
            return;
        }

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDevicePropertiesFn(physicalDevice, &properties);

        // driverVersion is vendor-encoded (each vendor packs its own bit layout), so it is
        // reported as raw hex instead of being decoded with the VK_VERSION_* macros.
        const String driverVersionString = format("0x{:08x}", properties.driverVersion);
        builder.report.rendererInfo = format("{} (Vulkan {}, driver {})", String(properties.deviceName),
                                             VkApiVersionToString(properties.apiVersion), driverVersionString);
        summary.devicePropsValid = true;
        summary.deviceName = String(properties.deviceName);
        summary.apiVersionString = VkApiVersionToString(properties.apiVersion);
        summary.driverVersionString = driverVersionString;

        // The chosen-device facts (name, enumeration count, graphics queue) ride along
        // on both outcomes so the device API verdict never hides them.
        const String deviceFacts =
            format("{} ({} device(s) enumerated, picked the first with a graphics queue); "
                   "graphics queue family present",
                   String(properties.deviceName), deviceCount);
        if (properties.apiVersion >= VK_API_VERSION_1_1) {
            builder.Pass("Graphics device",
                         deviceFacts +
                             format("; device API Vulkan {}", VkApiVersionToString(properties.apiVersion)));
        } else {
            builder.Fail("Graphics device",
                         deviceFacts + format("; but Device API version: Vulkan {} (< 1.1); the DirectVulkan "
                                              "backend requires a Vulkan 1.1 device",
                                              VkApiVersionToString(properties.apiVersion)));
        }

        EvaluateVertexAttribLimit(builder, static_cast<Int>(properties.limits.maxVertexInputAttributes),
                                  "Vertex attributes", "maxVertexInputAttributes");

        Vector<VkExtensionProperties> deviceExtensions;
        Uint32 deviceExtensionCount = 0;
        if (vkEnumerateDeviceExtensionPropertiesFn(physicalDevice, nullptr, &deviceExtensionCount, nullptr) ==
                VK_SUCCESS &&
            deviceExtensionCount > 0) {
            deviceExtensions.resize(deviceExtensionCount);
            if (vkEnumerateDeviceExtensionPropertiesFn(physicalDevice, nullptr, &deviceExtensionCount,
                                                       deviceExtensions.data()) == VK_SUCCESS) {
                deviceExtensions.resize(deviceExtensionCount);
            } else {
                deviceExtensions.clear();
            }
        }
        if (HasVkExtension(deviceExtensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            builder.Pass("VK_KHR_swapchain", "device extension present");
        } else {
            builder.Fail("VK_KHR_swapchain", "required device extension missing; presentation is impossible");
        }

        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeaturesFn(physicalDevice, &features);
        summary.samplerAnisotropySupported = features.samplerAnisotropy == VK_TRUE;
        if (features.multiDrawIndirect == VK_TRUE) {
            builder.Pass("multiDrawIndirect", "indirect multi-draw batches run as single native commands");
        } else {
            builder.Info("multiDrawIndirect",
                         "unsupported; multi-draw batches fall back to one draw per command (tier "
                         "\"indirect\" of the multi-draw dispatch is unavailable)");
        }
        if (features.drawIndirectFirstInstance == VK_TRUE) {
            builder.Pass("drawIndirectFirstInstance", "indirect commands may carry a non-zero firstInstance");
        } else {
            builder.Warn("drawIndirectFirstInstance",
                         "unsupported; indirect commands with a non-zero baseInstance cannot run natively");
        }
        // Multi-draw dispatch tiers (ext -> indirect -> unroll). INFO on the missing
        // pieces: every tier has a fallback, nothing is lost, only batched into more
        // commands. The renderer resolves the same chain at device creation, clamped
        // by MOBILEGL_MAGMA_MULTIDRAW_MODE.
        {
            Bool multiDrawExtUsable = false;
            if (HasVkExtension(deviceExtensions, VK_EXT_MULTI_DRAW_EXTENSION_NAME) &&
                vkGetPhysicalDeviceFeatures2Fn != nullptr) {
                VkPhysicalDeviceMultiDrawFeaturesEXT multiDrawFeatures{};
                multiDrawFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT;
                VkPhysicalDeviceFeatures2 features2{};
                features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                features2.pNext = &multiDrawFeatures;
                vkGetPhysicalDeviceFeatures2Fn(physicalDevice, &features2);
                multiDrawExtUsable = multiDrawFeatures.multiDraw == VK_TRUE;
            }
            if (multiDrawExtUsable) {
                builder.Pass("VK_EXT_multi_draw",
                             "supported; a glMultiDraw* batch runs as one vkCmdDrawMulti(Indexed)EXT");
            } else {
                builder.Info("VK_EXT_multi_draw",
                             "unsupported; glMultiDraw* batches use the indirect or unrolled tier");
            }
            const char* resolvedTier = multiDrawExtUsable                    ? "ext"
                                       : features.multiDrawIndirect == VK_TRUE ? "indirect"
                                                                               : "unroll";
            String tierDetail = format("default tier \"{}\" (chain: ext -> indirect -> unroll)", resolvedTier);
            const MG_Config::MultiDrawMode multiDrawMode = MG_Config::Features.MagmaMultiDrawMode;
            if (multiDrawMode != MG_Config::MultiDrawMode::Auto) {
                tierDetail += format("; MOBILEGL_MAGMA_MULTIDRAW_MODE={} caps it (clamped to device support)",
                                     multiDrawMode == MG_Config::MultiDrawMode::Ext        ? "ext"
                                     : multiDrawMode == MG_Config::MultiDrawMode::Indirect ? "indirect"
                                                                                           : "unroll");
            }
            builder.Info("Multi-draw dispatch tier", tierDetail);
        }
        if (features.vertexPipelineStoresAndAtomics == VK_TRUE) {
            builder.Pass("vertexPipelineStoresAndAtomics",
                         "supported by driver (not currently enabled by the DirectVulkan backend)");
        } else {
            builder.Warn("vertexPipelineStoresAndAtomics",
                         "unsupported; shaders that write storage buffers from the vertex stage will not work");
        }
        if (features.fillModeNonSolid == VK_TRUE) {
            builder.Pass("fillModeNonSolid", "glPolygonMode GL_LINE/GL_POINT rasterization supported");
        } else {
            builder.Warn("fillModeNonSolid",
                         "unsupported; glPolygonMode GL_LINE/GL_POINT falls back to GL_FILL (no wireframe/point "
                         "rasterization)");
        }
        if (features.independentBlend == VK_TRUE) {
            builder.Pass("independentBlend", "per-draw-buffer glColorMaski and indexed blend state supported");
        } else {
            builder.Warn("independentBlend",
                         "unsupported; per-draw-buffer glColorMaski falls back to draw buffer 0 for all attachments");
        }
        if (features.dualSrcBlend == VK_TRUE) {
            builder.Pass("dualSrcBlend", "GL_SRC1_* dual-source blend factors supported");
        } else {
            builder.Warn("dualSrcBlend", "unsupported; GL_SRC1_* dual-source blend factors hard-fail at draw");
        }
        // The Magma counterpart of the GLES "Buffer textures" row, so the two sections can be
        // read side by side. Vulkan has no optional-feature bit here: a uniform texel buffer is
        // core, and maxTexelBufferElements has a spec floor of 65536 - exactly the GL 3.1 floor
        // for GL_MAX_TEXTURE_BUFFER_SIZE - so this backend can always back a buffer texture and
        // the row exists to state the limit MobileGL derives its advertisement from, not to
        // report a risk. A driver below the floor would be non-conformant, hence the Warn.
        {
            const Uint32 maxTexelBufferElements = properties.limits.maxTexelBufferElements;
            constexpr Uint32 kGL31MinTextureBufferSize = 65536;
            if (maxTexelBufferElements >= kGL31MinTextureBufferSize) {
                builder.Pass("maxTexelBufferElements",
                             format("{}; uniform texel buffers are core in Vulkan, so buffer textures "
                                    "need no extension and MobileGL advertises "
                                    "GL_MAX_TEXTURE_BUFFER_SIZE from this limit",
                                    maxTexelBufferElements));
            } else {
                builder.Warn("maxTexelBufferElements",
                             format("{} (< {}); below the OpenGL 3.1 floor for "
                                    "GL_MAX_TEXTURE_BUFFER_SIZE, so a conformant application may "
                                    "create a buffer texture larger than this driver can view",
                                    maxTexelBufferElements, kGL31MinTextureBufferSize));
            }
        }
        {
            VkImageFormatProperties sliceProbe{};
            const Bool sliceCapable =
                vkGetPhysicalDeviceImageFormatProperties(
                    physicalDevice, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_3D, VK_IMAGE_TILING_OPTIMAL,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT, &sliceProbe) == VK_SUCCESS;
            if (sliceCapable) {
                builder.Pass("2D-array-compatible 3D images",
                             "supported for the common colour attachment formats (one z slice of a "
                             "GL_TEXTURE_3D texture can be attached to a framebuffer and cleared and read "
                             "back on its own; a format that refuses the flag is detected at image "
                             "creation and declines per-slice attachment)");
            } else {
                builder.Warn("2D-array-compatible 3D images",
                             "VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT unavailable for colour attachments; "
                             "glFramebufferTextureLayer on a GL_TEXTURE_3D texture is declined for every "
                             "slice past the first");
            }
        }
        if (features.imageCubeArray == VK_TRUE) {
            builder.Pass("imageCubeArray",
                         "GL_TEXTURE_CUBE_MAP_ARRAY textures get a Vulkan image and can be sampled and "
                         "attached to a framebuffer per layer");
        } else {
            builder.Warn("imageCubeArray",
                         "unsupported; a GL_TEXTURE_CUBE_MAP_ARRAY texture gets no image at all, so sampling "
                         "one reads nothing and glFramebufferTextureLayer on one is declined");
        }
        // Reported whichever way the device answers, because MobileGL no longer follows the
        // device here: every 64-bit float is narrowed to 32 bits before any module reaches this
        // backend (DemoteFloat64Pass), so the Float64 capability is never declared and a device
        // that HAS the feature gains nothing from it. The device's own answer is still worth
        // printing - it is the reason the demotion is unconditional.
        builder.Pass("fp64", AppendFp64AdvertisementNote(
                                 format("demoted to fp32 (device shaderFloat64 = {}) - every double / dvec / "
                                        "dmat in a shader is narrowed to 32 bits before pipeline creation, so "
                                        "such shaders BUILD AND RUN at single precision on every device "
                                        "instead of failing to create a shader module on the ones without the "
                                        "feature. A block containing a double is re-laid-out for the narrowed "
                                        "members, so an application that hard-codes std140 offsets computed "
                                        "for doubles must query them instead",
                                        features.shaderFloat64 == VK_TRUE ? "supported" : "unsupported")));
        builder.Warn("64-bit vertex attributes",
                     "not supported; there is no 64-bit shader input left to feed after the fp64 demotion "
                     "above, and no VK_FORMAT_R64*_SFLOAT vertex fetch to feed it with on most devices "
                     "anyway. glVertexAttribLFormat reports GL_INVALID_OPERATION - feed the attribute with "
                     "glVertexAttribPointer(GL_FLOAT), which a demoted dvec input reads correctly");

        Bool shaderDrawParameters = false;
        if (vkGetPhysicalDeviceFeatures2Fn != nullptr && properties.apiVersion >= VK_API_VERSION_1_1) {
            VkPhysicalDeviceShaderDrawParametersFeatures drawParametersFeatures{};
            drawParametersFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES;
            VkPhysicalDeviceFeatures2 features2{};
            features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features2.pNext = &drawParametersFeatures;
            vkGetPhysicalDeviceFeatures2Fn(physicalDevice, &features2);
            shaderDrawParameters = drawParametersFeatures.shaderDrawParameters == VK_TRUE;
        } else if (HasVkExtension(deviceExtensions, VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME)) {
            // Vulkan 1.0 device: the extension alone exposes the SPIR-V DrawParameters capability.
            shaderDrawParameters = true;
        }
        if (shaderDrawParameters) {
            builder.Pass("shaderDrawParameters", "gl_DrawID/gl_BaseVertex/gl_BaseInstance shaders supported");
        } else {
            builder.Warn("shaderDrawParameters",
                         "unavailable; shaders using gl_DrawID/gl_BaseInstance will not work");
        }

        Bool provokingVertexLast = false;
        Bool transformFeedbackPreservesProvokingVertex = false;
        Bool provokingVertexModePerPipeline = false;
        Bool transformFeedbackPreservesTriangleFanProvokingVertex = false;
        if (vkGetPhysicalDeviceFeatures2Fn != nullptr &&
            HasVkExtension(deviceExtensions, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME)) {
            VkPhysicalDeviceProvokingVertexFeaturesEXT provokingVertexFeatures{};
            provokingVertexFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT;
            VkPhysicalDeviceFeatures2 features2{};
            features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features2.pNext = &provokingVertexFeatures;
            vkGetPhysicalDeviceFeatures2Fn(physicalDevice, &features2);
            provokingVertexLast = provokingVertexFeatures.provokingVertexLast == VK_TRUE;
            transformFeedbackPreservesProvokingVertex =
                provokingVertexFeatures.transformFeedbackPreservesProvokingVertex == VK_TRUE;
            if (vkGetPhysicalDeviceProperties2Fn != nullptr) {
                VkPhysicalDeviceProvokingVertexPropertiesEXT provokingVertexProperties{};
                provokingVertexProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_PROPERTIES_EXT;
                VkPhysicalDeviceProperties2 properties2{};
                properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                properties2.pNext = &provokingVertexProperties;
                vkGetPhysicalDeviceProperties2Fn(physicalDevice, &properties2);
                provokingVertexModePerPipeline =
                    provokingVertexProperties.provokingVertexModePerPipeline == VK_TRUE;
                transformFeedbackPreservesTriangleFanProvokingVertex =
                    provokingVertexProperties.transformFeedbackPreservesTriangleFanProvokingVertex == VK_TRUE;
            }
        }
        if (provokingVertexLast) {
            builder.Pass("provokingVertexLast",
                         "supported; flat varyings take GL's last vertex and transform feedback records "
                         "strip/fan triangles in GL's vertex order");
        } else {
            builder.Warn("provokingVertexLast",
                         "unsupported; flat-shaded varyings take a primitive's first vertex instead of GL's "
                         "last, and transform feedback records TRIANGLE_STRIP/TRIANGLE_FAN triangles rotated "
                         "(e.g. 0,1,2 / 1,3,2 instead of 0,1,2 / 2,1,3)");
        }
        if (provokingVertexLast && !transformFeedbackPreservesProvokingVertex) {
            builder.Warn("transformFeedbackPreservesProvokingVertex",
                         "unsupported; the captured vertex order for strips/fans is not guaranteed by the "
                         "spec even though the flat-shading convention is correct");
        }
        if (provokingVertexLast && transformFeedbackPreservesProvokingVertex &&
            !transformFeedbackPreservesTriangleFanProvokingVertex && !provokingVertexModePerPipeline) {
            builder.Warn("transformFeedbackPreservesTriangleFanProvokingVertex",
                         "unsupported and per-pipeline modes unavailable; the transform-feedback "
                         "provoking-vertex guarantee is left off so GL_TRIANGLE_FAN pipelines stay legal");
        }

        Bool primitiveTopologyListRestart = false;
        if (vkGetPhysicalDeviceFeatures2Fn != nullptr &&
            HasVkExtension(deviceExtensions, VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME)) {
            VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT listRestartFeatures{};
            listRestartFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT;
            VkPhysicalDeviceFeatures2 features2{};
            features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features2.pNext = &listRestartFeatures;
            vkGetPhysicalDeviceFeatures2Fn(physicalDevice, &features2);
            primitiveTopologyListRestart = listRestartFeatures.primitiveTopologyListRestart == VK_TRUE;
        }
        if (primitiveTopologyListRestart) {
            builder.Pass("primitiveTopologyListRestart",
                         "primitive restart supported on list topologies (GL_PRIMITIVE_RESTART)");
        } else {
            builder.Warn("primitiveTopologyListRestart",
                         "unsupported; primitive restart works on strip/fan topologies only, list-topology restart "
                         "hard-fails at draw");
        }

        // Core 1.0 features the backend turns GL stages into pipeline stages with.
        VkPhysicalDeviceFeatures coreFeatures{};
        vkGetPhysicalDeviceFeatures(physicalDevice, &coreFeatures);
        if (coreFeatures.tessellationShader == VK_TRUE) {
            builder.Pass("tessellationShader",
                         "supported (GL_PATCHES draws run the tessellation control/evaluation stages)");
        } else {
            builder.Warn("tessellationShader",
                         "unsupported; a program with a tessellation control/evaluation shader cannot build a "
                         "pipeline, so GL_PATCHES draws render nothing");
        }

        Bool vertexAttributeInstanceRateDivisor = false;
        if (vkGetPhysicalDeviceFeatures2Fn != nullptr &&
            HasVkExtension(deviceExtensions, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME)) {
            VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT divisorFeatures{};
            divisorFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT;
            VkPhysicalDeviceFeatures2 features2{};
            features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features2.pNext = &divisorFeatures;
            vkGetPhysicalDeviceFeatures2Fn(physicalDevice, &features2);
            vertexAttributeInstanceRateDivisor = divisorFeatures.vertexAttributeInstanceRateDivisor == VK_TRUE;
        }
        if (vertexAttributeInstanceRateDivisor) {
            builder.Pass("vertexAttributeInstanceRateDivisor",
                         "supported (glVertexAttribDivisor advances an attribute every N instances)");
        } else {
            builder.Warn("vertexAttributeInstanceRateDivisor",
                         "unsupported; Vulkan's instance input rate can only advance once per instance, so "
                         "every non-zero glVertexAttribDivisor behaves as 1 and instanced attributes meant to "
                         "change every N instances change every one");
        }

        if (vkGetPhysicalDeviceProperties2Fn != nullptr && properties.apiVersion >= VK_API_VERSION_1_1) {
            VkPhysicalDeviceSubgroupProperties subgroupProperties{};
            subgroupProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
            VkPhysicalDeviceProperties2 properties2{};
            properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties2.pNext = &subgroupProperties;
            vkGetPhysicalDeviceProperties2Fn(physicalDevice, &properties2);
            const Bool subgroupUsable = subgroupProperties.subgroupSize > 0 &&
                                        (subgroupProperties.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0 &&
                                        (subgroupProperties.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT) != 0;
            // Same usability rule as the Vulkan capability loader's
            // HasUsableShaderSubgroupSupport, which feeds the GL_KHR_shader_subgroup
            // advertisement of the real backend.
            summary.shaderSubgroupUsable = subgroupUsable;
            if (subgroupUsable) {
                builder.Pass("Compute shader subgroup",
                             format("basic subgroup operations in compute, subgroup size {}",
                                    subgroupProperties.subgroupSize));
            } else {
                builder.Warn("Compute shader subgroup",
                             "basic subgroup operations are not usable from compute shaders");
            }
        } else {
            builder.Warn("Compute shader subgroup", "subgroup properties could not be queried");
        }

        if (HasVkExtension(deviceExtensions, VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME)) {
            builder.Pass("VK_KHR_draw_indirect_count",
                         "supported (count-buffer indirect draws run as single native "
                         "vkCmdDraw*IndirectCount commands)");
        } else {
            builder.Warn("VK_KHR_draw_indirect_count",
                         "not supported; count-buffer indirect draws (glMultiDraw*IndirectCount) fall "
                         "back to a CPU readback of the parameter buffer and one draw per command");
        }
        const Bool indexTypeUint8 = HasVkExtension(deviceExtensions, VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME) ||
                                    HasVkExtension(deviceExtensions, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME);
        if (indexTypeUint8) {
            builder.Pass("Index type uint8", "supported (native GL_UNSIGNED_BYTE index buffers)");
        } else {
            builder.Warn("Index type uint8",
                         "not supported; GL_UNSIGNED_BYTE index buffers cannot be drawn (the backend "
                         "has no conversion fallback and asserts on uint8 index draws)");
        }
        builder.DriverReported("Backend driver reported device", String(properties.deviceName));
        builder.DriverReported("Backend driver reported driver version", driverVersionString + " (vendor-encoded)");

        // Single "Timer queries" row: timestampValidBits, timestampPeriod, and the
        // functional timestamp probe fold into one combined verdict whose detail
        // always states the validBits and period values; the
        // MOBILEGL_DISABLE_TIMERQUERY note is appended to the same row.
        const Float timestampPeriod = properties.limits.timestampPeriod;
        // Same support rule as VulkanRenderer::CreateLogicalDeviceAndQueues
        // (m_timerQuerySupported): usable timer queries need valid timestamp bits on
        // the graphics queue family and a non-zero tick period.
        summary.timerQueriesSupported = graphicsQueueTimestampValidBits > 0 && timestampPeriod > 0.0f;
        if (graphicsQueueTimestampValidBits > 0) {
            ProbeVulkanTimerQuery(builder, getInstanceProcAddr, instance, physicalDevice,
                                  graphicsQueueFamilyIndex, graphicsQueueTimestampValidBits, timestampPeriod);
        } else {
            builder.Warn("Timer queries",
                         format("timestampValidBits = 0 on the graphics queue family; timestampPeriod = {} ns "
                                "per tick; timestamps unsupported on the graphics queue; timer queries "
                                "unavailable",
                                timestampPeriod) +
                             TimerQueryDisabledNote());
        }
        if (vkGetPhysicalDeviceFormatPropertiesFn != nullptr) {
            MG_External::VulkanCapabilities formatProbeCapabilities{};
            BackendLoader::FillInVulkanCapabilities(formatProbeCapabilities, properties);
            builder.report.formatCapabilities.emplace();
            MG_Backend::DirectVulkan::PopulateFormatCapabilities(
                physicalDevice, vkGetPhysicalDeviceFormatPropertiesFn, formatProbeCapabilities,
                builder.report.formatCapabilities.value());
        }
    }

    BackendPostReport RunVulkanDriverPost() {
        MGLOG_I("Driver POST: probing the device Vulkan driver");
        ReportBuilder builder;
        VulkanProbeSummary summary;
        ProbeVulkanDriver(builder, summary);

        // "MobileGL reported ..." rows: what applications running on the DirectVulkan
        // backend (Magma) would see. The backend API version string reuses the exact
        // GetBackendAPIVersionString format, fed with the strings this probe collected
        // (so the driver version appears in the probe's raw vendor-encoded hex form);
        // the extension list is built by the same helper the real backend uses.
        Optional<String> backendApiVersionString;
        Optional<String> advertisedExtensions;
        if (summary.devicePropsValid) {
            backendApiVersionString = MG_Backend::DirectVulkan::FormatBackendAPIVersionString(
                summary.deviceName, summary.apiVersionString, summary.driverVersionString);
            advertisedExtensions = JoinAdvertisedExtensions(MG_Backend::DirectVulkan::BuildAdvertisedExtensions(
                summary.shaderSubgroupUsable, summary.timerQueriesSupported, summary.samplerAnisotropySupported));
        }
        AppendMobileGLReportedRows(builder, MG_Backend::DirectVulkan::GetRendererIdentity(), backendApiVersionString,
                                   advertisedExtensions);

        builder.Finalize();
        MGLOG_I("Driver POST: Vulkan verdict = %s", builder.report.verdict.c_str());
        return builder.report;
    }
} // namespace MobileGL::MG_Util::SelfTest
