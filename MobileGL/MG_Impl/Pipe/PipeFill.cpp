// MobileGL - MobileGL/MG_Impl/Pipe/PipeFill.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The client side of the PipeInputs block (ARCHITECTURE.md 9.2 phase A): the only place in
// the push arm that reads MG_State::pGLContext. Holds the per-verb filler, the F-class
// forwarders and IsLive. Compiled only under MOBILEGL_PIPE_PUSH (CMakeLists.txt appends it
// to SOURCE_FILES there).
//
// Contract commit (P1 c1): the filler bumps the verb serial, records the verb and the
// context identity, and stamps the seven sticky fields once; the per-class field copies and
// stamps land in c2, the verify snapshot and comparator in c4.
#include <MG_State/GLState/Core.h>
#include <MG_Backend/MGPipe/PipeInputs.h>
#include <Config.h>

namespace MobileGL::MG_Pipe {
    namespace {
        MG_State::GLState::GLContext* LiveContext() { return MG_State::pGLContext.get(); }

        template <class T>
        const SharedPtr<T>& NullShared() {
            static const SharedPtr<T> null;
            return null;
        }
    } // namespace

    // ---- liveness ----
    Bool PipeInputs::IsLive() const { return LiveContext() != nullptr; }

    // ---- the seven F-class forwarders ----
    SizeT PipeInputs::GetBufferBindingPointCount(BufferTarget target) const {
        const auto* ctx = LiveContext();
        return ctx != nullptr ? ctx->GetBufferBindingPointCount(target) : 0;
    }

    const SharedPtr<PipeInputs::ProgramObject>& PipeInputs::GetProgramObject(Uint index) {
        auto* ctx = LiveContext();
        return ctx != nullptr ? ctx->GetProgramObject(index) : NullShared<ProgramObject>();
    }

    const SharedPtr<PipeInputs::ITextureObject>& PipeInputs::GetTextureObject(Uint index) {
        auto* ctx = LiveContext();
        return ctx != nullptr ? ctx->GetTextureObject(index) : NullShared<ITextureObject>();
    }

    Bool PipeInputs::HasOpenTransformFeedbackSpan(Uint64 lifetimeId) const {
        const auto* ctx = LiveContext();
        return ctx != nullptr && ctx->HasOpenTransformFeedbackSpan(lifetimeId);
    }

    void PipeInputs::InvalidateCompileEnv() {
        if (auto* ctx = LiveContext()) ctx->InvalidateCompileEnv();
    }

    Bool PipeInputs::ValidateProgramName(Uint index) const {
        const auto* ctx = LiveContext();
        return ctx != nullptr && ctx->ValidateProgramName(index);
    }

    void PipeInputs::RecordError(ErrorCode code, UniquePtr<ErrorInfo> info) {
        auto* ctx = LiveContext();
        if (ctx == nullptr) {
            MGLOG_E_ONCE("PipeInputs::RecordError: no live context, dropping error %d", static_cast<int>(code));
            return;
        }
        ctx->RecordError(code, Move(info));
    }

    // ---- the filler ----
    void MGPipeFillForVerb(MGPipeVerb verb) {
        PipeInputs& inputs = gPipeInputs;
#if MOBILEGL_PIPE_POISON
        // Starts at 1, so FilledGen == 0 means "never filled".
        ++inputs.m_filled.CurrentVerbSerial;
#endif
        inputs.m_currentVerb = verb;
        auto* ctx = LiveContext();
        if (ctx == nullptr) {
            inputs.m_live = false;
            inputs.m_contextIdentity = nullptr;
            return;
        }
        inputs.m_live = true;
        inputs.m_contextIdentity = ctx;
#if MOBILEGL_PIPE_POISON
        // The sticky (forwarded) fields are stamped once by the first fill that sees a live
        // context and stay fresh through the Sticky -> FilledGen != 0 branch of
        // MGPipeInputFieldIsFresh.
        for (SizeT i = 0; i < kMGPipeInputFieldCount; ++i) {
            if (kMGPipeInputFieldSticky[i] && inputs.m_filled.FilledGen[i] == 0) inputs.m_filled.FilledGen[i] = 1;
        }
#endif
        // c2: copy and stamp every field in kMGPipeClassFieldMask[kMGPipeVerbClass[verb]].
    }
} // namespace MobileGL::MG_Pipe
