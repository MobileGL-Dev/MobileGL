// MobileGL - MobileGL/MG_Backend/DirectGLES/DirectGLESSharedImage.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DirectGLESInternal.h"

namespace MobileGL::MG_Backend::DirectGLES {
#if MOBILEGL_BUILD_DISAGGREGATED
    // ---- SHARED IMAGES (DirectGLES.h; docs/archive/notes/anland/plan-ahb-dmabuf.md) ------

    namespace SharedImageImpl {
#if defined(__ANDROID__)
        namespace {
            namespace SI = MG_Remote::Server::SharedImages;

            // EGL_ANDROID_image_native_buffer's target and EGL_KHR_image_base's attribute, spelled
            // here so no extension header is needed.
            constexpr EGLenum kEglNativeBufferAndroid = 0x3140;
            constexpr EGLint kEglImagePreservedKhr = 0x30D2;

            using GetNativeClientBufferFn = EGLClientBuffer (*)(const AHardwareBuffer*);
            using CreateImageKhrFn = void* (*)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*);
            using DestroyImageKhrFn = EGLBoolean (*)(EGLDisplay, void*);
            using ImageTargetFn = void (*)(GLenum, void*);

            // EGL_KHR_fence_sync / EGL_KHR_wait_sync / EGL_ANDROID_native_fence_sync, likewise.
            constexpr EGLenum kEglSyncNativeFenceAndroid = 0x3144;
            constexpr EGLint kEglSyncNativeFenceFdAndroid = 0x3145;
            constexpr EGLint kEglConditionSatisfiedKhr = 0x30F6;
            using CreateSyncKhrFn = void* (*)(EGLDisplay, EGLenum, const EGLint*);
            using DestroySyncKhrFn = EGLBoolean (*)(EGLDisplay, void*);
            using WaitSyncKhrFn = EGLint (*)(EGLDisplay, void*, EGLint);
            using ClientWaitSyncKhrFn = EGLint (*)(EGLDisplay, void*, EGLint, Uint64);
            using DupNativeFenceFdFn = EGLint (*)(EGLDisplay, void*);

            struct EntryPoints {
                Bool Resolved = false;
                Bool Usable = false;
                String Why;
                GetNativeClientBufferFn GetNativeClientBuffer = nullptr;
                CreateImageKhrFn CreateImage = nullptr;
                DestroyImageKhrFn DestroyImage = nullptr;
                ImageTargetFn TargetTexture2D = nullptr;
                ImageTargetFn TargetRenderbuffer = nullptr;
                // Fences as sync_files: what lets one session's GPU wait for another's without a
                // CPU in between. Without them every wait below is a CPU wait.
                Bool FencesUsable = false;
                CreateSyncKhrFn CreateSync = nullptr;
                DestroySyncKhrFn DestroySync = nullptr;
                WaitSyncKhrFn WaitSync = nullptr;
                ClientWaitSyncKhrFn ClientWaitSync = nullptr;
                DupNativeFenceFdFn DupNativeFenceFd = nullptr;
                // YUV images: sampled as external textures (GL_OES_EGL_image_external_essl3), and
                // through GL_EXT_YUV_target's raw Y'CbCr sampler where the driver has it, so the
                // conversion is the import's own (its colour hints) rather than the driver's guess.
                Bool ExternalEssl3 = false;
                Bool YuvTarget = false;
            };

            // None of these is in the loader's tables, and eglGetProcAddress hands back live-looking
            // stubs for entry points a driver lacks, so the extension strings decide first. Resolved
            // once, from the first call that has a context current to read the strings from; the
            // answer is never modified afterwards, so it is read without the lock.
            const EntryPoints& ResolvedEntryPoints() {
                static auto* mutex = new std::mutex();
                static auto* entry = new EntryPoints();
                const std::lock_guard<std::mutex> lock(*mutex);
                if (entry->Resolved) return *entry;
                if (!g_EGLFuncs.eglGetProcAddress || !g_EGLFuncs.eglQueryString || !g_GLESFuncs.glGetString) {
                    entry->Why = "the loader has no eglGetProcAddress/eglQueryString/glGetString";
                    return *entry;
                }
                const char* egl = g_Display != EGL_NO_DISPLAY ? g_EGLFuncs.eglQueryString(g_Display, EGL_EXTENSIONS)
                                                              : nullptr;
                const char* gl = reinterpret_cast<const char*>(g_GLESFuncs.glGetString(GL_EXTENSIONS));
                if (egl == nullptr || gl == nullptr) {
                    entry->Why = "no current display and context to read the extension strings from";
                    return *entry; // asked again on the next call
                }
                entry->Resolved = true;
                if (HasExtensionWord(egl, "EGL_KHR_fence_sync") && HasExtensionWord(egl, "EGL_KHR_wait_sync") &&
                    HasExtensionWord(egl, "EGL_ANDROID_native_fence_sync")) {
                    entry->CreateSync =
                        reinterpret_cast<CreateSyncKhrFn>(g_EGLFuncs.eglGetProcAddress("eglCreateSyncKHR"));
                    entry->DestroySync =
                        reinterpret_cast<DestroySyncKhrFn>(g_EGLFuncs.eglGetProcAddress("eglDestroySyncKHR"));
                    entry->WaitSync = reinterpret_cast<WaitSyncKhrFn>(g_EGLFuncs.eglGetProcAddress("eglWaitSyncKHR"));
                    entry->ClientWaitSync =
                        reinterpret_cast<ClientWaitSyncKhrFn>(g_EGLFuncs.eglGetProcAddress("eglClientWaitSyncKHR"));
                    entry->DupNativeFenceFd = reinterpret_cast<DupNativeFenceFdFn>(
                        g_EGLFuncs.eglGetProcAddress("eglDupNativeFenceFDANDROID"));
                    entry->FencesUsable = entry->CreateSync && entry->DestroySync && entry->WaitSync &&
                                          entry->ClientWaitSync && entry->DupNativeFenceFd;
                }
                if (!entry->FencesUsable) {
                    MGLOG_I("Shared images: no EGL native fence sync / wait sync; sessions order their access "
                            "to an image with CPU waits");
                }
                if (!HasExtensionWord(egl, "EGL_KHR_image_base") ||
                    !HasExtensionWord(egl, "EGL_ANDROID_image_native_buffer") ||
                    !HasExtensionWord(egl, "EGL_ANDROID_get_native_client_buffer")) {
                    entry->Why = "EGL_KHR_image_base / EGL_ANDROID_image_native_buffer / "
                                 "EGL_ANDROID_get_native_client_buffer is not advertised";
                    return *entry;
                }
                if (!HasExtensionWord(gl, "GL_OES_EGL_image")) {
                    entry->Why = "GL_OES_EGL_image is not advertised";
                    return *entry;
                }
                entry->ExternalEssl3 = HasExtensionWord(gl, "GL_OES_EGL_image_external_essl3");
                entry->YuvTarget = HasExtensionWord(gl, "GL_EXT_YUV_target");
                entry->GetNativeClientBuffer = reinterpret_cast<GetNativeClientBufferFn>(
                    g_EGLFuncs.eglGetProcAddress("eglGetNativeClientBufferANDROID"));
                entry->CreateImage =
                    reinterpret_cast<CreateImageKhrFn>(g_EGLFuncs.eglGetProcAddress("eglCreateImageKHR"));
                entry->DestroyImage =
                    reinterpret_cast<DestroyImageKhrFn>(g_EGLFuncs.eglGetProcAddress("eglDestroyImageKHR"));
                entry->TargetTexture2D =
                    reinterpret_cast<ImageTargetFn>(g_EGLFuncs.eglGetProcAddress("glEGLImageTargetTexture2DOES"));
                entry->TargetRenderbuffer = reinterpret_cast<ImageTargetFn>(
                    g_EGLFuncs.eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
                if (!entry->GetNativeClientBuffer || !entry->CreateImage || !entry->DestroyImage ||
                    !entry->TargetTexture2D || !entry->TargetRenderbuffer) {
                    entry->Why = "eglGetProcAddress gave no address for an advertised EGLImage entry point";
                    return *entry;
                }
                entry->Usable = true;
                return *entry;
            }

            void DrainErrors() {
                for (Uint guard = 0; guard < 16 && g_GLESFuncs.glGetError() != GL_NO_ERROR; ++guard) {
                }
            }
        } // namespace
#endif

        struct EglImage {
#if defined(__ANDROID__)
            SI::ImageRef Image;
            void* Handle = nullptr;
            EGLDisplay Display = EGL_NO_DISPLAY;
            Uint64 DisplayTerminations = 0;
            DestroyImageKhrFn Destroy = nullptr;
            // AcquireForSampling's memory, touched only by the session whose twin holds this: the
            // write generation last waited for and the native context the wait was queued in (a
            // GPU wait orders that context alone), and the read frame the image was last noted in.
            Uint64 WaitedGeneration = 0;
            Uint64 WaitedContext = 0;
            Uint64 NotedFrame = 0;
            // A YUV image (ConvertYuvForSampling): an external texture over the EGLImage, the source
            // of the conversion into the texture's own RGBA storage, and the reader's frame the
            // conversion last ran in.
            GLuint ExternalTexture = 0;
            Uint64 ConvertedFrame = 0;

            ~EglImage() {
                // eglDestroyImageKHR needs the display, not a context, so a session's twin teardown
                // (no context current) still releases the image; only exit() and a terminated
                // display - which destroyed the image itself - skip it.
                if (Handle == nullptr || Destroy == nullptr || InProcessExit()) return;
                if (g_displayTerminations.load(std::memory_order_acquire) != DisplayTerminations) return;
                // Texture names are the share group's: deleting one needs any context of it, and
                // without one current the name goes with the display.
                if (ExternalTexture != 0 && t_native.boundContext != EGL_NO_CONTEXT)
                    g_GLESFuncs.glDeleteTextures(1, &ExternalTexture);
                Destroy(Display, Handle);
            }
#endif
        };

#if defined(__ANDROID__)
        EglImageRef CreateEglImage(Uint64 id, String& why) {
            const EntryPoints& entry = ResolvedEntryPoints();
            if (!entry.Usable) {
                why = entry.Why;
                return nullptr;
            }
            SI::ImageRef image = SI::Find(id);
            if (image == nullptr) {
                why = "no live shared image has this id";
                return nullptr;
            }
            if (image->Native == nullptr) {
                why = "the image has no AHardwareBuffer";
                return nullptr;
            }
            const EGLClientBuffer buffer =
                entry.GetNativeClientBuffer(static_cast<const AHardwareBuffer*>(image->Native));
            if (buffer == nullptr) {
                why = "eglGetNativeClientBufferANDROID returned null";
                return nullptr;
            }
            const EGLint attribs[] = {kEglImagePreservedKhr, EGL_TRUE, EGL_NONE};
            void* handle = entry.CreateImage(g_Display, EGL_NO_CONTEXT, kEglNativeBufferAndroid, buffer, attribs);
            if (handle == nullptr) {
                char text[96];
                std::snprintf(text, sizeof(text), "eglCreateImageKHR failed (EGL error 0x%04x)",
                              g_EGLFuncs.eglGetError ? static_cast<unsigned>(g_EGLFuncs.eglGetError()) : 0u);
                why = text;
                return nullptr;
            }
            auto result = MakeShared<EglImage>();
            result->Image = std::move(image);
            result->Handle = handle;
            result->Display = g_Display;
            result->DisplayTerminations = g_displayTerminations.load(std::memory_order_acquire);
            result->Destroy = entry.DestroyImage;
            return result;
        }

        Bool TargetBoundTexture2D(const EglImageRef& image) {
            const EntryPoints& entry = ResolvedEntryPoints();
            if (!entry.Usable || image == nullptr || image->Handle == nullptr) return false;
            DrainErrors();
            entry.TargetTexture2D(GL_TEXTURE_2D, image->Handle);
            return g_GLESFuncs.glGetError() == GL_NO_ERROR;
        }

        namespace {
            // ONE PRESENT TARGET PER IMAGE, PROCESS-WIDE: the EGLImage and a renderbuffer whose
            // storage it is. Every native context is in one share group, so the renderbuffer is
            // one object for every session's context; only the framebuffer it is attached to is
            // per context. Kept until no session holds the image any more (pruned on every present
            // - the cache's own reference is then the only one), with a small cap so a client that
            // cycles through many images cannot pile them up between presents.
            struct PresentTarget {
                EglImageRef Image;
                GLuint Renderbuffer = 0;
                Uint64 LastUse = 0;
                // A present has written the image through this target (its content is a frame).
                Bool Written = false;
            };
            struct PresentTargets {
                std::mutex Mutex;
                std::unordered_map<Uint64, PresentTarget> ById;
                Uint64 UseClock = 0;
            };
            constexpr SizeT kMaxPresentTargets = 8;

            PresentTargets& Targets() {
                static auto* targets = new PresentTargets();
                return *targets;
            }

            // The framebuffer a present draws through, one per native context (container objects
            // are never shared). It carries an attachment only for the duration of a present, so
            // no context's framebuffer keeps a dropped target's renderbuffer alive.
            PerNativeContext<GLuint> g_presentFramebuffers;

            Bool TargetIsCurrent(const PresentTarget& target) {
                return target.Image != nullptr &&
                       target.Image->DisplayTerminations == g_displayTerminations.load(std::memory_order_acquire);
            }

            void DeleteTarget(PresentTarget& target) {
                if (target.Renderbuffer != 0 && TargetIsCurrent(target)) {
                    g_GLESFuncs.glDeleteRenderbuffers(1, &target.Renderbuffer);
                }
                target.Renderbuffer = 0;
                target.Image = nullptr;
            }

            // Caller holds the mutex and has a context of the share group current.
            void PruneLocked(PresentTargets& targets) {
                for (auto it = targets.ById.begin(); it != targets.ById.end();) {
                    if (!TargetIsCurrent(it->second) || it->second.Image->Image.use_count() <= 1) {
                        DeleteTarget(it->second);
                        it = targets.ById.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            GLuint MakeRenderbuffer(const EglImage& image, String& why) {
                GLuint renderbuffer = 0;
                g_GLESFuncs.glGenRenderbuffers(1, &renderbuffer);
                if (renderbuffer == 0) {
                    why = "glGenRenderbuffers gave no name";
                    return 0;
                }
                // Espryt keeps no renderbuffer-binding shadow; put the driver's binding back as found.
                GLint previous = 0;
                g_GLESFuncs.glGetIntegerv(GL_RENDERBUFFER_BINDING, &previous);
                DrainErrors();
                g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
                ResolvedEntryPoints().TargetRenderbuffer(GL_RENDERBUFFER, image.Handle);
                const GLenum error = g_GLESFuncs.glGetError();
                g_GLESFuncs.glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(previous));
                if (error != GL_NO_ERROR) {
                    char text[96];
                    std::snprintf(text, sizeof(text), "glEGLImageTargetRenderbufferStorageOES -> 0x%04x",
                                  static_cast<unsigned>(error));
                    why = text;
                    g_GLESFuncs.glDeleteRenderbuffers(1, &renderbuffer);
                    return 0;
                }
                return renderbuffer;
            }

            // Everything the current context has submitted, waited out on the CPU: the fallback for
            // a driver that cannot hand a fence to another session.
            Bool WaitForCompletion() {
                if (g_GLESFuncs.glFenceSync && g_GLESFuncs.glClientWaitSync && g_GLESFuncs.glDeleteSync) {
                    if (GLsync fence = g_GLESFuncs.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)) {
                        constexpr GLuint64 kSliceNs = 1000ull * 1000 * 1000;
                        GLenum status = GL_TIMEOUT_EXPIRED;
                        GLbitfield flags = GL_SYNC_FLUSH_COMMANDS_BIT;
                        for (Uint slice = 0; slice < 10 && status == GL_TIMEOUT_EXPIRED; ++slice) {
                            status = g_GLESFuncs.glClientWaitSync(fence, flags, kSliceNs);
                            flags = 0;
                        }
                        g_GLESFuncs.glDeleteSync(fence);
                        if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) return true;
                        if (status == GL_TIMEOUT_EXPIRED) {
                            MGLOG_E("Shared images: the submitted work did not complete within 10 s");
                            return false;
                        }
                        // GL_WAIT_FAILED: glFinish is the same promise without the object.
                    }
                }
                g_GLESFuncs.glFinish();
                return true;
            }

            // How long a CPU wait on another session's fence may take before the order is given up
            // on (logged): a session whose GPU work never completes must not wedge its peers.
            constexpr Uint32 kFenceCpuWaitMs = 2000;

            void CloseFd(int fd) {
                if (fd >= 0) ::close(fd);
            }

            // The current context's LATER commands wait for `fence` (a sync_file) on the GPU; a CPU
            // wait when the driver cannot. Takes ownership of `fence`: a native fence sync owns it
            // from a successful create on, every other path closes it here.
            void WaitForFenceBeforeLaterCommands(int fence) {
                if (fence < 0) return;
                const EntryPoints& entry = ResolvedEntryPoints();
                if (entry.FencesUsable) {
                    const EGLint attribs[] = {kEglSyncNativeFenceFdAndroid, fence, EGL_NONE};
                    if (void* sync = entry.CreateSync(g_Display, kEglSyncNativeFenceAndroid, attribs)) {
                        if (entry.WaitSync(g_Display, sync, 0) != EGL_TRUE) {
                            constexpr Uint64 kWaitNs = Uint64{kFenceCpuWaitMs} * 1000 * 1000;
                            if (entry.ClientWaitSync(g_Display, sync, 0, kWaitNs) != kEglConditionSatisfiedKhr) {
                                MGLOG_E_ONCE("Shared images: a peer session's fence could not be waited for");
                            }
                        }
                        // A pending server wait keeps the sync alive past this (EGL_KHR_wait_sync).
                        entry.DestroySync(g_Display, sync);
                        return;
                    }
                }
                if (!SI::SyncFile::Wait(fence, kFenceCpuWaitMs)) {
                    MGLOG_E_ONCE("Shared images: a peer session's fence did not signal within %u ms",
                                 static_cast<unsigned>(kFenceCpuWaitMs));
                }
                CloseFd(fence);
            }

            // A sync_file for everything the current context has submitted so far (flushed, so it
            // will signal), in `*fence`. False when the driver cannot export one; the caller then
            // waits on the CPU and publishes -1.
            Bool ExportFence(int* fence) {
                *fence = -1;
                const EntryPoints& entry = ResolvedEntryPoints();
                if (!entry.FencesUsable) return false;
                const EGLint attribs[] = {EGL_NONE};
                void* sync = entry.CreateSync(g_Display, kEglSyncNativeFenceAndroid, attribs);
                if (sync == nullptr) return false;
                // The fence has no descriptor until its command reaches the driver's queue.
                g_GLESFuncs.glFlush();
                const EGLint fd = entry.DupNativeFenceFd(g_Display, sync);
                entry.DestroySync(g_Display, sync);
                if (fd < 0) return false;
                *fence = fd;
                return true;
            }

            // ONE READ FRAME PER SESSION: what the session has sampled since its last frame
            // boundary. A session is served by exactly one apply thread, so the thread owns it -
            // nothing about it is process-wide, and a session's end (its thread's) drops it. `Id`
            // is drawn from one counter across all sessions and renewed at every boundary, so an
            // EglImage's NotedFrame is a once-per-frame gate no other session's frame can match.
            struct ReadFrame {
                Uint64 Id = 0;
                SI::ReadTracker Tracker;
                // The session ends frames at publishing flushes (PublishPendingAccesses): it writes
                // images, so its first use of one in a frame waits for that image's pending reads.
                Bool ImplicitSync = false;
                // When a YUV image is converted again (ConvertYuvForSampling): renewed at EVERY
                // boundary - present, publishing flush, and each native fence whatever the session
                // is - since an offscreen producer (a browser's GPU process) may have no other.
                Uint64 YuvEpoch = 0;
            };
            std::atomic<Uint64> g_readFrameIds{0};

            ReadFrame& CurrentReadFrame() {
                thread_local ReadFrame frame;
                if (frame.Id == 0) frame.Id = g_readFrameIds.fetch_add(1, std::memory_order_relaxed) + 1;
                if (frame.YuvEpoch == 0) frame.YuvEpoch = g_readFrameIds.fetch_add(1, std::memory_order_relaxed) + 1;
                return frame;
            }

            void RenewYuvEpoch(ReadFrame& frame) {
                frame.YuvEpoch = g_readFrameIds.fetch_add(1, std::memory_order_relaxed) + 1;
            }
        } // namespace

        void AcquireForSampling(const EglImageRef& image) {
            if (image == nullptr || image->Image == nullptr) return;
            EglImage& state = *image;
            // Steady state: one lock and a compare (the write fence, once signaled, is dropped by
            // the registry and nothing is duplicated any more).
            Uint64 generation = 0;
            int fence = SI::DupWriteFence(*state.Image, &generation);
            const Uint64 context = CurrentNativeContextSerial();
            if (generation != state.WaitedGeneration || context != state.WaitedContext) {
                state.WaitedGeneration = generation;
                state.WaitedContext = context;
                WaitForFenceBeforeLaterCommands(fence);
                fence = -1;
            }
            CloseFd(fence);
            ReadFrame& frame = CurrentReadFrame();
            if (state.NotedFrame != frame.Id) {
                // A session that renders into images (it ends frames with a publishing flush) may be
                // about to overwrite this one: what other sessions still sample goes first.
                if (frame.ImplicitSync) {
                    for (const int read : SI::DupPendingReadFences(*state.Image)) WaitForFenceBeforeLaterCommands(read);
                }
                frame.Tracker.NoteRead(state.Image);
                state.NotedFrame = frame.Id;
            }
        }

        Bool IsYuv(const EglImageRef& image) {
            return image != nullptr && image->Image != nullptr && SI::FourccIsYuv(image->Image->Fourcc);
        }

        namespace {
            // THE CONVERSION PASS: one triangle over the texture, sampling the image through an
            // external texture. Programs are the share group's and rebuilt with a new context
            // generation; the framebuffer and vertex array are per context (never shared).
            struct YuvConversion {
                Uint ContextGeneration = ~0u;
                GLuint ExternalProgram = 0; // the driver's conversion (samplerExternalOES)
                GLuint TargetProgram = 0;   // the import's own (GL_EXT_YUV_target)
                GLint TargetMatrix = -1;
                GLint TargetOffset = -1;
                Bool Failed = false;
                NativeContextName Framebuffer;
                NativeContextName VertexArray;
            };
            YuvConversion& Conversion() {
                static auto* conversion = new YuvConversion();
                return *conversion;
            }

            constexpr const char* kYuvVertexSource =
                "#version 300 es\n"
                "out vec2 vUv;\n"
                "void main() {\n"
                "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
                "    vUv = p;\n"
                "}\n";
            constexpr const char* kYuvExternalFragmentSource =
                "#version 300 es\n"
                "#extension GL_OES_EGL_image_external_essl3 : require\n"
                "precision highp float;\n"
                "uniform samplerExternalOES uSource;\n"
                "in vec2 vUv;\n"
                "out vec4 oColor;\n"
                "void main() { oColor = vec4(texture(uSource, vUv).rgb, 1.0); }\n";
            constexpr const char* kYuvTargetFragmentSource =
                "#version 300 es\n"
                "#extension GL_EXT_YUV_target : require\n"
                "precision highp float;\n"
                "uniform __samplerExternal2DY2YEXT uSource;\n"
                "uniform mat3 uMatrix;\n"
                "uniform vec3 uOffset;\n"
                "in vec2 vUv;\n"
                "out vec4 oColor;\n"
                "void main() {\n"
                "    vec3 ycbcr = texture(uSource, vUv).xyz;\n"
                "    oColor = vec4(clamp(uMatrix * (ycbcr - uOffset), 0.0, 1.0), 1.0);\n"
                "}\n";

            GLuint BuildYuvProgram(const char* fragmentSource) {
                const GLuint vertexShader = g_GLESFuncs.glCreateShader(GL_VERTEX_SHADER);
                const GLuint fragmentShader = g_GLESFuncs.glCreateShader(GL_FRAGMENT_SHADER);
                GLint linked = GL_FALSE;
                GLuint program = 0;
                if (vertexShader != 0 && fragmentShader != 0) {
                    g_GLESFuncs.glShaderSource(vertexShader, 1, &kYuvVertexSource, nullptr);
                    g_GLESFuncs.glCompileShader(vertexShader);
                    g_GLESFuncs.glShaderSource(fragmentShader, 1, &fragmentSource, nullptr);
                    g_GLESFuncs.glCompileShader(fragmentShader);
                    program = g_GLESFuncs.glCreateProgram();
                    if (program != 0) {
                        g_GLESFuncs.glAttachShader(program, vertexShader);
                        g_GLESFuncs.glAttachShader(program, fragmentShader);
                        g_GLESFuncs.glLinkProgram(program);
                        g_GLESFuncs.glGetProgramiv(program, GL_LINK_STATUS, &linked);
                    }
                }
                if (vertexShader != 0) g_GLESFuncs.glDeleteShader(vertexShader);
                if (fragmentShader != 0) g_GLESFuncs.glDeleteShader(fragmentShader);
                if (linked != GL_TRUE) {
                    if (program != 0) g_GLESFuncs.glDeleteProgram(program);
                    return 0;
                }
                return program;
            }

            // MOBILEGL_YUV_DRIVER_CONVERSION=1: always the driver's conversion (samplerExternalOES),
            // even where GL_EXT_YUV_target would let the import's hints decide.
            Bool DriverConversionForced() {
                static const Bool forced = [] {
                    const char* value = std::getenv("MOBILEGL_YUV_DRIVER_CONVERSION");
                    return value != nullptr && value[0] == '1';
                }();
                return forced;
            }
        } // namespace

        void ConvertYuvForSampling(const EglImageRef& image, GLuint target, Uint32 width, Uint32 height) {
            if (!IsYuv(image) || target == 0 || width == 0 || height == 0) return;
            EglImage& state = *image;
            ReadFrame& frame = CurrentReadFrame();
            if (state.ConvertedFrame == frame.YuvEpoch) return;
            // Whatever happens below, this frame does not try again.
            state.ConvertedFrame = frame.YuvEpoch;
            const EntryPoints& entry = ResolvedEntryPoints();
            if (!entry.Usable || !entry.ExternalEssl3) {
                MGLOG_E_ONCE("Shared images: a YUV image cannot be sampled here (%s)",
                             entry.Usable ? "no GL_OES_EGL_image_external_essl3" : entry.Why.c_str());
                return;
            }
            // THE CPU-COPY FALLBACK, for a buffer this server did not allocate (SharedImageRegistry.h).
            String why;
            if (state.Image->IsForeign() && !SI::RefreshForeign(*state.Image, why)) {
                MGLOG_E_ONCE("Shared images: foreign YUV image %llu could not be refreshed: %s",
                             static_cast<unsigned long long>(state.Image->Id), why.c_str());
            }

            YuvConversion& conversion = Conversion();
            if (conversion.ContextGeneration != g_backendContextGeneration) {
                conversion.ExternalProgram = 0;
                conversion.TargetProgram = 0;
                conversion.TargetMatrix = conversion.TargetOffset = -1;
                conversion.Failed = false;
                conversion.ContextGeneration = g_backendContextGeneration;
            }
            if (conversion.Failed) return;
            const Bool ownConversion = entry.YuvTarget && !DriverConversionForced();
            GLuint& program = ownConversion ? conversion.TargetProgram : conversion.ExternalProgram;
            if (program == 0) {
                program = BuildYuvProgram(ownConversion ? kYuvTargetFragmentSource : kYuvExternalFragmentSource);
                if (program == 0) {
                    conversion.Failed = true;
                    MGLOG_E_ONCE("Shared images: the YUV conversion program could not be built");
                    return;
                }
                if (ownConversion) {
                    conversion.TargetMatrix = g_GLESFuncs.glGetUniformLocation(program, "uMatrix");
                    conversion.TargetOffset = g_GLESFuncs.glGetUniformLocation(program, "uOffset");
                }
                MGLOG_I("Shared images: YUV images convert through %s",
                        ownConversion ? "GL_EXT_YUV_target with the import's colour hints"
                                      : "the driver's external-sampler conversion");
            }
            if (conversion.Framebuffer == 0) g_GLESFuncs.glGenFramebuffers(1, &conversion.Framebuffer);
            if (conversion.VertexArray == 0) g_GLESFuncs.glGenVertexArrays(1, &conversion.VertexArray);
            if (conversion.Framebuffer == 0 || conversion.VertexArray == 0) return;

            // Every binding goes through Espryt's shadows or a guard that puts it back.
            ScopedEmulationDrawState drawState; // also activates TextureImpl::TempTextureUnit
            ScopedFramebufferBinding binding(/*saveRead=*/false, /*saveDraw=*/true);
            DrainErrors();
            if (state.ExternalTexture == 0) {
                g_GLESFuncs.glGenTextures(1, &state.ExternalTexture);
                g_GLESFuncs.glBindTexture(GL_TEXTURE_EXTERNAL_OES, state.ExternalTexture);
                entry.TargetTexture2D(GL_TEXTURE_EXTERNAL_OES, state.Handle);
                g_GLESFuncs.glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                g_GLESFuncs.glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                g_GLESFuncs.glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                g_GLESFuncs.glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                const GLenum error = g_GLESFuncs.glGetError();
                if (error != GL_NO_ERROR) {
                    MGLOG_E_ONCE("Shared images: YUV image %llu cannot be bound as an external texture (0x%04x)",
                                 static_cast<unsigned long long>(state.Image->Id), static_cast<unsigned>(error));
                }
            } else {
                g_GLESFuncs.glBindTexture(GL_TEXTURE_EXTERNAL_OES, state.ExternalTexture);
            }
            FramebufferImpl::BindFramebufferId(GL_DRAW_FRAMEBUFFER, conversion.Framebuffer);
            g_GLESFuncs.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
            const GLenum status = g_GLESFuncs.glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
            if (status == GL_FRAMEBUFFER_COMPLETE) {
                VertexArrayImpl::BindBackendVAOId(conversion.VertexArray);
                g_GLESFuncs.glUseProgram(program);
                g_GLESFuncs.glUniform1i(g_GLESFuncs.glGetUniformLocation(program, "uSource"),
                                        static_cast<GLint>(TextureImpl::TempTextureUnit));
                if (ownConversion) {
                    // A Y2Y sample is the stored code normalized as the format stores it: 8 bits, or
                    // P010's 10 in the high end of 16.
                    const double sampleMax = state.Image->Fourcc == SI::kFourccP010 ? 65535.0 / 64.0 : 255.0;
                    const auto c = SI::Yuv::ConversionFor(SI::GetYuvHints(*state.Image), sampleMax);
                    g_GLESFuncs.glUniformMatrix3fv(conversion.TargetMatrix, 1, GL_FALSE, c.Matrix);
                    g_GLESFuncs.glUniform3fv(conversion.TargetOffset, 1, c.Offset);
                }
                g_GLESFuncs.glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
                g_GLESFuncs.glDrawArrays(GL_TRIANGLES, 0, 3);
                const GLenum error = g_GLESFuncs.glGetError();
                if (error != GL_NO_ERROR) {
                    MGLOG_E_ONCE("Shared images: the YUV conversion draw failed (0x%04x)", static_cast<unsigned>(error));
                }
            } else {
                MGLOG_E_ONCE("Shared images: a YUV conversion target is not renderable (0x%04x)",
                             static_cast<unsigned>(status));
            }
            g_GLESFuncs.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            g_GLESFuncs.glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
        }

        Bool PublishPendingAccesses() {
            ReadFrame& frame = CurrentReadFrame();
            frame.ImplicitSync = true;
            RenewYuvEpoch(frame);
            // Sub-data waiting for a sync that nothing of this session would otherwise trigger.
            if (t_native.boundContext != EGL_NO_CONTEXT) TextureImpl::WritePendingSharedImageUploads();
            if (frame.Tracker.Pending() == 0) return true;
            int fence = -1;
            if (t_native.boundContext != EGL_NO_CONTEXT && !ExportFence(&fence)) {
                // No fence to publish: the work is waited out here, and published as completed.
                (void)WaitForCompletion();
            }
            frame.Tracker.PublishFrameAsWrite(fence);
            frame.Id = g_readFrameIds.fetch_add(1, std::memory_order_relaxed) + 1;
            return true;
        }

        Bool ExportNativeFence(int* fence) {
            *fence = -1;
            ReadFrame& frame = CurrentReadFrame();
            RenewYuvEpoch(frame);
            // No context current: nothing of this session is in flight to fence.
            if (t_native.boundContext == EGL_NO_CONTEXT) return true;
            TextureImpl::WritePendingSharedImageUploads();
            int exported = -1;
            if (!ExportFence(&exported)) {
                // No descriptor to give: the work is waited out, so the caller may report it done.
                exported = -1;
                (void)WaitForCompletion();
            }
            // A session that writes images (one that ends frames at flushes) ends one here too; a
            // compositor's reads stay for its swap, which comes after the fence's work anyway.
            if (frame.ImplicitSync && frame.Tracker.Pending() != 0) {
                frame.Tracker.PublishFrameAsWrite(exported >= 0 ? ::dup(exported) : -1);
                frame.Id = g_readFrameIds.fetch_add(1, std::memory_order_relaxed) + 1;
            }
            *fence = exported;
            return true;
        }

        void PublishPendingReads() {
            ReadFrame& frame = CurrentReadFrame();
            RenewYuvEpoch(frame);
            if (frame.Tracker.Pending() == 0) return;
            int fence = -1;
            // No context current: the reads' context is gone (and its work with it), and there is
            // nothing to fence or wait from - the frame is only forgotten.
            if (t_native.boundContext != EGL_NO_CONTEXT && !ExportFence(&fence)) {
                // FALLBACK: there is no fence to hand a writer, so the frame's reads are waited out
                // here instead - once this session moves on (and, say, releases the buffer to its
                // producer), nothing it submitted still samples the image.
                (void)WaitForCompletion();
            }
            // A session that writes images publishes every boundary as a write: an image it rendered
            // into must not leave the frame without its generation moving.
            if (frame.ImplicitSync)
                frame.Tracker.PublishFrameAsWrite(fence);
            else
                frame.Tracker.PublishFrame(fence);
            frame.Id = g_readFrameIds.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        Bool BlitDefaultFramebufferTo(const SharedImageView& view, const MG_Util::Damage::Region& region) {
            if (view.Id == 0 || view.Width == 0 || view.Height == 0 || view.NativeBuffer == nullptr) return false;
            if (!IsBackendContextCurrentOnThisThread()) {
                MGLOG_E_ONCE("Shared image present: no backend context is current on the calling thread");
                return false;
            }
            if (LatchIfDeviceLost("shared-image-present")) return false;
            const EGLSurface surface = CurrentContextDrawSurface();
            EGLint sourceWidth = 0;
            EGLint sourceHeight = 0;
            if (surface == EGL_NO_SURFACE || !g_EGLFuncs.eglQuerySurface ||
                !g_EGLFuncs.eglQuerySurface(g_Display, surface, EGL_WIDTH, &sourceWidth) ||
                !g_EGLFuncs.eglQuerySurface(g_Display, surface, EGL_HEIGHT, &sourceHeight) || sourceWidth <= 0 ||
                sourceHeight <= 0) {
                MGLOG_E_ONCE("Shared image present: the current context has no default framebuffer to copy");
                return false;
            }
            const SI::ImageRef image = SI::Find(view.Id);
            if (image == nullptr) {
                MGLOG_E_ONCE("Shared image present: image %llu is no longer live",
                             static_cast<unsigned long long>(view.Id));
                return false;
            }

            // WRITE AFTER READ: the frames that still sample the image finish before the copy
            // overwrites it - queued on this context's GPU timeline, or waited out on the CPU when
            // the driver has no fence to queue. Outside the targets' lock, which every session's
            // present takes.
            if (ResolvedEntryPoints().FencesUsable) {
                for (const int fence : SI::DupPendingReadFences(*image)) WaitForFenceBeforeLaterCommands(fence);
            } else if (!SI::WaitForReads(*image, kFenceCpuWaitMs)) {
                MGLOG_E_ONCE("Shared image present: image %llu's readers did not finish within %u ms",
                             static_cast<unsigned long long>(view.Id), static_cast<unsigned>(kFenceCpuWaitMs));
            }

            Bool copied = false;
            {
                auto& targets = Targets();
                const std::lock_guard<std::mutex> lock(targets.Mutex);
                PruneLocked(targets);
                auto it = targets.ById.find(view.Id);
                if (it != targets.ById.end() && it->second.Image->Image->Native != view.NativeBuffer) {
                    DeleteTarget(it->second);
                    targets.ById.erase(it);
                    it = targets.ById.end();
                }
                if (it == targets.ById.end()) {
                    String why;
                    PresentTarget target;
                    target.Image = CreateEglImage(view.Id, why);
                    if (target.Image != nullptr) target.Renderbuffer = MakeRenderbuffer(*target.Image, why);
                    if (target.Renderbuffer == 0) {
                        MGLOG_E_ONCE("Shared image present: image %llu cannot be rendered to: %s",
                                     static_cast<unsigned long long>(view.Id), why.c_str());
                        return false;
                    }
                    while (targets.ById.size() >= kMaxPresentTargets) {
                        auto oldest = targets.ById.begin();
                        for (auto candidate = targets.ById.begin(); candidate != targets.ById.end(); ++candidate) {
                            if (candidate->second.LastUse < oldest->second.LastUse) oldest = candidate;
                        }
                        DeleteTarget(oldest->second);
                        targets.ById.erase(oldest);
                    }
                    it = targets.ById.emplace(view.Id, std::move(target)).first;
                }
                it->second.LastUse = ++targets.UseClock;
                // A target written before keeps what it was given; one new to this process is
                // copied whole whatever the client asked (it cannot know the image's history here).
                const Bool wholeTarget = !it->second.Written;
                it->second.Written = true;

                GLuint& framebuffer = g_presentFramebuffers.Current();
                if (framebuffer == 0) g_GLESFuncs.glGenFramebuffers(1, &framebuffer);
                if (framebuffer == 0) {
                    MGLOG_E_ONCE("Shared image present: glGenFramebuffers gave no name");
                    return false;
                }

                // Every binding goes through Espryt's shadows (BindFramebufferId, the scissor test's
                // render-state shadow) and is put back by the guards, so the application's state and
                // the caches that describe it are what they were. A blit is scissored like a draw.
                ScopedScissorDisable scissorOff;
                ScopedFramebufferBinding binding(/*saveRead=*/true, /*saveDraw=*/true);
                FramebufferImpl::BindFramebufferId(GL_READ_FRAMEBUFFER, 0);
                FramebufferImpl::BindFramebufferId(GL_DRAW_FRAMEBUFFER, framebuffer);
                g_GLESFuncs.glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                                      it->second.Renderbuffer);
                const GLenum status = g_GLESFuncs.glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
                if (status == GL_FRAMEBUFFER_COMPLETE) {
                    // The default framebuffer's read buffer is context state the application may
                    // have set to GL_NONE; the copy reads the back buffer either way.
                    GLint readBuffer = GL_BACK;
                    g_GLESFuncs.glGetIntegerv(GL_READ_BUFFER, &readBuffer);
                    if (readBuffer != GL_BACK) g_GLESFuncs.glReadBuffer(GL_BACK);
                    DrainErrors();
                    const Bool sameExtent = static_cast<Uint32>(sourceWidth) == view.Width &&
                                            static_cast<Uint32>(sourceHeight) == view.Height;
                    // TOP ROW FIRST: GL's top row (y = height - 1) lands in the image's first memory
                    // row, which an AHardwareBuffer-backed renderbuffer addresses as y = 0.
                    //
                    // Only where the image differs from the frame: the client keeps, per image, the
                    // damage it missed (BufferDamageTracker), and a partial blit keeps the rest of
                    // the renderbuffer as GL promises (checked texel for texel against the frame on
                    // Adreno). What once left old texels around the rectangles was the scissor
                    // guard above trusting another session's shadow: the copy was clipped, and
                    // the missed-damage bookkeeping then never brought those texels over again.
                    if (sameExtent && !wholeTarget && !region.IsFull()) {
                        // Each rectangle flipped the same way (an empty region copies nothing: the
                        // image already is the frame).
                        for (const MG_Util::Damage::FlippedCopy& copy :
                             MG_Util::Damage::FlippedCopies(region, sourceWidth, sourceHeight)) {
                            const MG_Util::Damage::Rect& from = copy.Source;
                            const MG_Util::Damage::Rect& to = copy.Destination;
                            g_GLESFuncs.glBlitFramebuffer(from.X, from.Y, from.X + from.Width, from.Y + from.Height,
                                                          to.X, to.Y + to.Height, to.X + to.Width, to.Y,
                                                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
                        }
                    } else {
                        g_GLESFuncs.glBlitFramebuffer(0, 0, sourceWidth, sourceHeight, 0,
                                                      static_cast<GLint>(view.Height), static_cast<GLint>(view.Width),
                                                      0, GL_COLOR_BUFFER_BIT, sameExtent ? GL_NEAREST : GL_LINEAR);
                    }
                    const GLenum error = g_GLESFuncs.glGetError();
                    copied = error == GL_NO_ERROR;
                    if (readBuffer != GL_BACK) g_GLESFuncs.glReadBuffer(static_cast<GLenum>(readBuffer));
                    if (!copied) {
                        MGLOG_E_ONCE("Shared image present: glBlitFramebuffer %dx%d -> %ux%u failed (0x%04x)",
                                     sourceWidth, sourceHeight, view.Width, view.Height,
                                     static_cast<unsigned>(error));
                    }
                } else {
                    MGLOG_E_ONCE("Shared image present: image %llu's framebuffer is incomplete (0x%04x)",
                                 static_cast<unsigned long long>(view.Id), static_cast<unsigned>(status));
                }
                g_GLESFuncs.glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, 0);
            }
            if (!copied) return false;

            // The copy's completion goes to the readers as a fence and the present returns at once;
            // a reader that binds the image waits for it on its own GPU. Without fence export the
            // copy is waited out here and published as already complete.
            int fence = -1;
            if (!ExportFence(&fence) && !WaitForCompletion()) return false;
            SI::PublishWrite(*image, fence);
            // A session that samples images and presents into one (a nested compositor) closes its
            // read frame here too.
            PublishPendingReads();
            return true;
        }

        void ForgetPresentTargetsAfterTerminate() {
            auto& targets = Targets();
            const std::lock_guard<std::mutex> lock(targets.Mutex);
            // TargetIsCurrent is already false for every entry, so nothing reaches the driver.
            for (auto& entry : targets.ById) DeleteTarget(entry.second);
            targets.ById.clear();
        }
#else
        EglImageRef CreateEglImage(Uint64 id, String& why) {
            (void)id;
            why = "shared images are imported through AHardwareBuffer, which only Android has";
            return nullptr;
        }
        Bool TargetBoundTexture2D(const EglImageRef& image) {
            (void)image;
            return false;
        }
        Bool BlitDefaultFramebufferTo(const SharedImageView& view, const MG_Util::Damage::Region& region) {
            (void)view, (void)region;
            return false;
        }
        void AcquireForSampling(const EglImageRef& image) { (void)image; }

        Bool IsYuv(const EglImageRef& image) {
            (void)image;
            return false;
        }

        void ConvertYuvForSampling(const EglImageRef& image, GLuint target, Uint32 width, Uint32 height) {
            (void)image, (void)target, (void)width, (void)height;
        }
        void PublishPendingReads() {}
        Bool PublishPendingAccesses() { return true; }
        Bool ExportNativeFence(int* fence) {
            *fence = -1;
            return true;
        }
        void ForgetPresentTargetsAfterTerminate() {}
#endif
    } // namespace SharedImageImpl
#endif
} // namespace MobileGL::MG_Backend::DirectGLES
