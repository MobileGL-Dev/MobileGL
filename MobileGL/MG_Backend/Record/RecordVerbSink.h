// MobileGL - MobileGL/MG_Backend/Record/RecordVerbSink.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P13 W5: THE RECORD VERBS' CONSUMER. The verbs no MGPipeApply* entry point takes (clear, blit, the
// readbacks, the draws, dispatch, copies, image binds, XFB, mipmaps) end in the backend's own call
// through the private GlobalBackendFunctionsTable this sink holds. Under a transport the server's
// ServerVerbSink (MG_Remote/Server/PipeApplier.h) is this plus the session's verbs; the record
// arm's in-process port drives this class directly, which is what lets a library without a
// transport run the record arm.

#pragma once

#include <Includes.h>
#include <MG_Backend/BackendObject.h>
#include <MG_Backend/BackendObjects.h>
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/PipeVerbSink.h>

namespace MobileGL::MG_Record {

    class RecordVerbSink : public MG_Pipe::MGPipeVerbSink {
    public:
        // The server's private backend. Null until ServerLoop::CreateBackend has run, and a
        // verb that arrives before then declines by name rather than dereferencing.
        void SetBackend(MG_Backend::BackendObject* backend);
        MG_Backend::BackendObject* Backend() const { return m_backend; }
        void SetMaxReplyBytes(Uint64 bytes) { m_maxReplyBytes = bytes; }

        Bool OnClear(const MG_Pipe::MGPClear& clear) override;
        Bool OnBlit(const MG_Pipe::MGPBlit& blit) override;
        Bool OnReadPixels(const MG_Pipe::MGPReadbackInfo& info, Uint64 seq,
                          MG_Pipe::MGPipeReplySink* replies) override;
        Bool OnGetTextureImage(const MG_Pipe::MGPReadbackInfo& info, Uint64 seq,
                               MG_Pipe::MGPipeReplySink* replies) override;
        // P9 (CONTRACT-P9.md §1): the pack-buffer halves. Nothing is answered; the tight pixels
        // land in record.Dst through the applier's buffer-write gate.
        Bool OnReadPixelsToBuffer(const MG_Pipe::MGPReadbackToBuffer& record) override;
        Bool OnGetTextureImageToBuffer(const MG_Pipe::MGPReadbackToBuffer& record) override;
        Bool OnDrawVbo(const MG_Pipe::MGPDrawInfo& info, const MG_Pipe::MGPDrawRange* ranges,
                       const MG_Pipe::MGHostSpan* userIndices,
                       const MG_Pipe::MGPDrawIndirect* indirect) override;

        // ---- P5b (MG_Remote/CONTRACT-P5B.md): one override per row a migration package owns.
        // At the contract commit EVERY BODY BELOW IS A STUB that dies
        // Fatal{UnmigratedVerb, "<GL slot>"} by the slot's own name - the same line the client's
        // class-C table raises and the census greps - so a client flipped ahead of its server
        // half aborts by name rather than rendering nothing, and the census on this head is
        // unchanged (the client refuses first). The owning package replaces the body.
        //
        //   i1  OnLaunchGrid ("DispatchCompute"), OnMemoryBarrier, OnResourceCopyRegion
        //       ("CopyImageSubData"), OnBindShaderImage ("BindImageTexture"),
        //       OnSetStorageBlockBinding ("ShaderStorageBlockBinding")
        //   t2  LANDED. OnBeginStreamOutput / OnEndStreamOutput / OnPauseStreamOutput /
        //       OnResumeStreamOutput / OnBindStreamOutput / OnPatchParameter are real bodies
        //       now: the backend call the contract names, the null-slot DECLINE, a tally.
        //   f1  OnGenerateMipmap, OnCopyFramebufferToTexture ("CopyTexImage2D" /
        //       "CopyTexSubImage2D"), and OnClear's four non-Whole kinds (live already)
        //   d1  OnDrawVbo above: the indirect tail, the user-index span, NumDraws > 1 and the
        //       instanced arms - LIVE since d1 v1 (every shape the client's nineteen draw slots
        //       produce dispatches to the backend slot CONTRACT-P5B.md §2 d1 names)
        Bool OnLaunchGrid(const MG_Pipe::MGPGridInfo& grid) override;
        Bool OnMemoryBarrier(const MG_Pipe::MGPMemoryBarrier& barrier) override;
        Bool OnResourceCopyRegion(const MG_Pipe::MGPCopyRegion& copy) override;
        Bool OnBindShaderImage(const MG_Pipe::MGPImageBind& bind) override;
        Bool OnSetStorageBlockBinding(const MG_Pipe::MGPStorageBlockBinding& binding,
                                      const char* name) override;
        Bool OnBeginStreamOutput(const MG_Pipe::MGPStreamOutputBegin& begin) override;
        Bool OnEndStreamOutput(const MG_Pipe::MGPXfbAccounting& accounting) override;
        Bool OnPauseStreamOutput(const MG_Pipe::MGPStreamOutputControl& control) override;
        Bool OnResumeStreamOutput(const MG_Pipe::MGPStreamOutputControl& control) override;
        Bool OnBindStreamOutput(const MG_Pipe::MGPStreamOutputBind& bind) override;
        Bool OnDeleteStreamOutput(const MG_Pipe::MGPStreamOutputBind& object) override;
        Bool OnPatchParameter(const MG_Pipe::MGPPatchParameter& patch) override;
        Bool OnGenerateMipmap(const MG_Pipe::MGPMipPlan& plan) override;
        Bool OnCopyFramebufferToTexture(const MG_Pipe::MGPCopyFromFramebuffer& copy) override;

        // Per-verb tallies. The lane asserts these moved, because "the scenario passed" on a
        // split build is also what a scenario that ran entirely on the monolith path looks
        // like (R-16: a probe may not arm against a stub).
        Uint64 Clears() const { return m_clears; }
        Uint64 Draws() const { return m_draws; }
        Uint64 Readbacks() const { return m_readbacks; }
        Uint64 Blits() const { return m_blits; }
        Uint64 ReadbackBytes() const { return m_readbackBytes; }
        // P9: pack-buffer readbacks that landed in their buffer (both ops), and the buffer-write
        // records they took. Readbacks() above counts only the reply-slot form.
        Uint64 BufferReadbacks() const { return m_bufferReadbacks; }
        Uint64 BufferReadbackWrites() const { return m_bufferReadbackWrites; }
        // ---- P5b package i1's tallies. R-16: a probe may not arm against a stub, and on a
        // split build "the scenario passed" is also what a scenario that never left the
        // monolith path looks like - so the lane asserts the number that only this sink can
        // move. One per wire ROW, not per GL slot: launch_grid carries both dispatch entry
        // points and memory_barrier both barrier ones, and the sink is where they separate.
        Uint64 ImageBinds() const { return m_imageBinds; }
        Uint64 Dispatches() const { return m_dispatches; }
        Uint64 MemoryBarriers() const { return m_memoryBarriers; }
        Uint64 ImageCopies() const { return m_imageCopies; }
        Uint64 StorageBlockBindings() const { return m_storageBlockBindings; }
        // ID-49's tight-size control reads this: the scratch a read_pixels grew to. It must equal
        // the tight w*h*bpp extent of the read, never the client's DstSize - a scratch sized from
        // DstSize is exactly the heap overflow codex 1 found, one field over.
        Uint64 ReadbackScratchBytes() const { return static_cast<Uint64>(m_readbackScratch.size()); }

        // ---- P5b d1: what the LAST draw_vbo record carried, as the sink saw it ---------------
        //
        // Recorded BEFORE the backend is consulted, so a process with no backend object (every
        // unit case) can still assert the wire's fields rather than only that a draw "was
        // declined": the record's head, its first range, its indirect block, and whether a
        // user-index span rode with it and how many bytes it named. Nothing here outlives the
        // call except these copies (rule C: the pointers the sink was handed are not kept).
        struct LastDrawRecord {
            MG_Pipe::MGPDrawInfo Info{};
            MG_Pipe::MGPDrawRange FirstRange{};
            MG_Pipe::MGPDrawIndirect Indirect{};
            Uint64 UserIndexBytes = 0;
            Bool HadUserIndices = false;
            Bool HadIndirect = false;
        };
        const LastDrawRecord& LastDraw() const { return m_lastDraw; }
        // draw_vbo records seen, applied or declined; Draws() above counts only the applied.
        Uint64 DrawRecords() const { return m_drawRecords; }
        // P5b t2's tallies, for the same reason the five above exist (R-16): under split "the
        // scenario passed" is also what a scenario that ran entirely on the monolith path looks
        // like, so a lane that wants to say the XFB spans CROSSED has to read a counter the
        // server moved. Spans counts Begin and End together - they are one span and a lane that
        // saw only one of them has a bug the two-counter version would have hidden behind a
        // sum; controls counts Pause and Resume; binds and patch parameters count their own.
        Uint64 StreamOutputSpans() const { return m_streamOutputSpans; }
        Uint64 StreamOutputControls() const { return m_streamOutputControls; }
        Uint64 StreamOutputBinds() const { return m_streamOutputBinds; }
        Uint64 PatchParameters() const { return m_patchParameters; }

    protected:
        // A backend change: the subclass lets go of what it held on the old one.
        virtual void OnBackendChanged() {}
        // The wire's user-index span (a draw whose indices rode in a stage segment). The record
        // arm's port never hands one over - its indices are in the client's own buffer - so only
        // the wire's subclass, which can check a span against its segments, answers true.
        virtual Bool CheckUserIndices(const MG_Pipe::MGPDrawInfo&, const MG_Pipe::MGPDrawRange*,
                                      const MG_Pipe::MGHostSpan&) {
            return false;
        }

        const MG_Backend::GlobalBackendFunctionsTable* Table(const char* verb) const;

        // read_pixels' read, shared by the reply form and the pack-buffer form: the bound read
        // framebuffer, NEUTRAL pack state (ID-49), `tight` bytes into m_readbackScratch. False
        // when this server has no GL.ReadPixels - the caller owns what that means.
        Bool ReadBoundFramebufferTight(const MG_Pipe::MGPReadbackInfo& info, Uint64 tight);
        // get_texture_image's read, shared the same way: the whole level into `bytes`.
        Bool ReadTextureImageTight(const MG_Pipe::MGPReadbackInfo& image, Vector<Uint8>& bytes);
        // P9: `tight` holds Src's pixels, tightly packed; land them in record.Dst at the
        // record's layout through MGPipeApplyResourceSubData, one write per contiguous run. The
        // layout was checked before the read (PipeApplier.cpp ReadbackToBufferLayoutFault).
        Bool LandReadbackInBuffer(const MG_Pipe::MGPReadbackToBuffer& record, Uint8* tight,
                                  Uint64 bytesPerPixel);

        MG_Backend::BackendObject* m_backend = nullptr;
        Uint64 m_clears = 0;
        Uint64 m_draws = 0;
        Uint64 m_readbacks = 0;
        Uint64 m_blits = 0;
        Uint64 m_readbackBytes = 0;
        Uint64 m_bufferReadbacks = 0;
        Uint64 m_bufferReadbackWrites = 0;
        Uint64 m_imageBinds = 0;
        Uint64 m_dispatches = 0;
        Uint64 m_memoryBarriers = 0;
        Uint64 m_imageCopies = 0;
        Uint64 m_storageBlockBindings = 0;
        Uint64 m_streamOutputSpans = 0;
        Uint64 m_streamOutputControls = 0;
        Uint64 m_streamOutputBinds = 0;
        Uint64 m_patchParameters = 0;
        // ReadPixels' destination. The pixels go into the reply slot, but GLFunctionsTable::
        // ReadPixels writes into a caller buffer, so one staging vector per session sits
        // between them. Grown, never shrunk, and never handed out past the call.
        Vector<Uint8> m_readbackScratch;
        // Server-declared LinkTerms.maxReplyBytes, copied from the attached server link.
        Uint64 m_maxReplyBytes = 0;
        // P5b d1: the multi-draw arrays the glMultiDraw* slots take, rebuilt from the ranges
        // per record (rule C: bounded by NumDraws, owned here, never handed out past the call),
        // and the last-record witness above.
        Vector<GLsizei> m_multiCounts;
        Vector<GLint> m_multiFirsts;
        Vector<const void*> m_multiOffsets;
        Vector<GLint> m_multiBaseVertices;
        LastDrawRecord m_lastDraw{};
        Uint64 m_drawRecords = 0;
    };

} // namespace MobileGL::MG_Record
