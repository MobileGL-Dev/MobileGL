// MobileGL - MobileGL/MG_Remote/Protocol/mg_protocol_base.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Shared vocabulary of the MG_Remote wire contracts (transport, framing, ring,
// shm). Inherited from the earlier `Feat/CS-Delta-IPC` branch
// (MobileGL/Protocol/mg_protocol_base.h) and cut down to what plan B's
// transport actually needs: result codes, byte spans, a shm region reference
// and the id typedefs.
//
// Deliberately NOT inherited: MobileGLObjectKind / MobileGLObjectScope /
// MobileGLObjectHandle. Plan B does not put GL object identity on the wire at
// all - the frontend allocates {slot, generation} handles in MG_Pipe
// (PLAN-B.md section 4.2.1) and those are the only identity the backend ever
// sees, so a second object-identity vocabulary here would be a drift surface
// with no reader.
//
// This header must stay:
//   - pure C (compilable from C and C++, no MG C++ types, no exceptions/RTTI),
//   - dependency-free (only <stdbool.h>/<stddef.h>/<stdint.h>),
//   - append-only within an ABI major (see versioning rules below).
//
// Versioning rules (contract-wide):
//   - Every versioned struct starts with uint32_t structSize.
//   - Appending fields at the tail is a MINOR bump; receivers must ignore
//     bytes beyond the structSize they know.
//   - Changing/removing/reordering existing fields is a MAJOR bump.
//   - A major mismatch is a hard, structured failure, never an exception.
// (Plan B keeps the structSize-first discipline as the answer to risk B-R10,
// PLAN-B.md section 14.2.)

#ifndef MOBILEGL_REMOTE_PROTOCOL_BASE_H
#define MOBILEGL_REMOTE_PROTOCOL_BASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// ABI versions
// ---------------------------------------------------------------------------

#define MOBILEGL_PROTOCOL_ABI_MAJOR 1
#define MOBILEGL_PROTOCOL_ABI_MINOR 0

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
