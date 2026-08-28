// MobileGL - MobileGL/MG_Backend/DirectGLES/Managers.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <atomic>
#include <mutex>
#include "DirectGLES.h"
#include "MG_State/GLState/SamplerState/SamplerObject.h"
#include "MG_State/GLState/TextureState/TextureEnum.h"
#include <MG_State/GLState/TextureState/TextureObject.h>
#include <MG_State/GLState/Core.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>

namespace MobileGL::MG_Backend::DirectGLES {
    String EmulateBaseInstanceInVertexShader(String source, GLenum shaderType);
    String PromoteDrawParameterGlobalsToUniforms(String source, GLenum shaderType);

    // The ESSL half of the gl_ViewportIndex routing emulation, in the order a program's stages
    // meet it. Both are pure String -> String rewrites over what SPIRV-Cross emitted once
    // LowerViewportIndexPass has demoted the builtin to the plain global `mg_ViewportIndex`.
    //
    // The producing stage's global becomes an ordinary flat varying; true when there was one to
    // promote, which is also the answer to "does this program route viewports at all".
    Bool PromoteViewportIndexGlobalToVarying(String& source);
    // The fragment stage grows a matching flat input, the mg_ViewportPassMask uniform the draw
    // path writes, and a wrapper entry point that discards every fragment whose primitive routed
    // to an index the current replay pass is not drawing. False when the stage has no entry point
    // to wrap, which leaves the program renderable but unrouted.
    Bool InjectViewportIndexPassGate(String& source);

    // Whether a vertex shader may declare a storage block at all, given what the host driver
    // reports for GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS. Pure, and separated from the capability
    // global purely so the decision can be tested without one.
    //
    // The indirect half of the gl_BaseInstance lowering in PromoteDrawParameterGlobalsToUniforms
    // is the only thing that needs this, and it needs exactly one block. A driver reporting 0 is
    // conformant - the minimum is 0 in GL 4.6 table 23.64 and ES 3.2 table 21.44 - and ARM's
    // GLES driver does report 0, so this is a live path, not a defensive one.
    Bool VertexStageStorageBlockUsable(Int maxVertexShaderStorageBlocks);

    // True once the process has entered exit(): past that point the EGL library and
    // the driver may already be unloaded, so a backend twin's destructor must not
    // call into g_GLESFuncs (the observed crash is a jump through an unmapped driver
    // pointer from __run_exit_handlers) nor touch statics in other TUs (cross-TU
    // destruction order is unspecified). Deliberate leak: the process is exiting and
    // the driver reclaims GPU objects. The flag is set by a std::atexit handler that
    // EnsureProcessTeardownSentinel() registers lazily on first registry use - by
    // then every static everywhere has finished constructing, so this handler is
    // guaranteed to run BEFORE any static destructor (atexit is LIFO). A destructor
    // hook on the registry itself was tried first and is WRONG: tests and cache
    // resets destroy temporary registry instances mid-run, which would latch the
    // flag while the process is very much alive.
    Bool InProcessTeardown();
    void EnsureProcessTeardownSentinel();

    // Generation of the backend ES context that owns the driver ids currently handed
    // out. Bumped exactly once per DestroyEGLContext. Every backend twin that owns a
    // driver name (texture, framebuffer, renderbuffer, sampler) stamps this at
    // construction and compares it in its destructor: a twin outliving its context
    // must NOT glDelete* its id, because a successor context may already have recycled
    // that name and the delete would take out a live object of the new context.
    extern Uint g_backendContextGeneration;

    // Which optional pieces of state a draw needs synchronized before it is issued.
    // Index/indirect buffer syncs and the instancing-related work are skipped for
    // draws that provably cannot read them.
    enum class DrawSyncBit : Uint32 {
        None = 0,
        IndexBuffer = 1 << 0,
        IndirectBuffer = 1 << 1,
        Instancing = 1 << 2
    };
    // Deliberately the shared Flags<> rather than hand-written operators for this enum:
    // a namespace-local operator| here would hide MobileGL::operator|(Bit, Bit) from
    // every other scoped-enum flag set used inside this namespace.
    using DrawSyncFlags = Flags<DrawSyncBit>;

    // The GL-defined indirect command layouts, byte-identical to what the driver reads
    // out of a GL_DRAW_INDIRECT_BUFFER. Also the staging layout the multi-draw emulation
    // synthesizes commands into.
    struct DrawElementsIndirectCommand {
        Uint32 count = 0;
        Uint32 instanceCount = 0;
        Uint32 firstIndex = 0;
        Int32 baseVertex = 0;
        Uint32 baseInstance = 0;
    };

    struct DrawArraysIndirectCommand {
        Uint32 count = 0;
        Uint32 instanceCount = 0;
        Uint32 first = 0;
        Uint32 baseInstance = 0;
    };

    // Brings the whole draw-relevant frontend state onto the native ES context and binds
    // the program; every GL draw entry point calls it exactly once before issuing draws.
    void PrepareForDraw(DrawSyncFlags syncBits);
    // What an indexed draw has to do about primitive restart before it can be issued.
    //
    // Desktop GL restarts on an application-chosen index (glPrimitiveRestartIndex under
    // GL_PRIMITIVE_RESTART); GLES core restarts only on the all-ones value of the index type
    // (GL_PRIMITIVE_RESTART_FIXED_INDEX), which the render-state push enables for BOTH caps.
    // That leaves three cases, and the difference between the last two is not cosmetic - one
    // adds restarts, the other has to take away restarts the driver would otherwise make.
    enum class RestartSubstitutionKind : Uint8 {
        // Nothing to do: restart is off, the fixed-index cap is on, or the application's
        // restart index already IS the type's all-ones value. The overwhelmingly common answer.
        None,
        // The application's index is representable in this index type and differs from the
        // all-ones value: the index DATA has to be rewritten so the driver restarts where the
        // application asked.
        RewriteIndices,
        // The application's index cannot be held by this index type at all. GL 4.6 core 10.3.6
        // compares the fetched index, zero-extended, against the full 32-bit
        // PRIMITIVE_RESTART_INDEX, so no index can match and the draw restarts NOWHERE - but the
        // render-state push has already enabled the driver's fixed-index restart, so the
        // all-ones value has to be un-restarted for the duration of the draw.
        SuppressRestart,
    };
    RestartSubstitutionKind ResolveRestartSubstitution(GLenum indexType);

    // Turns the driver's fixed-index restart off for one draw and back on afterwards, for the
    // SuppressRestart case above. Separate from the substitution below because the multi-draw
    // tiers need it on its own: they rewrite the index stream themselves and only ever need the
    // cap half. Inert for every other kind, and it never touches the render-state shadow - it
    // puts the driver back exactly where SyncRenderState left it.
    class ScopedSuppressedPrimitiveRestart {
    public:
        explicit ScopedSuppressedPrimitiveRestart(RestartSubstitutionKind kind);
        ~ScopedSuppressedPrimitiveRestart();
        ScopedSuppressedPrimitiveRestart(const ScopedSuppressedPrimitiveRestart&) = delete;
        ScopedSuppressedPrimitiveRestart& operator=(const ScopedSuppressedPrimitiveRestart&) = delete;

    private:
        Bool m_suppressed = false;
    };

    // Swaps in a scratch element array buffer holding a copy of the index data in which the
    // application's restart index has been replaced by the value GLES restarts on. Inert
    // (and free) unless ResolveRestartSubstitution asks for it. The swap lives for the
    // object's lifetime, so it covers every pass of a viewport-routed draw, and the previous
    // GL_ELEMENT_ARRAY_BUFFER name is restored on destruction - which matters beyond tidiness,
    // because the VAO twin memoises that it already synced that binding.
    //
    // The copy may be WIDER than the source (see IndexType): when the source already contains
    // the type's all-ones value as an ordinary vertex index, that value cannot double as the
    // restart sentinel, and widening is the only way to keep both meanings. Callers must
    // therefore take the index type from this object, not from their own argument.
    class ScopedRestartIndexSubstitution {
    public:
        // count/indices describe the draw's index range when the CPU knows it. Pass
        // count == 0 for an indirect draw, whose count lives in GPU memory: the whole bound
        // element array buffer is rewritten instead, so every element keeps its position and
        // a GPU-resident firstIndex - an ELEMENT index, so it survives widening too - still
        // addresses the index it named.
        ScopedRestartIndexSubstitution(GLenum indexType, GLsizei count, const void* indices);
        ~ScopedRestartIndexSubstitution();
        ScopedRestartIndexSubstitution(const ScopedRestartIndexSubstitution&) = delete;
        ScopedRestartIndexSubstitution& operator=(const ScopedRestartIndexSubstitution&) = delete;

        // False only when a substitution was needed and could not be made. The draw must
        // then be skipped: issuing it would let the driver silently drop every restart and
        // weld the primitives on either side together, which is worse than drawing nothing.
        Bool DrawIsValid() const { return m_valid; }
        // The element-array offset (or client pointer) the draw must use. Identical to what
        // was passed in unless a substitution was made.
        const void* Indices() const { return m_indices; }
        // The index type the draw must be issued with. Identical to the constructor's unless
        // the copy had to be widened to keep an all-ones vertex index distinguishable from the
        // restart sentinel.
        GLenum IndexType() const { return m_indexType; }

    private:
        // Declared before m_capOverride so it is initialised first (members initialise in
        // declaration order): the whole decision is made once, and both the cap override and the
        // constructor body read the same answer.
        RestartSubstitutionKind m_kind = RestartSubstitutionKind::None;
        ScopedSuppressedPrimitiveRestart m_capOverride;
        const void* m_indices = nullptr;
        GLenum m_indexType = 0;
        Uint m_previousBinding = 0;
        Bool m_substituted = false;
        Bool m_valid = true;
    };

    // Drops the scratch element array buffer the substitution above stages through. Like
    // MultiDrawImpl's scratch names it is abandoned rather than deleted: the name belongs to
    // the dead ES context, and deleting it would target whatever its successor handed out.
    void OnRestartSubstitutionContextDestroyed();
    // Feed the current program's gl_BaseInstance / gl_DrawID / gl_BaseVertex emulation
    // uniforms. All are no-ops when the program does not read the corresponding builtin.
    void SetCurrentBaseInstance(Uint32 baseInstance);
    void SetCurrentDrawID(Uint32 drawId);
    // GL's gl_BaseVertex is the base-vertex parameter of an indexed draw and zero for every
    // command that has none - including all the DrawArrays forms - so every draw path that
    // does not carry one must leave this at zero rather than inherit the last draw's value.
    void SetCurrentBaseVertex(Int32 baseVertex);
    // True when the current program actually reads gl_DrawID, i.e. when a batched
    // (single driver call) multi-draw tier would have to feed it one value for the whole
    // batch and would therefore be wrong.
    Bool CurrentProgramReadsDrawID();
    // Same question for gl_BaseVertex: a batched multi-draw tier cannot give each sub-draw
    // its own base vertex through a uniform either.
    Bool CurrentProgramReadsBaseVertex();
    // Both of the above, conservatively, for a caller that must decide BEFORE PrepareForDraw
    // has synced the program - where "does not read it" is indistinguishable from "cannot be
    // asked yet". Answers true whenever the backend twin is missing or predates the current
    // link.
    Bool CurrentProgramMayNeedPerSubDrawBuiltins(Bool batchCarriesBaseVertices);

    // ---- gl_ViewportIndex routing emulation, draw half ---------------------------------------
    //
    // GLES has ONE viewport, ONE scissor rectangle and ONE depth range; GL 4.1 has sixteen of
    // each, selected per primitive by gl_ViewportIndex. There is no ES entry point to program the
    // other fifteen with (GL_OES_viewport_array exists but Adreno 830 does not have it, verified
    // three ways), so the only way to rasterize a primitive against index i's rectangle is to
    // make index i's rectangle THE viewport for the duration of a draw - which means issuing the
    // draw once per distinct viewport state and letting the fragment stage throw away the
    // primitives that belong to the other indices (the gate Managers.cpp injects).
    //
    // Indices whose whole state tuple (viewport rectangle, scissor rectangle, scissor-test enable,
    // depth range) is identical share ONE pass, so the overwhelmingly common case - every index
    // still holding what glViewport/glScissor/glDepthRange broadcast to all sixteen - collapses
    // to a single pass with an all-ones gate mask, i.e. one draw and no behaviour change at all.
    //
    // Whether emulation runs. Off only under MOBILEGL_FORCE_VIEWPORT_ARRAY_EMULATION falsy, which
    // restores the pre-emulation path as a negative control.
    Bool ViewportArrayEmulationEnabled();
    // Whether ANY program built in this process has come out with a viewport gate. Sticky once
    // true; it exists so that BeginViewportRoutingPasses - which runs on every draw of every
    // workload - can answer with one static load in the case that matters, which is every
    // application that has never heard of gl_ViewportIndex.
    extern Bool g_anyProgramRoutesViewportIndex;
    // Number of times the current draw has to be issued. Always >= 1, and exactly 1 - with no
    // state touched - whenever the current program does not route viewports, whenever every
    // configured index shares one state, and whenever replaying would multiply a side effect the
    // fragment gate cannot undo (transform feedback, rasterizer discard). Also seeds the pass
    // mask uniform for that single-pass case, so a gated fragment shader never runs against the
    // zero every GLSL uniform starts at - which would discard the whole draw.
    Uint BeginViewportRoutingPasses();
    // Push pass `pass`'s viewport / scissor / scissor-test / depth range onto the ES context and
    // set the gate mask to the indices it serves. Only called when the count above exceeds 1.
    void ApplyViewportRoutingPass(Uint pass);
    // Restore the gate mask and mark the render-state shadow dirty, so the next ordinary draw
    // re-pushes index 0's state. Takes the count so it can do nothing at all in the common case.
    void EndViewportRoutingPasses(Uint passCount);

    // Issue one draw, replayed once per viewport-routing pass. Every application-visible draw
    // entry point wraps its native glDraw* call in this; the internal blit and clear helpers
    // deliberately do not, because they bind their own programs, which never route.
    template <typename IssueDraw>
    inline void ForEachViewportRoutingPass(IssueDraw&& issue) {
        const Uint passCount = BeginViewportRoutingPasses();
        for (Uint pass = 0; pass < passCount; ++pass) {
            if (passCount > 1) {
                ApplyViewportRoutingPass(pass);
            }
            issue();
        }
        EndViewportRoutingPasses(passCount);
    }

    template <typename StateObject, typename BackendObject>
    class StateBackendObjectRegistry {
    public:

        using StatePtr = SharedPtr<StateObject>;
        using StateWeakPtr = std::weak_ptr<StateObject>;
        using BackendPtr = SharedPtr<BackendObject>;

        // The backend twin and the weak reference that decides whether the raw key still
        // names the state object the twin was built for. Both live in one entry: a
        // separate liveness map answered nothing the backend probe had not already found
        // and cost a second hash lookup on every Find, which the draw path runs ~10 times.
        struct Entry {
            BackendPtr backend;
            StateWeakPtr stateRef;
        };
        using BackendMap = UnorderedMap<StateObject*, Entry>;
        using iterator = typename BackendMap::iterator;
        using const_iterator = typename BackendMap::const_iterator;

        BackendPtr& GetOrCreate(const StatePtr& stateObj) {
            MOBILEGL_ASSERT(stateObj != nullptr, "State object must not be null");

            // Twin creation is the moment a driver-owned id starts needing a guarded
            // destructor; cold path, so the once-guard costs nothing per draw.
            EnsureProcessTeardownSentinel();
            // Sweep BEFORE the entry reference below exists: the map is open-addressed and an
            // erase relocates the rest of the probe cluster, so collecting once that reference
            // is taken would invalidate it. The sweep is therefore owed from an earlier call
            // rather than triggered by this one.
            if (m_creationTick >= kCreationGCInterval) {
                m_creationTick = 0;
                CollectGarbage();
            }
            const SizeT entryCountBeforeInsert = m_entries.size();
            auto& entry = m_entries[stateObj.get()];
            if (m_entries.size() != entryCountBeforeInsert) {
                // A key the registry has never held. Nothing tells the backend that a texture or
                // renderbuffer was DELETED - the twin, and the driver storage it owns, lives
                // until a collection - and CollectGarbageIfNeeded is ticked only from the
                // per-draw sync paths, which a CTS-shaped workload runs about ten times per
                // case. 1024 of those ticks then span ~100 cases, so ~100 cases' worth of dead
                // (and, for this suite, gigabyte-sized) objects stay allocated at once. Object
                // CHURN rather than draw count is what makes the sweep urgent, so a twin the
                // registry has never seen ticks it too - and it does so on the path that is
                // about to allocate, which is exactly when the memory is needed.
                ++m_creationTick;
            }
            if (entry.stateRef.expired()) {
                // The previous owner of this address is gone and the allocator handed it
                // to a new object: its twin describes ids the new state object never made.
                entry.backend.reset();
            }
            entry.stateRef = stateObj;
            return entry.backend;
        }

        // Null when no live state object owns this key. The result points into the map, so
        // it stays valid only until the next GetOrCreate/Find/CollectGarbage on this registry.
        // Take that literally, including for Find: the map is open-addressed and erases by
        // shifting the rest of the probe cluster into the hole, so an erase relocates entries
        // OTHER than the erased one - and Find erases, whenever it lands on a key whose state
        // object has expired. Callers that need the twin across another registry call must copy
        // the BackendPtr out (or keep only the pointee, which is heap-allocated and never moves).
        BackendPtr* Find(StateObject* stateObj) {
            const auto entryIt = m_entries.find(stateObj);
            if (entryIt == m_entries.end()) {
                return nullptr;
            }
            if (entryIt->second.stateRef.expired()) {
                m_entries.erase(entryIt);
                return nullptr;
            }
            return &entryIt->second.backend;
        }

        const BackendPtr* Find(StateObject* stateObj) const {
            return const_cast<StateBackendObjectRegistry*>(this)->Find(stateObj);
        }

        iterator begin() { return m_entries.begin(); }
        const_iterator begin() const { return m_entries.begin(); }
        iterator end() { return m_entries.end(); }
        const_iterator end() const { return m_entries.end(); }

        void CollectGarbageIfNeeded() {
            ++m_gcTick;
            if (m_gcTick < kGCInterval) {
                return;
            }
            CollectGarbage();
            m_gcTick = 0;
        }

        void CollectGarbageNow() { CollectGarbage(); }

    private:
        void CollectGarbage() {
            if (m_isCollecting) {
                return;
            }

            m_isCollecting = true;

            Vector<StateObject*> staleKeys;
            staleKeys.reserve(m_entries.size());
            for (const auto& [stateKey, entry] : m_entries) {
                if (entry.stateRef.expired()) {
                    staleKeys.push_back(stateKey);
                }
            }

            for (auto* stateKey : staleKeys) {
                m_entries.erase(stateKey);
            }

            m_isCollecting = false;
        }

    private:
        static constexpr Uint32 kGCInterval = 1024;
        // Creations are far rarer than draws, so this counts in a much smaller unit than
        // kGCInterval does.
        static constexpr Uint32 kCreationGCInterval = 64;
        BackendMap m_entries;
        Uint32 m_gcTick = 0;
        Uint32 m_creationTick = 0;
        Bool m_isCollecting = false;
    };

    namespace BufferImpl {
        const GLenum TempBufferTarget = GL_ARRAY_BUFFER;

        // --- Buffer-mutation epoch -------------------------------------------------
        // Manager-wide monotonic counter: it moves whenever ANY buffer resource may
        // have gone from draw-clean to dirty. Draw-path memos read it once per pass
        // (CurrentBufferMutationEpoch, acquire), re-run their IsBufferDrawClean
        // probes only when it moved, and stamp the PRE-pass value after a pass in
        // which every probe came up clean - so a concurrent bump lands strictly
        // after the stamped value and forces a re-probe on the next pass no matter
        // how the probe interleaved with the mutation. Conservative-correct: a bump
        // never skips work, it only re-runs the probes once.
        //
        // Every clean->dirty transition path bumps it (BumpBufferMutationEpoch,
        // release, AFTER the mutation lands so an acquire reader that still sees
        // the old epoch cannot have missed the mutation):
        //   * the frontend BufferBackendOps table - Respecify, SubData,
        //     FlushMappedRange, AcquirePersistentMap, ReadbackFromGpu, OnDestroy -
        //     which every frontend change-serial bump and every pending-range
        //     queueing reaches while ops are registered (upload, orphan/respecify,
        //     map flush/unmap writeback, persistent-map adoption, delete/pooling);
        //   * backend-initiated shadow writebacks that bump the frontend change
        //     serial without an op: transform-feedback capture readback
        //     (XfbImpl::ReadbackCapturedRanges and the scatter path) and every
        //     pack-PBO WritebackFromBackend site (glReadPixels/glGetTexImage);
        //   * RegisterBufferBackendOps/UnregisterBufferBackendOps - while ops are
        //     unregistered, frontend writes advance serials silently, so both edges
        //     of that window re-open every memo;
        //   * OnBackendContextDestroyed - the buffer context generation moved, so
        //     every previously clean resource is invalid.
        // NOT bumped (cleanliness provably unchanged): MarkGpuWritten (the backend
        // copy is authoritative; IsBufferDrawClean does not consult it),
        // NotifyContentWrite on a GPU-resident buffer (persistent-mapped resources
        // are clean by construction), and EnsureBufferResource itself (it only
        // repairs toward clean). A non-persistent map (draws on it are GL errors
        // the frontend rejects) sets IsMapped without an op; persistent maps reach
        // AcquirePersistentMap or (FLUSH_EXPLICIT) publish only via FlushMappedRange.
        Uint64 CurrentBufferMutationEpoch();
        void BumpBufferMutationEpoch();

        // The DirectGLES storage behind one frontend buffer. Owned (refcounted) by
        // the frontend BufferObject; immediate BufferBackendOps keep it current, so
        // draw-time "sync" reduces to ensuring the storage exists.
        class GLESBufferResource : public MG_State::GLState::BackendBufferResource {
        public:
            ~GLESBufferResource() override = default;

            Uint id = 0;
            SizeT storageSize = 0;
            Bool storageInitialized = false;
            // ES context generation this resource's id belongs to; ids from a
            // destroyed context are invalid and must not be deleted or reused.
            Uint contextGeneration = 0;
            // Frontend change serial the backend storage reflects. When immediate
            // ops cannot run (ops unregistered, no current context), this lags and
            // EnsureBufferResource falls back to a full re-upload. Atomic: read on
            // the context-owning thread while ops on other threads may update it.
            std::atomic<Uint64> syncedChangeSerial{0};
            // Ops that arrived while no ES context was current on the calling thread
            // (or before storage existed); replayed by EnsureBufferResource. The ES
            // context migrates between app threads, so deferring ops can race with
            // the owning thread replaying them: guard both fields with pendingMutex.
            Bool pendingRespecify = false;
            VecRange1D pendingRanges;
            std::mutex pendingMutex;
            // Buffer-mutation epoch (see CurrentBufferMutationEpoch) at which this
            // resource last probed IsBufferDrawClean == true, 0 = never (epochs start
            // at 1). Written only on the draw thread; per-draw resource consumers
            // (the UBO binding walk) skip the probe while their pre-pass epoch read
            // matches, exactly like the per-VAO memo stamps.
            Uint64 drawCleanEpoch = 0;
            // Zero-copy coherent persistent map (EXT_buffer_storage): the GL store is
            // immutable, persistently+coherently mapped, and persistentPtr is what the app
            // (and the frontend PipeResource) write into directly. While set, draw-time
            // sync is a no-op and no per-draw glBufferSubData is issued. Cleared on ES
            // context loss.
            Bool persistentMapped = false;
            void* persistentPtr = nullptr;
            // The GL store behind `id` was created with glBufferStorageEXT and is
            // therefore IMMUTABLE - glBufferData cannot respecify it and it must never be
            // recycled through the size-keyed buffer pool. Tracked separately from
            // persistentMapped because the two come apart: a glMapBufferRange that fails
            // after its glBufferStorageEXT succeeded leaves immutable storage behind with
            // no map, and a respecification then has to retire the id rather than hand it
            // to glBufferData, which the driver would silently refuse.
            Bool immutableStorage = false;
        };

        // Registered as the frontend's BufferBackendOps at backend init and on
        // every MakeCurrent (the ES context can be destroyed and recreated, e.g.
        // by the trace replayer's probe context).
        void RegisterBufferBackendOps();
        void UnregisterBufferBackendOps();
        // The ES context died: unregister ops, invalidate all outstanding GL ids
        // (they belonged to the dead context) and drop deferred deletes.
        void OnBackendContextDestroyed();

        // Get-or-create the backend resource and bring its storage up to date
        // (creates the GL buffer, replays pending ops, pushes persistent-mapped
        // ranges). Requires the ES context to be current. Returns nullptr only
        // for null input.
        GLESBufferResource* EnsureBufferResource(const SharedPtr<MG_State::GLState::BufferObject>& bufferObject);
        // Existing resource or nullptr; performs no GL calls.
        GLESBufferResource* GetBufferResource(MG_State::GLState::BufferObject* bufferObject);
        // True when EnsureBufferResource(frontend) would provably fall straight through
        // every branch and do no work — i.e. `resource` is still the frontend's own
        // resource, its id belongs to the live ES context, and either it is the
        // zero-copy coherent persistent store (draw-time sync is a no-op by design) or
        // the storage is initialized at the right size with no pending ops and a synced
        // change serial while the buffer is not mapped (an active map may owe a
        // per-draw persistent-range push, so it always takes the full path).
        // `frontend` must be non-null and alive; the caller guarantees that by holding
        // (or shadowing something that holds) a SharedPtr to it. Enables the per-VAO
        // resolved-buffers memo to skip EnsureBufferResource on clean static buffers.
        Bool IsBufferDrawClean(const MG_State::GLState::BufferObject* frontend, const GLESBufferResource* resource);

        // Deletes GL buffers whose owning frontend objects died (possibly on a
        // thread without a current ES context). Called from draw-time sync.
        void ProcessDeferredBufferReleases();

        // glBindBuffer with a redundant-bind cache for GL_ARRAY_BUFFER.
        void BindBufferId(GLenum target, Uint id);
        void InvalidateArrayBufferBindingCache();
        // Redundant-bind caches for the driver-level GL_PIXEL_PACK/UNPACK_BUFFER
        // bindings. Every backend readback (glReadPixels / pack-PBO map) and pixel
        // upload site routes its binding through these so the shadow always matches
        // the driver; the resting state between operations is 0, which keeps any
        // path that implicitly assumes "no PBO bound" correct. Scrubbed when a
        // buffer id is deleted/pooled (GL resets a deleted buffer's bindings to 0,
        // and a recycled name matching the shadow would false-skip the rebind) and
        // invalidated on MakeCurrent (context may reset).
        void BindPixelPackBufferId(Uint id);
        void BindPixelUnpackBufferId(Uint id);
        void InvalidatePixelBufferBindingCaches();
        // A GL buffer id is being deleted by code outside BufferImpl (e.g. the VAO
        // client-attribute staging buffers): scrub every buffer-binding shadow that
        // could false-skip when the name is recycled.
        void NoteBufferIdDeleted(Uint id);
        // Bumped whenever a live GLESBufferResource's driver id is retired and re-minted
        // while its frontend buffer stays alive (persistent-map adoption, immutable-store
        // retire). The VAO twins' baked glVertexAttribPointer / element-array bindings
        // key on FRONTEND versions, which a backend-side re-mint does not move - without
        // this generation the driver VAO would keep fetching through the deleted id (or
        // its retained store) forever. Compared and stamped by
        // BackendVertexArrayObject::SyncToBackend.
        extern Uint64 g_bufferBackendIdGeneration;
        // Redundant-bind cache for INDEXED buffer bindings (glBindBufferBase/Range on
        // GL_UNIFORM_BUFFER / GL_SHADER_STORAGE_BUFFER / GL_TRANSFORM_FEEDBACK_BUFFER):
        // skips the GL call when the (id, range) already at that index matches, like the
        // array-buffer/texture/sampler caches already do. Invalidated on MakeCurrent
        // (context may reset).
        // Binds the transform feedback capture points [0, bufferCount) from the frontend
        // state, and touches nothing else - in particular it never binds a zero the
        // application did not ask for. See the definition for why that matters on Mali.
        void SyncTransformFeedbackBindingPoints(SizeT bufferCount);
        void BindBufferBaseCached(GLenum glTarget, Uint index, Uint id);
        void BindBufferRangeCached(GLenum glTarget, Uint index, Uint id, GLintptr offset, GLsizeiptr size);
        void InvalidateIndexedBufferBindingCache();
        // The transform feedback capture points are per-transform-feedback-OBJECT state, so
        // every glBindTransformFeedback swaps all of them under the shadow above. XfbImpl
        // calls this on each bind/delete.
        void InvalidateTransformFeedbackBindingShadows();
        // Re-issues the GL_ATOMIC_COUNTER_BUFFER binding points a program's shaders declare as
        // GL_SHADER_STORAGE_BUFFER bindings at the reserved slots the transpiled ESSL was built
        // against (BackendProgramObjectImpl::GetAtomicCounterBindings /
        // GetAtomicCounterEsslBindingTop). ES has no counter-buffer target at all, so without
        // this the shader reads a storage block nobody ever bound a buffer to and the buffer the
        // application bound never reaches the driver.
        void SyncAtomicCounterBuffers(const Vector<Int>& glBindings, Int esslBindingTop);
        // Buffer-storage pool maintenance. TrimBufferPool evicts over-budget entries
        // (called once per frame from Present); ClearBufferPool drops all pooled ids
        // without glDeleteBuffers (called when the ES context is going away).
        void TrimBufferPool();
        void ClearBufferPool();

        // --- Global-UBO ring ------------------------------------------------------
        // One persistently+coherently mapped buffer (EXT_buffer_storage) shared by
        // every program's lowered default-uniform block. Each content change is
        // bump-allocated into a fresh slot and bound with glBindBufferRange, so the
        // CPU never rewrites bytes the GPU may still be reading — the per-draw
        // glBufferSubData into one static UBO forced Adreno to resolve that
        // write-after-read hazard on every uniform-dirtying draw (MC dirties
        // uniforms every draw). Reclamation rides the Present() frame-fence
        // watermark; no ring bytes are recycled before their frame's GPU work
        // completed.
        //
        // A program's cached slot, reusable within one frame while the frontend UBO
        // content version is unchanged. Cross-frame reuse is intentionally not
        // attempted: later same-frame allocations may recycle bytes of completed
        // frames, so re-referencing them would need per-bind pinning — rewriting
        // GetUBOSize() bytes once per program per frame is far cheaper.
        struct UboRingAllocation {
            Uint32 contentVersion = ~0u; // frontend UBO content version held at `offset`
            Uint32 ringGeneration = 0;   // ring identity the slot lives in (0 = never valid)
            Uint64 frameSerial = ~Uint64{0}; // frame the slot was written in
            SizeT offset = 0;
        };
        // False when the feature is disabled, EXT_buffer_storage / fences are
        // missing, the ES context is not current, or ring creation already failed
        // under this context (callers then take the legacy glBufferSubData path).
        Bool UboRingAvailable();
        // Bump-allocate `size` bytes aligned to GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT.
        // Grows the ring (new GL store, generation bump) when the in-flight span
        // would be overrun. Returns false when storage (re)creation fails.
        Bool UboRingAllocate(SizeT size, SizeT& outOffset);
        void* UboRingMappedPtr();
        Uint UboRingBufferId();
        Uint32 UboRingGeneration();
        // Present()-time upkeep: records the frame's high-water mark for reclamation
        // and deletes grown-away ring stores once the GPU is done with them.
        void UboRingOnPresent();

        // --- Texture unpack-PBO ring ----------------------------------------------
        // The same persistent-mapped bump allocator, staging TEXTURE UPLOADS. A
        // glTexSubImage from client memory hands the driver a pointer it must read
        // before the call returns, so the copy has to be ordered against whatever GPU
        // work still reads the destination texture: Mali resolves that by BLOCKING the
        // calling thread (osup_sync_object_wait) instead of ghosting, and Minecraft
        // re-uploads animated atlas sprites and the lightmap every tick into textures
        // the in-flight frame is still sampling. Staging the bytes into a
        // GPU-visible unpack PBO and passing an OFFSET instead lets the driver queue
        // the copy in the command stream with no CPU wait at all.
        //
        // Same reclamation contract as the UBO ring: no ring bytes are recycled before
        // the frame that referenced them completed on the GPU, so a staged block stays
        // intact for as long as the queued transfer can still be reading it. The store
        // therefore settles at roughly (bytes staged per frame) x (frames in flight),
        // which is what to watch if this ring ever shows up in an RSS regression: it
        // grows on demand from 4 MiB and is capped, not unbounded.
        //
        // False when the feature is disabled (MOBILEGL_DISABLE_UNPACK_RING),
        // EXT_buffer_storage / fences are missing, the ES context is not current, or
        // ring creation already failed under this context. Callers then upload from
        // the client pointer exactly as before.
        Bool UnpackRingAvailable();
        // Bump-allocate `size` bytes aligned to 64 (a PBO-sourced glTexSubImage only
        // owes the driver the pixel type's own alignment). Grows the ring when the
        // in-flight span would be overrun; false when the request exceeds the ring's
        // size cap or storage (re)creation fails.
        Bool UnpackRingAllocate(SizeT size, SizeT& outOffset);
        void* UnpackRingMappedPtr();
        Uint UnpackRingBufferId();
        // Largest single staging request the ring can ever satisfy.
        SizeT UnpackRingMaxBytes();
        void UnpackRingOnPresent();

        // --- Buffer upload ring ---------------------------------------------------
        // The same persistent-mapped bump allocator, staging APP BUFFER UPDATES
        // (glBufferSubData / non-persistent map flushes) whose destination store may
        // still be referenced by in-flight GPU work. Mali resolves that WAR hazard by
        // BLOCKING the calling glBufferSubData (osup_sync_object_wait) until every
        // referencing job retires - Minecraft 26.3 rewrites its chunk-section and
        // dynamic-transform UBOs and streams chunk meshes with per-frame SubData, and
        // each such call serialized against the whole GPU queue (~1 fps while chunks
        // stream in, and again on every camera pan). App SubData ranges are queued on
        // the resource instead (the frontend shadow already holds the bytes) and
        // draw-time sync drains them: bytes staged into this ring, then one
        // glCopyBufferSubData per merged range - the copy is ordered on the GPU
        // timeline, so the hazard costs no CPU wait. Reclamation contract identical
        // to the other two rings. MOBILEGL_DISABLE_UPLOAD_RING restores the
        // historical immediate-upload path (negative control / escape hatch).
        void UploadRingOnPresent();
    } // namespace BufferImpl

    namespace VertexArrayImpl {
        class BackendVertexArrayObject {
        public:
            BackendVertexArrayObject();
            ~BackendVertexArrayObject();
            void SyncToBackend(const SharedPtr<MG_State::GLState::VertexArrayObject>& stateVAOObject);
            void SyncClientSideAttributesForDrawArrays(
                const SharedPtr<MG_State::GLState::VertexArrayObject>& stateVAOObject, GLint first, GLsizei count);
            Uint GetBackendVertexArrayId() const { return m_backendVAOId; }
            void Bind() const;

            // Draw-path memo of SyncNeccessaryBuffers' attribute walk for this VAO: the
            // distinct enabled-attribute buffers (deduped) and the index buffer, resolved
            // to their backend resources once. Valid while the VAO's config version is
            // unchanged — every attach/enable/disable/format mutation bumps it (the same
            // invariant SyncToBackend's gate already leans on), and the VAO's attribute
            // SharedPtrs pin each memoed frontend buffer for exactly that long, so the raw
            // pointers cannot dangle on a hit. Per-buffer cleanliness is NOT memoed here:
            // each hit re-checks IsBufferDrawClean (resource identity, context generation,
            // pending ops, change serial) and falls back to EnsureBufferResource for just
            // the dirty entries via their attribute index. The IBO entry is keyed on the
            // slot's bound-object identity instead (its slot version is a wrapping Uint16
            // and is not covered by the config version).
            struct ResolvedDrawBuffers {
                struct Entry {
                    MG_State::GLState::BufferObject* frontend = nullptr;
                    BufferImpl::GLESBufferResource* resource = nullptr;
                    Uint8 attribIndex = 0;
                };
                Bool valid = false;
                Uint32 configVersion = 0;
                Uint count = 0;
                Array<Entry, MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS> entries;
                MG_State::GLState::BufferObject* iboFrontend = nullptr;
                BufferImpl::GLESBufferResource* iboResource = nullptr;
                // Buffer-mutation epoch (BufferImpl::CurrentBufferMutationEpoch) at which
                // the LAST probe pass found every entry / the IBO clean; 0 = not stamped
                // (epochs start at 1). While a stamp matches the pre-pass epoch read, the
                // probes are skipped outright: any path that can dirty ANY buffer bumps
                // the epoch (the exhaustive site list lives at the epoch declaration).
                // The IBO stamp is only trusted together with the bound-object identity
                // compare - the VAO's index slot can rebind with no epoch or config move.
                Uint64 vboCleanEpoch = 0;
                Uint64 iboCleanEpoch = 0;
            };
            ResolvedDrawBuffers& GetResolvedDrawBuffersMemo() { return m_resolvedDrawBuffers; }

            // Memo for SyncCurrentVertexAttributeValues: which of a program's ACTIVE
            // attribute locations lack an enabled array in this VAO (those read the
            // context's current generic value instead of a buffer). Keyed on the VAO
            // config version (enable/disable bumps it) and the program's active-location
            // mask. Hosted per twin — the former function-static single entry missed on
            // every draw once the app cycled VAOs, re-reading the cold attribute slots.
            struct PendingAttribValueMask {
                Bool valid = false;
                Uint32 configVersion = 0;
                Uint32 activeMask = 0;
                Uint32 pendingMask = 0;
            };
            PendingAttribValueMask& GetPendingAttribValueMaskMemo() { return m_pendingAttribValueMask; }

        private:
            // Narrows one enabled GL_DOUBLE array into a tightly packed float32 stream held in
            // this VAO's own scratch buffer and declares the attribute against it. ES has no
            // 64-bit vertex format, but the source bytes are ordinary IEEE-754 doubles and every
            // fp64 value in every shader is already narrowed to 32 bits (DemoteFloat64Pass), so
            // narrowing the ARRAY is the coherent completion of that decision rather than
            // dropping it. Returns false when the stream cannot be built, in which case the
            // caller must DISABLE the array - leaving a 64-bit array enabled with no pointer is
            // what the Adreno driver turns into a SIGSEGV at the next draw.
            Bool SyncFloat64AttributeAsFloat32(Uint attribIndex, const MG_State::GLState::VertexAttribute& attrib,
                                               Uint32 fetchBaseInstance);

            // What the converted float32 stream in m_convertedAttributeBufferIds[i] was built
            // from. A hit skips the CPU conversion and the re-upload; the buffer's change serial
            // is part of the key, so a glBufferSubData into the source invalidates it.
            struct ConvertedFloat64Stream {
                Bool valid = false;
                Uint64 sourceLifetimeId = 0;
                Uint64 sourceChangeSerial = 0;
                SizeT sourceOffset = 0;
                SizeT sourceStride = 0;
                SizeT componentCount = 0;
                SizeT elementCount = 0;
            };

            ResolvedDrawBuffers m_resolvedDrawBuffers;
            PendingAttribValueMask m_pendingAttribValueMask;
            Uint m_backendVAOId = 0;
            Array<Uint, MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS> m_clientAttributeBufferIds;
            // Scratch stores for the buffer-backed GL_DOUBLE narrowing. Deliberately separate
            // from m_clientAttributeBufferIds: that one holds the per-draw upload of a
            // CLIENT-MEMORY array, and an attribute index can carry both shapes over its life.
            Array<Uint, MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS> m_convertedAttributeBufferIds;
            Array<ConvertedFloat64Stream, MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS>
                m_convertedAttributeStreams;
            // True while at least one attribute of this VAO is fed by a converted stream. Such a
            // stream is derived from buffer CONTENT, which no VAO version covers, so the config
            // version early-out in SyncToBackend must not be trusted while it is set.
            Bool m_hasConvertedFloat64Attribute = false;
            Bool m_isInitialized = false;
            Uint16 m_syncedIndexBufferVersion = 0;
            // Identity of the buffer the version above was stamped against. Raw and never
            // dereferenced: the slot version is a wrapping Uint16 (see the ResolvedDrawBuffers
            // IBO memo and the packed_pixels postmortem at BindCurrentFBO), so the version
            // alone would read a wrapped-back count with a different buffer bound as clean.
            const MG_State::GLState::BufferObject* m_syncedIndexBufferObject = nullptr;
            // Aggregate gate over the per-attribute walk below: the frontend bumps its config
            // version on every per-attribute version bump (the three Bump*Version functions are
            // its only writers), so an unchanged config version proves every per-attribute
            // compare in SyncToBackend would come up clean. The index-buffer slot has its own
            // version and is NOT covered. The Bool (not a sentinel value) marks "never synced".
            Bool m_hasSyncedConfigVersion = false;
            Uint32 m_syncedConfigVersion = 0;
            Array<MG_State::GLState::VertexAttributeVersion, MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS>
                m_syncedAttributeVersions;
            // Byte shift currently baked into the instanced arrays' offsets by the baseInstance
            // emulation (see SetPendingFetchBaseInstance). It is draw state, not VAO state, so it
            // is deliberately NOT covered by the config version: the frontend never bumps for it.
            // Kept here because it describes what was last EMITTED, which is what the next sync
            // has to correct.
            Uint32 m_syncedFetchBaseInstance = 0;
            // BufferImpl::g_bufferBackendIdGeneration as of this twin's last emit. A
            // mismatch means some live buffer's driver id was re-minted since; the ids
            // baked into the driver VAO's attribute/element bindings may be dead even
            // though every frontend version matches, so the next sync re-emits them all.
            Uint64 m_syncedBufferIdGeneration = 0;
        };

        extern StateBackendObjectRegistry<MG_State::GLState::VertexArrayObject, BackendVertexArrayObject>
            g_backendVertexArrayObjects;

        // Shadowed glBindVertexArray: every backend VAO bind goes through here so a
        // draw's second bind of the same VAO (SyncToBackend, then PrepareForDraw's
        // re-bind) reaches the driver once. Invalidate whenever the ES context is
        // replaced - ids restart and the resting binding is 0 again.
        void BindBackendVAOId(Uint id);
        void InvalidateVAOBindingCache();
        // ES resets the binding to 0 when the currently bound VAO is deleted.
        void NoteVAOIdDeleted(Uint id);

        // baseInstance emulation for drivers without GL_EXT_base_instance. GL fetches an
        // instanced array at element "floor(instance / divisor) + baseInstance", and ES has no
        // way to say the "+ baseInstance" part - so it is folded into the attribute's own byte
        // offset (baseInstance * stride) for every divisor'd array, which is exactly equivalent.
        // Must be set BEFORE PrepareForDraw so the VAO sync sees it, and cleared after the draw
        // so the next one refetches from element 0; ScopedFetchBaseInstance does both.
        void SetPendingFetchBaseInstance(Uint32 baseInstance);
        Uint32 GetPendingFetchBaseInstance();

        class ScopedFetchBaseInstance {
        public:
            explicit ScopedFetchBaseInstance(Uint32 baseInstance) { SetPendingFetchBaseInstance(baseInstance); }
            ~ScopedFetchBaseInstance() { SetPendingFetchBaseInstance(0); }
            ScopedFetchBaseInstance(const ScopedFetchBaseInstance&) = delete;
            ScopedFetchBaseInstance& operator=(const ScopedFetchBaseInstance&) = delete;
        };
    } // namespace VertexArrayImpl

    namespace TextureImpl {
        inline Bool IsSupportedTextureTarget(TextureTarget target) {
            // Every desktop-only target is stored on an ES one; see MapToBackendTextureTarget.
            (void)target;
            return true;
        }

        // ES has none of the desktop-only targets: 1D textures are stored as 2D (height 1), 1D
        // arrays as 2D arrays (height 1, layers in depth), and rectangle textures as plain 2D -
        // they are single-level and already clamp, so only the non-normalized coordinates differ.
        // Must match the shader-side emulation: SPIRV-Cross handles 1D/1D-array itself, and
        // ShaderCompiler::LowerRectImages rewrites rectangle images (declining any module
        // whose lookups are not integer-coordinate, which SPIRV-Cross then still rejects).
        inline TextureTarget MapToBackendTextureTarget(TextureTarget target) {
            switch (target) {
            case TextureTarget::Texture1D:
            case TextureTarget::TextureRectangle:
                return TextureTarget::Texture2D;
            case TextureTarget::Texture1DArray:
                return TextureTarget::Texture2DArray;
            default:
                return target;
            }
        }

        inline GLenum ConvertTextureTargetToBackendGLEnum(TextureTarget target) {
            return MG_Util::ConvertTextureTargetToGLEnum(MapToBackendTextureTarget(target));
        }

        inline GLenum ConvertTextureUploadTargetToBackendGLEnum(TextureUploadTarget uploadTarget) {
            switch (uploadTarget) {
            case TextureUploadTarget::Texture1D:
            case TextureUploadTarget::TextureRectangle:
                return GL_TEXTURE_2D;
            case TextureUploadTarget::Texture1DArray:
                return GL_TEXTURE_2D_ARRAY;
            default:
                return MG_Util::ConvertTextureUploadTargetToGLEnum(uploadTarget);
            }
        }

        // 1D arrays store layers in the state-side height; the ES 2D-array image keeps height 1 and
        // moves the layer count into depth.
        inline IntVec3 GetBackendUploadSize(TextureTarget stateTarget, const IntVec3& texelSize) {
            if (stateTarget == TextureTarget::Texture1DArray) {
                return {texelSize.x(), 1, texelSize.y()};
            }
            return texelSize;
        }

        inline Bool IsMultisampleTextureTarget(TextureTarget target) {
            return target == TextureTarget::Texture2DMultisample ||
                   target == TextureTarget::Texture2DMultisampleArray;
        }

        inline Bool SupportsWrapR(TextureTarget target) {
            return target == TextureTarget::Texture3D || target == TextureTarget::TextureCubeMap;
        }

        // Components per texel the frontend format's client data carries, for the three-channel
        // formats that can be widened to a four-channel colour-renderable target; 0 for everything
        // else. See PrepareChannelWidenedUpload.
        Uint GetWidenableClientComponentCount(TextureInternalFormat format);

        // True when a widenable format's components are integer rather than normalized, which is
        // what decides the synthetic alpha's value: GL_RGB8I and GL_RGB8_SNORM are both uploaded
        // as GL_BYTE, but their 1.0 is 1 and 0x7F respectively.
        Bool IsIntegerWidenableFormat(TextureInternalFormat format);

        // Repacks three-component client data as four components with an alpha of 1.0 in
        // `uploadType`, for a format the backend widened to keep a colour attachment renderable.
        // Returns `data` untouched when no widening applies. Pure CPU and context-free so a unit
        // test can exercise the exact packing the driver is handed; `widenedData` is the caller's
        // scratch buffer and has to outlive the returned pointer.
        // `alphaOneCodeOverride`, when non-zero, replaces the value written into the synthetic
        // alpha channel: an image carrier that holds a NORMALIZED format's channel CODES has to
        // pad alpha with that channel's saturated CODE (65535, 32767, 3), which neither of the
        // transfer type's own "ones" is.
        const void* PrepareChannelWidenedUpload(Uint componentCount, const IntVec3& texelSize, const void* data,
                                                SizeT byteSize, GLenum uploadType, Vector<Uint8>& widenedData,
                                                Bool integerData = false, Uint32 alphaOneCodeOverride = 0u);

        // Splits a GL_UNSIGNED_INT_2_10_10_10_REV shadow (rgb10_a2, rgb10_a2ui) into the four
        // GL_UNSIGNED_SHORT channel CODES its GL_RGBA16UI image carrier is uploaded as: red in
        // bits 0-9, green 10-19, blue 20-29, alpha 30-31. Pure CPU and context-free so a unit test
        // can pin the exact fields; `widenedData` is the caller's scratch and has to outlive the
        // returned pointer.
        const void* PreparePackedIntWidenedUpload(const IntVec3& texelSize, const void* data, SizeT byteSize,
                                                  Vector<Uint8>& widenedData);

        struct StateTextureBasicInfo { // Used for tracking texture state changes
            TextureInternalFormat internalFormat = TextureInternalFormat::Unknown;
            SizeT width = 0;
            SizeT height = 0;
            SizeT depth = 0;
            SizeT mipmapLevels = 0;
            Uint bufferExternalIndex = 0;
            Int samples = 0;
            Bool fixedSampleLocations = true;

            bool operator==(const StateTextureBasicInfo& other) const {
                return internalFormat == other.internalFormat && width == other.width && height == other.height &&
                       depth == other.depth && mipmapLevels == other.mipmapLevels &&
                       bufferExternalIndex == other.bufferExternalIndex && samples == other.samples &&
                       fixedSampleLocations == other.fixedSampleLocations;
            }

            bool operator!=(const StateTextureBasicInfo& other) const { return !(*this == other); }
        };

        inline const Uint TempTextureUnit = 0;
        class BackendTextureObject {
        public:
            BackendTextureObject();
            // Deletes the GL texture (frontend glDeleteTextures used to leak every
            // backend id for the context lifetime) and scrubs the binding/scratch-FBO
            // shadows so a recycled name or heap address cannot false-skip a rebind.
            ~BackendTextureObject();
            BackendTextureObject(const BackendTextureObject&) = delete;
            BackendTextureObject& operator=(const BackendTextureObject&) = delete;
            void SyncMipmapsToBackend(const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject);
            // The storage half of the sync for a texture created by glTextureView. Instead of
            // allocating storage and replaying uploads, it makes this object's ES name BE a view
            // of the storage texture's ES name (EXT/OES_texture_view), which is what gives the
            // two names one image and independent per-texture parameters at the same time. The
            // parameter and sampler halves are unchanged and run on this name as on any other.
            void SyncTextureViewToBackend(const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject);
            void StampViewSyncKeys(const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject);
            // The storage half of the sync for a texture created by glTextureView. Instead of
            // allocating storage and replaying uploads, it makes this object's ES name BE a view
            // of the storage texture's ES name (EXT/OES_texture_view), which is what gives the
            // two names one image and independent per-texture parameters at the same time. The
            // parameter and sampler halves are unchanged and run on this name as on any other.
            void SyncBuiltinSamplerToBackend(const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject);
            void SyncTextureParamsToBackend(const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject);
            // Marks the texture as one whose ES storage has to be image-bindable, which for a
            // non-core image format means re-minting it in the widening's carrier. Takes the state
            // object because the levels already uploaded have to be marked dirty again: the
            // re-mint allocates fresh storage and only replays what the shadow still calls dirty.
            void RequireImageBindableStorage(
                const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject);
            // Whether this texture's ES storage was minted in an image carrier rather than in the
            // frontend format's own layout - the readback has to ask, because for a NORMALIZED
            // carrier the storage is an integer texture holding codes and glGetTexImage still owes
            // the application floats.
            Bool RequiresImageBindableStorage() const { return m_imageBindableStorageRequired; }
            void Bind(GLenum target, Uint unit = TempTextureUnit);
            Uint GetBackendTextureId() const;

            // The id to hand glBindImageTexture for a SPLIT buffer image, or 0 when this texture
            // takes no split. See m_bufferImageSplitViewId.
            Uint GetBufferImageSplitViewId() const { return m_bufferImageSplitViewId; }

            // Aggregate first-level clean gate for the per-draw trio
            // SyncTextureParamsToBackend + SyncBuiltinSamplerToBackend +
            // SyncMipmapsToBackend: EXACTLY the conjunction of their own early-outs
            // (params version == synced params version; builtin-sampler version ==
            // synced sampler version; and SyncMipmapsToBackend's cheap gate - stamped
            // trio + content version + Mipmap storage). True means each of the three
            // would provably return without work, so the caller may skip the calls;
            // false only falls through to the three calls, whose own gates re-decide
            // individually - this gate must never be MORE permissive than they are.
            // `contextId`/`samplingGeneration` are the frontend context's current
            // values, hoisted by the caller so a per-draw list walk reads them once
            // instead of per texture. `t` must be the live frontend texture.
            // True while a driver-side re-mint has left the parameter caches describing a texture
            // that no longer exists; SyncTextureObjectToBackend re-pushes them in the same sync.
            Bool NeedsParameterResync() const { return m_forceTextureParamsResync || m_forceSamplerResync; }

            Bool IsDrawSyncClean(const MG_State::GLState::ITextureObject* t, Uint64 contextId,
                                 Uint64 samplingGeneration) const {
                if (!m_isInitialized || m_syncedShapeContextId == 0 || m_syncedShapeContextId != contextId ||
                    m_syncedShapeGeneration != samplingGeneration) {
                    return false;
                }
                const Uint16 paramsVersion = t->GetTextureParamsVersion();
                if (m_syncedShapeParamsVersion != paramsVersion || m_syncedTextureParamsVersion != paramsVersion) {
                    return false;
                }
                if (m_syncedContentVersion == 0 || m_syncedContentVersion != t->GetContentVersion()) {
                    return false;
                }
                const auto& samplerObject = t->GetSamplerObject();
                if (!samplerObject || m_syncedSamplerVersion != samplerObject->GetVersion()) {
                    return false;
                }
                return t->GetStorageType() == TextureStorageType::Mipmap;
            }

        private:
            void RecreateBackendTexture();

            Uint m_backendTextureId = 0;
            // A SECOND buffer-texture name over the SAME buffer object, viewed in the split's
            // single-channel base format, used only as the glBindImageTexture target.
            //
            // The split needs the view to say r32f where the application said rg32f, but a buffer
            // texture that is image-bound may ALSO be read through a samplerBuffer - and the
            // sampler side is not subscript-rewritten, so re-describing the application's own
            // texture broke it: texelFetch(s, i) returned component 2i of the base view instead of
            // texel i's pair. That is exactly and only
            // KHR-GL42/43.shader_image_load_store.advanced-sync-imageAccess, which image-stores
            // into a GL_RG32F buffer texture and then reads the same texture through both an
            // imageBuffer and a samplerBuffer in one shader, comparing the two.
            //
            // Two names over one buffer cost nothing and alias exactly: a buffer texture owns no
            // storage, so both views are the application's bytes, and the split's whole premise is
            // that the two describe the same memory. The application's own name therefore keeps
            // the format it asked for - rg32f IS a legal SAMPLED buffer-texture format in ES 3.2,
            // it is only the IMAGE binding ES cannot spell - and the private name below carries
            // the split the shader was rewritten against. 0 when this texture takes no split.
            Uint m_bufferImageSplitViewId = 0;
            // For a texture created by glTextureView: the ES name of the storage texture this
            // one was last made a view OF. EXT_texture_view may be called only once per name, so
            // a storage texture that got re-minted underneath (RecreateBackendTexture) has to be
            // detected here and answered with a fresh name for the view as well - otherwise the
            // view would keep aliasing storage that no longer exists.
            Uint m_viewSourceBackendTextureId = 0;
            // For a texture created by glTextureView: the ES name of the storage texture this
            // one was last made a view OF. EXT_texture_view may be called only once per name, so
            // a storage texture that got re-minted underneath (RecreateBackendTexture) has to be
            // detected here and answered with a fresh name for the view as well - otherwise the
            // view would keep aliasing storage that no longer exists.
            // ES context generation the id was created under; a dtor running after
            // that context died must not delete a foreign (recycled) name.
            Uint m_contextGeneration = 0;
            Bool m_isInitialized = false;
            Bool m_imageBindableStorageRequired = false;
            Bool m_backendStorageImmutable = false;
            // Latches the "this driver has no buffer textures" report to once per texture. The
            // report is emitted from the respecify path, which bails before recording the state
            // it was asked to apply - so without the latch the texture stays permanently dirty
            // and every draw of every frame logs the same line.
            Bool m_bufferTextureUnsupportedReported = false;
            StateTextureBasicInfo m_prevTextureInfo;
            // Frontend content version at the last completed mipmap sync. The per-draw
            // clean probe compares this before rebuilding shape info and scanning
            // per-level dirty flags; 0 never matches a real version (they start at 1).
            Uint64 m_syncedContentVersion = 0;
            // First-level clean gate for SyncMipmapsToBackend, checked before even the
            // IsComplete()/shape-probe walk. Valid only as a trio with the content and
            // texture-params versions: the context's sampling-resolution generation moves on
            // EVERY texture-shape mutation (BumpShapeVersion is the only writer of shape and
            // unconditionally bumps it), the content version on every CPU pixel mutation, and
            // the params version covers SetSamples/SetFixedSampleLocations, which bump neither
            // of the other two but feed the shape probe. The context id pins the generation to
            // the context that produced it - generations restart at 0 with a new context, and a
            // texture is owned by exactly one context (share groups are not implemented), so a
            // mutation can never happen under a context this key does not name. 0 = never
            // stamped (real context ids start at 1). Backend-side invalidation rides on
            // m_isInitialized: RequireImageBindableStorage and RecreateBackendTexture clear it.
            Uint64 m_syncedShapeContextId = 0;
            Uint64 m_syncedShapeGeneration = 0;
            Uint16 m_syncedShapeParamsVersion = 0;
            SamplerParameters m_cacheSamplerParameters;
            UintVec2 m_cacheLodRange = {0, 1000};
            // All three representations plus the form, because none of them alone identifies the
            // border colour the driver texture is holding: two integer borders can share one float
            // (anything differing above 2^24), and a Float -> Int transition can leave every number
            // unchanged while still needing a different driver entry point.
            FloatVec4 m_cacheBorderColor = {0.0f, 0.0f, 0.0f, 0.0f};
            IntVec4 m_cacheBorderColorI = {0, 0, 0, 0};
            UintVec4 m_cacheBorderColorUI = {0, 0, 0, 0};
            BorderColorForm m_cacheBorderColorForm = BorderColorForm::Float;
            Vec4<TextureSwizzleParam> m_cacheSwizzleParams = {TextureSwizzleParam::Red, TextureSwizzleParam::Green,
                                                              TextureSwizzleParam::Blue, TextureSwizzleParam::Alpha};
            // GL_DEPTH_STENCIL_TEXTURE_MODE. GL_DEPTH_COMPONENT is the GL and ES default, so a
            // texture that never asks for the stencil aspect never emits the call. The
            // depth/stencil readback and replicate-blit emulations also write this parameter
            // raw, but only ever on their own scratch textures (never on an application
            // texture), so they cannot desynchronise this cache.
            GLenum m_cacheDepthStencilTextureMode = GL_DEPTH_COMPONENT;
            Uint16 m_syncedSamplerVersion = 0;
            Uint16 m_syncedTextureParamsVersion = 0;
            // Set when the driver texture underneath was regenerated and has therefore lost every
            // parameter already pushed onto it: the params-version early-out has to be overridden
            // once, or an unchanged version would skip the re-push forever.
            Bool m_forceTextureParamsResync = false;
            // The same problem for the FILTER state, which lives in m_cacheSamplerParameters and
            // is gated on the frontend sampler's version rather than on the params version. A
            // re-mint leaves that cache describing values the new driver texture never received,
            // and an unchanged sampler version would then skip re-pushing them forever. This
            // matters more than mis-filtering: ES makes a texture INCOMPLETE when its filters do
            // not suit its level set (any integer texture with a non-NEAREST filter, or a
            // single-level texture with a mipmapping filter), and an incomplete texture samples
            // (0, 0, 0, 1) rather than its contents.
            Bool m_forceSamplerResync = false;
        };

        void ActivateTextureUnit(Uint unit);
        void UnbindTexture(Uint unit, GLenum target);
        extern StateBackendObjectRegistry<MG_State::GLState::ITextureObject, BackendTextureObject>
            g_backendTextureObjects;
        SharedPtr<BackendTextureObject>& SyncTextureObjectToBackend(
            const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
            Bool imageBindableStorageRequired = false);
        // Brings every texture the next draw reads - the touched units' bindings and the draw
        // FBO's texture attachments - onto the backend, through the two borrowed-pair memos
        // documented at their definitions. Declared here so tests can drive those memos directly.
        void SyncNeccessaryTextures();
        extern Array<Array<BackendTextureObject*, (SizeT)TextureTarget::TextureTargetCount>,
                     MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS>
            g_boundTexturesCache;
        extern Uint g_activeTextureUnit;
    } // namespace TextureImpl

    namespace FramebufferImpl {
        class BackendFramebufferObject {
        public:
            BackendFramebufferObject();
            // Deletes the driver framebuffer and scrubs the binding shadow. Without it every
            // frontend glDeleteFramebuffers leaked one ES framebuffer for the process lifetime;
            // an app that creates a framebuffer per readback (GL CTS packed_pixels does ~3300
            // per case) walked the driver into hundreds of megabytes of dead framebuffers and
            // out of the resources a later attachment needs.
            ~BackendFramebufferObject();
            BackendFramebufferObject(const BackendFramebufferObject&) = delete;
            BackendFramebufferObject& operator=(const BackendFramebufferObject&) = delete;
            void SyncToBackend(const SharedPtr<MG_State::GLState::FramebufferObject>& stateFBOObject,
                               FramebufferTarget asTarget);
            // Apply only this FBO's read buffer (glReadBuffer) to the backend. Split out so it can
            // still run when SyncCurrentFBO skips the READ-target sync because the same GL FBO is
            // bound as both draw and read (otherwise glReadBuffer changes would be silently dropped).
            void SyncReadBufferToBackend(const SharedPtr<MG_State::GLState::FramebufferObject>& stateFBOObject);
            void InvalidateSyncedState();
            Uint GetBackendFramebufferId() const { return m_backendFBOId; }
            void Bind(FramebufferTarget target) const;
            //            FramebufferAttachmentType GetCompactedAttachmentTypeAtDrawBufferIndex(Int index);
            GLenum GetBackendAttachmentType(FramebufferAttachmentType frontendAtt) const;

        private:
            Uint m_backendFBOId = 0;
            Uint m_contextGeneration = 0;

            /* this will save buffers in its original form,
               reversion, absence or not consecutive are all allowed, as long as GL spec allows it
               i.e. it could be like [COLOR_ATTACHMENT0, COLOR_ATTACHMENT5, NONE, COLOR_ATTACHMENT4]
               Probably useful to re-link shader output according to this.
               aka. realizing `glBindFragDataLocation`
             */
            FramebufferAttachmentType m_frontendDrawBuffers[MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS] = {
                FramebufferAttachmentType::None};
            /* this will save buffers in stricter ES rules
               reversion, absence or not consecutive are not allowed, according to ES spec
               i.e. it could be like [COLOR_ATTACHMENT0, COLOR_ATTACHMENT1, NONE, COLOR_ATTACHMENT3, ...]
               this array could be provided as data directly to ES `glDrawBuffers` function
             */
            GLenum m_backendDrawBuffers[MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS] = {GL_NONE};

            static constexpr Uint MAX_COLOR_ATTACHMENT_SLOTS =
                static_cast<Uint>(FramebufferAttachmentType::Color31) -
                static_cast<Uint>(FramebufferAttachmentType::Color0) + 1;
            /* Where each frontend GL_COLOR_ATTACHMENTn image physically lives in the backend ES
               framebuffer, as a GL_COLOR_ATTACHMENTm enum. ES only accepts glDrawBuffers bufs[s] ==
               GL_COLOR_ATTACHMENTs, so a GL draw-buffer slot s naming attachment a forces a's image
               under backend slot s. This table is the single owner of that decision and is kept a
               PERMUTATION of the backend colour slots: every other attachment keeps its identity
               slot when that slot survived, and is parked on the lowest free slot when it did not.
               Deriving the point per-query from the draw-buffer array instead handed the identity
               point to any attachment that was not a draw buffer - i.e. exactly the point a
               relocated draw buffer had just taken over. The permutation is only true of the
               PHYSICAL framebuffer because the attachment loop detaches a point whose frontend
               owner is empty; do not remove that detach. */
            GLenum m_backendColorSlots[MAX_COLOR_ATTACHMENT_SLOTS] = {GL_NONE};
            /* Rebuild m_backendColorSlots from the frontend draw-buffer array. Returns true when any
               attachment moved, i.e. when the physical attachments and the memoised read buffer have
               to be re-applied. */
            Bool RecomputeBackendColorSlots(
                const MG_State::GLState::FramebufferObject::FramebufferAttachmentArray& stateDrawBuffers);

            FramebufferAttachmentType m_frontendReadBuffer = FramebufferAttachmentType::Color0;
            GLenum m_backendReadBuffer = GL_COLOR_ATTACHMENT0;

            using FramebufferObject = MG_State::GLState::FramebufferObject;
            FramebufferObject::FramebufferAttachmentVersionArray m_syncedFrontendAttachmentVersions = {0};
            // g_attachmentBackendIdGeneration as of this twin's last attachment walk. A
            // mismatch means some backend texture id was re-minted since, and any of this
            // twin's attachment points may still hold the dead id even though the frontend
            // attachment versions match - so the walk re-attaches everything first.
            Uint64 m_syncedBackendIdGeneration = 0;
        };

        extern StateBackendObjectRegistry<MG_State::GLState::FramebufferObject, BackendFramebufferObject>
            g_backendFramebufferObjects;
        // True when the read buffer names a fixed-point (norm/snorm) attachment that the
        // backend actually stores in a floating-point format. GL clamps a read from a
        // fixed-point colour buffer to [0,1] (GL_CLAMP_READ_COLOR defaults to
        // GL_FIXED_ONLY); the substituted float storage would not, so the readback path
        // has to apply the clamp itself.
        Bool IsFixedPointFallbackReadAttachment();

        // True when the read buffer names a three-channel attachment the backend actually stores
        // in a four-channel format (the colour-renderable widening). A format without alpha reads
        // back as 1.0, so the readback path has to overwrite the alpha the draw left behind -
        // unconditionally, since this is the format's own semantics rather than the
        // GL_CLAMP_READ_COLOR rule the clamp above implements.
        Bool IsAlphaWidenedFallbackReadAttachment();

        // True when this attachment's storage carries an alpha channel its frontend format does
        // not (the three-channel colour-renderable widening).
        Bool IsAlphaWidenedColorAttachment(const MG_State::GLState::FramebufferAttachmentObject& attachmentObject);

        // Bit i set = DRAW BUFFER i of `fbo` resolves to a colour attachment the backend widened
        // from three channels to four. Indexed by draw-buffer slot, not by attachment point,
        // because that is what glColorMaski / glClearBufferfv address.
        Uint32 ComputeAlphaWidenedDrawBufferMask(const MG_State::GLState::FramebufferObject& fbo);

        // The same mask for whatever is currently bound to GL_DRAW_FRAMEBUFFER, recomputed by
        // SyncCurrentFBO (BackendFramebufferObject::SyncToBackend for the DRAW target, and reset
        // to 0 on the default framebuffer). Read by the draw/clear state sync, so it is only
        // trustworthy after SyncCurrentFBO has run in the same entry point.
        //
        // WHY IT EXISTS (the dst-alpha discipline). A widened attachment has a real alpha channel
        // the application's format does not, and GL says a missing channel reads as 1.0. Readback
        // can paper over that (ForceWideReadAlphaToOne), but GL_DST_ALPHA /
        // GL_ONE_MINUS_DST_ALPHA blending and glBlitFramebuffer read the STORED alpha inside the
        // driver where no interception is possible. So the stored alpha is kept at 1.0 instead:
        // a clear touching a widened buffer writes alpha 1.0, and every draw into it has its
        // alpha write mask forced off, so nothing can ever move it again. The application's own
        // colour mask is untouched - glGet(GL_COLOR_WRITEMASK) still reports what it set.
        extern Uint32 g_alphaWidenedDrawBufferMask;

        // Bit i set = DRAW BUFFER i of the framebuffer bound as DRAW resolves to a colour
        // attachment with an INTEGER format. Recomputed beside the mask above and for its sake:
        // glClearBufferfv on an integer colour buffer is GL_INVALID_OPERATION, so the
        // per-draw-buffer clear route the widening needs has to stand down when one is present.
        // (glClear on an integer colour buffer is left undefined by ES in the first place, and
        // an application that wants a defined answer has to call glClearBufferuiv/iv - which does
        // carry the widened alpha substitution.)
        extern Uint32 g_integerColorDrawBufferMask;

        // The colour a clear has to hand the driver for one draw buffer: the application's value,
        // except that a widened attachment's alpha is replaced by the 1.0 its three-channel
        // format implies. `one` is 1.0 encoded in the clear call's own component type - the
        // integer clears carry the integer 1, the float clear carries 1.0f.
        //
        // Returns `value` itself when nothing is substituted, so the ordinary path allocates and
        // copies nothing; `scratch` is the caller's buffer and has to outlive the returned
        // pointer. Free of GL state on purpose, so the substitution can be unit-tested exactly as
        // the driver sees it.
        template <typename T>
        const T* SubstituteWidenedClearAlpha(const T* value, Bool widened, T one, T (&scratch)[4]) {
            if (!widened || value == nullptr) {
                return value;
            }
            scratch[0] = value[0];
            scratch[1] = value[1];
            scratch[2] = value[2];
            scratch[3] = one;
            return scratch;
        }

        // What SyncCurrentFBO last pushed for each target, as a (binding, object, revision)
        // triple; it re-syncs unless all three still match. Stamped by SyncCurrentFBO and
        // ForceBindCurrentFBO, cleared by InvalidateFramebufferBindingCache. The three are
        // only meaningful together - see SyncCurrentFBO.
        //
        // The binding slot's own version, which changes whenever a different object is bound
        // to this target. Distinguishes a rebind from an in-place edit, and keeps the raw
        // pointer below from matching an address the allocator recycled for a new FBO.
        extern Array<Uint16, SizeT(FramebufferTarget::FramebufferTargetCount)> g_fboSyncedSlotVersions;
        // Tracks the bound FBO's object version (bumped on any attachment/drawbuffer change)
        // per target: re-attaching textures or changing draw buffers on an already-bound FBO
        // must re-sync it even when the binding-slot version has not moved.
        extern Array<Uint16, SizeT(FramebufferTarget::FramebufferTargetCount)> g_fboSyncedObjectVersions;
        // Which object was synced. Raw and never dereferenced: only compared for identity.
        extern Array<MG_State::GLState::FramebufferObject*, SizeT(FramebufferTarget::FramebufferTargetCount)>
            g_fboSyncedObjects;

        // Bumped whenever a live backend texture's driver id is re-minted while its
        // frontend texture may still be attached to application FBOs
        // (BackendTextureObject::RecreateBackendTexture - e.g. a respecify of a texture
        // whose backend storage went immutable). The FBO twins' attachment memos key on
        // FRONTEND attachment versions, which a backend-side re-mint does not move, so
        // the driver FBO would keep the deleted texture name attached forever. The
        // SyncCurrentFBO gate compares this generation (below) to re-enter the sync,
        // and each twin re-arms its per-attachment memo on a mismatch (SyncToBackend).
        extern Uint64 g_attachmentBackendIdGeneration;
        // What g_attachmentBackendIdGeneration was when SyncCurrentFBO last stamped each
        // target; part of the synced tuple above.
        extern Array<Uint64, SizeT(FramebufferTarget::FramebufferTargetCount)> g_fboSyncedBackendIdGenerations;

        // Driver-level READ/DRAW framebuffer-binding shadow. Every backend
        // glBindFramebuffer routes through BindFramebufferId so scoped helpers can
        // save/restore the current binding without a glGetIntegerv round-trip (that
        // query forces a driver pipeline sync) and so redundant rebinds no-op.
        // Starts unknown; the first CurrentFramebufferBinding() query pins it from
        // the driver once. Invalidated on MakeCurrent (context may reset).
        // GL_FRAMEBUFFER binds both targets.
        void BindFramebufferId(GLenum fbTarget, Uint id);
        Uint CurrentFramebufferBinding(FramebufferTarget target);
        void InvalidateFramebufferBindingCache();
        // A driver framebuffer id is about to be deleted: ES reverts every target that
        // currently binds it to 0, so the binding shadow has to follow or the next
        // BindFramebufferId(0) would be deduped away and leave the deleted name bound.
        void NoteFramebufferIdDeleted(Uint id);
    } // namespace FramebufferImpl

    // Shared scratch framebuffers for the readback/copy/blit emulation paths, with a
    // driver-side attachment shadow: repeated uses skip redundant detach/attach GL
    // calls, and an attachment left by one use (e.g. a depth copy's DEPTH_STENCIL
    // texture) is detached exactly when a later use of another aspect would
    // otherwise inherit it (stale cross-aspect attachments made the shared temp FBO
    // incomplete and silently degraded later readbacks).
    namespace ScratchFBOImpl {
        struct ScratchFramebuffer {
            Uint id = 0;
            // false => attachment state unknown; scrub every point on next use.
            // A fresh FBO starts with nothing attached, so creation sets it true.
            Bool attachmentsKnown = false;
            Uint colorTex = 0;
            GLenum colorTarget = 0;
            GLint colorLevel = 0;
            GLint colorLayer = -1; // >= 0 => attached via glFramebufferTextureLayer
            Uint depthTex = 0;
            GLenum depthTarget = 0;
            GLint depthLevel = 0;
            Bool depthHasStencil = false;
            // Per-FBO read/draw buffer state (0 = unknown, set on first use).
            GLenum readBuffer = 0;
            GLenum drawBuffer = 0;
        };
        ScratchFramebuffer& TempFramebuffer();     // GetTexImage READ / CopyTex*Image2D depth DRAW
        ScratchFramebuffer& BlitReadFramebuffer(); // texture-to-texture blit source
        ScratchFramebuffer& BlitDrawFramebuffer(); // texture-to-texture blit destination
        // Returns the GL id, generating it if needed (requires a current ES context).
        Uint EnsureId(ScratchFramebuffer& fb);
        // The fb must currently be bound at fbTarget (glReadBuffer/glDrawBuffers
        // target the READ/DRAW binding respectively). Each Ensure* performs the
        // minimal detach/attach set and keeps the shadow in sync; a failed attach
        // records the point as detached so the completeness check fails instead of
        // silently reading a stale attachment.
        void EnsureColorAttachment2D(ScratchFramebuffer& fb, GLenum fbTarget, Uint tex, GLenum texTarget, GLint level);
        void EnsureColorAttachmentLayer(ScratchFramebuffer& fb, GLenum fbTarget, Uint tex, GLint level, GLint layer);
        void EnsureDepthAttachment2D(ScratchFramebuffer& fb, GLenum fbTarget, Uint tex, GLenum texTarget, GLint level,
                                     Bool withStencil);
        void EnsureNoColorAttachment(ScratchFramebuffer& fb, GLenum fbTarget);
        void EnsureNoDepthAttachment(ScratchFramebuffer& fb, GLenum fbTarget);
        void EnsureReadBuffer(ScratchFramebuffer& fb, GLenum readBuffer);
        void EnsureDrawBuffer(ScratchFramebuffer& fb, GLenum drawBuffer);
        // A 1x1 RGBA8-renderbuffer-complete FBO (GenerateMipmap needs a complete
        // binding while respecifying texture storage). Attachment is set once at
        // creation and never changes.
        Uint EnsureCompleteTinyFramebufferId();
        // A backend texture id is being deleted or respecified: a scratch FBO still
        // referencing it would hold a dangling attachment (ES only auto-detaches
        // from the *bound* framebuffer), and a recycled name could false-skip a
        // re-attach; force a full scrub on next use.
        void NoteTextureIdDeleted(Uint textureId);
        // The ES context (and the scratch FBO ids with it) is going away.
        void OnBackendContextDestroyed();
    } // namespace ScratchFBOImpl

    // Driver-level GL_PACK_* pixel-store shadow, the readback-side sibling of the
    // upload path's ScopedDefaultUnpackState (Managers.cpp): the backend PACK state
    // is written ONLY through ApplyPackState, so scoped helpers can save/restore it
    // from the shadow instead of glGetIntegerv (which forces a driver pipeline
    // sync), and redundant glPixelStorei calls no-op. The first Apply/Current call
    // pins the driver to the shadow by writing all fields once. Invalidated on
    // MakeCurrent (context may reset). PACK_IMAGE_HEIGHT/SKIP_IMAGES/SWAP_BYTES/
    // LSB_FIRST have no ES equivalents; readbacks honor them on the CPU from the
    // frontend context state instead.
    namespace PixelStoreImpl {
        struct PackState {
            GLint Alignment = 4;
            GLint RowLength = 0;
            GLint SkipRows = 0;
            GLint SkipPixels = 0;
            Bool operator==(const PackState& o) const {
                return Alignment == o.Alignment && RowLength == o.RowLength && SkipRows == o.SkipRows &&
                       SkipPixels == o.SkipPixels;
            }
        };
        void ApplyPackState(const PackState& desired);
        PackState CurrentPackState();
        void InvalidatePackStateCache();
    } // namespace PixelStoreImpl

    namespace SamplerImpl {
        class BackendSamplerObject; // for PrgramImpl's sampler-pass memo rows below
    }

    // Image uniforms take their unit from the layout(binding=N) qualifier baked into
    // the transpiled ESSL; unlike samplers they must not (and in ES cannot) be
    // assigned through glUniform1i.
    //
    // ALL THIRTY-THREE of them, in the one contiguous block ARB_shader_image_load_store allocated
    // (GL_IMAGE_1D 0x904C through GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE_ARRAY 0x906C). The list
    // used to hold only the fifteen whose TARGET exists in ES, which read as a reasonable
    // shortcut and was two bugs: an image uniform this says "no" to is one
    // CollectImageFormatBakeInputs never walks, so its non-core format is neither baked nor
    // widened and SPIRV-Cross throws for the whole stage ("Attempting to use image format not
    // supported in ES profile"), and it is also one SyncToBackend then treats as a SAMPLER and
    // assigns with glUniform1i, which ES makes an INVALID_OPERATION. A GL_TEXTURE_CUBE_MAP_ARRAY
    // image - which ES 3.2 has in core, so it is not even an emulated target - hit both.
    inline Bool IsImageUniformType(GLenum type) {
        switch (type) {
        case 0x904C: /*GL_IMAGE_1D*/
        case 0x904D: /*GL_IMAGE_2D*/
        case 0x904E: /*GL_IMAGE_3D*/
        case 0x904F: /*GL_IMAGE_2D_RECT*/
        case 0x9050: /*GL_IMAGE_CUBE*/
        case 0x9051: /*GL_IMAGE_BUFFER*/
        case 0x9052: /*GL_IMAGE_1D_ARRAY*/
        case 0x9053: /*GL_IMAGE_2D_ARRAY*/
        case 0x9054: /*GL_IMAGE_CUBE_MAP_ARRAY*/
        case 0x9055: /*GL_IMAGE_2D_MULTISAMPLE*/
        case 0x9056: /*GL_IMAGE_2D_MULTISAMPLE_ARRAY*/
        case 0x9057: /*GL_INT_IMAGE_1D*/
        case 0x9058: /*GL_INT_IMAGE_2D*/
        case 0x9059: /*GL_INT_IMAGE_3D*/
        case 0x905A: /*GL_INT_IMAGE_2D_RECT*/
        case 0x905B: /*GL_INT_IMAGE_CUBE*/
        case 0x905C: /*GL_INT_IMAGE_BUFFER*/
        case 0x905D: /*GL_INT_IMAGE_1D_ARRAY*/
        case 0x905E: /*GL_INT_IMAGE_2D_ARRAY*/
        case 0x905F: /*GL_INT_IMAGE_CUBE_MAP_ARRAY*/
        case 0x9060: /*GL_INT_IMAGE_2D_MULTISAMPLE*/
        case 0x9061: /*GL_INT_IMAGE_2D_MULTISAMPLE_ARRAY*/
        case 0x9062: /*GL_UNSIGNED_INT_IMAGE_1D*/
        case 0x9063: /*GL_UNSIGNED_INT_IMAGE_2D*/
        case 0x9064: /*GL_UNSIGNED_INT_IMAGE_3D*/
        case 0x9065: /*GL_UNSIGNED_INT_IMAGE_2D_RECT*/
        case 0x9066: /*GL_UNSIGNED_INT_IMAGE_CUBE*/
        case 0x9067: /*GL_UNSIGNED_INT_IMAGE_BUFFER*/
        case 0x9068: /*GL_UNSIGNED_INT_IMAGE_1D_ARRAY*/
        case 0x9069: /*GL_UNSIGNED_INT_IMAGE_2D_ARRAY*/
        case 0x906A: /*GL_UNSIGNED_INT_IMAGE_CUBE_MAP_ARRAY*/
        case 0x906B: /*GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE*/
        case 0x906C: /*GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE_ARRAY*/
            return true;
        default:
            return false;
        }
    }

    namespace PrgramImpl {
        // Defined further down, next to CollectImageFormatBakeInputs; only referenced here.
        struct ImageFormatBakeInputs;

        class BackendProgramObjectImpl {
        public:
            // Per-link cache of a sampler-style uniform's backend location: built once in
            // SyncToBackend so draws stop issuing glGetUniformLocation string queries.
            // lastAssignedUnit mirrors the program-state value set through glUniform1i
            // (program state persists across binds, so caching per program is exact).
            struct SamplerUniformBinding {
                Uint frontendLocation = 0;
                Int backendLocation = -1;
                GLenum uniformType = 0;
                Int lastAssignedUnit = -1;
                // Location of this sampler's emulated GL_TEXTURE_LOD_BIAS uniform
                // (PrgramImpl::EmulateTextureLodBias), -1 when the shader has none.
                // lastAssignedLodBias mirrors the value the program currently holds,
                // so an unbiased shader issues no per-draw glUniform1f at all.
                Int lodBiasLocation = -1;
                Float lastAssignedLodBias = 0.0f;
            };

            // Memo of the whole per-draw sampler-uniform pass (glUniform1i unit
            // assignments, lod-bias uniform, raw-depth-fetch substitution and the
            // per-unit sampler-object binds) in BindCurrentProgramWithResources.
            // The pass is a pure function of the keys below, and its only driver-side
            // effect is the sampler binding of each sampled unit, so replaying it as
            // "do nothing" additionally requires those bindings to still be on the
            // driver - the per-entry row compare against g_boundSamplersCache (the
            // shadow every sampler bind in this backend already routes through).
            //
            // Invalidation enumeration:
            //  * sampler-uniform unit assignment (glUniform1i) and uniform-block
            //    binding edits -> frontend backendStateVersion;
            //  * any texture/sampler bind moving on any unit (incl. the high-water
            //    mark moving) -> unitBindingsEpoch;
            //  * any sampler parameter (incl. lod bias, compare mode) or texture
            //    shape/format change -> samplingGeneration;
            //  * another frontend context -> contextId (never-reused id);
            //  * ES context recreation -> textureContextGeneration;
            //  * relink / backend program rebuild -> SyncToBackend resets `valid`
            //    (it rebuilds m_samplerUniformBindings, whose lastAssignedUnit /
            //    lastAssignedLodBias dedup state this memo leans on);
            //  * any other writer moving a sampled unit's sampler binding
            //    (BindCurrentUnitSamplers on a unit-sampler change, scratch binds)
            //    -> the row snapshot compare.
            struct SamplerPassMemo {
                static constexpr SizeT kMaxEntries = 16;
                Bool valid = false;
                Uint8 count = 0;
                Uint64 contextId = 0;
                Uint64 unitBindingsEpoch = 0;
                Uint64 samplingGeneration = 0;
                Uint32 backendStateVersion = 0;
                Uint textureContextGeneration = 0;
                Array<Uint8, kMaxEntries> units{};
                Array<SamplerImpl::BackendSamplerObject*, kMaxEntries> rows{};
            };

            BackendProgramObjectImpl();
            ~BackendProgramObjectImpl();
            void SyncToBackend(const SharedPtr<MG_State::GLState::ProgramObject>& stateProgramObject);
            void Use();
            void SetBaseInstance(Uint32 baseInstance) const;
            void SetBaseInstanceWordIndex(Int32 wordIndex) const;
            void SetDrawID(Uint32 drawId) const;
            void SetBaseVertex(Int32 baseVertex) const;
            // True when the transpiled program kept a gl_DrawID uniform, i.e. SetDrawID
            // actually reaches a shader read rather than being discarded.
            Bool ReadsDrawID() const { return m_drawIdUniformLocation >= 0; }
            // Same for gl_BaseVertex: only a program that reads it pays for the per-draw
            // uniform write, and only such a program needs the reset after one.
            Bool ReadsBaseVertex() const { return m_baseVertexUniformLocation >= 0; }
            // Which viewport indices the next draw's fragments may keep, one bit each. Written
            // once per replay pass; see ForEachViewportRoutingPass.
            void SetViewportPassMask(Uint32 indexMask) const;
            // True when this build injected the fragment-stage viewport gate, i.e. when a
            // pre-rasterization stage routes by gl_ViewportIndex AND the fragment stage can act
            // on it. The uniform is the honest test for both halves: it exists only where the
            // gate was injected, and the gate is injected only where a stage routes.
            Bool RoutesViewportIndex() const { return m_viewportPassMaskUniformLocation >= 0; }
            Int GetIndirectParamsBinding() const { return m_indirectParamsBinding; }
            Uint GetBackendProgramId() const { return m_backendProgramId; }
            // False when the last SyncToBackend could not produce a usable program (a
            // shader failed to transpile or compile, or the link itself failed). Use()
            // must not leave the previously bound program current in that case.
            Bool IsBackendProgramUsable() const { return m_backendProgramUsable; }
            Uint GetBackendGlobalUBOId() const { return m_backendGlobalUBOId; }
            Uint32 GetSnormFallbackClampOutputMask() const { return m_snormFallbackClampOutputMask; }
            Uint32 GetUnormFallbackClampOutputMask() const { return m_unormFallbackClampOutputMask; }
            Uint GetFragColorBroadcastCount() const { return m_fragColorBroadcastCount; }
            // Signature of the glShaderStorageBlockBinding override set the generated ESSL was
            // transpiled against (ES can only express a storage-block binding as the declared
            // qualifier, so the overrides are baked into the source). A mismatch means the
            // program is stale exactly like the clamp masks above.
            Uint64 GetShaderStorageBlockBindingSignature() const { return m_shaderStorageBlockBindingSignature; }
            // GL atomic-counter binding points the transpiled stages declare (sorted, unique),
            // and the top of the reserved shader-storage range their counter blocks were
            // transpiled against - the slot for GL binding N is `top - N`. Empty for every
            // program that uses no atomic counter, which is what keeps the per-draw cost of the
            // counter sync at one empty-vector test.
            const Vector<Int>& GetAtomicCounterBindings() const { return m_atomicCounterGlBindings; }
            Int GetAtomicCounterEsslBindingTop() const { return m_atomicCounterEsslBindingTop; }
            // GL_PATCH_VERTICES the synthesized pass-through tessellation control stage was built
            // for, or -1 when this program needed no such stage. Another of the same shape as the
            // signatures above: the value is compiled INTO the synthesized stage as
            // `layout(vertices = N) out`, so a program built for one patch size is stale for
            // another and the draw path has to say so. -1 compares equal to itself for every
            // program that has a control stage of its own, i.e. for all but a handful.
            Int GetPassthroughTessControlPatchVertices() const {
                return m_passthroughTessControlPatchVertices;
            }
            // GL_PATCH_DEFAULT_{OUTER,INNER}_LEVEL the same synthesized stage was built with, for
            // the same reason: ES has neither the state nor an entry point to forward it to, so
            // glPatchParameterfv's values are compiled in as literals and a program built with one
            // set is stale for another. Meaningless (and never read) when the patch-vertices field
            // above is -1, which is the gate the draw path tests first.
            const FloatVec4& GetPassthroughTessControlOuterLevel() const {
                return m_passthroughTessControlOuterLevel;
            }
            const FloatVec2& GetPassthroughTessControlInnerLevel() const {
                return m_passthroughTessControlInnerLevel;
            }

            Bool HasGlobalUboBlock() const { return m_globalUboBackendBlockIndex >= 0; }
            const Vector<Int>& GetUniformBlockBackendIndices() const { return m_uniformBlockBackendIndices; }
            Vector<SamplerUniformBinding>& GetSamplerUniformBindings() { return m_samplerUniformBindings; }
            Uint32 GetLastUploadedGlobalUboVersion() const { return m_lastUploadedGlobalUboVersion; }
            void SetLastUploadedGlobalUboVersion(Uint32 version) { m_lastUploadedGlobalUboVersion = version; }
            // Backend-reported GL_UNIFORM_BLOCK_DATA_SIZE of the global block; ring
            // bindings must span at least this much (may exceed the frontend's
            // reflected size when the transpiled block pads differently).
            Int GetGlobalUboBackendBlockSize() const { return m_globalUboBackendBlockSize; }
            BufferImpl::UboRingAllocation& GetGlobalUboRingAllocation() { return m_globalUboRingAllocation; }
            SamplerPassMemo& GetSamplerPassMemo() { return m_samplerPassMemo; }
            // Frontend link version this backend program (and its resource caches) was
            // built from; a mismatch means every link-derived cache here is stale.
            Uint32 GetSyncedLinkVersion() const { return m_syncedLinkVersion; }
            // Image-uniform unit generation this backend program was GENERATED against.
            // Separate from the link version because it is not link state: ES forbids
            // glUniform1i on an image uniform, so RebindImageUniformsToFrontendUnits bakes the
            // unit into the ESSL, and a program built before glUniform1i moved that unit is as
            // stale as one built before a relink - while the sampler half, which really is
            // re-issued per draw, needs nothing of the sort.
            Uint32 GetSyncedImageUnitVersion() const { return m_syncedImageUnitVersion; }
            // Whether the (unit, bound format) pairs this program's FORMAT-LESS image uniforms
            // resolve to are still the ones its ESSL was generated against.
            //
            // A fourth condition of the same family as the three above, and the only one that
            // reads live state rather than a program-side counter, because that is where the
            // dependency actually is. GLSL ES requires a format layout qualifier on every image
            // where desktop GLSL lets a writeonly declaration omit one, and the only correct
            // qualifier is whatever glBindImageTexture named - so a declaration with no format
            // is compiled against the BINDING, and a rebind to a different format makes the
            // built program wrong. Keyed on the units the program's own images address (cached
            // at sync, since a unit can only move by glUniform1i, which bumps the image-unit
            // version above and forces a re-sync anyway), so the cost on a program with no
            // format-less image - which is all but a handful - is one empty-vector test.
            //
            // Deliberately NOT reached from glBindImageTexture: that entry point must never
            // trigger a build (same constraint as glShaderStorageBlockBinding). It moves the
            // state and this comparison notices at the next Prepare, which is also what makes
            // an image first bound AFTER link work.
            Bool ImageUnitFormatsStillMatch() const;
            // The value ImageUnitFormatsStillMatch() compares against, recomputed from live
            // image-unit state. 0 when the program has no format-less image uniform.
            Uint64 ComputeImageUnitFormatSignature() const;

        private:
            void CacheResourceLocations(const SharedPtr<MG_State::GLState::ProgramObject>& stateProgramObject);

            // Builds, compiles and attaches the pass-through tessellation control stage GL 4.6
            // core 11.2.2 describes, for a program that has an evaluation stage and none of its
            // own - which ES 3.2 rejects outright. Called from SyncToBackend after every real
            // stage has been attached and before the link; see the definition for why it cannot
            // regress a program that works today.
            void AttachPassthroughTessControlStage(
                const MG_State::GLState::ProgramObject& stateProgramObject, Int tessEvalShaderIndex,
                const Vector<Vector<unsigned int>>& shaderSpirvs, const String& vertexStageEssl,
                const String& tessEvalStageEssl);

            // One stage's SPIR-V through the DirectGLES pass chain and SPIRV-Cross, producing
            // the raw emitted ESSL and the interface blocks this stage's XFB flattening
            // rewrote. This is the segment the L2 shader-translation memo keys on, so every
            // input it reads must appear in EsslTranslationKeyInputs - see the definition's
            // header comment in Managers.cpp and MG_Util/ShaderTranspiler/TranslationCache.h.
            // False means SPIRV-Cross refused the module; `outError` then carries its message.
            Bool TranspileSpirvToEssl(const Vector<unsigned int>& spirvCode, GLenum glShaderType,
                                      const std::set<String>& xfbCaptureBlockNames,
                                      const ImageFormatBakeInputs& imageFormatBake,
                                      const UnorderedMap<String, Int>& storageBlockBindingOverrides,
                                      const std::map<String, String>& inputBlockRenames,
                                      const std::map<String, String>& outputBlockRenames,
                                      Bool stripInputBlockLocations, Bool stripOutputBlockLocations,
                                      Int atomicCounterEsslBindingTop, Bool enableSpirvValidation,
                                      String& outSource,
                                      std::set<String>& outFlattenedXfbBlockNames,
                                      Vector<Int>& outAtomicCounterGlBindings, String& outError) const;

            Uint m_backendProgramId = 0;
            // GL name of the frontend program this was last synced from; diagnostics only, so
            // an unusable backend program can be traced back to the glCreateProgram id the app
            // knows it by.
            Uint m_frontendProgramId = 0;
            Uint m_backendGlobalUBOId = 0;
            Int m_baseInstanceUniformLocation = -1;
            Int m_drawIdUniformLocation = -1;
            Int m_baseVertexUniformLocation = -1;
            Int m_baseInstanceWordIndexUniformLocation = -1;
            Int m_viewportPassMaskUniformLocation = -1;
            Int m_indirectParamsBinding = -1;
            Uint32 m_snormFallbackClampOutputMask = 0;
            Uint32 m_unormFallbackClampOutputMask = 0;
            // Draw buffers a legacy gl_FragColor write has to reach (see
            // PrgramImpl::BroadcastLegacyFragColor); 1 keeps the plain single-output shader.
            Uint m_fragColorBroadcastCount = 1;
            // 0 is the signature of an empty override set, i.e. what almost every program has.
            Uint64 m_shaderStorageBlockBindingSignature = 0;
            Vector<Int> m_atomicCounterGlBindings;
            Int m_atomicCounterEsslBindingTop = -1;
            // -1 for every program that has a tessellation control stage of its own (or none at
            // all); otherwise the GL_PATCH_VERTICES the synthesized pass-through stage was built
            // with. See GetPassthroughTessControlPatchVertices.
            Int m_passthroughTessControlPatchVertices = -1;
            // The default tessellation levels baked into that same stage. Only meaningful while
            // the field above is not -1.
            FloatVec4 m_passthroughTessControlOuterLevel = FloatVec4(1.0f, 1.0f, 1.0f, 1.0f);
            FloatVec2 m_passthroughTessControlInnerLevel = FloatVec2(1.0f, 1.0f);
            Bool m_isInitialized = false;
            Bool m_backendProgramUsable = false;
            // Set by SyncToBackend every time it relinks the driver program, cleared by the
            // next Use(). Use() dedupes on a GL program NAME, and a relink replaces the
            // executable behind that name without changing it - see the note at the
            // glLinkProgram in SyncToBackend for what the driver runs otherwise.
            Bool m_rebindAfterRelink = false;

            Int m_globalUboBackendBlockIndex = -1;
            Int m_globalUboBackendBlockSize = 0;
            Vector<Int> m_uniformBlockBackendIndices; // frontend block index -> backend index (-1 = absent)
            Vector<SamplerUniformBinding> m_samplerUniformBindings;
            Uint32 m_lastUploadedGlobalUboVersion = ~0u;
            BufferImpl::UboRingAllocation m_globalUboRingAllocation;
            Uint32 m_syncedLinkVersion = ~0u;
            Uint32 m_syncedImageUnitVersion = ~0u;
            // Image units addressed by the program's FORMAT-LESS image uniforms, and the digest
            // of the (unit, format) pairs the generated ESSL baked. Empty/0 for every program
            // that declares a format on all of its images, which is the overwhelming majority -
            // and what keeps the per-draw comparison free for them.
            Vector<Int> m_formatlessImageUnits;
            Uint64 m_imageUnitFormatSignature = 0;
            SamplerPassMemo m_samplerPassMemo;
        };

        extern Uint32 g_snormFallbackClampOutputMask;
        extern Uint32 g_unormFallbackClampOutputMask;
        // Draw buffers the current draw framebuffer enables. Like the clamp masks above it
        // is framebuffer state that the shader has to be compiled against, so a program
        // whose snapshot no longer matches is relinked.
        extern Uint g_fragColorBroadcastCount;
        // Backend id of the last glUseProgram issued through this backend; lets Use()
        // skip redundant rebinds. Reset to 0 wherever glUseProgram(0) is issued or the
        // ES context is recreated.
        extern Uint g_lastUsedBackendProgramId;
        extern StateBackendObjectRegistry<MG_State::GLState::ProgramObject, BackendProgramObjectImpl>
            g_backendProgramObjects;

        // Points one shader storage block of an ALREADY-LINKED backend program at
        // `binding`. `blockName` is the frontend interface-query spelling; the real
        // driver's own index for it is looked up here, because the transpiled ESSL's
        // block order is not the frontend's. Returns false when the block does not exist
        // on the backend program (eliminated as unused, or the driver lacks the entry
        // points), which is not an error - GL_BUFFER_BINDING is served from the frontend
        // record either way.
        //
        // NOT how a rebinding reaches the shader. glShaderStorageBlockBinding has no ES
        // equivalent and is absent from every real ES driver, so this is a no-op there;
        // SyncToBackend bakes the effective binding into the ESSL it generates instead
        // (SpvcSession::SetShaderStorageBlockBinding). This is kept as the cheaper path on
        // a driver that does happen to expose the entry point.
        Bool ApplyShaderStorageBlockBinding(Uint backendProgramId, const String& blockName, Uint binding);
        // Replays every glShaderStorageBlockBinding recorded on the program onto a backend
        // program that was just built - best effort, on the same "only where the driver has
        // the entry point" terms as ApplyShaderStorageBlockBinding above. Mirrors
        // DirectVulkan's reseed-on-rebuild in BuildProgramResourceCache.
        void ReseedShaderStorageBlockBindings(Uint backendProgramId,
                                              const MG_State::GLState::ProgramObject& stateProgramObject);
        // Order-independent digest of the program's glShaderStorageBlockBinding overrides.
        // The generated ESSL carries them (ES has no way to move a storage block's binding
        // after link), so a program built against a different set is stale and the draw path
        // has to rebuild it. Computed from the values, so re-setting a block to the binding it
        // already has costs nothing. 0 when nothing was ever rebound.
        Uint64 ComputeShaderStorageBlockBindingSignature(
            const MG_State::GLState::ProgramObject& stateProgramObject);

        // Everything the image-format bake needs from one walk of a program's uniform
        // reflection. GLSL ES requires a format layout qualifier on every image uniform;
        // desktop GLSL lets a writeonly (or readonly) declaration omit one, and the only
        // format that is CORRECT to substitute is whatever glBindImageTexture named for the
        // unit that uniform addresses - so the transpile bakes it in and the build is keyed
        // on it.
        struct ImageFormatBakeInputs {
            // Uniform name (SPIR-V spelling, i.e. an array named once, unsubscripted) to the GL
            // internal format to bake. Holds only uniforms that DECLARED no format; a declared
            // one is authoritative and is never overridden.
            UnorderedMap<String, Uint> glFormatByUniformName;
            // The same uniforms whose format SPIRV-Cross REFUSES to print for ESSL (it throws on
            // its desktop-only set, which loses the stage), paired with the ESSL spelling to
            // write into the emitted declaration instead. Disjoint from the map above by
            // construction: a format is baked into the module or completed in the text, never
            // both. r8ui - the stencil half of the packed_depth_stencil case - lands here.
            UnorderedMap<String, String> esslFormatQualifierByUniformName;
            // Units those uniforms address, kept so the draw path can re-read their formats
            // without walking the reflection again.
            Vector<Int> units;
            // Digest of the (unit, format) pairs above. 0 when the program has no format-less
            // image uniform, which is all but a handful.
            Uint64 signature = 0;
            // Array uniforms whose elements resolved to units holding DIFFERENT formats: one
            // declaration carries one qualifier, so there is nothing correct to bake and they
            // are dropped from the map above. Kept for diagnostics.
            Vector<String> conflictedNames;
            // Some format in play - declared or baked - is outside the GLSL ES core image
            // format set, so the emitted ESSL needs the GL_NV_image_formats directive.
            Bool needsExtendedImageFormats = false;
            // Some DECLARED format in play is one WidenImageFormatsForEssl will re-declare in a
            // core carrier. Answered from the uniform reflection rather than from a module parse
            // on purpose: the widening is armed on every driver, so a per-stage BuildModule to
            // find out would land on every stage of every program - which is the cost
            // SpirvGateFeatures exists to avoid. Program-wide, so it can over-arm a stage that
            // declares no image; the pass then finds nothing, reports no change, and the caller
            // keeps the module it already had.
            Bool declaresWidenableImageFormat = false;
        };
        ImageFormatBakeInputs CollectImageFormatBakeInputs(
            const MG_State::GLState::ProgramObject& stateProgramObject);
    } // namespace PrgramImpl

    namespace SamplerImpl {
        class BackendSamplerObject {
        public:
            BackendSamplerObject();
            // Deletes the driver sampler and clears the units whose binding shadow still names
            // this twin (a recycled heap address would otherwise false-skip a later Bind).
            // Frontend glDeleteSamplers used to leak the backend id for the process lifetime.
            ~BackendSamplerObject();
            BackendSamplerObject(const BackendSamplerObject&) = delete;
            BackendSamplerObject& operator=(const BackendSamplerObject&) = delete;
            void SyncToBackend(const SharedPtr<MG_State::GLState::SamplerObject>& stateSamplerObject);
            void Bind(Uint unit);
            Uint GetBackendSamplerId() const;

        private:
            Uint m_backendSamplerId = 0;
            Uint m_contextGeneration = 0;
            Bool m_isInitialized = false;
            SamplerParameters m_cacheSamplerParameters;
            Uint16 m_syncedSamplerVersion = 0;
        };

        void UnbindSampler(Uint unit);

        extern Array<BackendSamplerObject*, MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS>
            g_boundSamplersCache;
        extern StateBackendObjectRegistry<MG_State::GLState::SamplerObject, BackendSamplerObject>
            g_backendSamplerObjects;
    } // namespace SamplerImpl

    namespace RenderbufferImpl {
        class BackendRenderbufferObject {
        public:
            BackendRenderbufferObject();
            // Deletes the driver renderbuffer; frontend glDeleteRenderbuffers used to leak it
            // (with its whole image allocation) for the process lifetime.
            ~BackendRenderbufferObject();
            BackendRenderbufferObject(const BackendRenderbufferObject&) = delete;
            BackendRenderbufferObject& operator=(const BackendRenderbufferObject&) = delete;
            void SyncToBackend(const SharedPtr<MG_State::GLState::RenderbufferObject>& stateRBOObject);
            Uint GetBackendRenderbufferId() const { return m_backendRBOId; }
            void Bind() const;

        private:
            Uint m_backendRBOId = 0;
            Uint m_contextGeneration = 0;
            Bool m_isInitialized = false;
            TextureInternalFormat m_cacheInternalFormat = TextureInternalFormat::Unknown;
            Int m_cacheWidth = 0;
            Int m_cacheHeight = 0;
            Int m_cacheSamples = 0;
        };

        extern StateBackendObjectRegistry<MG_State::GLState::RenderbufferObject, BackendRenderbufferObject>
            g_backendRenderbufferObjects;
    } // namespace RenderbufferImpl
} // namespace MobileGL::MG_Backend::DirectGLES
