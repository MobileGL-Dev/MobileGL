// t4_gbm_peer -- T4's outside-Android end: an image allocated in the container.
//
// T4 asks whether the render server can draw into an image that was allocated
// OUTSIDE Android -- the {Owner=Platform, Storage=OfferedImage} cell an
// Anland-style Wayland host needs.  Only one side of that question can be asked
// from the probe's own process: the probe runs in the Android app domain and
// has no GBM device, while the Anland-backed GBM device (/dev/dri/renderD128,
// msm_drm) belongs to the Droidspaces container.  So the container side
// allocates, fills, and later re-reads, and the probe imports.  That is why the
// probe LISTENS on an abstract unix socket and this program dials it -- the
// reverse of every other leg's topology.
//
// Not part of the Android build, not in any CMake target, no library beyond
// libc and libgbm.  Build it INSIDE the container, because that is where the
// GBM device is:
//
//   gcc -O2 -o t4_gbm_peer t4_gbm_peer.c -lgbm
//
// The wire format is mirrored byte for byte from the T4 section of
// extmem_probe.cpp.  Nothing here is defensive about the layout: a peer that
// invented its own struct would prove nothing about the probe's import.
//
// Two properties of this container shape the code:
//   * gbm_bo_unmap() segfaults for every WRITE mapping here (distro libgbm /
//     dri_gbm 26.1.5 mixed with an unowned self-built libgallium-26.2.0-devel),
//     while READ mappings unmap cleanly.  The fill mapping is therefore left
//     mapped on purpose: this process is about to exit, and a leaked mapping
//     cannot be mistaken for a driver answer the way a SIGSEGV could.
//   * Every check takes its OWN fresh read mapping, because a mapping taken
//     before the probe's GL write would answer a question about this CPU's
//     cache lines rather than about the buffer the probe drew into.
//
// T4 has two rows and this peer serves both, in whichever order the probe asks:
//   * T4-image-import -- the image is allocated HERE (GBM) and imported there.
//     The probe asks that row only when its driver advertises
//     EGL_EXT_image_dma_buf_import; when it does not, the row is recorded as a
//     capability answer and this side never sees MSG_T4_READ or MSG_T4_WROTE at
//     all.  No frame may therefore be waited for by name, which is why the
//     receive loop at the end dispatches on the tag instead.
//   * T4-ahb-image -- the image is allocated on the ANDROID side and imported
//     HERE, because an app cannot adopt this side's fd as an AHardwareBuffer
//     (AHardwareBuffer_createFromHandle is a SystemApi, not part of the NDK):
//     the buffer has to be born on the side that can name it, while this side
//     only needs an fd it can gbm_bo_import.

#include <gbm.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

// DRM_FORMAT_MOD_INVALID lives in drm_fourcc.h, which is not part of libgbm's
// interface.  This build must not need libdrm's headers for one constant; the
// value below is the kernel UAPI value.
#define T4_DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL

// DRM_FORMAT_MOD_LINEAR, same source, same reason.  The Android-minted row is
// imported with GBM_BO_IMPORT_FD_MODIFIER because that is the import form that
// names a modifier at all, and a buffer an app allocated with CPU read and write
// access is the linear one.
#define T4_DRM_FORMAT_MOD_LINEAR 0ULL

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------

// Every line is flushed as it is written: a crash later in the run (unmap of a
// WRITE mapping is one on this stack) must not be able to take the raw
// observations with it.
static void pr(const char* f, ...) __attribute__((format(printf, 1, 2)));
static void pr(const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    fputs("[t4-peer] ", stdout);
    vfprintf(stdout, f, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

static const char* ynStr(int v) { return v < 0 ? "n/a" : v ? "yes" : "no"; }

// A colour is always four raw bytes in ARGB8888 memory order, so that is how it
// is printed; --seed=RRGGBBAA is the readable spelling of the same four bytes.
static void hex4(const uint8_t b[4], char out[16]) {
    snprintf(out, 16, "%02X,%02X,%02X,%02X", b[0], b[1], b[2], b[3]);
}

// ---------------------------------------------------------------------------
// the T4 wire format, mirrored from extmem_probe.cpp
// ---------------------------------------------------------------------------

enum MsgTag {
    MSG_T4_OFFER = 50,   // peer -> probe, one dma-buf fd riding in SCM_RIGHTS
    MSG_T4_READ = 51,    // probe -> peer: what the probe read back through its imports
    MSG_T4_WROTE = 52,   // probe -> peer: what the probe just cleared, and which path
    MSG_T4_ACK = 53,     // peer -> probe: what the peer then saw in its own mapping
    MSG_T4_RESULT = 54,  // declared by the leg; this sequence never sends it to the peer
    MSG_T4_AHB_OFFER = 55,  // probe -> peer: an AHardwareBuffer minted on the Android side
    MSG_T4_AHB_ACK = 56,    // peer -> probe: what importing it saw, and what was written back
    MSG_BYE = 99,        // the probe's own teardown tag, tolerated below
};

struct MsgHeader {
    uint32_t tag;
    uint32_t len;
};

// magic = 0x5434474D; on a little-endian machine the four bytes read 'M','G','4','T'.
#define T4_MAGIC 0x5434474Du
// The description the probe prints back at the user.  modifier carries the
// protocol's own sentinel: UINT64_MAX means "this peer has no modifier to
// declare", which is what DRM_FORMAT_MOD_INVALID is translated to on the way out.
struct T4Layout {
    uint32_t magic, version, width, height, fourcc, stride, offset, reserved;
    uint64_t modifier;
    uint8_t seed[4];
    char peer[128];
    char note[128];
};

// The probe's two read paths, two pixels each: (0,0) and the far corner.  A
// wrong stride or offset cannot pass a corner check, which is the whole reason
// the corner is in the protocol.
struct T4Read {
    int32_t texOk, rbOk, rung, pad;
    uint8_t texWord[4], texCorner[4], rbWord[4], rbCorner[4];
    char importNote[160];
};

struct T4Wrote {
    int32_t path;  // 0 = renderbuffer, 1 = texture
    uint8_t color[4];
};

struct T4Ack {
    int32_t ok;
    uint8_t observed[4];
    char note[128];
};

// MSG_T4_AHB_OFFER: the Android side's AHardwareBuffer, described so this side
// can import it without guessing any part of the layout.  The fourcc is the DRM
// name whose memory order is the order AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM
// names -- DRM_FORMAT_ABGR8888, fourcc_code('A','B','2','4') = 0x34324241,
// whose little-endian [31:0] A:B:G:R layout puts R,G,B,A in bytes 0..3 -- so the
// bytes compared below are the bytes the probe's glClearColor produced, with no
// channel-renaming step in between where a convention could hide a mistake.
// The stride is the probe's own reported stride, never width*4.
struct T4AhbOffer {
    uint32_t magic, version, width, height, stride, fourcc;
    uint8_t seed[4];  // what the probe cleared into the buffer, in memory order
    char note[128];
};

// MSG_T4_AHB_ACK: what gbm_bo_import made of the fd, what both pixels held
// BEFORE anything was written, and the pattern then written over the whole
// mapping.  `matched` is this side's own comparison -- bit0 for (0,0), bit1 for
// the far corner -- and the probe recomputes it from the two pixel fields, so a
// disagreement between the bits and the bytes is visible rather than trusted.
struct T4AhbAck {
    int32_t imported;  // gbm_bo_import(GBM_BO_IMPORT_FD_MODIFIER) returned a bo
    int32_t matched;   // 1 = (0,0) held the announced seed, 2 = the far corner did
    uint32_t stride;   // the stride this side's own mapping reports
    uint8_t observed[4];
    uint8_t observedCorner[4];
    uint8_t pattern[4];  // zero when nothing could be written
    char note[128];
};

// Every probe -> peer payload, so that one recvMsg can take whatever arrives and
// the tag decides how it is read (see the receive loop in main).
union T4Payload {
    struct T4Read read;
    struct T4Wrote wrote;
    struct T4AhbOffer ahb;
};

// ---------------------------------------------------------------------------
// message plumbing (same shape as the probe's sendMsg/recvMsg)
// ---------------------------------------------------------------------------

// errno as it stood AT the failing call: the caller prints it, and any work in
// between would otherwise clobber it.
static int gMsgErrno = 0;

static bool writeAll(int fd, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return false;
        }
        b += w;
        n -= (size_t)w;
    }
    return true;
}

static bool readAll(int fd, void* p, size_t n) {
    uint8_t* b = (uint8_t*)p;
    while (n) {
        ssize_t r = read(fd, b, n);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return false;
        }
        b += r;
        n -= (size_t)r;
    }
    return true;
}

// header + payload go out in one sendmsg so the fd lands with the header byte
static bool sendMsg(int sock, uint32_t tag, const void* payload, size_t len, int fdToPass) {
    struct MsgHeader h = {tag, (uint32_t)len};
    struct iovec iov[2];
    iov[0].iov_base = &h;
    iov[0].iov_len = sizeof(h);
    iov[1].iov_base = (void*)payload;
    iov[1].iov_len = len;

    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof(cbuf));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = len ? 2 : 1;
    if (fdToPass >= 0) {
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof(cbuf);
        struct cmsghdr* cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fdToPass, sizeof(int));
    }
    ssize_t s;
    do {
        s = sendmsg(sock, &msg, 0);
    } while (s < 0 && errno == EINTR);
    if (s < 0) {
        gMsgErrno = errno;
        return false;
    }
    size_t total = sizeof(h) + len;
    if ((size_t)s == total) return true;
    // partial: finish the tail with plain writes (control data already delivered)
    size_t done = (size_t)s;
    if (done < sizeof(h)) {
        gMsgErrno = EIO;
        return false;
    }
    if (!writeAll(sock, (const uint8_t*)payload + (done - sizeof(h)), total - done)) {
        gMsgErrno = errno;
        return false;
    }
    return true;
}

static bool recvMsg(int sock, uint32_t* tag, void* payload, size_t maxLen, size_t* outLen, int* fdOut) {
    if (fdOut) *fdOut = -1;
    struct MsgHeader h;
    memset(&h, 0, sizeof(h));
    struct iovec iov;
    iov.iov_base = &h;
    iov.iov_len = sizeof(h);

    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof(cbuf));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    ssize_t r;
    do {
        r = recvmsg(sock, &msg, MSG_WAITALL);
    } while (r < 0 && errno == EINTR);
    if (r != (ssize_t)sizeof(h)) {
        gMsgErrno = r < 0 ? errno : 0;
        return false;
    }

    for (struct cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
            int got = -1;
            memcpy(&got, CMSG_DATA(cm), sizeof(int));
            if (fdOut) {
                *fdOut = got;
            } else if (got >= 0) {
                close(got);
            }
        }
    }
    *tag = h.tag;
    if (outLen) *outLen = h.len;
    if (h.len > maxLen) {
        gMsgErrno = EMSGSIZE;
        return false;
    }
    if (h.len && !readAll(sock, payload, h.len)) {
        gMsgErrno = errno;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// peer state: built up as the run proceeds and printed raw at the end
// ---------------------------------------------------------------------------

static const char* gEndpoint = "@mgl-t4";
static uint32_t gWidth = 64, gHeight = 64;
static uint32_t gSeedWord = 0x1A2B3C4Du;  // RRGGBBAA
static int gTimeout = 60;
static uint8_t gSeed[4];

static struct gbm_device* gDev = NULL;
static struct gbm_bo* gBo = NULL;
static uint32_t gStride = 0;
static uint32_t gFourcc = GBM_FORMAT_ARGB8888;
static uint64_t gModifierRaw = 0, gModifierTx = 0;
static int gBoFd = -1;
static char gBoFdName[512] = "<none>";
static unsigned gBoHandle = 0;

// Each description string is built once, where its value is learned, so the
// final line never has to guess whether a zero means "linear" or "not reached".
static char gDevDesc[160] = "device=<none>";
static char gBoDesc[256] = "bo=<none>";
static char gFdDesc[1024] = "fd=<none>";
static char gFillDesc[224] = "fill=<none>";

static int gHaveRead = 0;
static struct T4Read gRead;
static int gHaveWrote[2] = {0, 0};
static uint8_t gWrote[2][4];
static uint8_t gObserved[2][4];
static uint8_t gObservedCorner[2][4];
static int gMatched[2] = {-1, -1};
static int gMatchedCorner[2] = {-1, -1};

// The Android-minted row's own state.  `gHaveAhbOffer` is also what tells the
// verdict that this row was asked at all: the probe skips it when the Android
// side cannot even mint the buffer, and a row that was never asked is not a
// failure.
static int gHaveAhbOffer = 0;
static int gAhbImported = 0;
static int gAhbFilled = 0;
static uint32_t gAhbStride = 0;
static uint8_t gAhbSeed[4];
static uint8_t gAhbPattern[4];
static uint8_t gAhbObserved[4];
static uint8_t gAhbObservedCorner[4];
static char gAhbNote[160] = "<none>";
static char gAhbFdDesc[768] = "fd=<none>";
static char gAhbBoDesc[200] = "bo=<none>";
static char gAhbFillDesc[240] = "fill=<none>";

// The four bytes this side writes over the probe's clear, in the buffer's own
// memory order.  No two of them are equal, so a repeated-byte fill cannot pass
// for it; the ack announces it and the probe compares against THAT, never against
// a constant of its own, so the two sides cannot agree on the wrong number.
static const uint8_t T4_AHB_PATTERN[4] = {0xC7, 0x39, 0x04, 0xE1};

// ---------------------------------------------------------------------------
// GBM side
// ---------------------------------------------------------------------------

// The fd's own kernel name and its exporter are the evidence that this is the
// same KIND of buffer the compositor scans out: kwin holds fds named /dmabuf:
// with exp_name system (its own dma-heap allocations) next to /dmabuf:<id>
// SurfaceView[com.anland...] with exp_name qcom,system (Android buffers it
// imported).  A peer buffer from a different heap would be a different answer to
// T4 even if the import itself succeeded, so both halves are printed verbatim.
// Fills the caller's two buffers and prints both lines verbatim.  Shared with the
// Android-minted row: an fd that arrived from the other direction carries the
// other side's exporter, and that is exactly as much evidence as this one.
static void fdEvidence(int fd, char* nameOut, size_t nameLen, char* exporterOut, size_t exporterLen) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    char link[500];
    ssize_t n = readlink(path, link, sizeof(link) - 1);
    if (n < 0) {
        int e = errno;
        snprintf(nameOut, nameLen, "<readlink errno=%d (%s)>", e, strerror(e));
        pr("readlink(%s) failed errno=%d (%s)", path, e, strerror(e));
    } else {
        link[n] = 0;
        snprintf(nameOut, nameLen, "%s", link);
    }
    pr("readlink(/proc/self/fd/%d)=\"%s\"", fd, nameOut);

    char info[64];
    snprintf(info, sizeof(info), "/proc/self/fdinfo/%d", fd);
    snprintf(exporterOut, exporterLen, "<unread>");
    FILE* f = fopen(info, "r");
    if (!f) {
        int e = errno;
        snprintf(exporterOut, exporterLen, "<fdinfo errno=%d (%s)>", e, strerror(e));
        pr("open(%s) failed errno=%d (%s)", info, e, strerror(e));
        return;
    }
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "exp_name:", 9)) {
            size_t l = strlen(line);
            while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
            snprintf(exporterOut, exporterLen, "%s", line + 9);
            found = 1;
            break;
        }
    }
    fclose(f);
    if (!found) snprintf(exporterOut, exporterLen, "<no exp_name line>");
    pr("%s exp_name:%s", info, exporterOut);
}

static void printFdEvidence(int fd) {
    char expName[300];
    fdEvidence(fd, gBoFdName, sizeof(gBoFdName), expName, sizeof(expName));
    snprintf(gFdDesc, sizeof(gFdDesc), "fd=%d fdname=\"%s\" exporter=\"%s\"", fd, gBoFdName, expName);
}

// ARGB8888 in memory order is the byte triple B,G,R,A, and --seed=RRGGBBAA is
// read in exactly that order.  The fill is SOLID, so a vertical flip cannot turn
// a working import into a reported failure; the whole mapping (stride * height
// bytes, row padding included) gets the same four bytes repeated.
static bool fillWithSeed(void) {
    uint32_t mapStride = 0;
    void* mapData = NULL;
    void* map = gbm_bo_map(gBo, 0, 0, gWidth, gHeight, GBM_BO_TRANSFER_WRITE, &mapStride, &mapData);
    if (!map || map == MAP_FAILED) {
        int e = errno;
        pr("gbm_bo_map(WRITE) failed errno=%d (%s)", e, strerror(e));
        return false;
    }
    uint8_t* base = (uint8_t*)(mapData ? mapData : map);
    gSeed[0] = (uint8_t)((gSeedWord >> 8) & 0xFFu);   // B
    gSeed[1] = (uint8_t)((gSeedWord >> 16) & 0xFFu);  // G
    gSeed[2] = (uint8_t)((gSeedWord >> 24) & 0xFFu);  // R
    gSeed[3] = (uint8_t)(gSeedWord & 0xFFu);          // A
    uint32_t s = mapStride ? mapStride : gStride;
    uint64_t n = (uint64_t)s * gHeight;
    for (uint64_t i = 0; i < n; ++i) base[i] = gSeed[i & 3u];

    char seedS[16];
    hex4(gSeed, seedS);
    pr("gbm_bo_map(WRITE) = %p mapping stride=%u (bo stride=%u), filled %llu bytes with seed 0x%08X mem=[%s]",
       (void*)base, s, gStride, (unsigned long long)n, gSeedWord, seedS);
    snprintf(gFillDesc, sizeof(gFillDesc), "seed=0x%08X seedMem=[%s] filled=%llu mapStride=%u",
             gSeedWord, seedS, (unsigned long long)n, s);
    // Deliberately not unmapped: see the header.  The mapping stays valid until
    // the process exits, which is the next thing that happens to it.
    return true;
}

// ---------------------------------------------------------------------------
// one check: fresh mapping, both pixels, raw bytes on stdout before the unmap
// ---------------------------------------------------------------------------

// Fills the ack and returns 1 only when the announced colour was observed at
// BOTH pixels.  On a mismatch it also scans the mapping for the announced colour
// and for our own seed, which separates "the probe's write never landed here"
// from "it landed somewhere else" -- the same distinction the T1 leg reports as
// payloadAt=.
static int checkPath(int path, const uint8_t color[4], struct T4Ack* ack, int* cornerMatchOut,
                     uint8_t cornerOut[4]) {
    const char* name = path == 0 ? "renderbuffer" : path == 1 ? "texture" : "unknown";
    char aS[16], fS[16], cS[16];
    hex4(color, aS);
    *cornerMatchOut = -1;
    memset(cornerOut, 0, 4);
    ack->ok = 0;
    memset(ack->observed, 0, 4);
    ack->note[0] = 0;

    uint32_t mapStride = 0;
    void* mapData = NULL;
    void* map = gbm_bo_map(gBo, 0, 0, gWidth, gHeight, GBM_BO_TRANSFER_READ, &mapStride, &mapData);
    if (!map || map == MAP_FAILED) {
        int e = errno;
        pr("MSG_T4_WROTE path=%d (%s) announced=[%s]: gbm_bo_map(READ) failed errno=%d (%s)",
           path, name, aS, e, strerror(e));
        snprintf(ack->note, sizeof(ack->note), "gbm_bo_map(READ) failed errno=%d (%s)", e, strerror(e));
        return 0;
    }
    const uint8_t* base = (const uint8_t*)(mapData ? mapData : map);
    uint32_t s = mapStride ? mapStride : gStride;
    size_t cornerOff = (size_t)(gHeight - 1) * s + (size_t)(gWidth - 1) * 4;
    size_t n = (size_t)s * gHeight;
    uint8_t first[4], corner[4];
    memcpy(first, base, 4);
    memcpy(corner, base + cornerOff, 4);
    hex4(first, fS);
    hex4(corner, cS);

    int mFirst = memcmp(first, color, 4) == 0;
    int mCorner = memcmp(corner, color, 4) == 0;
    pr("MSG_T4_WROTE path=%d (%s) announced=[%s] -> fresh mapping stride=%u base=%p: "
       "px(0,0)=[%s] match=%s  px(%u,%u)@%zu=[%s] match=%s",
       path, name, aS, s, (const void*)base, fS, ynStr(mFirst), gWidth - 1, gHeight - 1, cornerOff, cS,
       ynStr(mCorner));

    long annAt = -1, seedAt = -1;
    if (!(mFirst && mCorner)) {
        for (size_t i = 0; i + 4 <= n; ++i) {
            if (annAt < 0 && memcmp(base + i, color, 4) == 0) annAt = (long)i;
            if (seedAt < 0 && memcmp(base + i, gSeed, 4) == 0) seedAt = (long)i;
            if (annAt >= 0 && seedAt >= 0) break;
        }
        pr("  mismatch scan over %zu mapped bytes: announcedColorAt=%ld seedAt=%ld", n, annAt, seedAt);
    }

    ack->ok = (mFirst && mCorner) ? 1 : 0;
    // ack.observed carries the FIRST pixel, which is the four bytes the probe
    // compares against the colour it announced.  The corner travels in the note
    // and on stdout: a stride or offset mistake shows up there and nowhere else.
    memcpy(ack->observed, first, 4);
    memcpy(cornerOut, corner, 4);
    *cornerMatchOut = mCorner;
    snprintf(ack->note, sizeof(ack->note), "%s stride=%u px00=[%s] corner=[%s]%s", name, s, fS, cS,
             (mFirst && mCorner) ? "" : (annAt >= 0 ? " announced-seen-elsewhere" : " announced-absent"));
    // The observation is already on stdout: a crash in unmap must not be able to
    // hide the bytes this check produced.
    gbm_bo_unmap(gBo, mapData ? mapData : map);
    return mFirst && mCorner;
}

// ---------------------------------------------------------------------------
// the Android-minted row: an AHardwareBuffer's dma-buf, imported and answered
// ---------------------------------------------------------------------------

// The buffer does not arrive as a frame of this protocol: the probe writes its
// Android handle into one end of a socketpair and sends the OTHER end here in
// SCM_RIGHTS beside MSG_T4_AHB_OFFER, so what is on that fd is Android's own
// handle encoding -- of no use in a glibc process, and libandroid is not linkable
// from one anyway.  The kernel carries the part this side can use: the buffer's
// plane fd travels as SCM_RIGHTS control data, so the fd is taken from whichever
// recvmsg carries it.  The bytes are read -- a message carrying only control data
// would otherwise look like EOF -- and reported, never interpreted.
static int recvAhbFd(int fd) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        char data[512];
        char cbuf[CMSG_SPACE(sizeof(int) * (GBM_MAX_PLANES + 4))];
        struct iovec iov;
        iov.iov_base = data;
        iov.iov_len = sizeof(data);
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof(cbuf);
        ssize_t r;
        do {
            r = recvmsg(fd, &msg, 0);
        } while (r < 0 && errno == EINTR);
        if (r < 0) {
            int e = errno;
            pr("recvmsg(ahb socket) failed errno=%d (%s)", e, strerror(e));
            return -1;
        }
        int got[GBM_MAX_PLANES + 4];
        int nfds = 0;
        for (struct cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
            if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
            size_t bytes = cm->cmsg_len > CMSG_LEN(0) ? cm->cmsg_len - CMSG_LEN(0) : 0;
            int count = (int)(bytes / sizeof(int));
            int room = (int)(sizeof(got) / sizeof(got[0])) - nfds;
            if (count > room) count = room;
            if (count > 0) {
                memcpy(got + nfds, CMSG_DATA(cm), (size_t)count * sizeof(int));
                nfds += count;
            }
        }
        pr("ahb socket: recvmsg #%d -> %zd byte(s) of Android handle encoding, %d fd(s) in SCM_RIGHTS%s",
           attempt + 1, r, nfds,
           (msg.msg_flags & MSG_CTRUNC) ? " (MSG_CTRUNC: the control buffer was too small)" : "");
        if (nfds > 0) {
            for (int i = 1; i < nfds; ++i) close(got[i]);
            if (nfds > 1)
                pr("ahb socket: %d fds arrived; fd[0]=%d is used and the other %d are closed", nfds, got[0],
                   nfds - 1);
            return got[0];
        }
        if (r == 0) break;  // the sender is gone and no fd was ever attached
    }
    pr("ahb socket: no SCM_RIGHTS fd arrived on the socketpair");
    return -1;
}

// Sends the ack and records what it said, so the FINAL line reports the same
// numbers that went over the wire rather than a second guess at them.
static void sendAhbAck(int sock, struct T4AhbAck* ack) {
    char patS[16], obsS[16], corS[16];
    hex4(ack->pattern, patS);
    hex4(ack->observed, obsS);
    hex4(ack->observedCorner, corS);
    ack->note[sizeof(ack->note) - 1] = 0;
    snprintf(gAhbNote, sizeof(gAhbNote), "%s", ack->note);
    gAhbImported = ack->imported;
    gAhbStride = ack->stride;
    memcpy(gAhbObserved, ack->observed, 4);
    memcpy(gAhbObservedCorner, ack->observedCorner, 4);
    memcpy(gAhbPattern, ack->pattern, 4);
    pr("MSG_T4_AHB_ACK sent imported=%d matched=0x%x stride=%u observed=[%s] observedCorner=[%s] "
       "pattern=[%s] note=\"%s\"",
       (int)ack->imported, (int)ack->matched, ack->stride, obsS, corS, patS, ack->note);
    if (!sendMsg(sock, MSG_T4_AHB_ACK, ack, sizeof(*ack), -1))
        pr("sendMsg(MSG_T4_AHB_ACK) failed errno=%d (%s)", gMsgErrno, strerror(gMsgErrno));
}

// One MSG_T4_AHB_OFFER: import the fd, read both pixels before writing anything,
// then fill the mapping with this side's pattern.  `fd` is the socketpair end that
// arrived in SCM_RIGHTS and `off` is the payload that travelled beside it; the
// caller has already checked the payload's tag and length.
static void serveAhbOffer(int sock, int fd, const struct T4AhbOffer* off) {
    struct T4AhbAck ack;
    memset(&ack, 0, sizeof(ack));
    memcpy(ack.pattern, T4_AHB_PATTERN, 4);

    char seedS[16], patS[16];
    hex4(off->seed, seedS);
    hex4(ack.pattern, patS);
    pr("MSG_T4_AHB_OFFER w=%u h=%u stride=%u fourcc=0x%08X seed=[%s] fd=%d note=\"%s\"", off->width,
       off->height, off->stride, (unsigned)off->fourcc, seedS, fd, off->note);

    gHaveAhbOffer = 1;
    memcpy(gAhbSeed, off->seed, 4);
    memcpy(gAhbPattern, ack.pattern, 4);

    if (fd < 0) {
        snprintf(ack.note, sizeof(ack.note), "the offer carried no socketpair fd in SCM_RIGHTS");
        pr("MSG_T4_AHB_OFFER: %s", ack.note);
        sendAhbAck(sock, &ack);
        return;
    }
    const int ahbFd = recvAhbFd(fd);
    if (ahbFd < 0) {
        snprintf(ack.note, sizeof(ack.note), "no dma-buf fd arrived on the socketpair");
        sendAhbAck(sock, &ack);
        return;
    }
    char ahbName[400], ahbExport[300];
    fdEvidence(ahbFd, ahbName, sizeof(ahbName), ahbExport, sizeof(ahbExport));
    snprintf(gAhbFdDesc, sizeof(gAhbFdDesc), "fd=%d fdname=\"%s\" exporter=\"%s\"", ahbFd, ahbName, ahbExport);

    if (off->width == 0 || off->height == 0 || off->stride == 0) {
        snprintf(ack.note, sizeof(ack.note), "degenerate offer %ux%u stride=%u", off->width, off->height,
                 off->stride);
        pr("MSG_T4_AHB_OFFER: %s", ack.note);
        close(ahbFd);
        sendAhbAck(sock, &ack);
        return;
    }

    struct gbm_import_fd_modifier_data imp;
    memset(&imp, 0, sizeof(imp));
    imp.width = off->width;
    imp.height = off->height;
    imp.format = off->fourcc;
    imp.num_fds = 1;
    imp.fds[0] = ahbFd;
    imp.strides[0] = (int)off->stride;
    imp.offsets[0] = 0;
    imp.modifier = T4_DRM_FORMAT_MOD_LINEAR;
    struct gbm_bo* bo =
        gbm_bo_import(gDev, GBM_BO_IMPORT_FD_MODIFIER, &imp, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!bo) {
        int e = errno;
        pr("gbm_bo_import(FD_MODIFIER, %ux%u format=0x%08X stride=%d offset=0 modifier=0x%016" PRIx64
           " fd=%d) refused errno=%d (%s)",
           off->width, off->height, (unsigned)off->fourcc, imp.strides[0], (uint64_t)imp.modifier, ahbFd, e,
           strerror(e));
        snprintf(ack.note, sizeof(ack.note), "gbm_bo_import refused errno=%d (%s)", e, strerror(e));
        close(ahbFd);
        sendAhbAck(sock, &ack);
        return;
    }
    ack.imported = 1;
    pr("gbm_bo_import = OK bo=%u %ux%u stride=%u format=0x%08X modifier=0x%016" PRIx64 " planes=%u",
       gbm_bo_get_handle(bo).u32, gbm_bo_get_width(bo), gbm_bo_get_height(bo), gbm_bo_get_stride(bo),
       (unsigned)gbm_bo_get_format(bo), (uint64_t)gbm_bo_get_modifier(bo), gbm_bo_get_plane_count(bo));
    snprintf(gAhbBoDesc, sizeof(gAhbBoDesc),
             "bo=%u w=%u h=%u stride=%u fourcc=0x%08X modifier=0x%016" PRIx64 " planes=%u",
             gbm_bo_get_handle(bo).u32, gbm_bo_get_width(bo), gbm_bo_get_height(bo), gbm_bo_get_stride(bo),
             (unsigned)gbm_bo_get_format(bo), (uint64_t)gbm_bo_get_modifier(bo), gbm_bo_get_plane_count(bo));
    // The import holds its own reference on the buffer, so this process's copy of
    // the fd has done its job.
    close(ahbFd);

    // ---- read direction: a fresh READ mapping, both pixels, before any write ---
    uint32_t mapStride = 0;
    void* mapData = NULL;
    void* map = gbm_bo_map(bo, 0, 0, off->width, off->height, GBM_BO_TRANSFER_READ, &mapStride, &mapData);
    if (!map || map == MAP_FAILED) {
        int e = errno;
        pr("gbm_bo_map(imported, READ) failed errno=%d (%s)", e, strerror(e));
        snprintf(ack.note, sizeof(ack.note), "gbm_bo_map(READ) failed errno=%d (%s)", e, strerror(e));
        gbm_bo_destroy(bo);  // no mapping is live here, so this is the clean case
        sendAhbAck(sock, &ack);
        return;
    }
    const uint8_t* base = (const uint8_t*)(mapData ? mapData : map);
    const uint32_t s = mapStride ? mapStride : off->stride;
    ack.stride = s;
    const size_t cornerOff = (size_t)(off->height - 1) * s + (size_t)(off->width - 1) * 4;
    uint8_t first[4], corner[4];
    memcpy(first, base, 4);
    memcpy(corner, base + cornerOff, 4);
    memcpy(ack.observed, first, 4);
    memcpy(ack.observedCorner, corner, 4);
    const int mFirst = memcmp(first, off->seed, 4) == 0;
    const int mCorner = memcmp(corner, off->seed, 4) == 0;
    ack.matched = (mFirst ? 1 : 0) | (mCorner ? 2 : 0);
    char fS[16], cS[16];
    hex4(first, fS);
    hex4(corner, cS);
    pr("AHB import: mapStride=%u (bo stride=%u) px(0,0)=[%s] match=%s  px(%u,%u)@%zu=[%s] match=%s  "
       "announced seed=[%s]",
       s, gbm_bo_get_stride(bo), fS, ynStr(mFirst), off->width - 1, off->height - 1, cornerOff, cS, ynStr(mCorner),
       seedS);
    if (!(mFirst && mCorner)) {
        // The same distinction checkPath() reports: the announced colour either
        // landed somewhere else in this mapping or never arrived at all.
        const size_t n = (size_t)s * off->height;
        long annAt = -1;
        for (size_t i = 0; i + 4 <= n; ++i) {
            if (memcmp(base + i, off->seed, 4) == 0) {
                annAt = (long)i;
                break;
            }
        }
        pr("  mismatch scan over %zu mapped bytes: announcedSeedAt=%ld", n, annAt);
    }
    // A READ mapping unmaps cleanly on this stack (only WRITE unmaps crash), and
    // the fill below needs a mapping of its own anyway.
    gbm_bo_unmap(bo, mapData ? mapData : map);

    // ---- fill direction: one WRITE mapping, filled and left mapped ------------
    uint32_t fillStride = 0;
    void* fillData = NULL;
    void* fill = gbm_bo_map(bo, 0, 0, off->width, off->height, GBM_BO_TRANSFER_WRITE, &fillStride, &fillData);
    if (!fill || fill == MAP_FAILED) {
        int e = errno;
        pr("gbm_bo_map(imported, WRITE) failed errno=%d (%s) -- nothing was written, so nothing is "
           "announced",
           e, strerror(e));
        memset(ack.pattern, 0, 4);
        snprintf(ack.note, sizeof(ack.note), "gbm_bo_map(WRITE) failed errno=%d (%s): pattern not written", e,
                 strerror(e));
        gbm_bo_destroy(bo);
        sendAhbAck(sock, &ack);
        return;
    }
    uint8_t* fillBase = (uint8_t*)(fillData ? fillData : fill);
    const uint32_t fs = fillStride ? fillStride : s;
    const uint64_t fillBytes = (uint64_t)fs * off->height;
    for (uint64_t i = 0; i < fillBytes; ++i) fillBase[i] = ack.pattern[i & 3u];
    gAhbFilled = 1;
    pr("gbm_bo_map(imported, WRITE) = %p mapping stride=%u (bo stride=%u), wrote %llu bytes of pattern [%s]",
       (void*)fillBase, fs, gbm_bo_get_stride(bo), (unsigned long long)fillBytes, patS);
    snprintf(gAhbFillDesc, sizeof(gAhbFillDesc), "pattern=[%s] filled=%llu mapStride=%u", patS,
             (unsigned long long)fillBytes, fs);
    snprintf(ack.note, sizeof(ack.note),
             "imported via gbm_bo_import(FD_MODIFIER) stride=%u px00=[%s] corner=[%s] pattern=[%s]", s, fS, cS,
             patS);
    // The WRITE mapping is deliberately NOT unmapped, and this bo is deliberately
    // not destroyed either (destroy tears the mapping down): on this stack unmapping
    // a write mapping segfaults, and this process is about to exit.  Whether the
    // fill actually reached the buffer is not this side's claim to make -- the probe
    // reads it back through the GPU, and that read is the evidence.
    sendAhbAck(sock, &ack);
}

// ---------------------------------------------------------------------------
// endpoint
// ---------------------------------------------------------------------------

// The endpoint is an abstract name by default and has to stay one: a Droidspaces
// container runs net_mode=host, so the abstract namespace is common to both
// sides, while a filesystem path would have to exist in the other side's root.
static int connectEndpoint(const char* ep) {
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) {
        int e = errno;
        pr("socket(AF_UNIX) failed errno=%d (%s)", e, strerror(e));
        return -1;
    }
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    socklen_t len;
    if (ep[0] == '@') {
        strncpy(a.sun_path + 1, ep + 1, sizeof(a.sun_path) - 2);
        len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(ep + 1));
    } else {
        strncpy(a.sun_path, ep, sizeof(a.sun_path) - 1);
        len = (socklen_t)sizeof(a);
    }
    if (connect(s, (struct sockaddr*)&a, len) != 0) {
        int e = errno;
        pr("connect(%s) failed errno=%d (%s)", ep, e, strerror(e));
        close(s);
        return -1;
    }
    struct timeval tv;
    tv.tv_sec = gTimeout;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return s;
}

// ---------------------------------------------------------------------------
// the final line: every raw observation, nothing summarised away
// ---------------------------------------------------------------------------

static int finish(int rc, const char* why) {
    char texW[16] = "-", texC[16] = "-", rbW[16] = "-", rbC[16] = "-";
    char rbA[16] = "-", rbO[16] = "-", rbK[16] = "-";
    char txA[16] = "-", txO[16] = "-", txK[16] = "-";
    char ahbSeedS[16] = "-", ahbObsS[16] = "-", ahbCorS[16] = "-", ahbPatS[16] = "-";
    if (gHaveRead) {
        hex4(gRead.texWord, texW);
        hex4(gRead.texCorner, texC);
        hex4(gRead.rbWord, rbW);
        hex4(gRead.rbCorner, rbC);
    }
    if (gHaveWrote[0]) {
        hex4(gWrote[0], rbA);
        hex4(gObserved[0], rbO);
        hex4(gObservedCorner[0], rbK);
    }
    if (gHaveWrote[1]) {
        hex4(gWrote[1], txA);
        hex4(gObserved[1], txO);
        hex4(gObservedCorner[1], txK);
    }
    if (gHaveAhbOffer) {
        hex4(gAhbSeed, ahbSeedS);
        hex4(gAhbObserved, ahbObsS);
        hex4(gAhbObservedCorner, ahbCorS);
        hex4(gAhbPattern, ahbPatS);
    }
    // "Both directions held": the probe saw our seed bytes at both pixels of both
    // paths, and we saw the probe's announced colour at both pixels of both
    // paths.  Either half alone is a one-way mapping and is not T4's answer.
    int readDir = gHaveRead && gRead.texOk && gRead.rbOk &&
                  memcmp(gRead.texWord, gSeed, 4) == 0 && memcmp(gRead.texCorner, gSeed, 4) == 0 &&
                  memcmp(gRead.rbWord, gSeed, 4) == 0 && memcmp(gRead.rbCorner, gSeed, 4) == 0;
    int writeDir = gHaveWrote[0] && gHaveWrote[1] && gMatched[0] && gMatched[1];
    // The Android-minted row is not two paths of one leg: what this side can see of
    // it is the import, the two pixels of the imported mapping, and its own fill.
    // The other half of it -- the probe reading that fill back through the GPU -- is
    // the probe's leg, and no number here can stand in for it.
    int ahbMatched = gHaveAhbOffer && memcmp(gAhbObserved, gAhbSeed, 4) == 0 &&
                     memcmp(gAhbObservedCorner, gAhbSeed, 4) == 0;
    int ahbDir = gAhbImported && gAhbFilled && ahbMatched;
    // A row the probe never asked is not a failure here: with no
    // EGL_EXT_image_dma_buf_import on the Android side the probe records
    // T4-image-import as a capability answer and never sends MSG_T4_READ or
    // MSG_T4_WROTE, so only the rows that were actually exercised count.
    int dbufRan = gHaveRead || gHaveWrote[0] || gHaveWrote[1];
    int pass = (!dbufRan || (readDir && writeDir)) && (!gHaveAhbOffer || ahbDir);

    pr("FINAL endpoint=%s %s %s %s %s "
       "texOk=%d rbOk=%d rung=%d texWord=[%s] texCorner=[%s] rbWord=[%s] rbCorner=[%s] importNote=\"%s\" "
       "rb_announced=[%s] rb_observed=[%s] rb_corner=[%s] rb_match=%s rb_corner_match=%s "
       "tex_announced=[%s] tex_observed=[%s] tex_corner=[%s] tex_match=%s tex_corner_match=%s "
       "readDirection=%s writeDirection=%s "
       "ahbOffer=%s ahbImported=%d ahbStride=%u ahbSeed=[%s] ahbObserved=[%s] ahbObservedCorner=[%s] "
       "ahbPattern=[%s] ahbMatched=%s ahbDirection=%s ahbFd=%s ahbBo=%s ahbFill=%s ahbNote=\"%s\" "
       "verdict=%s why=\"%s\"",
       gEndpoint, gDevDesc, gBoDesc, gFdDesc, gFillDesc,
       gHaveRead ? (int)gRead.texOk : -1, gHaveRead ? (int)gRead.rbOk : -1, gHaveRead ? (int)gRead.rung : -1,
       texW, texC, rbW, rbC, gHaveRead ? gRead.importNote : "-",
       rbA, rbO, rbK, ynStr(gMatched[0]), ynStr(gMatchedCorner[0]),
       txA, txO, txK, ynStr(gMatched[1]), ynStr(gMatchedCorner[1]),
       readDir ? "held" : (gHaveRead ? "broken" : "notrun"),
       writeDir ? "held" : (gHaveWrote[0] || gHaveWrote[1] ? "broken" : "notrun"),
       gHaveAhbOffer ? "yes" : "no", gHaveAhbOffer ? gAhbImported : -1, gAhbStride, ahbSeedS, ahbObsS, ahbCorS,
       ahbPatS, ynStr(ahbMatched), ahbDir ? "held" : (gHaveAhbOffer ? "broken" : "notrun"), gAhbFdDesc,
       gAhbBoDesc, gAhbFillDesc, gAhbNote,
       pass ? "PASS" : "FAIL", why && why[0] ? why : "-");
    return pass ? 0 : rc;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void usage(void) {
    printf("usage: t4_gbm_peer [--endpoint=@mgl-t4] [--width=64] [--height=64] "
           "[--seed=RRGGBBAA] [--timeout=60]\n"
           "  Allocates a GBM buffer in the container, fills it with the seed, offers its\n"
           "  dma-buf fd to extmem_probe --only-t4 over an abstract unix socket, and checks\n"
           "  what the probe drew into it.  It also answers the probe's own AHardwareBuffer\n"
           "  (T4-ahb-image): imports its fd as a GBM bo, compares both pixels against the\n"
           "  colour the probe announced, and fills its mapping with a pattern of its own.\n"
           "  --width/--height size the GBM buffer and keep the probe's AHardwareBuffer at\n"
           "  the same size.  Exit 0 only when every row the probe asked for held.\n");
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strncmp(a, "--endpoint=", 11)) {
            gEndpoint = a + 11;
        } else if (!strncmp(a, "--width=", 8)) {
            gWidth = (uint32_t)strtoul(a + 8, NULL, 0);
        } else if (!strncmp(a, "--height=", 9)) {
            gHeight = (uint32_t)strtoul(a + 9, NULL, 0);
        } else if (!strncmp(a, "--seed=", 7)) {
            gSeedWord = (uint32_t)strtoul(a + 7, NULL, 16);
        } else if (!strncmp(a, "--timeout=", 10)) {
            gTimeout = (int)strtol(a + 10, NULL, 0);
        } else if (!strcmp(a, "--help")) {
            usage();
            return 0;
        } else {
            pr("unknown option \"%s\"", a);
            usage();
            return 2;
        }
    }
    if (!gWidth || !gHeight || gWidth > 16384 || gHeight > 16384) {
        pr("--width/--height must be 1..16384 (got %u x %u)", gWidth, gHeight);
        return 2;
    }
    if (gTimeout < 1) {
        pr("--timeout must be at least 1 second (got %d)", gTimeout);
        return 2;
    }

    // A probe that dies mid-exchange must not take this process down with it; the
    // send path reports EPIPE instead.
    signal(SIGPIPE, SIG_IGN);

    const char* devPath = "/dev/dri/renderD128";
    pr("t4_gbm_peer: endpoint=%s %ux%u seed=0x%08X timeout=%ds uid=%d pid=%d", gEndpoint, gWidth,
       gHeight, gSeedWord, gTimeout, (int)getuid(), (int)getpid());

    int devFd = open(devPath, O_RDWR | O_CLOEXEC);
    if (devFd < 0) {
        int e = errno;
        pr("open(%s) failed errno=%d (%s) -- the peer must run inside the container: that is where "
           "the Anland-backed GBM device lives",
           devPath, e, strerror(e));
        return 3;
    }
    gDev = gbm_create_device(devFd);
    if (!gDev) {
        int e = errno;
        pr("gbm_create_device(%s) failed fd=%d errno=%d (%s)", devPath, devFd, e, strerror(e));
        return 3;
    }
    snprintf(gDevDesc, sizeof(gDevDesc), "device=%s backend=\"%s\" devFd=%d", devPath,
             gbm_device_get_backend_name(gDev), devFd);
    pr("gbm_create_device = OK %s", gDevDesc);

    // The driver's own verdict on this format and these flags IS the result.
    // There is no fallback to another format: an ARGB8888 RENDERING|LINEAR
    // refusal is an answer about the device, and a silent substitution would
    // destroy it.
    const uint32_t flags = GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR;
    gBo = gbm_bo_create(gDev, gWidth, gHeight, GBM_FORMAT_ARGB8888, flags);
    if (!gBo) {
        int e = errno;
        pr("gbm_bo_create(%ux%u, GBM_FORMAT_ARGB8888=0x%08X, flags=0x%X "
           "GBM_BO_USE_RENDERING|GBM_BO_USE_LINEAR) refused: errno=%d (%s)",
           gWidth, gHeight, (unsigned)GBM_FORMAT_ARGB8888, flags, e, strerror(e));
        return 4;
    }

    gStride = gbm_bo_get_stride(gBo);
    gFourcc = gbm_bo_get_format(gBo);
    gModifierRaw = gbm_bo_get_modifier(gBo);
    gBoHandle = gbm_bo_get_handle(gBo).u32;
    // The protocol carries UINT64_MAX for "no modifier"; DRM_FORMAT_MOD_INVALID
    // is the kernel's way of saying exactly that, so it is translated on the way
    // out.  The raw value is printed either way.
    gModifierTx = (gModifierRaw == T4_DRM_FORMAT_MOD_INVALID) ? UINT64_MAX : gModifierRaw;
    pr("gbm_bo_create = OK handle=%u width=%u height=%u planes=%u", gBoHandle, gbm_bo_get_width(gBo),
       gbm_bo_get_height(gBo), gbm_bo_get_plane_count(gBo));
    pr("gbm_bo_get_stride   = %u", gStride);
    pr("gbm_bo_get_format   = 0x%08X (DRM_FORMAT_ARGB8888 = 'AR24')", (unsigned)gFourcc);
    pr("gbm_bo_get_modifier = 0x%016" PRIx64 " (DRM_FORMAT_MOD_LINEAR = 0; sent as 0x%016" PRIx64 ")",
       (uint64_t)gModifierRaw, (uint64_t)gModifierTx);
    snprintf(gBoDesc, sizeof(gBoDesc),
             "bo=%u w=%u h=%u planes=%u stride=%u fourcc=0x%08X modifier_raw=0x%016" PRIx64
             " modifier_tx=0x%016" PRIx64,
             gBoHandle, gbm_bo_get_width(gBo), gbm_bo_get_height(gBo), gbm_bo_get_plane_count(gBo),
             gStride, (unsigned)gFourcc, (uint64_t)gModifierRaw, (uint64_t)gModifierTx);

    if (!fillWithSeed()) return 5;

    gBoFd = gbm_bo_get_fd(gBo);
    if (gBoFd < 0) {
        int e = errno;
        pr("gbm_bo_get_fd failed errno=%d (%s) -- an unexportable buffer is not a shareable one", e,
           strerror(e));
        return 5;
    }
    printFdEvidence(gBoFd);

    int sock = connectEndpoint(gEndpoint);
    if (sock < 0) return 6;
    pr("connected to %s (SOCK_STREAM, %ds recv timeout)", gEndpoint, gTimeout);

    struct T4Layout lay;
    memset(&lay, 0, sizeof(lay));
    lay.magic = T4_MAGIC;
    lay.version = 1;  // the only version this leg has ever had; the probe prints it
    lay.width = gWidth;
    lay.height = gHeight;
    lay.fourcc = gFourcc;
    lay.stride = gStride;
    lay.offset = 0;  // a fresh single-plane bo is mapped from its base
    lay.modifier = gModifierTx;
    memcpy(lay.seed, gSeed, 4);
    snprintf(lay.peer, sizeof(lay.peer), "t4_gbm_peer pid=%d backend=%s bo=%u %ux%u stride=%u",
             (int)getpid(), gbm_device_get_backend_name(gDev), gBoHandle, gWidth, gHeight, gStride);
    // The note stays numeric on purpose: the exported name and the exporter are
    // printed on stdout in full, and a name longer than this field would be
    // silently cut here, which is worse than not carrying it at all.
    snprintf(lay.note, sizeof(lay.note), "fill=0x%08X stride=%u fourcc=0x%08X", gSeedWord, gStride,
             (unsigned)gFourcc);
    lay.peer[sizeof(lay.peer) - 1] = 0;
    lay.note[sizeof(lay.note) - 1] = 0;

    char seedS[16];
    hex4(gSeed, seedS);
    if (!sendMsg(sock, MSG_T4_OFFER, &lay, sizeof(lay), gBoFd)) {
        pr("sendMsg(MSG_T4_OFFER) failed errno=%d (%s)", gMsgErrno, strerror(gMsgErrno));
        close(sock);
        return 6;
    }
    pr("MSG_T4_OFFER sent size=%zu magic=0x%08X version=%u w=%u h=%u fourcc=0x%08X stride=%u "
       "offset=%u modifier=0x%016" PRIx64 " seed=0x%08X mem=[%s] fd=%d fdname=\"%s\"",
       sizeof(lay), lay.magic, lay.version, lay.width, lay.height, (unsigned)lay.fourcc, lay.stride,
       lay.offset, (uint64_t)lay.modifier, gSeedWord, seedS, gBoFd, gBoFdName);
    pr("MSG_T4_OFFER peer=\"%s\"", lay.peer);
    pr("MSG_T4_OFFER note=\"%s\"", lay.note);

    // The probe drives the exchange and this side answers whatever it sends.  Either
    // row can be skipped by the probe -- with no EGL_EXT_image_dma_buf_import on the
    // Android side the dma-buf row is recorded as a capability answer and neither
    // MSG_T4_READ nor MSG_T4_WROTE is ever sent -- so no tag may be waited for by
    // name.  The probe closing the socket after its own verdicts is the normal end of
    // the exchange, not an error.
    const char* why = "-";
    uint32_t tag = 0;
    size_t got = 0;
    union T4Payload payload;
    memset(&gRead, 0, sizeof(gRead));
    for (;;) {
        int fd = -1;
        memset(&payload, 0, sizeof(payload));
        if (!recvMsg(sock, &tag, &payload, sizeof(payload), &got, &fd)) {
            if (gMsgErrno != 0) {
                why = "recvMsg failed while waiting for the probe";
                pr("recvMsg failed errno=%d (%s)", gMsgErrno, strerror(gMsgErrno));
            } else if (!gHaveRead && !gHaveWrote[0] && !gHaveWrote[1] && !gHaveAhbOffer) {
                why = "no MSG_T4_READ, MSG_T4_WROTE or MSG_T4_AHB_OFFER from the probe";
                pr("%s: the probe closed the socket first", why);
            } else {
                pr("the probe closed the socket: end of the exchange");
            }
            break;
        }
        if (tag == MSG_T4_RESULT || tag == MSG_BYE) {
            pr("probe closed the exchange with tag=%u", tag);
            if (!(gHaveWrote[0] && gHaveWrote[1]) && !gHaveAhbOffer) why = "probe closed the exchange early";
            break;
        }
        if (tag != MSG_T4_AHB_OFFER && fd >= 0) {
            pr("tag=%u arrived with fd=%d, which this protocol does not define: closed", tag, fd);
            close(fd);
            fd = -1;
        }
        if (tag == MSG_T4_READ) {
            if (got != sizeof(payload.read)) {
                pr("expected MSG_T4_READ(%zu), got tag=%u len=%zu", sizeof(payload.read), tag, got);
                why = "unexpected length for MSG_T4_READ";
                break;
            }
            gRead = payload.read;
            gHaveRead = 1;
            gRead.importNote[sizeof(gRead.importNote) - 1] = 0;
            char tw[16], tc[16], rw[16], rc[16];
        hex4(gRead.texWord, tw);
        hex4(gRead.texCorner, tc);
        hex4(gRead.rbWord, rw);
        hex4(gRead.rbCorner, rc);
        pr("MSG_T4_READ texOk=%d rbOk=%d rung=%d texWord=[%s] texCorner=[%s] rbWord=[%s] rbCorner=[%s] "
           "importNote=\"%s\"",
           (int)gRead.texOk, (int)gRead.rbOk, (int)gRead.rung, tw, tc, rw, rc, gRead.importNote);
        pr("MSG_T4_READ vs our seed 0x%08X mem=[%s]: texWord=%s texCorner=%s rbWord=%s rbCorner=%s",
           gSeedWord, seedS, memcmp(gRead.texWord, gSeed, 4) == 0 ? "match" : "MISMATCH",
           memcmp(gRead.texCorner, gSeed, 4) == 0 ? "match" : "MISMATCH",
           memcmp(gRead.rbWord, gSeed, 4) == 0 ? "match" : "MISMATCH",
           memcmp(gRead.rbCorner, gSeed, 4) == 0 ? "match" : "MISMATCH");

            continue;
        }
        if (tag == MSG_T4_WROTE) {
            // The probe writes twice, once per path, and waits for each ACK.  Its
            // verdict is local (its step 8), so nothing here answers with anything
            // but the bytes this side actually saw.
            struct T4Wrote w = payload.wrote;
            if (got != sizeof(w)) {
                pr("expected MSG_T4_WROTE(%zu), got tag=%u len=%zu", sizeof(w), tag, got);
                why = "unexpected length for MSG_T4_WROTE";
                break;
            }
            if (w.path < 0 || w.path > 1) {
                pr("MSG_T4_WROTE path=%d is neither 0 (renderbuffer) nor 1 (texture); acking ok=0", w.path);
                struct T4Ack bad;
                memset(&bad, 0, sizeof(bad));
                snprintf(bad.note, sizeof(bad.note), "unknown path %d", (int)w.path);
                if (!sendMsg(sock, MSG_T4_ACK, &bad, sizeof(bad), -1))
                    pr("sendMsg(MSG_T4_ACK) failed errno=%d (%s)", gMsgErrno, strerror(gMsgErrno));
                continue;
            }
            int path = (int)w.path;
            memcpy(gWrote[path], w.color, 4);
            gHaveWrote[path] = 1;
            struct T4Ack ack;
            memset(&ack, 0, sizeof(ack));
            // The ack carries the first pixel, because that is the four bytes the
            // probe compares; the corner is kept here and travels in the note and on
            // stdout, where a stride or offset mistake shows up.
            int cornerMatch = -1;
            int matched = checkPath(path, w.color, &ack, &cornerMatch, gObservedCorner[path]);
            gMatched[path] = matched;
            gMatchedCorner[path] = cornerMatch;
            memcpy(gObserved[path], ack.observed, 4);
            if (!sendMsg(sock, MSG_T4_ACK, &ack, sizeof(ack), -1)) {
                pr("sendMsg(MSG_T4_ACK) failed errno=%d (%s)", gMsgErrno, strerror(gMsgErrno));
                why = "could not send MSG_T4_ACK";
                break;
            }
            char oS[16];
            hex4(ack.observed, oS);
            pr("MSG_T4_ACK sent path=%d ok=%d observed=[%s] note=\"%s\"", path, (int)ack.ok, oS, ack.note);
            continue;
        }
        if (tag == MSG_T4_AHB_OFFER) {
            // The AHB offer is the other direction of the same question: an image
            // the Android side minted, which arrives as a socketpair end in
            // SCM_RIGHTS plus this payload.
            if (got != sizeof(payload.ahb)) {
                pr("expected MSG_T4_AHB_OFFER(%zu), got tag=%u len=%zu", sizeof(payload.ahb), tag, got);
                if (fd >= 0) close(fd);
                why = "unexpected length for MSG_T4_AHB_OFFER";
                break;
            }
            payload.ahb.note[sizeof(payload.ahb.note) - 1] = 0;
            serveAhbOffer(sock, fd, &payload.ahb);
            if (fd >= 0) close(fd);
            continue;
        }
        pr("unexpected tag=%u len=%zu: the exchange stops here", tag, got);
        why = "unexpected tag from the probe";
        break;
    }

    close(sock);
    return finish(8, why);
}
