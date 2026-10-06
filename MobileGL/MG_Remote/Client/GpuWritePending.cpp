// MobileGL - MobileGL/MG_Remote/Client/GpuWritePending.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GpuWritePending.h"
#include <MG_Remote/FatalFunnel.h>

#include "ClientSession.h"
// RunsAsTheServerRole: declared beside the wire emitters that need the same predicate, so this
// TU does not pull a server header in for it.
#include "WireTables.h"

#include <MG_Pipe/PipeMutation.h>
#include <MG_State/GLState/BufferState/BufferObject.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/TextureState/TextureObjectBuffer.h>
#include <MG_State/GLState/TextureState/TextureState.h>
#include <MG_Util/Debug/Log.h>

#include <cstdlib>

namespace MobileGL::MG_Remote::Client {

    using MG_State::GLState::BufferObject;

    SizeT BufferWritebackSliceBytes() {
        ClientSession* session = ClientSession::Active();
        if (session == nullptr) return 0;
        // A quarter of the ring, not the MaxRecordBytes half: the record carries its own
        // header and the 24-byte EventBufferWritebackHead beside the payload, and the ring
        // may still hold a few small events (gpu-written, gl-error) posted earlier in the
        // same verb's apply. Each slice round-trips with its own barrier + drain, so the
        // ring never holds more than one slice's bytes.
        const Uint64 slice = session->EventRingCapacityBytes() / 4;
        // A floor so a pathologically small operator-supplied ring cannot make the slicing
        // loop in SyncGpuWrites spin at zero width; such a ring is broken anyway, and the
        // producer's Fatal{EventRingOverflow} names it on the first post.
        return static_cast<SizeT>(slice < 4096 ? 4096 : slice);
    }

    SizeT MGPipeStageChunkBytesFor(Uint64 stageCapacityBytes) {
        if (stageCapacityBytes == 0) return 0;
        // A quarter of the arena, not all of it, for the readback slice's reason one ring over
        // (above): the pieces of one range are staged one after another, an allocation is only
        // reclaimable once the record carrying it has retired, and the allocator skips a
        // remainder it cannot fill contiguously. A quarter leaves room for the pieces still in
        // flight, so an ordinary whole-buffer upload never waits on a retirement it could have
        // avoided.
        const Uint64 quarter = stageCapacityBytes / 4;
        // A floor so a pathologically small operator-supplied segment cannot make the piece
        // width zero, which the walk reads as "refuse" (ResourceTracker.h:254); such a segment
        // is broken anyway, and the producer's Fatal{RingOverrun} names it on the first stage.
        const Uint64 chunk = quarter < 4096 ? 4096 : quarter;
        return static_cast<SizeT>(chunk > stageCapacityBytes ? stageCapacityBytes : chunk);
    }

    SizeT MGPipeStageChunkBytes() {
        // The server role's own uploads run the monolith adapter (RunsAsTheServerRole's comment
        // in WireTables.h): cutting them would change how many calls the server's own backend
        // sees for one application call, which is a monolith behaviour change on the apply
        // thread and not this emitter's to make.
        if (MG_Config::Transport == MG_Config::TransportMode::Monolith) return 0;
        if (RunsAsTheServerRole()) return 0;
        ClientSession* session = ClientSession::Active();
        // No session: the emission WAS the application (every unit gate, and a monolith-shaped
        // lane in a split build), so there is no arena to fit and the record's own bound stands.
        if (session == nullptr) return 0;
        return MGPipeStageChunkBytesFor(session->StageCapacityBytes());
    }

    void AwaitBufferWriteback(BufferObject& buffer) {
        // THE WAIT IS THE BARRIER'S WAIT (R-3). The reply-slot id IS the record's seq, so
        // "appliedSeq reached my readback" and "my answer is back" are one condition, and
        // ClientSession::EmitAndWait is what pays for it. With no session - a build-split lane
        // running monolith, and every unit case - the emission WAS the application,
        // synchronously, so the writeback has already landed and there is nothing to wait for.
        // Spelling that as "return" rather than as a loop is deliberate: a loop here would be
        // a hang in exactly that configuration, which is the configuration every gate lane
        // runs.
        if (ClientSession::Active() == nullptr) return;

        // AND THE OTHER ARM IS A NAMED FATAL, NOT AN EMPTY BODY. A session exists, so the
        // apply side is no longer synchronous, and if the flag is still set the shadow this
        // caller is about to read is STALE - which is the whole failure the third state was
        // introduced to stop. An empty body here would make that failure silent and would let
        // s1/c1 land a session without noticing that nobody ever wrote the wait; a stub that
        // aborts by name is the house shape for exactly this (EmitTables.cpp's
        // UnmigratedVerbFatal), and it is what gives the hole a red spelling before the
        // transport arrives.
        if (!buffer.HasOutstandingGpuWrite()) return;
        // P9 F1 (CONTRACT-P9.md §4): THE DEVICE WENT AWAY WITH THE WRITE STILL OWED. A lost device
        // DECLINES every record, so the readback that was to bring the GPU's bytes back was never
        // sent - and the shadow the caller is about to read holds whatever the buffer held BEFORE
        // the write (for a pack-buffer read, the bytes before the pixels). Returning it would be the
        // one outcome the reverse channel exists to prevent: stale bytes that look like a
        // successful read. It is the reply form's ReadbackDeclined, arriving one call later.
        if (ClientSession::DeviceLost()) {
            SessionFail(MGFatalFamily::ReadbackDeclined, "MGPipe: Fatal{ReadbackDeclined, \"buffer-writeback\"} - buffer %u "
                    "still owes a GPU write (a pack-buffer read or a shader store) and the device is "
                    "lost, so its readback was declined; its shadow holds the bytes from before the "
                    "write and is not returned as the GPU's",
                    buffer.GetExternalIndex());
        }
        SessionFail(MGFatalFamily::UnimplementedWritebackWait, "MGPipe: Fatal{UnimplementedWritebackWait} - a ClientSession is active and buffer %u "
                "still has an outstanding GPU write after its readback was emitted. The wait is "
                "ClientSession::EmitAndWait's (R-3: the reply slot id IS the record seq); P5 package "
                "b1 landed the third state and s1/c1 own the wait itself.",
                buffer.GetExternalIndex());
    }

} // namespace MobileGL::MG_Remote::Client
