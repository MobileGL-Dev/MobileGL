// MobileGL - MobileGL/MG_Remote/Protocol/mg_protocol_base.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Shared vocabulary of the MG_Remote wire contracts (transport, framing, ring, shm): result
// codes, byte spans, a shm region reference and id typedefs. No GL object identity lives here;
// the wire's only object identity is MG_Pipe's {slot, gen} handle (ARCHITECTURE.md §2.1).
// Keep this header pure C, dependent only on <stdbool.h>/<stddef.h>/<stdint.h>, and
// append-only within an ABI major. Every versioned struct starts with uint32_t structSize.
// Appending fields is a minor bump and receivers ignore bytes past the structSize they know;
// changing, removing or reordering fields is a major bump, and a major mismatch is a hard,
// structured failure.

#ifndef MOBILEGL_REMOTE_PROTOCOL_BASE_H
#define MOBILEGL_REMOTE_PROTOCOL_BASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // ---------------------------------------------------------------------------
    // ABI versions
    // ---------------------------------------------------------------------------

#define MOBILEGL_PROTOCOL_ABI_MAJOR 1
#define MOBILEGL_PROTOCOL_ABI_MINOR 0

// The control schema's revision (protocol.fbs), mixed into wireFingerprint; the fingerprint's
// other inputs cover only the data plane. Bump it with every protocol.fbs change a peer must
// agree on, or old and new peers handshake and then misread a field.
// scripts/ci/protocol_revision_pin.py pins each revision to sha256(protocol.fbs)
// (protocol_revision_pins.json) and fails a schema whose digest is not the current revision's;
// it runs as the ProtocolSchema.RevisionPinsDigest test and in CI's flatc-check job. After a
// bump, record the new row with `python3 scripts/ci/protocol_revision_pin.py --write`.
#define MOBILEGL_PROTOCOL_CONTROL_REVISION 6

#define MOBILEGL_ABI_VERSION(major, minor) (((uint32_t)(major) << 16) | (uint32_t)(minor))
#define MOBILEGL_ABI_MAJOR_OF(version) ((uint32_t)(version) >> 16)
#define MOBILEGL_ABI_MINOR_OF(version) ((uint32_t)(version) & 0xFFFFu)

    // ---------------------------------------------------------------------------
    // Ids
    // ---------------------------------------------------------------------------

    typedef uint64_t MobileGLSessionId;  // one client GL context flow
    typedef uint64_t MobileGLRequestSeq; // matches a request to its reply
    typedef uint32_t MobileGLSegmentId;  // shm segment id within a connection

    // ---------------------------------------------------------------------------
    // Spans / regions
    // ---------------------------------------------------------------------------

    // Borrowed, read-only byte span. The pointee is owned by the producing side
    // and is only valid for the duration documented at the consuming call site.
    typedef struct MobileGLByteSpan {
        const void* data;
        uint64_t size;
    } MobileGLByteSpan;

    typedef struct MobileGLMutableByteSpan {
        void* data;
        uint64_t size;
    } MobileGLMutableByteSpan;

    // A byte range inside an already-established shm segment. Segments are
    // announced out of band (the SegmentRef table on the control channel, with the
    // fd itself passed by SCM_RIGHTS) and stay stable for their declared lifetime;
    // offsets are segment-relative.
    typedef struct MobileGLShmRegion {
        MobileGLSegmentId segmentId;
        uint32_t reserved;
        uint64_t offset;
        uint64_t size;
    } MobileGLShmRegion;

    // ---------------------------------------------------------------------------
    // Result codes (structured errors across every contract boundary)
    // ---------------------------------------------------------------------------

    typedef enum MobileGLResult {
        MOBILEGL_OK = 0,
        MOBILEGL_ERR_NOT_INITIALIZED = 1,
        MOBILEGL_ERR_INVALID_ARGUMENT = 2,
        MOBILEGL_ERR_UNSUPPORTED = 3,
        MOBILEGL_ERR_OUT_OF_MEMORY = 4,
        MOBILEGL_ERR_PROTOCOL_MISMATCH = 5, // ABI/wire major mismatch, bad framing
        MOBILEGL_ERR_TRANSPORT_CLOSED = 6,  // peer gone / EOF
        MOBILEGL_ERR_TIMEOUT = 7,           // nothing arrived within the deadline
        MOBILEGL_ERR_SHM_EXHAUSTED = 8,
        MOBILEGL_ERR_SESSION_UNKNOWN = 9,
        MOBILEGL_ERR_HANDLE_UNKNOWN = 10,
        // The caller's buffer is smaller than the pending message. The message is
        // NOT consumed and the required size is reported back; see
        // ITransport::ReceiveFrame.
        MOBILEGL_ERR_BUFFER_TOO_SMALL = 11,
        MOBILEGL_ERR_FORCE_U32 = 0x7FFFFFFF
    } MobileGLResult;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // MOBILEGL_REMOTE_PROTOCOL_BASE_H
