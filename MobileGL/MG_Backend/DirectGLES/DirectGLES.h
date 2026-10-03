// MobileGL - MobileGL/MG_Backend/DirectGLES/DirectGLES.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Backend/BackendObject.h>
#include <MG_State/GLState/FramebufferState/FramebufferObject.h>
#include <MG_State/GLState/TextureState/TextureState.h>
#include <MG_State/GLState/SamplerState/SamplerObject.h>
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>

#define CallAndCheck(operation)                                                                                        \
    MGLOG_D("Call GLES func: %s", #operation);                                                                         \
    operation Utils::CheckGLESError();

namespace MobileGL::MG_Backend::DirectGLES {
    // Re-establishes the frontend texture-unit bindings on the native ES context.
    // Content uploads use scratch bindings, so draws and dispatches call this after
    // texture synchronization.
    void BindCurrentTextures();
    // Driver-side color/depth level readback. Returns tightly packed, owned
    // bytes in the requested pair; never consults a frontend texture or PACK/PBO.
    // The caller supplies identity and extent, and whether the SOURCE allocation
    // already uses an image carrier.
    Bool ReadTextureLevelTight(GLuint texture, TextureTarget target, TextureUploadTarget uploadTarget,
                               TextureInternalFormat logicalFormat, GLint level, const IntVec3& logicalExtent,
                               Bool sourceUsesImageCarrier, GLenum format, GLenum type, Vector<Uint8>& bytes);
#if MOBILEGL_BUILD_DISAGGREGATED
    Bool ReadTextureImageWire(const MG_Pipe::MGPReadbackInfo& info, Vector<Uint8>& bytes);
    // Whether the NATIVE texture currently holds `level` of `uploadTarget` at `logicalExtent`
    // (glGetTexLevelParameteriv). False for a level the driver never allocated - one defined after
    // the texture's last sync - so a re-mint does not read what is not there. A query the driver
    // refuses answers true, keeping the caller on its readback path.
    Bool NativeTextureLevelHasExtent(GLuint texture, TextureTarget target, TextureUploadTarget uploadTarget,
                                     GLint level, const IntVec3& logicalExtent);
#endif
    void ClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
    void ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value);
    void ClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value);
    void ClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value);
    void Clear(GLbitfield mask);
    void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices);
    void DrawArrays(GLenum mode, GLint first, GLsizei count);
    void DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices, GLint basevertex);
    void MultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount);
    void MultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                           GLsizei drawcount);
    void MultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                                     GLsizei drawcount, const GLint* basevertex);
    void MultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, GLsizei stride);
    void MultiDrawElementsIndirectCount(GLenum mode, GLenum type, const void* indirect, GLintptr drawcount,
                                        GLsizei maxdrawcount, GLsizei stride);
    void MultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride);
    void MultiDrawArraysIndirectCount(GLenum mode, const void* indirect, GLintptr drawcount, GLsizei maxdrawcount,
                                      GLsizei stride);
    void DrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                     const void* indices, GLint basevertex);
    void DrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices);
    void DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                     GLsizei instancecount, GLint basevertex, GLuint baseinstance);
    void DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                         GLsizei instancecount, GLint basevertex);
    void DrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                           GLsizei instancecount, GLuint baseinstance);
    void DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount);
    void DrawElementsIndirect(GLenum mode, GLenum type, const void* indirect);
    void DrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                         GLuint baseinstance);
    void DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);
    void DrawArraysIndirect(GLenum mode, const void* indirect);
    void ClearNamedFramebufferfv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                 GLenum buffer, GLint drawbuffer, const GLfloat* value);
    void ClearNamedFramebufferfi(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                 GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
    void ClearNamedFramebufferiv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                 GLenum buffer, GLint drawbuffer, const GLint* value);
    void ClearNamedFramebufferuiv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                  GLenum buffer, GLint drawbuffer, const GLuint* value);
    void BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
                         GLint dstY1, GLbitfield mask, GLenum filter);
    void BlitNamedFramebuffer(const SharedPtr<MG_State::GLState::FramebufferObject>& readFramebuffer,
                              const SharedPtr<MG_State::GLState::FramebufferObject>& drawFramebuffer,
                              GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                              GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                              GLbitfield mask, GLenum filter);
    void CopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                        GLsizei height, GLint border);
    void CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width,
                           GLsizei height);
    void CopyImageSubData(const CopyImageEndpoint& src,
                          GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                          const CopyImageEndpoint& dst,
                          GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                          GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth);
    void GenerateMipmap(GLenum target);
    const GLubyte* GetString(GLenum name);
    void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
    void GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels);
    void DispatchCompute(GLuint numGroupsX, GLuint numGroupsY, GLuint numGroupsZ);
    void DispatchComputeIndirect(GLintptr indirect);
    void MemoryBarrier(GLbitfield barriers);
    void MemoryBarrierByRegion(GLbitfield barriers);
    void BindImageTexture(GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access,
                          GLenum format);
    void GetIntegeri_v(GLenum target, GLuint index, GLint* data);
    void ShaderStorageBlockBinding(GLuint program, const GLchar* storageBlockName, GLuint storageBlockBinding);
    Bool InitWindowSurface(NativeWindowType window);
    Bool InitPbufferSurface(EGLint width, EGLint height);
    // The calling thread's session's current native draw surface (the one the last Init*Surface
    // created, or the last BindSessionSurface named).
    EGLSurface CurrentSessionDrawSurface();
    // The surface the calling thread's ACTIVE context last drew to (EGL_NO_SURFACE for none).
    EGLSurface CurrentContextDrawSurface();
    // EGL_BUFFER_AGE_EXT of the session's current draw surface for the next frame: the driver's
    // answer for a window surface, 1 for a pbuffer that has been presented (a pbuffer's content stays
    // across swaps), 0 otherwise. A driver whose buffer age is only EGL_KHR_partial_update's keeps
    // nothing inside the frame's damage region - the whole surface unless one is set - so it is asked
    // only when `damageRegionFollows` (SetCurrentDrawDamageRegion before the first draw).
    Int32 CurrentDrawBufferAge(Bool damageRegionFollows);
    // eglSetDamageRegionKHR(region) on the session's current window surface (a pbuffer, or a driver
    // without EGL_KHR_partial_update, ignores it). The driver wants the age asked first in the frame;
    // when the client was answered without asking it, it is asked here.
    Bool SetCurrentDrawDamageRegion(const MG_Util::Damage::Region& region);
    // Makes an EXISTING native surface of the calling thread's session the one it draws to, and
    // binds it to the active context. False for a surface the session does not own.
    Bool BindSessionSurface(EGLSurface surface);
    // The same selection without the bind (the caller binds - MakeCurrent - unconditionally).
    Bool SelectSessionSurface(EGLSurface surface);
    // Destroys one native surface of the calling thread's session - and only it; the session's
    // contexts and other surfaces are untouched.
    void DestroySessionSurface(EGLSurface surface);
    // A new pbuffer of the calling thread's session, created with the active context's config and
    // bound to nothing (EGL: creating a surface changes no binding).
    EGLSurface CreateSessionPbuffer(EGLint width, EGLint height);
    // Every context of the session that drew to `previous` draws to `replacement` instead, and
    // `previous` is destroyed (a pbuffer-backed window's resize).
    void ReplaceSessionSurface(EGLSurface previous, EGLSurface replacement);
    // The server window going away (screen off): every context of the session that drew to the
    // window surface `windowSurface` draws to a new 1x1 placeholder pbuffer instead, and the window
    // surface is destroyed - no longer current anywhere once this returns. The contexts and their
    // objects stay. Returns the placeholder, or EGL_NO_SURFACE (nothing changed) when none could be
    // made.
    EGLSurface SuspendSessionWindowSurface(EGLSurface windowSurface);
    // The way back: a window surface on `window` replaces `placeholder` (EGL_NO_SURFACE: none to
    // replace) for every context that drew to it, is bound, and its extent is published as a
    // creation publishes it. Returns the new surface, or EGL_NO_SURFACE.
    EGLSurface ResumeSessionWindowSurface(EGLSurface placeholder, NativeWindowType window);
    // The window under `surface` changed size: its extent (the display's) is published again, at once.
    Bool RepublishWindowSurfaceShape(EGLSurface surface, NativeWindowType window);
    Bool MakeCurrent();
    Bool ReleaseCurrent();
    // True when the backend ES context is current on the calling thread, i.e.
    // immediate buffer ops may issue GL calls right now.
    Bool IsBackendContextCurrentOnThisThread();
#if MOBILEGL_BUILD_DISAGGREGATED
    // DEVICE LOSS, Espryt's half (Magma's is VulkanRenderer::LatchWireDeviceLoss). Asks the
    // driver whether the calling thread's context was reset (glGetGraphicsResetStatus); a reset -
    // or the debug knob MGPipeDebugDeviceLossDue - latches THIS session through
    // MGPipeSessionLatch and answers true, and the caller returns from its verb without issuing
    // more GL. Asked at the frame boundaries (Present, a shared-image present), before a readback,
    // and by the apply loop each time it wakes (LatchIfGpuFaulted), never per draw. The contexts ask
    // for reset notification (NativeContextsNotifyResets) so the driver can name the GUILTY one;
    // only GUILTY (or a reset of unknown cause) ends the session - an INNOCENT report, a context
    // whose work merely waited behind another's hang (the compositor's among them), is only noted.
    // `debugKnob` false: the knob is not consulted here (the on-screen Present, so an injected loss
    // lands on an offscreen client's shared-image present or readback, never on the compositor's).
    Bool LatchIfDeviceLost(const char* site, Bool debugKnob = true);
    // The apply loop's form of it (BackendObject::LatchIfGpuFaulted), asked each time the apply thread
    // wakes: the driver's report only, no debug knob, and no EGL ground-truth verification spent.
    Bool LatchIfGuiltyBeforeApply();
#endif
    // True once the process's native contexts were created with EGL_LOSE_CONTEXT_ON_RESET (decided
    // by the first one; the driver must offer EGL_EXT_create_context_robustness).
    Bool NativeContextsNotifyResets();
    // GL fence sync objects, backed by native ES fences. FenceSync returns null
    // (the frontend then falls back to an always-signaled sync) when the calling
    // thread does not own the ES context. Waits/queries degrade to "signaled" in
    // the same situation, and handles created under a since-destroyed ES context
    // are always treated as signaled.
    BackendSyncHandle FenceSync();
    GLenum ClientWaitSync(BackendSyncHandle sync, GLbitfield flags, GLuint64 timeout);
    void WaitSync(BackendSyncHandle sync, GLbitfield flags, GLuint64 timeout);
    void DeleteSync(BackendSyncHandle sync);
    Bool GetSyncStatus(BackendSyncHandle sync);
    // True when GL_EXT_disjoint_timer_query and every entry point the timer
    // hooks below need are present. Also gates the E_GL_ARB_timer_query
    // advertisement in BackendObject_DirectGLES::InitCapabilities, and is
    // registered as the GLFunctionsTable::IsTimerQuerySupported hook: a pure
    // capability read needs no current ES context, and it stays false until
    // the ES capabilities have been filled in.
    Bool AreTimerQueriesSupported();
    // True when the host ES driver can back a GL_TEXTURE_BUFFER at all - ES 3.2 core, or
    // EXT/OES_texture_buffer, with glTexBuffer resolved. Desktop GL has had buffer textures as
    // core since 3.1, so the frontend advertises them unconditionally and an app may call
    // glTexBuffer whenever it likes; this is the only thing standing between that call and a
    // null entry point. False also means every shader declaring a samplerBuffer is
    // uncompilable on this driver, which the program build reports by name.
    Bool AreBufferTexturesSupported();
    // Human-readable name of the buffer-texture tier for diagnostics and the driver POST:
    // "core (ES 3.2)", "GL_EXT_texture_buffer", "GL_OES_texture_buffer" or "unsupported".
    const char* GetBufferTextureTierName();
    // glTexBuffer / glTexBufferRange through whichever spelling this driver's buffer-texture
    // support actually ships: the unsuffixed names are ES 3.2 core, while an EXT/OES driver
    // exports glTexBuffer{,Range}EXT / OES. Callers must have checked
    // AreBufferTexturesSupported() first. CallTexBufferRange reports whether it could honour
    // the range - no tier is required to expose the range form, and the whole-buffer form is
    // the documented fallback.
    void CallTexBuffer(GLenum target, GLenum internalFormat, GLuint buffer);
    Bool CallTexBufferRange(GLenum target, GLenum internalFormat, GLuint buffer, GLintptr offset, GLsizeiptr size);
    // GL timer-query objects, backed by GL_EXT_disjoint_timer_query. The
    // creators return null (the frontend then falls back to an immediately
    // available zero result) when the calling thread does not own the ES
    // context or the extension/entry points are missing, and handles created
    // under a since-destroyed ES context are always treated as complete with
    // a zero result (mirrors the fence-sync handles above).
    BackendQueryHandle BeginTimeElapsedQuery();
    void EndTimeElapsedQuery(BackendQueryHandle query);
    BackendQueryHandle QueryCounterTimestamp();
    // GL_ANY_SAMPLES_PASSED(_CONSERVATIVE) occlusion queries: core ES3, independent of
    // GL_EXT_disjoint_timer_query and of MOBILEGL_DISABLE_TIMERQUERY. Results/deletion
    // flow through GetQueryResult64/DeleteBackendQuery like the timer queries above.
    BackendQueryHandle BeginOcclusionQuery();
    void EndOcclusionQuery(BackendQueryHandle query);
    // GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN / GL_PRIMITIVES_GENERATED, also core ES
    // (GL_PRIMITIVES_GENERATED from ES 3.2 on). Null when the target is unavailable, in
    // which case the frontend falls back to counting primitives from the draw calls.
    BackendQueryHandle BeginXfbPrimitivesQuery(Bool generated);
    void EndXfbPrimitivesQuery(BackendQueryHandle query);
    Bool IsQueryResultAvailable(BackendQueryHandle query);
    // Returns true when a final value landed in *outNanoseconds (a zero for
    // null or stale-generation handles IS final: the frontend may cache it
    // and release the handle). Returns false only when the calling thread
    // does not own the ES context, so the value is genuinely unobtainable
    // right now; the handle stays alive and readable later.
    Bool GetQueryResult64(BackendQueryHandle query, Bool wait, Uint64* outNanoseconds);
    void DeleteBackendQuery(BackendQueryHandle query);
    Int64 GetGpuTimestampNs();
    void Present();
    // Frame-completion watermarks for the buffer-storage pool: CurrentFrameSerial()
    // is bumped once per Present(); CompletedFrameSerial() is the newest frame whose
    // GPU work has provably finished (advanced by polling a one-fence-per-frame ring).
    // A buffer retired during frame N is safe to recycle once CompletedFrameSerial() >= N.
    Uint64 CurrentFrameSerial();
    Uint64 CompletedFrameSerial();
    // Block (up to timeoutNs) until the given frame serial provably retired on the
    // GPU, using the per-frame fence ring. False when no usable fence covers the
    // serial (fence-less context, foreign thread, or the slot was recycled);
    // completion state is untouched in that case.
    Bool WaitForFrameSerialCompleted(Uint64 serial, Uint64 timeoutNs);
    // Applies (or defers until the window surface exists) the app-requested
    // eglSwapInterval on the native EGL surface.
    void SetSwapInterval(Int interval);
#if MOBILEGL_BUILD_DISAGGREGATED
    // P12 review fix: the swap interval an ended server session asked for is not the next one's.
    // The request outlives the context on purpose (a surface re-created mid-session re-applies it),
    // so the in-process display server's session end forgets it: the next session starts at the
    // driver's default until it asks (BackendObject_DirectGLES's destructor under a transport).
    void ForgetRequestedSwapInterval();
#endif
    void SetEGLFuncsTable(const MG_External::EGLFunctionsTable& eglFuncs);
    void SetGLESFuncsTable(const MG_External::GLESFunctionsTable& glesFuncs);
    void SetGLESCapabilities(const MG_External::GLESCapabilities& capabilities);

    // ---------------------------------------------------------------------------------
    // P14 S4 (docs/Disaggregated/design/11-state-ownership.md): THE NATIVE TUPLE IS PER CONTEXT
    // ---------------------------------------------------------------------------------
    //
    // The display is process state and stays one object (eglInitialize once per process, terminated
    // when the last native context leaves). The native EGLContext, the config it was created with
    // and the native surfaces are keyed by (session, client context token) - the identity S1 put on
    // the wire and S2 gave a per-thread owner. Creating a second context therefore no longer tears
    // the first one's native context down, which is what lets two contexts (and two share groups)
    // be live at once in one process.

    // Answers, for the CALLING THREAD, which session it belongs to and which client context token
    // that session has bound. Installed once by the server layer; with no resolver installed -
    // monolith, a frontend-only process, a unit case - the answer is the single key {0, 0}, which is
    // exactly the process singleton this replaces. `false` also means {0,0}.
    using NativeContextKeyResolver = Bool (*)(Uint64* outSessionKey, Uint64* outContextToken);
    void SetNativeContextKeyResolver(NativeContextKeyResolver resolver);

#if MOBILEGL_PIPE_PUSH
    // ---------------------------------------------------------------------------------
    // P14 S6 (docs/Disaggregated/design/11-state-ownership.md): THE TWIN TABLES' KEY, AND ITS
    // SHAPE IS S4's, ONE LEVEL DOWN. A twin is a DRIVER object, so it lives in one native
    // context's object namespace; the identity a twin table must be keyed on is therefore
    // {session, share group} - the SAME two words the applier's OBJECT RECORDS are filed under
    // (PipeApply.h's MGPipeApplierKey, derived by the same probe and the same `0 means its own
    // group` rule, so the two agree by construction rather than by two conventions).
    //
    // Answers, for the CALLING THREAD, which session it belongs to and which share group that
    // session's current context is in. Installed once by the server layer beside the native and
    // applier probes; with no probe installed - or on a thread that is no session's own, which
    // is the client process, the in-process client+server shape and every unit case - the
    // answer is the single key {0, 0}, exactly the process-wide table this replaces.
    struct TwinKey {
        Uint64 SessionKey = 0;
        Uint64 ShareGroupKey = 0;
        Bool operator==(const TwinKey& other) const {
            return SessionKey == other.SessionKey && ShareGroupKey == other.ShareGroupKey;
        }
        Bool operator!=(const TwinKey& other) const { return !(*this == other); }
    };
    struct TwinKeyHash {
        SizeT operator()(const TwinKey& key) const {
            // The 64-bit mix SlotAllocator and the applier's registry already use for their own
            // pair keys, so every keyed table in this tree hashes a session the same way.
            Uint64 mixed = key.SessionKey ^ (key.ShareGroupKey + 0x9E3779B97F4A7C15ull +
                                             (key.SessionKey << 6) + (key.SessionKey >> 2));
            mixed ^= mixed >> 33;
            mixed *= 0xFF51AFD7ED558CCDull;
            mixed ^= mixed >> 33;
            return static_cast<SizeT>(mixed);
        }
    };
    using TwinKeyResolver = Bool (*)(TwinKey* outKey);
    void SetTwinKeyResolver(TwinKeyResolver resolver);
    // The calling thread's key. Answers false for {0, 0} - the process-wide group - which is
    // also the answer when no probe has ever been installed.
    Bool CurrentTwinKey(TwinKey* outKey);
#endif

    // Builds the native context for `contextToken` under the calling thread's session. A
    // `shareGroupToken` whose group already has a live native context in this session hands that
    // context to eglCreateContext as the share argument - which is what makes two contexts see one
    // set of buffer/texture objects. Idempotent: a token that already has one answers true.
    Bool CreateNativeContextFor(Uint64 contextToken, Uint64 shareGroupToken);
    // Destroys ONE context's native tuple (the DestroyContext control frame). The session's other
    // contexts are untouched; the display is terminated only when the last context in the process
    // has gone.
    void DestroyNativeContextFor(Uint64 contextToken);

    // ---- objects that belong to ONE native context ----
    //
    // Framebuffers, vertex arrays, transform feedbacks and queries are container objects: GL never
    // shares them, not even between contexts of one share group, and every context numbers its own
    // from 1. A process-wide scratch FBO id minted in one context and bound in another therefore
    // names THAT context's own framebuffer - often an application's - and the scratch path's
    // attach/scrub then rewires it onto staging textures. With one process serving several
    // sessions (each with its own native contexts) that silently corrupted other clients' frames.
    //
    // The calling thread's current native context as a lifetime serial: assigned when the context
    // is created and never reused (a recycled EGLContext address is a new serial). 0 when none was
    // made current through MakeCurrent.
    Uint64 CurrentNativeContextSerial();

    class PerNativeContextBase {
    public:
        PerNativeContextBase();
        PerNativeContextBase(const PerNativeContextBase&) = delete;
        PerNativeContextBase& operator=(const PerNativeContextBase&) = delete;
        // The context died, and its names with it: forget them (no GL call - there is no context
        // left to make one in).
        virtual void ForgetContext(Uint64 serial) = 0;

    protected:
        ~PerNativeContextBase();
    };

    // Called where a native context is destroyed.
    void ForgetNativeContextObjects(Uint64 serial);
    // Moves whenever any per-context entry is dropped; a thread's cached entry is valid while
    // it has not moved.
    Uint64 PerNativeContextEpoch();
    void BumpPerNativeContextEpoch();

    // One T per native context, created on first use from that context.
    template <typename T>
    class PerNativeContext final : public PerNativeContextBase {
    public:
        T& Current() {
            const Uint64 serial = CurrentNativeContextSerial();
            // The draw path asks for the same entry over and over from one thread: a one-entry
            // thread-local cache, invalidated by any drop anywhere (rare).
            thread_local CurrentCache cache;
            const Uint64 epoch = PerNativeContextEpoch();
            if (cache.Owner == this && cache.Serial == serial && cache.Epoch == epoch) return *cache.Entry;
            const std::lock_guard<std::mutex> lock(m_mutex);
            T& entry = m_bySerial[serial]; // node-based: the reference survives other contexts' inserts
            cache = {this, serial, epoch, &entry};
            return entry;
        }
        template <typename F>
        void ForEach(F&& visit) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& entry : m_bySerial) visit(entry.second);
        }
        void Clear() {
            const std::lock_guard<std::mutex> lock(m_mutex);
            BumpPerNativeContextEpoch();
            m_bySerial.clear();
        }
        void ForgetContext(Uint64 serial) override {
            const std::lock_guard<std::mutex> lock(m_mutex);
            BumpPerNativeContextEpoch();
            m_bySerial.erase(serial);
        }

    private:
        struct CurrentCache {
            const PerNativeContext* Owner = nullptr;
            Uint64 Serial = 0;
            Uint64 Epoch = 0;
            T* Entry = nullptr;
        };
        std::mutex m_mutex;
        std::unordered_map<Uint64, T> m_bySerial;
    };

    // The GL name of one per-context object, spelled like the plain `GLuint` it replaces: it reads
    // as the CURRENT context's name, `= 0` resets that one, and `&name` is where glGen* writes it.
    class NativeContextName {
    public:
        operator GLuint() const { return m_names.Current(); }
        NativeContextName& operator=(GLuint name) {
            m_names.Current() = name;
            return *this;
        }
        GLuint* operator&() { return &m_names.Current(); }

    private:
        mutable PerNativeContext<GLuint> m_names;
    };

    // P14 S4. THE NATIVE HALF OF A CONTEXT SWITCH. `bind_context` moves which context the session's
    // records belong to; this makes the calling thread's native tuple follow it, binding the
    // session's current surface to the newly named context's native EGLContext. A no-op when the
    // named context has no native context yet (the session never sent CreateContext) or when the
    // native context already current on this thread IS that one - so the single-context world pays
    // nothing for it. Called from the bind_context path on the apply thread.
    Bool MakeNativeContextCurrentForBoundToken();
#if MOBILEGL_BUILD_DISAGGREGATED
    // P14: the server's backend turn moved to the calling apply thread (ServerLoop.cpp's
    // BackendTurnLock). Takes the backend's GL ownership for this thread and drops every
    // binding shadow, which still describes the previous holder's native context.
    void OnBackendTurnHandoff();
#endif
    // How many native contexts this process holds, and how many of them belong to one session. The
    // test observables for "a second context did not tear the first one down".
    Uint64 NativeContextCount();
    Uint64 NativeContextCountForSession(Uint64 sessionKey);

    // The calling thread's session, as the resolver above reports it (0 with no resolver).
    Uint64 CurrentNativeSessionKey();

    // ---- Test observables (P14 S4). EGL exposes no "which EGLContext does this client context
    // hold" query and no way to ask a context for its share group, so a case that must tell "the
    // same native context still serves" from "a fresh one was built", or a sharing pair from two
    // unrelated contexts, reads it here. These are pure reads plus one key override; neither is on
    // a production path and neither changes behaviour.
    EGLContext NativeContextHandleFor(Uint64 sessionKey, Uint64 contextToken);
    // Overrides the key the calling thread resolves to, for the life of the call. The backend's
    // ordinary answer comes from the server layer's probe; this is how a case drives exactly the
    // tuple it means when no session runtime is installed.
    void ForceNativeContextKeyForTesting(Uint64 sessionKey, Uint64 contextToken);

    void DestroyEGLContext();

    // Transform feedback capture spans, performed by the real ES driver. The
    // capture set is declared on the backend program at link time; the driver-side
    // begin is deferred to the first draw of the span (ES needs the capturing
    // program current and the capture buffers bound), and the end also mirrors the
    // captured bytes back into the frontend buffer shadows.
    void PatchParameteri(GLenum pname, GLint value);

    namespace XfbImpl {
        Bool AreTransformFeedbacksSupported();
        // True while a capture span is open on the current transform feedback object
        // (frontend Begin seen and not paused), whether or not the deferred driver-side
        // Begin has been issued yet. Draw paths that would restructure the primitive
        // stream, or that need to dispatch compute mid-draw, decline while it is set.
        Bool IsCaptureSpanOpen();
        void BeginTransformFeedback(GLenum primitiveMode);
        void EndTransformFeedback();
        void PauseTransformFeedback();
        void ResumeTransformFeedback();
        void BindTransformFeedback(GLuint name);
        void DeleteTransformFeedback(GLuint name);
        void OnBackendContextDestroyed();
    } // namespace XfbImpl

    namespace RenderStateImpl {
        // Pushes the frontend's render-state block to the ES driver, diffed against what was
        // last pushed.
        //
        // `forColorClear` names the CALLER, and the only thing it changes is the colour write
        // mask handed to the driver. A draw into a colour attachment the backend widened from
        // three channels to four gets that buffer's alpha channel masked OFF, so nothing can
        // move the stored alpha away from the 1.0 the application's three-channel format
        // implies (see FramebufferImpl::g_alphaWidenedDrawBufferMask). A CLEAR is how that 1.0
        // gets there in the first place, so it must be allowed to write alpha - hence the flag
        // rather than an unconditional doctoring. It is part of the sync memo, so a clear
        // followed by a draw re-pushes the mask instead of early-outing on an unchanged
        // frontend version.
        //
        // The application's own colour mask is never modified: glGet(GL_COLOR_WRITEMASK)
        // answers from the frontend state, which this function only reads.
        void SyncRenderState(Bool forColorClear = false);
        void InvalidateSyncedRenderState();

        // GL_SCISSOR_TEST as the driver has it in the native context current on this thread -
        // what a guard that turns the test off around its own blit, and puts it back, needs to
        // know. The shadow answers only while it describes this context (synced since the last
        // invalidation, in this context epoch); otherwise the driver is asked. The shadow is one
        // per process: once another session's context synced into it, or a context switch
        // invalidated it, its bits are some other context's.
        Bool DriverScissorTestEnabled();

        // TESTING: the shadow as SyncRenderState leaves it in the current context epoch, with
        // GL_SCISSOR_TEST `enabled`.
        void SetSyncedScissorTestForTesting(Bool enabled);
        // TESTING: `inside` runs within the guard the shared-image present and the readbacks
        // copy under (ScopedScissorDisable).
        void RunScissorDisabledForTesting(void (*inside)(void*), void* data);
    } // namespace RenderStateImpl

#if MOBILEGL_BUILD_DISAGGREGATED
    // ---- SHARED IMAGES (docs/Disaggregated/notes/anland/plan-ahb-dmabuf.md) ----
    //
    // A shared image reaches the driver only through the Android import path: its AHardwareBuffer
    // becomes an EGLImage (EGL_ANDROID_get_native_client_buffer + EGL_ANDROID_image_native_buffer)
    // and that EGLImage the storage of a texture or renderbuffer (GL_OES_EGL_image). On a host
    // build there is no such path and every entry below declines.
    namespace SharedImageImpl {
        // One EGLImage over one shared image, holding the image's registry reference for as long
        // as the EGLImage lives (SharedImageRegistry.h's lifetime rule). The last reference
        // destroys the EGLImage, unless the display that made it has been terminated since (which
        // destroyed it already). Opaque outside DirectGLES.cpp.
        struct EglImage;
        using EglImageRef = SharedPtr<EglImage>;

        // An EGLImage for the live shared image `id`, or null with `why`. Needs a current context
        // (the first call resolves the extension entry points).
        EglImageRef CreateEglImage(Uint64 id, String& why);
        // glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image) on what the active unit has bound to
        // GL_TEXTURE_2D. False when the driver refused it (its GL error is consumed).
        Bool TargetBoundTexture2D(const EglImageRef& image);
        // BackendObject::BlitDefaultFramebufferToSharedImage, for the calling thread's context.
        Bool BlitDefaultFramebufferTo(const SharedImageView& image, const MG_Util::Damage::Region& region);

        // THE READER'S HALF of the image's sync state (SharedImageRegistry.h). Called on every use
        // of a name whose level 0 is `image`, before the command that samples it: when a write
        // landed since this context last waited, the context's later commands wait for its fence
        // on the GPU; and the image is noted as read by the calling session's current frame.
        void AcquireForSampling(const EglImageRef& image);
        // The calling session's frame boundary for what it has read: hands a fence for everything
        // the current context has submitted to every image noted since the last boundary. Called
        // at Present and wherever the current native context stops being current (its reads can
        // only be fenced from it). Nothing noted = nothing done.
        void PublishPendingReads();
        // IMPLICIT SYNC BY FLUSH (BackendObject::PublishSharedImageAccesses): everything the
        // calling session used since its last boundary is published as written and read by one
        // fence, and the session's first use of an image in each frame from now on also waits for
        // the image's pending reads.
        Bool PublishPendingAccesses();
        // EGL_ANDROID_native_fence_sync's fence command (BackendObject::ExportNativeFence): a
        // sync_file of everything the current context submitted in `*fence` (-1: none, and the work
        // is waited out here), and - for a session that writes images - the boundary
        // PublishPendingAccesses makes.
        Bool ExportNativeFence(int* fence);
    } // namespace SharedImageImpl
#endif

    extern MG_External::EGLFunctionsTable g_EGLFuncs;
    extern MG_External::GLESFunctionsTable g_GLESFuncs;
    extern MG_External::GLESCapabilities g_GLESCapabilities;
} // namespace MobileGL::MG_Backend::DirectGLES
