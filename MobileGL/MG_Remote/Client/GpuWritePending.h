// MobileGL - MobileGL/MG_Remote/Client/GpuWritePending.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P13 W5: the session's half of the client GPU-write set - the writeback wait and the two
// clamps that depend on a live session's rings. The set itself (the producers, the walks,
// the row predicates) is MG_Record's (MG_Impl/Pipe/Verb/GpuWriteSet.h): the record arm
// keeps it in a library without a transport too.

#pragma once
#include <Includes.h>

#include <Config.h>
#include <MG_Impl/Pipe/Verb/GpuWriteSet.h>

namespace MobileGL::MG_State::GLState {
    class BufferObject;
    struct ImageTextureBinding;
} // namespace MobileGL::MG_State::GLState

namespace MobileGL::MG_Remote::Client {


    // ---- SyncGpuWrites' third state (CONTRACT-P5.md section 3) -----------------------

    // Blocks until this buffer's OnBufferWriteback has landed. With no session - a build-split
    // lane running monolith, and every unit case - the emission was synchronous and the answer
    // is already in, so this returns at once; that is why it is a call rather than a loop the
    // caller writes, because the loop would be a hang in exactly that configuration.
    void AwaitBufferWriteback(MG_State::GLState::BufferObject& buffer);


    // The slice a whole-buffer readback is cut into so one writeback event always fits
    // SEG_EVENT. The writeback's bytes travel INLINE in the event record (P5c ev, CONTRACT-P5C
    // §4.2), one record must fit the ring (RingProducer::MaxRecordBytes == capacity/2,
    // Ring.h:288), and a 24 MiB arena's single shot cannot (measured: Fatal{EventRingOverflow}
    // on LargeArenaAdoptionScenario.GpuWriteIntoTheArenaIsReadBack). 0 when no session is
    // active - the synchronous arm never slices.
    SizeT BufferWritebackSliceBytes();

    // The same question for CONTENT: how many bytes one resource_subdata record may stage
    // before the range has to be cut. SEG_STAGE is a linear arena, one record's blob is
    // allocated from it whole, and a blob larger than the arena is
    // Fatal{RingOverrun, "SEG_STAGE"} at the encoder rather than a split
    // (PipeWireCodec.cpp:856-864) - measured on the CI traces, where a 128 MiB arena's
    // whole-buffer follow-up against a 32 MiB segment aborted. 0 means "do not cut", i.e. the
    // record's own bound (MGPipeForEachSubDataRecordRange's default), which is the answer for
    // monolith, for the server role's own uploads (they run the monolith adapter) and for a
    // process with no session - every unit gate.
    SizeT MGPipeStageChunkBytes();

    // The clamp itself, a pure function of the segment's size so that a unit gate can drive the
    // splitter at exactly the value a live session produces. A quarter of the segment, never
    // below 4096 (a zero-width piece is a refusal, ResourceTracker.h:254) and never above the
    // segment (a larger cap would stage the very blob the arena refuses).
    SizeT MGPipeStageChunkBytesFor(Uint64 stageCapacityBytes);

} // namespace MobileGL::MG_Remote::Client
