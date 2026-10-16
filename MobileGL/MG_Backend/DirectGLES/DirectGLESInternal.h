// MobileGL - MobileGL/MG_Backend/DirectGLES/DirectGLESInternal.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "DirectGLES.h"
#include "ContextEpoch.h"
#include "MG_Util/X11/DisplayGuard.h"
#include "EGL/egl.h"
#include "MG_Util/Types.h"
#include "Utils.h"
#include "Managers.h"
#include "MultiDraw.h"
#include <MG_Backend/ClientReadback.h>
#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Classifiers/TextureEnumClassifier.h>
#include <MG_Util/Metrics/TextureMetrics.h>
#include <MG_State/GLState/Core.h>
#include <MG_Backend/MGPipe/PipeInputs.h>
// P3a: the applier's vertex-input records the re-keyed draw-buffer memo is validated against.
#include <MG_Pipe/PipeApply.h>
// P5c ev: the surface-changed event's producer callback, installed by the server session.
#include <MG_Pipe/MGPipeCallbacks.h>
// P5e (pa): MGPipeShaderCsoRecord::Archive is a SharedPtr<const ProgramArchive> and PipeApply.h
// deliberately only forward-declares the type (its own note at :46 says to come here for it).
// The draw path's attribute-values sync reads Archive->Link, so this TU needs it complete -
// Managers.cpp already includes it for ProgramArchiveSource, which is the same reason.
#include <MG_State/GLState/ProgramState/ProgramArtifactsCodec.h>
// P5c (tx): §1's server-side per-level extent derivation, for GenerateMipmap's shape reads.
#include <MG_Backend/Record/StagedTextureStore.h>
// The record arm's deaths and latches (MGPipeRecordFail / MGPipeRecordLatch), in every build.
#include <MG_Pipe/PipeSessionFail.h>
#if MOBILEGL_BUILD_DISAGGREGATED
// Shared images: the registry the EGLImages are built from (SharedImageImpl below).
#include <MG_Remote/Server/SharedImageRegistry.h>
// Device loss ends the session through MG_Pipe's latch seam (LatchIfDeviceLost).
#include <MG_Pipe/PipeSessionFail.h>
#if defined(__ANDROID__)
#include <android/hardware_buffer.h>
#include <unistd.h>
#endif
#endif
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
#include <MG_State/GLState/ErrorState/Error.h>
#include <MG_State/GLState/TextureState/TextureObjectBuffer.h>
#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>
#include <MG_Util/SelfTest/DriverBugProbes.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToGL/FramebufferEnumConverter.h>
#include <MG_Util/Converters/MGToStr/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToGL/RenderStateEnumConverter.h>
#include <MG_Util/Math/HalfFloat.h>
#include <MG_Util/Metrics/BufferMetrics.h>
#include <MG_Util/Metrics/PipeStats.h>
#include <MG_Util/Texture/PixelStoreProcessor.h>
#include <Config.h>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#if defined(__linux__) && !defined(__ANDROID__) && __has_include(<X11/Xlib.h>)
#pragma push_macro("Bool")
#pragma push_macro("None")
#include <X11/Xlib.h>
#pragma pop_macro("None")
#pragma pop_macro("Bool")
#endif

namespace MobileGL::MG_Backend::DirectGLES {
    Bool QueryCurrentSurfaceSize(Int& outWidth, Int& outHeight);

    // A4, TAKEN AT THE VERIFICATION ROUND, AND THE FOUR TEMPORARY LATCHES ARE GONE WITH IT.
    // This package landed BEFORE D and so carried its own bit tests, plus a copy of D-K2's
    // dependency directions, so that a half-running mask was refused in the meantime too.
    // Package D has landed, so every consult below calls D's PUBLIC WRAPPERS instead -
    // FramebufferSubsystemEnabled() / TextureResourceSubsystemEnabled() /
    // SamplerSubsystemEnabled() / ProgramSubsystemEnabled() (Managers.h, beside the four
    // Resolve<Family>SubsystemArm() resolvers; the WRAPPER holds the `static const` latch, so
    // the resolver is never the thing to call) - which additionally LOG each dependency
    // refusal naming both bits and reach Fatal{PipeLegacyMemosDisabled} under
    // MOBILEGL_PIPE_LEGACY_MEMOS=0, neither of which a copy here could do.
    //
    // NOTHING HERE RESTATES A DEPENDENCY DIRECTION any more - D-K2's fourth row (bit 10
    // requires bit 11, ID-15) included. It is in ResolveTextureResourceSubsystemArm, which is
    // its owner, and two writers of one diagnostic is how the two drift.

    // THE ONE PLACE THIS FILE ASKS THE APPLIER "which framebuffer record describes this
    // binding". Six reads used to spell `MGPipeApplier().DrawFramebuffer` / `.ReadFramebuffer`
    // inline; they go through here instead, for exactly one reason.
    //
    // ID-19 turns the applier's framebuffer state into a PER-OBJECT table keyed by the
    // framebuffer handle, with `BoundFramebuffer[Draw|Read]` holding the bound handles, so that
    // the DSA entry points (BlitNamedFramebuffer, the four ClearNamedFramebuffer*) can be
    // handed a record for a framebuffer that is bound to neither binding. Wire v3 keeps
    // `DrawFramebuffer` / `ReadFramebuffer` as ACCESSORS OF THOSE NAMES resolving through the
    // bound handle, precisely so this package keeps its shape. If they land as member functions
    // rather than as members, this one function grows a pair of parentheses and nothing else in
    // this file moves; if the accessor can answer "no record for the bound handle", that answer
    // arrives here as a null Fbo, which is already what every caller treats as a decline.
    //
    // WIRE v3 LANDED BOTH HALVES OF THAT PREDICTION: they are member FUNCTIONS and they return
    // a NULLABLE POINTER, so the four call sites gained a null check each and nothing else in
    // this file moved. Null is a real answer with exactly three causes (PipeApply.h): nothing
    // is bound to that binding, no record has been written at the bound handle's slot, or the
    // slot's generation has moved on under the handle - the third counted AND logged by the
    // applier itself in StaleFramebufferRecordLookups. All three already arrived here as a
    // null Fbo and were already a decline; the callers test the pointer instead.
    static const MG_Pipe::MGPFramebufferState* BoundFramebufferRecord(FramebufferTarget target) {
        const auto& st = MG_Pipe::MGPipeApplier();
        return target == FramebufferTarget::Draw ? st.DrawFramebuffer() : st.ReadFramebuffer();
    }

    [[noreturn]] static void RefuseFramebufferBindingSlotRead() {
        MG_Pipe::MGPipeInputPoisonFatalForVerb(MG_Pipe::MGPipeInputField::GetFramebufferBindingSlot,
                                               MG_Pipe::gPipeInputs->CurrentVerb());
    }

    SamplerImpl::BackendSamplerObject* GetRawDepthFetchSampler();

    Bool NeedsRawDepthFetchSampler(const SamplerParameters& samplerParams, TextureInternalFormat textureFormat);

    Bool NeedsRawDepthFetchSampler(const SharedPtr<MG_State::GLState::SamplerObject>& samplerObject,
                                   TextureInternalFormat textureFormat);

    namespace BufferImpl {
        Uint ResolveDrawVertexBuffersFromRecord(const MG_Pipe::MGPipeApplierState& st, DrawVertexBufferRequest* out,
                                                Uint capacity);

        DrawIndexBufferRequest ResolveDrawIndexBufferFromRecord(const MG_Pipe::MGPipeApplierState& st);

        extern Bool g_shaderStorageWritesUnordered;

        void SyncBufferBindingPointsByRecord(Uint32 shaderBufferClass, GLenum glTarget);

        void SyncAtomicCounterBuffers(const Vector<Int>& glBindings, Int esslBindingTop);
    } // namespace BufferImpl

    // Transform feedback is captured by the real ES driver: the backend program
    // declares the capture set at link time (see BackendProgramObjectImpl::SyncToBackend)
    // and the span below wraps the driver's own glBeginTransformFeedback/glEndTransformFeedback.
    //
    // The driver-side Begin is deferred from the frontend's glBeginTransformFeedback to
    // the first draw of the span: ES requires the capturing program to be current and
    // the capture buffers bound when Begin is issued, and both of those only become true
    // once PrepareForDraw has run. A span that never draws therefore never touches the
    // driver at all, which is also what the GL semantics amount to.
    //
    // Transform feedback objects (ARB_transform_feedback2) are ES 3.0 core, so each
    // frontend object gets one of the driver's: a paused span lives inside the ES object,
    // which is the only way several of them can be paused at once - and the only reason
    // the default object alone would not do.

    namespace XfbImpl {
        struct XfbCaptureTarget {
            SharedPtr<MG_State::GLState::BufferObject> buffer;
            Uint backendId = 0;
            SizeT start = 0;
            SizeT end = 0;
            // WHICH capture buffer of the program this is. The list is COMPACTED - a
            // capture buffer with no bound buffer object contributes no entry - so the
            // position in the vector is not the program's buffer index, and everything
            // that asks the program about a target (its stride, which varyings land in
            // it) has to ask about this index instead. A capture list beginning with
            // gl_NextBuffer is the shape that makes them differ: buffer 0 has stride 0
            // and nothing bound, so target 0 describes buffer 1.
            SizeT bufferIndex = 0;
            MG_Pipe::MGPipeHandle handle = MG_Pipe::kMGPipeNullHandle;
        };

        struct XfbProgramSource {
            SharedPtr<MG_State::GLState::ProgramObject> Frontend;
            SharedPtr<const MG_State::GLState::ProgramArchive> Archive;
            explicit operator Bool() const { return Frontend || Archive; }
            const XfbProgramSource* operator->() const { return this; }
            SizeT GetTransformFeedbackBufferCount() const {
                return Archive ? Archive->Link.xfbStrides.size() : Frontend->GetTransformFeedbackBufferCount();
            }
            Uint32 GetTransformFeedbackStride(Uint32 index) const {
                return Archive ? (index < Archive->Link.xfbStrides.size() ? Archive->Link.xfbStrides[index] : 0)
                               : Frontend->GetTransformFeedbackStride(index);
            }
            Bool NeedsScatteredTransformFeedbackCapture() const {
                return Archive ? Archive->Link.xfbNeedsScatteredCapture
                               : Frontend->NeedsScatteredTransformFeedbackCapture();
            }
            Uint32 GetTransformFeedbackPackedStride() const {
                return Archive ? Archive->Link.xfbPackedStride : Frontend->GetTransformFeedbackPackedStride();
            }
            GLenum GetTransformFeedbackBufferMode() const {
                return Archive ? Archive->Link.xfbBufferMode : Frontend->GetTransformFeedbackBufferMode();
            }
            const auto& GetTransformFeedbackVaryings() const {
                return Archive ? Archive->Link.xfbVaryings : Frontend->GetTransformFeedbackVaryings();
            }
        };
        // Per frontend transform feedback object. The default object (name 0) maps to
        // the driver's default object (id 0) and is always present.
        struct XfbObjectState {
            GLuint esId = 0;
            Bool pending = false; // frontend Begin seen, driver capture not started yet
            Bool started = false; // driver capture running
            Bool paused = false;  // frontend Pause seen and not yet resumed
            GLenum primitiveMode = GL_POINTS;
            Vector<XfbCaptureTarget> targets;
            // Set for a layout ES cannot express (gl_SkipComponents / gl_NextBuffer):
            // the driver captures gap-free records into the scratch buffer below and
            // End scatters them into `targets`.
            Bool scattered = false;
            SharedPtr<MG_State::GLState::ProgramObject> scatterProgram;
            SharedPtr<const MG_State::GLState::ProgramArchive> scatterArchive;
            // Paused spans in different served contexts may coexist. Their captured
            // bytes must not share the monolith's one reusable scratch buffer.
            GLuint serverScatterBuffer = 0;
            SizeT serverScatterSize = 0;
            SizeT scatterCapacityVertices = 0;
        };

        extern GLuint g_scatterBufferId;

        extern SizeT g_scatterBufferSize;

        extern UnorderedMap<GLuint, XfbObjectState> g_xfbObjects;

        extern GLuint g_currentXfbName;

        extern XfbObjectState* g_currentXfbState;

        struct RoleXfbState {
            Uint NativeGeneration = 0;
            // Server keys are record-supplied, never-reused XFB lifetime ids.
            // The monolith role keeps its existing GL-name semantics separately.
            UnorderedMap<Uint64, XfbObjectState> Objects;
            Uint64 Current = 0;
            XfbObjectState* Cached = nullptr;
            GLuint ScatterBuffer = 0;
            SizeT ScatterSize = 0;
            Bool NeedsBind = true;
        };

        extern Array<RoleXfbState, 1> g_roleXfbState;

        XfbObjectState& CurrentXfb();

        // EVERY way this path can lose a capture used to be silent: three unlogged early
        // returns before the driver Begin, an unchecked glBeginTransformFeedback, and two
        // `continue`s in the readback. The application sees a buffer that kept its
        // pre-draw bytes, GL_NO_ERROR, and GL_LINK_STATUS true - which is how one defect
        // reached ~320 conformance bodies across four families before anyone could say
        // which of the branches fired. Nothing below changes what MobileGL DOES on a
        // healthy capture; it only makes a lost one name itself in /sdcard/MG/latest.log.
        //
        // MGLOG_E_ONCE (not _D) on purpose: these have to be readable in an INFO-level
        // artifact, the same reason the backend link failure at Managers.cpp is MGLOG_E.
        constexpr Int kMaxDrainedXfbErrors = 32;

        namespace {
            // The ES error raised by the call just issued, GL_NO_ERROR if it succeeded. Drains
            // the rest of the queue so the next probe cannot read this one as its own.
            GLenum TakeXfbDriverError() {
                const GLenum first = g_GLESFuncs.glGetError();
                if (first == GL_NO_ERROR) return GL_NO_ERROR;
                for (Int i = 0; i < kMaxDrainedXfbErrors && g_GLESFuncs.glGetError() != GL_NO_ERROR; ++i) {}
                return first;
            }
        } // namespace

        void OnBackendContextDestroyed();
    } // namespace XfbImpl

    namespace TextureImpl {
        // The frontend texture-state keys the per-draw texture stages
        // (SyncNeccessaryTextures, then BindCurrentTextures) both consume. Captured
        // ONCE per draw/dispatch preparation and passed to both: nothing between the
        // two stages can move any of these - only frontend GL entry points mutate
        // them, and none run inside a backend preparation (the FBO/program syncs in
        // between only read frontend state). The no-arg wrappers keep capturing at
        // the call for every non-draw call site (Clear, readbacks).
        struct DrawTextureSyncKeys {
            Uint64 contextId = 0;
            Uint64 samplingGeneration = 0;
            Uint64 unitBindingsEpoch = 0;
            Int maxTouchedUnit = -1;
        };

        DrawTextureSyncKeys CaptureDrawTextureSyncKeys();

        void SyncNeccessaryTextures(const DrawTextureSyncKeys& keys);

        void SyncNeccessaryTextures();

        extern Uint g_imageUnitHighWaterMark;

        void SyncImageTextureBinding(const MG_Pipe::MGPImageView& view);

        void MarkWritableImageBufferTexturesGpuWritten();

        void SyncImageTextureBindings();
    } // namespace TextureImpl

    namespace FramebufferImpl {
        // P4a e1 (D-C2, D-B3). The framebuffer family's handle arm.
        //
        // THE FOUR-PART MEMO BECOMES TWO VALUES, and the halves are not symmetric.
        // g_fboSyncedSlotVersions / ObjectVersions / Objects are three answers to one question
        // - "has the state the client would describe moved?" - asked of the frontend from the
        // backend. The applier answers it with one number: FramebufferSerial is bumped by the
        // applier before the backend is told anything, on every set_framebuffer_state it
        // accepts, and a record the client's ContentHash suppressor withheld provably describes
        // state that did not move. So one Uint64 compare replaces the three.
        //
        // The FOURTH stays, and must: g_attachmentBackendIdGeneration answers "did *I* re-mint
        // a driver texture id", which no client-side version can answer, and dropping it is the
        // shape of bug commit d7655247 fixed on the buffer side (D-B3, D-O). The ES context
        // generation joins it for the same reason the legacy arm's other memos carry it - the
        // driver FBO names die with the context.
        // Kept PER TARGET, like the arrays it replaces, and for the one reason that survives
        // the collapse: ForceBindCurrentFBO syncs a single target and has to be able to say so.
        // What collapses is the three-value "has the state moved" question into one serial.
        struct SyncedFramebufferSerialMemo {
            Uint64 serial = 0; // the FramebufferSerial this target was last synced at
            Uint64 backendIdGeneration = 0;
            Uint contextGeneration = 0;
            Bool valid = false;
        };

        extern Array<SyncedFramebufferSerialMemo, SizeT(FramebufferTarget::FramebufferTargetCount)> g_fboSyncedSerials;

        extern Bool g_fboRecordsTrusted;

        // A9 (the review's MAJOR-4, and it is PACKAGE D's fix, not this file's): every call
        // site of this function in DirectGLES.cpp is paired with
        // FramebufferImpl::InvalidateFramebufferBindingCache() - all six, verified - but that
        // function lives in Managers.cpp (D's file for the whole phase) and has three further
        // callers this package cannot reach: MG_Test/SanityTest.cpp's ScopedStateGuardMocks::
        // ResetShadows and ScopedBackendTwinMocks' ctor and dtor. Those three clear the
        // pre-handle trio and leave g_fboSyncedSerials and g_fboRecordsTrusted stale across a
        // GLES function-table swap - harmless while every arm declines, but on the integrated
        // tree with bit 9 set a fixture can be entered with the records trusted and a memo
        // claiming a target is synced, so BindCurrentFBO would bind from a record while the
        // mock table is installed. THE FIX IS TO CALL THIS FROM INSIDE
        // InvalidateFramebufferBindingCache so no caller can forget; it is one line in D's
        // file and it is on D's rework list.
        static void InvalidateFramebufferHandleArmMemos() {
            for (auto& memo : g_fboSyncedSerials)
                memo.valid = false;
            g_fboRecordsTrusted = false;
        }

        void SyncCurrentFBO();
    } // namespace FramebufferImpl

    namespace RenderStateImpl {
        extern RenderStateParameters g_syncedRenderStateParameters;

        extern Uint32 g_syncedColorMaskAlphaWidenMask;

        void InvalidateSyncedRenderState();

        Bool DriverScissorTestEnabled();

        void SyncRenderState(Bool forColorClear);
    } // namespace RenderStateImpl

    namespace PrgramImpl {
        extern BackendProgramObjectImpl* g_currentDrawBackendProgram;

        extern MG_Pipe::MGPipeHandle g_currentDrawProgramHandle;

        extern Bool g_broadcastMemoValid;

        extern Bool g_broadcastMemoHandleValid;

        const MG_Pipe::MGPipeShaderCsoRecord* FindShaderCsoRecord(MG_Pipe::MGPipeHandle handle);

        const MG_Pipe::MGPipeShaderCsoRecord* ResolveGlobalConstantsRecordForHandle(MG_Pipe::MGPipeHandle handle,
                                                                                    Uint uboSize);

        void SyncCurrentProgramByHandle(MG_Pipe::MGPipeHandle cso, Bool forDraw);
    } // namespace PrgramImpl

    // Defined with the surface registry, below.
    void FlushAheadOfWindowBufferUse();

    void BindCurrentFBO(FramebufferTarget target);

    void BindCurrentProgramWithResources(const SharedPtr<MG_State::GLState::ProgramObject>& currentProgram,
                                         const TextureImpl::DrawTextureSyncKeys& keys,
                                         MG_Pipe::MGPipeHandle programCso);
    void BindCurrentTextures(const TextureImpl::DrawTextureSyncKeys& keys,
                             const SharedPtr<MG_State::GLState::ProgramObject>& currentProgram);

    void PrepareForDraw(DrawSyncFlags syncBit);

    Bool ResolveAndBindUnitTextures(const SharedPtr<MG_State::GLState::ProgramObject>& currentProgram,
                                    Int maxTouchedUnit);

    extern Uint64 g_unitSamplerWalkContextId;

    extern Uint64 g_unitSamplerWalkEpoch;

    extern Int g_unitSamplerWalkMaxUnit;

    extern Uint g_unitSamplerWalkContextGeneration;

    extern Bool g_unitSamplerWalkValid;

    void BindCurrentUnitSamplers(const TextureImpl::DrawTextureSyncKeys& keys);

    // Memo of the resolved per-unit TEXTURE bindings, so a steady-state draw loop stops
    // re-deriving an answer nothing has invalidated (a Minecraft frame issues thousands of
    // draws that touch none of the inputs below, and the resolution walks every binding slot
    // of every touched unit three times).
    //
    // ResolveAndBindUnitTextures is a pure function of:
    //   * the GLContext identity - a never-reused id, since the counters below restart at 0 in
    //     a new context and a recreated one can land on the old heap address;
    //   * the per-unit bindings - which texture object every slot holds and which sampler
    //     object each unit carries, keyed as TextureImpl::CurrentUnitBindingsEpoch rather
    //     than as the texture bind generation: the generation also bumps on redundant
    //     re-binds (26.2 re-binds the unit's own sampler around every texture-unit switch),
    //     which would force a full re-resolution per draw for an answer that cannot have
    //     moved. The one input the old generation key covered that the epoch does not - a
    //     default texture gaining or losing an image with no bind moving - bumps the
    //     sampling-resolution generation below (SetInternalFormat -> BumpShapeVersion);
    //   * the touched-unit high-water mark (units above it have provably-empty slots);
    //   * the program that arbitrates two real textures aliased onto one native target -
    //     identity, plus the lifetime id because a freed program can be replaced at the same
    //     address, plus the backend-state version which moves on every relink and on every
    //     sampler-uniform unit assignment, plus the link status;
    //   * the sampling-resolution generation - any texture shape change or any sampler
    //     parameter change, i.e. everything mipmap-completeness is computed from, and
    //     completeness is what decides whether a texture is bound at all;
    //   * the ES context generation, because the backend texture ids and the driver's own
    //     binding state die with the context.
    //
    // A matching key only says the ANSWER is unchanged; replaying it as "do nothing" also
    // requires the bindings it established to still be on the driver. Rather than enumerate
    // every writer, the memo keeps the binding shadow it left and compares it: that shadow is
    // already the authority every redundant-bind filter in this backend trusts, and every path
    // that puts a texture on a unit behind this function's back maintains it - the upload
    // path's scratch bind on the temp unit (BackendTextureObject::Bind out of
    // SyncMipmapsToBackend), CopyTexSubImage2D and GenerateMipmap binding on the active unit,
    // the glBindTextures fast path, and the self-scrub a BackendTextureObject performs when it
    // is destroyed or respecified.
    struct ResolvedTextureBindingMemo {
        Bool valid = false;
        Uint64 glContextId = 0;
        Int maxTouchedUnit = -1;
        Uint64 unitBindingsEpoch = 0;
        Uint64 samplingResolutionGeneration = 0;
        const void* program = nullptr;
        Uint64 programLifetimeId = 0;
        Uint32 programBackendStateVersion = 0;
        Bool programLinked = false;
        Uint contextGeneration = 0;
        // P5e (pg) -> tx2, ruling 6 / ID-86: THE PROGRAM HALF OF THIS KEY HAS TO GO BY HANDLE,
        // and this struct is tx2's line, so pg states the shape rather than writing it. The four
        // program fields above are read off the frontend ProgramObject on EVERY DRAW, memo hit
        // included (kimi row 100, the worst row of the program family). What replaces them on
        // the handle arm is {DrawProgram.Slot, DrawProgram.Gen, ShaderCso record Serial,
        // BindingsSerial}: Serial moves on every applied create_shader_state (how a relink
        // travels) and BindingsSerial on every applied set_program_bindings (how a glUniform1i
        // on a sampler travels). Both are needed - ruling 6's "both" - because a relink with
        // UNCHANGED texture resolution still hands the twin new unit assignments.
        // P5e (tx2), CONTRACT-P5E §5.3 / ruling 6 (ID-86): THE KEY IS BOTH HALVES.
        //
        // S2 argued SamplerViewsSerial alone - it already mixes the program-resolved opaque units,
        // so a resolution that moved moves it - and S4 argued the program's rows. The integrator
        // ruled BOTH, for the case neither covers alone: a RELINK whose resolution happens to be
        // unchanged still needs the twin's new unit assignments, and SamplerViewsSerial does not
        // move for it (the emitter's content hash is over the resolved set, which did not change).
        //
        // The frontend program rows above (pointer, lifetime id, backend-state version, link
        // status) stay for the monolith arm and are simply not read on this one.
        MG_Pipe::MGPipeHandle drawProgram = MG_Pipe::kMGPipeNullHandle;
        Uint64 shaderCsoSerial = 0;
        Uint64 bindingsSerial = 0;
        Uint64 samplerViewsSerial = 0;
        Uint64 contextSerial = 0;
        decltype(TextureImpl::g_boundTexturesCache) boundTextures{};
    };

    extern Array<ResolvedTextureBindingMemo, 4> g_resolvedTextureBindingMemos;

    extern Uint32 g_resolvedTextureBindingMemoCursor;

    void BindCurrentTextures(const TextureImpl::DrawTextureSyncKeys& keys,
                             const SharedPtr<MG_State::GLState::ProgramObject>& currentProgram);

    void BindCurrentTextures();

    void BindCurrentProgramWithResources(const SharedPtr<MG_State::GLState::ProgramObject>& currentProgram,
                                         const TextureImpl::DrawTextureSyncKeys& keys,
                                         MG_Pipe::MGPipeHandle programCso);

    PrgramImpl::BackendProgramObjectImpl* GetCurrentBackendProgram();

    void SetCurrentBaseInstance(Uint32 baseInstance);

    void SetCurrentDrawID(Uint32 drawId);

    void SetCurrentBaseVertex(Int32 baseVertex);

    Bool CurrentProgramReadsDrawID();

    Bool CurrentProgramReadsBaseVertex();

    void OnRestartSubstitutionContextDestroyed();

    void DrainDriverErrors(const char* site);

    static void DrainBlitErrors() {
        DrainDriverErrors("BlitFramebuffer");
    }

    // ---------------------------------------------------------------------------------
    // Shared guard for the emulation passes that have to DRAW to get their work done: the
    // single-sample -> multisample blit replicate below, and the depth/stencil readback
    // emulation further down. Both borrow the application's ES context for a full-screen
    // pass, so everything they disturb is captured here and put back on the way out - the
    // sync layer's shadow of the driver state has to stay true, and one leaked binding
    // regresses every draw that follows.
    //
    // Three members of that set are not obvious:
    //
    //  - The borrowed texture unit. Both passes bind their scratch texture to
    //    TextureImpl::TempTextureUnit, and the unit's binding is only restored correctly by
    //    asking TextureImpl::g_boundTexturesCache what is supposed to be there: the raw
    //    glBindTexture the passes issue never moves that cache, so restoring a *queried*
    //    id leaves the driver and the cache disagreeing and the per-draw binding memo
    //    false-skips the re-bind - the borrowed-slot failure mode that showed up as
    //    process-wide glyph death under Iris. Reading GL_TEXTURE_BINDING_2D was doubly
    //    wrong here because the replicate path may already have switched units and bound
    //    its own scratch texture by the time it asked.
    //
    //  - The sampler object on that unit. It would override the scratch texture's own
    //    filter and compare parameters. Unbinding through SamplerImpl::UnbindSampler moves
    //    the sampler cache, which is exactly what re-opens BindCurrentUnitSamplers' memo,
    //    so the application's sampler comes back on the next draw with no explicit restore.
    //
    //  - An active transform feedback capture. GL rejects a draw issued with a program
    //    other than the one that began the capture, and would otherwise append the
    //    emulation's vertices to the application's buffers.
    //
    //  - The application's clip control. The passes place their triangle in the default
    //    lower-left, [-1, 1] clip space; an upper-left origin would mirror it vertically.
    class ScopedEmulationDrawState {
    public:
        ScopedEmulationDrawState() {
            g_GLESFuncs.glGetIntegerv(GL_CURRENT_PROGRAM, &m_program);
            g_GLESFuncs.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &m_vertexArray);
            g_GLESFuncs.glGetIntegerv(GL_VIEWPORT, m_viewport);
            g_GLESFuncs.glGetIntegerv(GL_SCISSOR_BOX, m_scissorBox);
            g_GLESFuncs.glGetBooleanv(GL_COLOR_WRITEMASK, m_colorMask);
            g_GLESFuncs.glGetIntegerv(GL_DEPTH_FUNC, &m_depthFunc);
            g_GLESFuncs.glGetBooleanv(GL_DEPTH_WRITEMASK, &m_depthMask);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_FUNC, &m_stencilFunc[0]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_BACK_FUNC, &m_stencilFunc[1]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_REF, &m_stencilRef[0]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_BACK_REF, &m_stencilRef[1]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_VALUE_MASK, &m_stencilValueMask[0]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_BACK_VALUE_MASK, &m_stencilValueMask[1]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_WRITEMASK, &m_stencilWriteMask[0]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &m_stencilWriteMask[1]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_FAIL, &m_stencilFail[0]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_BACK_FAIL, &m_stencilFail[1]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &m_stencilDepthFail[0]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_FAIL, &m_stencilDepthFail[1]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &m_stencilPass[0]);
            g_GLESFuncs.glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_PASS, &m_stencilPass[1]);
            if (g_GLESCapabilities.SupportsClipDistance) {
                m_capabilityCount = kBaseCapabilityCount + kClipDistanceCapabilityCount;
            }
            for (Uint i = 0; i < m_capabilityCount; ++i) {
                m_capabilities[i].enabled = g_GLESFuncs.glIsEnabled(m_capabilities[i].cap);
            }
            // GL_SAMPLE_MASK is ES 3.1; on an older driver the query above just raised
            // GL_INVALID_ENUM and answered GL_FALSE, which is also the right thing to
            // restore. Drop the flag so it is not misattributed to the emulation's own work.
            DrainBlitErrors();

            if (MG_Pipe::gPipeInputs->IsTransformFeedbackActive() &&
                !MG_Pipe::gPipeInputs->IsTransformFeedbackPaused() && g_GLESFuncs.glPauseTransformFeedback) {
                g_GLESFuncs.glPauseTransformFeedback();
                m_pausedTransformFeedback = true;
                DrainBlitErrors();
            }

            if (g_GLESCapabilities.SupportsClipControl) {
                g_GLESFuncs.glGetIntegerv(GL_CLIP_ORIGIN, &m_clipOrigin);
                g_GLESFuncs.glGetIntegerv(GL_CLIP_DEPTH_MODE, &m_clipDepthMode);
                g_GLESFuncs.glClipControlEXT(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
            }

            m_activeTextureUnit = TextureImpl::g_activeTextureUnit;
            TextureImpl::ActivateTextureUnit(TextureImpl::TempTextureUnit);
            SamplerImpl::UnbindSampler(TextureImpl::TempTextureUnit);

            // The neutral baseline every emulation pass wants: nothing culled, nothing
            // clipped, nothing tested, no coverage games, and colour writes open. Callers
            // turn back on only what they need (the replicate pass wants the depth and
            // stencil tests, and masks colour off because it writes neither).
            for (Uint i = 0; i < m_capabilityCount; ++i) {
                g_GLESFuncs.glDisable(m_capabilities[i].cap);
            }
            g_GLESFuncs.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            g_GLESFuncs.glDepthMask(GL_FALSE);
            DrainBlitErrors();
        }

        ~ScopedEmulationDrawState() {
            g_GLESFuncs.glUseProgram(static_cast<GLuint>(m_program));
            // Put back whatever the binding cache says lives on the borrowed unit, not what
            // the driver happened to hold: see the class comment.
            auto* cachedBound = TextureImpl::g_boundTexturesCache[TextureImpl::TempTextureUnit]
                                                                 [static_cast<SizeT>(TextureTarget::Texture2D)];
            g_GLESFuncs.glBindTexture(GL_TEXTURE_2D, cachedBound ? cachedBound->GetBackendTextureId() : 0);
            TextureImpl::ActivateTextureUnit(m_activeTextureUnit);
            VertexArrayImpl::BindBackendVAOId(static_cast<GLuint>(m_vertexArray));
            g_GLESFuncs.glViewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]);
            g_GLESFuncs.glScissor(m_scissorBox[0], m_scissorBox[1], m_scissorBox[2], m_scissorBox[3]);
            g_GLESFuncs.glColorMask(m_colorMask[0], m_colorMask[1], m_colorMask[2], m_colorMask[3]);
            g_GLESFuncs.glDepthFunc(static_cast<GLenum>(m_depthFunc));
            g_GLESFuncs.glDepthMask(m_depthMask);
            const GLenum faces[2] = {GL_FRONT, GL_BACK};
            for (SizeT face = 0; face < 2; ++face) {
                g_GLESFuncs.glStencilFuncSeparate(faces[face], static_cast<GLenum>(m_stencilFunc[face]),
                                                  m_stencilRef[face], static_cast<GLuint>(m_stencilValueMask[face]));
                g_GLESFuncs.glStencilOpSeparate(faces[face], static_cast<GLenum>(m_stencilFail[face]),
                                                static_cast<GLenum>(m_stencilDepthFail[face]),
                                                static_cast<GLenum>(m_stencilPass[face]));
                g_GLESFuncs.glStencilMaskSeparate(faces[face], static_cast<GLuint>(m_stencilWriteMask[face]));
            }
            for (Uint i = 0; i < m_capabilityCount; ++i) {
                if (m_capabilities[i].enabled) {
                    g_GLESFuncs.glEnable(m_capabilities[i].cap);
                } else {
                    g_GLESFuncs.glDisable(m_capabilities[i].cap);
                }
            }
            // The per-draw-buffer colour masks are not covered by the non-indexed
            // glColorMask above. Restore what the SYNC actually pushed, not the raw
            // application masks: a widened attachment's alpha write is forced off by
            // SyncRenderState and memoized in g_syncedColorMaskAlphaWidenMask, and the
            // next sync early-outs on an unchanged version - restoring the undoctored
            // mask here would leave alpha writes enabled on the widened buffer with
            // nothing left to repair it. Same three-way pointer fallback as
            // SyncRenderState's push: gating on the core name alone left EXT/OES-only
            // devices holding buffer 0's mask broadcast across every buffer.
            const auto colorMaskiFn = g_GLESFuncs.glColorMaski      ? g_GLESFuncs.glColorMaski
                                      : g_GLESFuncs.glColorMaskiEXT ? g_GLESFuncs.glColorMaskiEXT
                                                                    : g_GLESFuncs.glColorMaskiOES;
            if (colorMaskiFn) {
                for (Uint index = 0; index < MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS; ++index) {
                    BoolVec4 colorMask = RenderStateImpl::g_syncedRenderStateParameters.ColorMasks[index];
                    if (index < 32 && (RenderStateImpl::g_syncedColorMaskAlphaWidenMask & (1u << index)) != 0) {
                        colorMask.w() = false;
                    }
                    colorMaskiFn(index, colorMask.x() ? GL_TRUE : GL_FALSE, colorMask.y() ? GL_TRUE : GL_FALSE,
                                 colorMask.z() ? GL_TRUE : GL_FALSE, colorMask.w() ? GL_TRUE : GL_FALSE);
                }
            }
            if (g_GLESCapabilities.SupportsClipControl) {
                g_GLESFuncs.glClipControlEXT(static_cast<GLenum>(m_clipOrigin), static_cast<GLenum>(m_clipDepthMode));
            }
            if (m_pausedTransformFeedback && g_GLESFuncs.glResumeTransformFeedback) {
                g_GLESFuncs.glResumeTransformFeedback();
            }
            DrainBlitErrors();
        }

        ScopedEmulationDrawState(const ScopedEmulationDrawState&) = delete;
        ScopedEmulationDrawState& operator=(const ScopedEmulationDrawState&) = delete;

    private:
        struct CapabilityState {
            GLenum cap;
            GLboolean enabled;
        };

        GLint m_program = 0;
        GLint m_vertexArray = 0;
        GLint m_viewport[4] = {0, 0, 0, 0};
        GLint m_scissorBox[4] = {0, 0, 0, 0};
        GLboolean m_colorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
        GLint m_depthFunc = GL_LESS;
        GLboolean m_depthMask = GL_TRUE;
        GLint m_stencilFunc[2] = {GL_ALWAYS, GL_ALWAYS};
        GLint m_stencilRef[2] = {0, 0};
        GLint m_stencilValueMask[2] = {~0, ~0};
        GLint m_stencilWriteMask[2] = {~0, ~0};
        GLint m_stencilFail[2] = {GL_KEEP, GL_KEEP};
        GLint m_stencilDepthFail[2] = {GL_KEEP, GL_KEEP};
        GLint m_stencilPass[2] = {GL_KEEP, GL_KEEP};
        Uint m_activeTextureUnit = 0;
        Bool m_pausedTransformFeedback = false;
        GLint m_clipOrigin = GL_LOWER_LEFT;
        GLint m_clipDepthMode = GL_NEGATIVE_ONE_TO_ONE;
        // The eight GL_CLIP_DISTANCE0_EXT..7_EXT entries are last so that a driver without
        // GL_EXT_clip_cull_distance can be served by shortening the count instead of asking
        // it about tokens it does not know. They belong here at all because an emulation pass
        // draws its full-screen triangle with its OWN program, which writes no gl_ClipDistance:
        // leaving the app's enables on would clip that triangle by undefined distances.
        static constexpr Uint kBaseCapabilityCount = 10;
        static constexpr Uint kClipDistanceCapabilityCount = 8;
        Uint m_capabilityCount = kBaseCapabilityCount;
        CapabilityState m_capabilities[kBaseCapabilityCount + kClipDistanceCapabilityCount] = {
            {GL_SCISSOR_TEST, GL_FALSE},
            {GL_DEPTH_TEST, GL_FALSE},
            {GL_STENCIL_TEST, GL_FALSE},
            {GL_CULL_FACE, GL_FALSE},
            {GL_BLEND, GL_FALSE},
            {GL_RASTERIZER_DISCARD, GL_FALSE},
            {GL_POLYGON_OFFSET_FILL, GL_FALSE},
            {GL_SAMPLE_ALPHA_TO_COVERAGE, GL_FALSE},
            {GL_SAMPLE_COVERAGE, GL_FALSE},
            {GL_SAMPLE_MASK, GL_FALSE},
            {0x3000, GL_FALSE},
            {0x3001, GL_FALSE},
            {0x3002, GL_FALSE},
            {0x3003, GL_FALSE},
            {0x3004, GL_FALSE},
            {0x3005, GL_FALSE},
            {0x3006, GL_FALSE},
            {0x3007, GL_FALSE},
        };
    };

    // ---------------------------------------------------------------------------------
    // Single-sample -> multisample blit ("replicate")
    //
    // Desktop GL replicates the source sample into every destination sample when the read
    // framebuffer is single-sampled and the draw framebuffer is not. ES forbids the whole
    // call ("INVALID_OPERATION if SAMPLE_BUFFERS for the draw framebuffer is greater than
    // zero"), so the blit silently did nothing - KHR-GL3x.packed_depth_stencil.blit's
    // second loop then read a destination that still held its clear values.
    //
    // Emulated by drawing a full-screen triangle into the multisample framebuffer: every
    // pixel is fully covered, so every sample of it receives the same value, which is
    // exactly what the replicate rule asks for. Depth comes from gl_FragDepth; stencil has
    // no shader output on ES, so it is written one bit plane at a time with REPLACE and a
    // discard for the pixels whose source bit is clear.
    // Defined further down with the other small GL helpers; the attachment-format probe below
    // needs it to tell a rejected bind apart from a successful one.
    static void ClearGLErrors();

    namespace ReplicateBlitImpl {
        GLuint BuildProgram(const char* fragmentSource);

        GLenum QueryAttachmentSizedFormat(GLenum attachment);

        static Bool FormatHasDepth(GLenum internalFormat) {
            return internalFormat == GL_DEPTH_COMPONENT16 || internalFormat == GL_DEPTH_COMPONENT24 ||
                   internalFormat == GL_DEPTH_COMPONENT32F || internalFormat == GL_DEPTH24_STENCIL8 ||
                   internalFormat == GL_DEPTH32F_STENCIL8;
        }

        static Bool FormatHasStencil(GLenum internalFormat) {
            return internalFormat == GL_DEPTH24_STENCIL8 || internalFormat == GL_DEPTH32F_STENCIL8 ||
                   internalFormat == GL_STENCIL_INDEX8;
        }

        static GLenum ScratchAttachmentFor(GLenum internalFormat) {
            if (FormatHasDepth(internalFormat) && FormatHasStencil(internalFormat)) {
                return GL_DEPTH_STENCIL_ATTACHMENT;
            }
            return FormatHasDepth(internalFormat) ? GL_DEPTH_ATTACHMENT : GL_STENCIL_ATTACHMENT;
        }
    } // namespace ReplicateBlitImpl

    // ---- Scoped driver-state guards for the readback/copy/blit emulation paths --------------------
    // These paths borrow driver state (FBO bindings, scratch-FBO attachments, PACK
    // pixel-store, the pack-PBO binding, scissor) that the app never asked to
    // change; every mutation is scoped by an RAII guard so no exit path can leak
    // it. Saves/restores go through the DirectGLES driver-state shadows
    // (Managers.h) instead of glGetIntegerv - no driver round-trips, and redundant
    // rebinds/resets no-op.

    // Saves the driver READ/DRAW framebuffer binding(s) and restores them on exit.
    // Per-instance state: nesting-safe.
    class ScopedFramebufferBinding {
    public:
        ScopedFramebufferBinding(Bool saveRead, Bool saveDraw) : m_saveRead(saveRead), m_saveDraw(saveDraw) {
            if (m_saveRead) m_prevRead = FramebufferImpl::CurrentFramebufferBinding(FramebufferTarget::Read);
            if (m_saveDraw) m_prevDraw = FramebufferImpl::CurrentFramebufferBinding(FramebufferTarget::Draw);
        }
        ~ScopedFramebufferBinding() {
            if (m_saveRead) FramebufferImpl::BindFramebufferId(GL_READ_FRAMEBUFFER, m_prevRead);
            if (m_saveDraw) FramebufferImpl::BindFramebufferId(GL_DRAW_FRAMEBUFFER, m_prevDraw);
        }
        ScopedFramebufferBinding(const ScopedFramebufferBinding&) = delete;
        ScopedFramebufferBinding& operator=(const ScopedFramebufferBinding&) = delete;

    private:
        const Bool m_saveRead;
        const Bool m_saveDraw;
        GLuint m_prevRead = 0;
        GLuint m_prevDraw = 0;
    };

    // Applies a PACK pixel-store configuration and restores the previous one on exit.
    class ScopedPackState {
    public:
        explicit ScopedPackState(const PixelStoreImpl::PackState& desired)
            : m_prev(PixelStoreImpl::CurrentPackState()) {
            PixelStoreImpl::ApplyPackState(desired);
        }
        ~ScopedPackState() { PixelStoreImpl::ApplyPackState(m_prev); }
        ScopedPackState(const ScopedPackState&) = delete;
        ScopedPackState& operator=(const ScopedPackState&) = delete;

    private:
        const PixelStoreImpl::PackState m_prev;
    };

    // Binds a pixel PACK buffer (0 = client memory) for the scope and returns the
    // binding to the resting 0 state on exit, so no later readback can accidentally
    // capture into a stale PBO.
    class ScopedPixelPackBuffer {
    public:
        explicit ScopedPixelPackBuffer(GLuint id) { BufferImpl::BindPixelPackBufferId(id); }
        ~ScopedPixelPackBuffer() { BufferImpl::BindPixelPackBufferId(0); }
        ScopedPixelPackBuffer(const ScopedPixelPackBuffer&) = delete;
        ScopedPixelPackBuffer& operator=(const ScopedPixelPackBuffer&) = delete;
    };

    // Force-disables GL_SCISSOR_TEST for the scope (emulation blits and clears are
    // scissored; readback copies must not be clipped by app scissor state) and
    // restores the app state on exit.
    //
    // WHAT THE DRIVER HAS, NOT WHAT THE SHADOW LAST SAID. The render-state shadow is one per
    // process and is synced at draws; a session that presents or reads back without drawing
    // since another session's context synced (a frame that redraws nothing, a client whose
    // turn came after the compositor's) found the other context's bit there. Its own scissor,
    // still on from its last draw, then clipped the shared-image copy to a box - flipped into
    // the wrong rows, too - and the image kept the frame it held before.
    class ScopedScissorDisable {
    public:
        ScopedScissorDisable() : m_wasEnabled(RenderStateImpl::DriverScissorTestEnabled()) {
            if (m_wasEnabled) g_GLESFuncs.glDisable(GL_SCISSOR_TEST);
        }
        ~ScopedScissorDisable() {
            if (m_wasEnabled) g_GLESFuncs.glEnable(GL_SCISSOR_TEST);
        }
        ScopedScissorDisable(const ScopedScissorDisable&) = delete;
        ScopedScissorDisable& operator=(const ScopedScissorDisable&) = delete;

    private:
        const Bool m_wasEnabled;
    };

    // Binds the shared scratch FBO at READ (isRead) or DRAW for one temp operation,
    // restoring the previous binding on exit. Attachments are managed through the
    // ScratchFBOImpl attachment shadow by the caller (see Framebuffer()).
    class TempFBOBinder {
    public:
        explicit TempFBOBinder(Bool isRead)
            : m_binding(/*saveRead=*/isRead, /*saveDraw=*/!isRead),
              m_target(isRead ? GL_READ_FRAMEBUFFER : GL_DRAW_FRAMEBUFFER) {
            FramebufferImpl::BindFramebufferId(m_target, ScratchFBOImpl::EnsureId(Framebuffer()));
        }
        ScratchFBOImpl::ScratchFramebuffer& Framebuffer() const { return ScratchFBOImpl::TempFramebuffer(); }
        GLenum Target() const { return m_target; }

    private:
        ScopedFramebufferBinding m_binding;
        const GLenum m_target;
    };

    static void ClearGLErrors() {
        DrainDriverErrors("DirectGLES");
    }

    // DirectGLESReadback.cpp defines it: E's extent-scoped stand-in for a
    // driver that refuses to read a colour level back, asked here too so a host reaches the store
    // route below (P8-SE).
    Bool WireTextureReadbackRefusedForTesting(const IntVec3& extent);
    using FramebufferImpl::SubstituteWidenedClearAlpha;

    // ---- Client-format readback conversion ---------------------------------------------------------------------
    // ES 3.x glReadPixels only guarantees GL_RGBA/GL_UNSIGNED_BYTE, GL_RGBA_INTEGER/GL_(UNSIGNED_)INT,
    // GL_RGBA/GL_FLOAT for float buffers plus one implementation-defined pair, while desktop GL clients read
    // back narrower layouts (RED, RG, RGB, BGR, byte-order packed types, ...). For those we read a guaranteed
    // wide RGBA format into scratch memory and repack into the caller's (format, type) layout on the CPU,
    // honoring the client-side PACK pixel-store parameters. The pure repacking helpers live in
    // ClientReadback (MG_Backend/ClientReadback.cpp) so unit tests can assert the exact packed words.

    using ClientReadback::GetReadbackChannelMapping;
    using ClientReadback::GetReadbackComponentSize;
    using ClientReadback::GetReadbackDstPixelSize;
    using ClientReadback::ReadbackChannelMapping;

    extern GLuint g_packedWordScratchTextureId;

    extern GLsizei g_packedWordScratchWidth;

    extern GLsizei g_packedWordScratchHeight;

    Bool ReadTextureLevelTight(GLuint texture, TextureTarget target, TextureUploadTarget uploadTarget,
                               TextureInternalFormat logicalFormat, GLint level, const IntVec3& logicalExtent,
                               Bool sourceUsesImageCarrier, GLenum format, GLenum type, Vector<Uint8>& bytes);

    Bool WireTextureReadbackRefusedForTesting(const IntVec3& extent);

    extern EGLDisplay g_Display;

    extern std::atomic<Uint64> g_displayTerminations;

    struct NativeContextTuple {
        Uint64 SessionKey = 0;
        Uint64 ContextToken = 0;
        Uint64 ShareGroupToken = 0;
        EGLContext Context = EGL_NO_CONTEXT;
        // Context's lifetime serial (CurrentNativeContextSerial); a new one per eglCreateContext.
        Uint64 Serial = 0;
        EGLConfig Config = nullptr;
        // The session surface this context draws to / reads from, as last made current for it.
        EGLSurface Draw = EGL_NO_SURFACE;
        EGLSurface Read = EGL_NO_SURFACE;
    };

    struct NativeSessionState {
        Uint64 SessionKey = 0;
        // Every native surface this session created, so its teardown destroys its own and only its own.
        // The window is recorded with each entry: a stale surface still bound to a window that a
        // LATER session wants (a crashed predecessor's server-owned window) is found and destroyed
        // by InitWindowSurface before the driver refuses the second eglCreateWindowSurface.
        struct SurfaceEntry {
            NativeWindowType Window;
            EGLSurface Surface;
            // Presents made on this native surface since it was created: a pbuffer's content is
            // the last presented frame once there has been one (its buffer age is then 1).
            Uint64 Presents = 0;
        };
        Vector<SurfaceEntry> Surfaces;
        // The surface the session currently draws to / reads from - what a bind to another context
        // re-makes current, and what Present/eglSwapBuffers names.
        EGLSurface Draw = EGL_NO_SURFACE;
        EGLSurface Read = EGL_NO_SURFACE;
    };

    extern std::mutex g_nativeRegistryMutex;

    extern std::atomic<Uint64> g_nativeRegistryEpoch;

    extern std::atomic<Uint64> g_surfaceSizeEpoch;
    static void NoteDrawSurfaceMoved() {
        g_surfaceSizeEpoch.fetch_add(1, std::memory_order_acq_rel);
    }

    Vector<NativeContextTuple*>& NativeContexts();

    Vector<NativeSessionState*>& NativeSessions();

    extern std::atomic<NativeContextKeyResolver> g_nativeContextKeyResolver;

    // A one-way override for a case that drives the registry with no session runtime installed:
    // when `t_native.forcedValid` is set, the key is the forced pair instead of the probe's
    // answer. The setter clears the active-tuple cache, which is keyed on the effective key.
    //
    // P14: ONE thread-local block for this file's per-thread native-context state rather than ten
    // separate thread_locals. The library is built with emulated TLS (minSdk 26), where every
    // thread_local access is a call; the per-draw "is my context current" check read seven of
    // them. One block is one lookup per function.
    struct NativeThreadState {
        Bool forcedValid = false;
        Uint64 forcedSession = 0;
        Uint64 forcedToken = 0;
        NativeContextTuple* activeTuple = nullptr;
        Uint64 activeEpoch = 0;
        Uint64 activeSession = 0;
        Uint64 activeToken = 0;
        EGLContext boundContext = EGL_NO_CONTEXT;
        EGLSurface boundDraw = EGL_NO_SURFACE;
        // QueryCurrentSurfaceSize's memo (P15): the last answer, good while the session, the
        // registry epoch and the surface-size epoch all hold.
        Bool sizeMemoValid = false;
        Uint64 sizeMemoSession = 0, sizeMemoToken = 0, sizeMemoRegistryEpoch = 0, sizeMemoSizeEpoch = 0;
        Int sizeMemoWidth = 0, sizeMemoHeight = 0;
    };

    extern thread_local NativeThreadState t_native;

    static void ResolveNativeKey(Uint64& outSession, Uint64& outToken) {
        if (t_native.forcedValid) {
            outSession = t_native.forcedSession;
            outToken = t_native.forcedToken;
            return;
        }
        outSession = 0;
        outToken = 0;
        const NativeContextKeyResolver resolver = g_nativeContextKeyResolver.load(std::memory_order_acquire);
        if (resolver != nullptr) (void)resolver(&outSession, &outToken);
    }

    static NativeSessionState& AcquireNativeSession(Uint64 sessionKey) {
        for (auto* session : NativeSessions()) {
            if (session->SessionKey == sessionKey) return *session;
        }
        auto* created = new NativeSessionState{};
        created->SessionKey = sessionKey;
        NativeSessions().push_back(created);
        g_nativeRegistryEpoch.fetch_add(1, std::memory_order_acq_rel);
        return *created;
    }

    static NativeSessionState& ActiveNativeSession() {
        Uint64 session = 0;
        Uint64 token = 0;
        ResolveNativeKey(session, token);
        const std::lock_guard<std::mutex> lock(g_nativeRegistryMutex);
        return AcquireNativeSession(session);
    }

    NativeContextTuple& ActiveNativeContext();

    Bool QueryCurrentSurfaceSize(Int& outWidth, Int& outHeight);

    EGLSurface CurrentContextDrawSurface();

    extern thread_local EGLSurface t_ageAskedSurface;

    extern thread_local Bool t_windowBufferFlushIssued;

    namespace {
        // The calling session's record of `surface`; the registry lock is held by the caller (another
        // session's window bring-up may erase stale entries of this one).
        NativeSessionState::SurfaceEntry* FindSessionSurfaceLocked(NativeSessionState& session, EGLSurface surface) {
            for (auto& entry : session.Surfaces) {
                if (entry.Surface == surface) return &entry;
            }
            return nullptr;
        }
    } // namespace

    void FlushAheadOfWindowBufferUse();

    extern std::atomic<std::thread::id> g_backendContextOwnerThread;

    extern Uint g_syncContextGeneration;

    extern std::atomic<Uint64> g_currentFrameSerial;

    extern std::atomic<Uint64> g_completedFrameSerial;
    // Deep enough for several sessions presenting in turn: a slot is only reused once its
    // fence has signaled (InsertFrameFence waits for it rather than dropping it).
    constexpr int kFrameFenceRingDepth = 32;
    struct FrameFence {
        GLsync sync = nullptr;
        Uint contextGeneration = 0;
        Uint64 serial = 0;
    };

    extern FrameFence g_frameFenceRing[kFrameFenceRingDepth];

    extern std::atomic<Uint64> g_eglVerifiedFrameSerial;

    extern std::atomic<Uint> g_eglVerifiedContextGeneration;

    Bool IsBackendContextCurrentOnThisThread();

    GLenum ClientWaitSync(BackendSyncHandle handle, GLbitfield flags, GLuint64 timeout);

    void WaitSync(BackendSyncHandle handle, GLbitfield flags, GLuint64 timeout);

    Uint64 CurrentFrameSerial();

#if MOBILEGL_BUILD_DISAGGREGATED
    Bool LatchIfDeviceLost(const char* site, Bool debugKnob);
#endif

#if MOBILEGL_BUILD_DISAGGREGATED
    namespace SharedImageImpl {
#if defined(__ANDROID__)
        using MG_Util::BackendLoader::HasExtensionWord;
#endif
    } // namespace SharedImageImpl
#endif
} // namespace MobileGL::MG_Backend::DirectGLES
