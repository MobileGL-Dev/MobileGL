// extmem_probe -- MobileGL disaggregation P0 spike B (plan-B §8.3, §11 P0).
//
// Question this program answers, per device:
//   Can the memory behind AcquirePersistentMap be shared with another process
//   and mapped there -- for BOTH backends -- and by which route?
//
//   T1  server exports its own allocation      (VkExportMemoryAllocateInfo +
//       vkGetMemoryFdKHR, opaque-fd and dma-buf, handed over SCM_RIGHTS; the
//       importer tries plain mmap() *and* a Vulkan import + vkMapMemory)
//   T1-gles  the same exported fd imported into GLES        (GL_EXT_memory_object
//       + GL_EXT_memory_object_fd: glCreateMemoryObjectsEXT + glImportMemoryFdEXT
//       + glBufferStorageMemEXT + glMapBufferRange(PERSISTENT|COHERENT)),
//       first in-process, then cross-process.  DirectGLES ("Espryt") reaches
//       AcquirePersistentMap through a GL mapping, not a VkDeviceMemory, so the
//       Vulkan-only T1 answer does not decide the tier for it.
//   T0  server imports a client allocation     (AHardwareBuffer BLOB sent over a
//       unix socket, imported into VkDeviceMemory via
//       VK_ANDROID_external_memory_android_hardware_buffer and into a GL buffer
//       via EGL_ANDROID_get_native_client_buffer + glBufferStorageExternalEXT)
//   T3  server imports a client host mapping   (VK_EXT_external_memory_host);
//       both directions: the process that imports allocates the memfd, and --
//       the direction that actually makes T3 a tier -- the *client* allocates
//       the memfd and the *server* imports the client's host pointer.
//
// Every route that can reach OK also takes a real GPU access (vkCmdCopyBuffer
// out of the shared allocation + vkCmdFillBuffer into it, queue-idle, host-read
// barrier), so an OK verdict means the tier survives GPU use and not merely a
// successful map call.
//
// Verdict rule (deliberately strict): a route is OK only when every decisive
// leg round-tripped bytes in both directions, PARTIAL when at least one decisive
// leg did, FAIL otherwise -- and a FAIL always names the failing step and its
// driver error code.
//
// SELINUX CAVEAT: run from `adb shell`, this executes in the `shell` domain, not
// the `untrusted_app` domain MobileGL actually runs in.  See README.md; the
// summary repeats it.
//
// Standalone: depends on nothing from MobileGL. Build with the NDK toolchain
// (see CMakeLists.txt / build_android.sh), push to /data/local/tmp and run.
//
// Process topology mirrors the target design (client spawns the server as a
// separate process): the probe re-execs /proc/self/exe with --child=<route> and
// hands it one end of a socketpair on fd 3. A plain fork() without exec is not
// usable here -- the Vulkan driver's own threads and device state do not
// survive fork, and both routes need live Vulkan on both sides.

#ifdef __ANDROID__
#  define VK_USE_PLATFORM_ANDROID_KHR 1
#  define PROBE_HAVE_AHB 1
#else
// The probe is an Android deliverable; the host build exists only so the
// T1/T1-gles/T3 harness itself can be validated against a driver that is known
// to implement those routes (lavapipe/llvmpipe), which is what makes a
// device-side FAIL attributable to the driver rather than to this program.
// T0 is Android-only by nature.
#  define PROBE_HAVE_AHB 0
#endif

#include <vulkan/vulkan.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>


// T4 row names, outside the platform guard because BOTH branches report them: the
// Android branch measures, the host branch says what it cannot measure.
static const char* kT4Row = "T4-image-import";
static const char* kT4HostRow = "T4-host-image";

#if PROBE_HAVE_AHB
#  include <android/hardware_buffer.h>
#  include <sys/system_properties.h>
#else
#  define PROP_VALUE_MAX 92
#endif

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

// bionic declares memfd_create only from API 30; the trace APK (MOBILEGL_BUILD_EXTMEM_PROBE)
// builds at the project's API 26, where the syscall itself exists.
#if defined(__ANDROID__) && __ANDROID_API__ < 30
#  include <sys/syscall.h>
static int memfd_create(const char* name, unsigned int flags) {
    return (int)syscall(__NR_memfd_create, name, flags);
}
#endif

// The NDK's libvulkan stub exports Vulkan 1.1 entry points only from API 28; below that
// (the API-26 trace APK) the two 1.1 calls the probe makes go through the instance.
static VkInstance gVk11ShimInstance = VK_NULL_HANDLE;
#if defined(__ANDROID__) && __ANDROID_API__ < 28
static void probeGetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2* out) {
    auto fn = (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(gVk11ShimInstance,
                                                                         "vkGetPhysicalDeviceProperties2");
    if (fn) fn(pd, out);
}
static void probeGetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice pd,
                                                           const VkPhysicalDeviceExternalBufferInfo* info,
                                                           VkExternalBufferProperties* out) {
    auto fn = (PFN_vkGetPhysicalDeviceExternalBufferProperties)vkGetInstanceProcAddr(
        gVk11ShimInstance, "vkGetPhysicalDeviceExternalBufferProperties");
    if (fn) fn(pd, info, out);
}
#  define vkGetPhysicalDeviceProperties2 probeGetPhysicalDeviceProperties2
#  define vkGetPhysicalDeviceExternalBufferProperties probeGetPhysicalDeviceExternalBufferProperties
#endif

// ---------------------------------------------------------------------------
// tiny logging / result table
// ---------------------------------------------------------------------------

static const char* gRole = "parent";

// ro.* on Android, empty elsewhere
static void getProp(const char* name, char* out, size_t n) {
    out[0] = 0;
#if PROBE_HAVE_AHB
    __system_property_get(name, out);
#else
    (void)name;
    (void)n;
#endif
}

static void pr(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void pr(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stdout, "[%s] %s\n", gRole, buf);
    fflush(stdout);
}

struct RouteResult {
    std::string route;
    std::string status;  // OK / PARTIAL / UNSUPPORTED / FAIL / SKIP
    std::string detail;
};
static std::vector<RouteResult> gResults;

static void record(const char* route, const char* status, const std::string& detail) {
    gResults.push_back(RouteResult{route, status, detail});
    pr("RESULT %-34s %-12s %s", route, status, detail.c_str());
}

static std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
static std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return std::string(buf);
}

// Returns a fresh std::string per call: several vkStr() results routinely appear
// in one format call, and a shared static buffer would make all of them show the
// last one.
static std::string vkStr(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
        case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
        case VK_ERROR_FRAGMENTATION: return "VK_ERROR_FRAGMENTATION";
        case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS: return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
        default: return fmt("VkResult(%d)", (int)r);
    }
}

static std::string glErrStr(GLenum e) {
    switch (e) {
        case GL_NO_ERROR: return "GL_NO_ERROR";
        case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
        case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
        case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
        default: return fmt("GL(0x%04x)", (unsigned)e);
    }
}

// drains and returns the last error, so one failing call cannot be blamed on the
// previous one
static GLenum glDrain() {
    GLenum last = GL_NO_ERROR, e;
    while ((e = glGetError()) != GL_NO_ERROR) last = e;
    return last;
}

static std::string readSmallFile(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return fmt("<%s: errno=%d>", path, errno);
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return "<empty>";
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == 0)) buf[--n] = 0;
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// payload patterns
// ---------------------------------------------------------------------------

static const uint64_t kRegion = 4096;      // bytes per verification region
static const uint64_t kRegionCount = 8;    // A..F plus slack
static const uint64_t kDefaultSize = 65536;

// region indices inside the shared allocation
enum {
    REG_A = 0,  // first writer's payload (CPU, allocating side)
    REG_B = 1,  // importer write through the plain host mapping (mmap / AHB lock / vk map)
    REG_C = 2,  // importer write through the imported Vulkan mapping
    REG_D = 3,  // importer write through the imported GL mapping (cross-process)
    REG_E = 4,  // GPU write (vkCmdFillBuffer)
    REG_F = 5,  // in-process GL-import write
};

static void fillPattern(void* p, uint64_t bytes, uint32_t seed) {
    uint8_t* b = (uint8_t*)p;
    for (uint64_t i = 0; i < bytes; ++i) {
        b[i] = (uint8_t)((seed * 2654435761u + (uint32_t)i * 31u + (uint32_t)(i >> 8) * 7u) & 0xFF);
    }
}

// returns -1 on match, else the index of the first mismatching byte
static int64_t checkPattern(const void* p, uint64_t bytes, uint32_t seed) {
    const uint8_t* b = (const uint8_t*)p;
    for (uint64_t i = 0; i < bytes; ++i) {
        uint8_t want = (uint8_t)((seed * 2654435761u + (uint32_t)i * 31u + (uint32_t)(i >> 8) * 7u) & 0xFF);
        if (b[i] != want) return (int64_t)i;
    }
    return -1;
}

// Byte offset of region 0 inside the shared allocation. 0 by default; --regions-at-end
// moves the whole A..F window to the last kRegionCount*kRegion bytes, so a large --size
// proves the far end of the allocation is mapped and GPU-reachable, not only its first
// pages. The parent hands the value to every child with --region-base=.
static uint64_t gRegionBase = 0;
// --hold-ms=N (P11 B2 memory calibration): the T0S client keeps its AHB locked this long after
// the last round, while the parent still holds its Vulkan and GL imports, so an outside sampler
// can read what a held, imported AHB of --size bytes costs in each accounting.
static uint32_t gHoldMs = 0;
static uint64_t regOff(int region) { return gRegionBase + (uint64_t)region * kRegion; }

static void writeRegion(void* base, int region, uint32_t seed) {
    fillPattern((uint8_t*)base + regOff(region), kRegion, seed);
}
static int64_t checkRegion(const void* base, int region, uint32_t seed) {
    return checkPattern((const uint8_t*)base + regOff(region), kRegion, seed);
}

// vkCmdFillBuffer writes a repeating 32-bit word; -1 on match, else the first
// mismatching word index * 4
static int64_t checkFillWord(const void* base, int region, uint32_t word) {
    const uint32_t* w = (const uint32_t*)((const uint8_t*)base + regOff(region));
    for (uint64_t i = 0; i < kRegion / 4; ++i)
        if (w[i] != word) return (int64_t)(i * 4);
    return -1;
}

// ---------------------------------------------------------------------------
// verdict: decisive legs must round-trip bytes, in both directions
// ---------------------------------------------------------------------------

struct Leg {
    std::string name;
    bool decisive = false;   // counted by the verdict; informational legs are not
    bool attempted = false;
    bool readOk = false;     // the allocating side's bytes were visible to the other side
    bool writeOk = false;    // the other side's bytes came back
    std::string fail;        // failing step + driver error code
};

static const char* legVerdict(const std::vector<Leg>& legs, std::string* why) {
    int decisive = 0, round = 0;
    std::string bad;
    for (const Leg& l : legs) {
        if (!l.decisive) continue;
        ++decisive;
        if (l.attempted && l.readOk && l.writeOk) {
            ++round;
        } else {
            if (!bad.empty()) bad += "; ";
            bad += l.name + "=" + (l.fail.empty() ? std::string("no round trip") : l.fail);
        }
    }
    if (why) *why = bad;
    if (decisive == 0) return "SKIP";
    if (round == decisive) return "OK";
    if (round > 0) return "PARTIAL";
    return "FAIL";
}

// compact per-leg trace that stays in the summary line
static std::string legTrace(const std::vector<Leg>& legs) {
    std::string s;
    for (const Leg& l : legs) {
        if (!s.empty()) s += " ";
        const char* v = !l.attempted ? "notrun"
                        : (l.readOk && l.writeOk) ? "rt"
                        : l.readOk                ? "read-only"
                        : l.writeOk               ? "write-only"
                                                  : "no";
        s += l.name + "[" + (l.decisive ? "D" : "i") + "]=" + v;
    }
    return s;
}

// ---------------------------------------------------------------------------
// socket message plumbing
// ---------------------------------------------------------------------------

enum MsgTag : uint32_t {
    MSG_T1_OFFER = 1,
    MSG_T1_RESULT = 2,
    MSG_T0_REQUEST = 3,
    MSG_T0_ALLOC = 4,
    MSG_T0_VERIFY = 5,
    MSG_T0_RESULT = 6,
    MSG_T3_OFFER = 7,
    MSG_T3_RESULT = 8,
    MSG_T1GL_OFFER = 9,
    MSG_T1GL_RESULT = 10,
    MSG_T3C_REQUEST = 11,
    MSG_T3C_READY = 12,
    MSG_T3C_VERIFY = 13,
    MSG_T3C_RESULT = 14,
    MSG_T0S_REQUEST = 15,
    MSG_T0S_ALLOC = 16,
    MSG_T0S_ROUND = 17,
    MSG_T0S_GPUDONE = 18,
    MSG_T0S_CHECK = 19,
    MSG_T0S_DONE = 20,
    MSG_BYE = 99,
};

struct MsgHeader {
    uint32_t tag;
    uint32_t len;
};

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

// header + payload go out in one sendmsg so SCM_RIGHTS lands with the header byte
static bool sendMsg(int sock, uint32_t tag, const void* payload, size_t len, int fdToPass) {
    MsgHeader h{tag, (uint32_t)len};
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
    if (s < 0) return false;
    size_t total = sizeof(h) + len;
    if ((size_t)s == total) return true;
    // partial: finish the tail with plain writes (control data already delivered)
    size_t done = (size_t)s;
    if (done < sizeof(h)) return false;  // should not happen for such small headers
    return writeAll(sock, (const uint8_t*)payload + (done - sizeof(h)), total - done);
}

static bool recvMsg(int sock, uint32_t* tag, void* payload, size_t maxLen, size_t* outLen, int* fdOut) {
    if (fdOut) *fdOut = -1;
    MsgHeader h{};
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
    if (r != (ssize_t)sizeof(h)) return false;

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
    if (h.len > maxLen) return false;
    if (h.len && !readAll(sock, payload, h.len)) return false;
    return true;
}

static void setRecvTimeout(int sock, int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static std::string describeFd(int fd) {
    if (fd < 0) return "no-fd";
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    char link[512];
    ssize_t n = readlink(path, link, sizeof(link) - 1);
    std::string desc;
    if (n > 0) {
        link[n] = 0;
        desc = link;
    } else {
        desc = "<readlink failed>";
    }
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz >= 0) {
        desc += fmt(" size=%lld", (long long)sz);
        lseek(fd, 0, SEEK_SET);
    } else {
        desc += fmt(" lseek-errno=%d(%s)", errno, strerror(errno));
    }
    struct stat st;
    if (fstat(fd, &st) == 0) {
        const char* kind = S_ISREG(st.st_mode) ? "reg" : S_ISCHR(st.st_mode) ? "chr"
                           : S_ISFIFO(st.st_mode) ? "fifo" : S_ISSOCK(st.st_mode) ? "sock" : "other";
        desc += fmt(" kind=%s stsize=%lld", kind, (long long)st.st_size);
    }
    return desc;
}

// ---------------------------------------------------------------------------
// Vulkan context
// ---------------------------------------------------------------------------

struct VkCtx {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps{};
    VkPhysicalDeviceProperties props{};
    uint8_t deviceUUID[VK_UUID_SIZE]{};
    std::vector<std::string> deviceExts;

    bool hasExtMemFd = false;
    bool hasDmaBuf = false;
    bool hasExtMemHost = false;
    bool hasAhb = false;
    bool hasQueueFamilyForeign = false;

    PFN_vkGetMemoryFdKHR pGetMemoryFdKHR = nullptr;
    PFN_vkGetMemoryFdPropertiesKHR pGetMemoryFdPropertiesKHR = nullptr;
#if PROBE_HAVE_AHB
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID pGetAhbProps = nullptr;
#endif
    PFN_vkGetMemoryHostPointerPropertiesEXT pGetHostPtrProps = nullptr;

    VkDeviceSize minImportedHostPointerAlignment = 0;

    bool hasExt(const char* name) const {
        for (const std::string& s : deviceExts)
            if (s == name) return true;
        return false;
    }
};

static bool vkCtxInit(VkCtx& c, bool verbose) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "extmem_probe";
    app.apiVersion = VK_API_VERSION_1_1;

    uint32_t instExtCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, nullptr);
    std::vector<VkExtensionProperties> instExts(instExtCount);
    if (instExtCount) vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, instExts.data());

    std::vector<const char*> wanted;
    auto haveInst = [&](const char* n) {
        for (auto& e : instExts)
            if (!strcmp(e.extensionName, n)) return true;
        return false;
    };
    if (haveInst(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
        wanted.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    if (haveInst(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME))
        wanted.push_back(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)wanted.size();
    ici.ppEnabledExtensionNames = wanted.empty() ? nullptr : wanted.data();

    VkResult r = vkCreateInstance(&ici, nullptr, &c.instance);
    if (r != VK_SUCCESS) {
        pr("vkCreateInstance failed: %s", vkStr(r).c_str());
        return false;
    }
    gVk11ShimInstance = c.instance;

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(c.instance, &n, nullptr);
    if (!n) {
        pr("no physical devices");
        return false;
    }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(c.instance, &n, devs.data());
    c.phys = devs[0];

    VkPhysicalDeviceIDProperties idp{};
    idp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProps{};
    hostProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
    idp.pNext = &hostProps;
    VkPhysicalDeviceProperties2 p2{};
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &idp;
    vkGetPhysicalDeviceProperties2(c.phys, &p2);
    c.props = p2.properties;
    memcpy(c.deviceUUID, idp.deviceUUID, VK_UUID_SIZE);
    c.minImportedHostPointerAlignment = hostProps.minImportedHostPointerAlignment;

    vkGetPhysicalDeviceMemoryProperties(c.phys, &c.memProps);

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(c.phys, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    if (extCount) vkEnumerateDeviceExtensionProperties(c.phys, nullptr, &extCount, exts.data());
    for (auto& e : exts) c.deviceExts.push_back(e.extensionName);

    c.hasExtMemFd = c.hasExt(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    c.hasDmaBuf = c.hasExt(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    c.hasExtMemHost = c.hasExt(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    c.hasAhb = c.hasExt("VK_ANDROID_external_memory_android_hardware_buffer");
    c.hasQueueFamilyForeign = c.hasExt(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);

    uint32_t qf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &qf, nullptr);
    std::vector<VkQueueFamilyProperties> qfp(qf);
    vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &qf, qfp.data());
    c.queueFamily = 0;
    for (uint32_t i = 0; i < qf; ++i) {
        if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            c.queueFamily = i;
            break;
        }
    }

    std::vector<const char*> devExts;
    if (c.hasExt(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME)) devExts.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
    if (c.hasExtMemFd) devExts.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    if (c.hasDmaBuf) devExts.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    if (c.hasExtMemHost) devExts.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    if (c.hasAhb) {
        devExts.push_back("VK_ANDROID_external_memory_android_hardware_buffer");
        if (c.hasQueueFamilyForeign) devExts.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
        if (c.hasExt(VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME))
            devExts.push_back(VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME);
        if (c.hasExt(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME))
            devExts.push_back(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
        if (c.hasExt(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME))
            devExts.push_back(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
        if (c.hasExt(VK_KHR_BIND_MEMORY_2_EXTENSION_NAME))
            devExts.push_back(VK_KHR_BIND_MEMORY_2_EXTENSION_NAME);
        if (c.hasExt(VK_KHR_MAINTENANCE_1_EXTENSION_NAME))
            devExts.push_back(VK_KHR_MAINTENANCE_1_EXTENSION_NAME);
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo q{};
    q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = c.queueFamily;
    q.queueCount = 1;
    q.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &q;
    dci.enabledExtensionCount = (uint32_t)devExts.size();
    dci.ppEnabledExtensionNames = devExts.empty() ? nullptr : devExts.data();

    r = vkCreateDevice(c.phys, &dci, nullptr, &c.device);
    if (r != VK_SUCCESS) {
        pr("vkCreateDevice failed: %s", vkStr(r).c_str());
        return false;
    }

    vkGetDeviceQueue(c.device, c.queueFamily, 0, &c.queue);
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = c.queueFamily;
    VkResult pr_ = vkCreateCommandPool(c.device, &cpi, nullptr, &c.cmdPool);
    if (pr_ != VK_SUCCESS) {
        c.cmdPool = VK_NULL_HANDLE;
        pr("vkCreateCommandPool failed: %s (GPU touch will be skipped)", vkStr(pr_).c_str());
    }

    c.pGetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(c.device, "vkGetMemoryFdKHR");
    c.pGetMemoryFdPropertiesKHR =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(c.device, "vkGetMemoryFdPropertiesKHR");
#if PROBE_HAVE_AHB
    c.pGetAhbProps = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(
        c.device, "vkGetAndroidHardwareBufferPropertiesANDROID");
#endif
    c.pGetHostPtrProps = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(
        c.device, "vkGetMemoryHostPointerPropertiesEXT");

    if (verbose) {
        pr("vulkan device: %s api=%u.%u.%u driverVersion=0x%08x vendor=0x%04x", c.props.deviceName,
           VK_VERSION_MAJOR(c.props.apiVersion), VK_VERSION_MINOR(c.props.apiVersion),
           VK_VERSION_PATCH(c.props.apiVersion), c.props.driverVersion, c.props.vendorID);
        char uuid[64] = {0};
        for (uint32_t i = 0; i < VK_UUID_SIZE; ++i) snprintf(uuid + i * 2, 3, "%02x", c.deviceUUID[i]);
        pr("deviceUUID=%s minImportedHostPointerAlignment=%llu queueFamily=%u", uuid,
           (unsigned long long)c.minImportedHostPointerAlignment, c.queueFamily);
    }
    return true;
}

static void vkCtxDestroy(VkCtx& c) {
    if (c.cmdPool) vkDestroyCommandPool(c.device, c.cmdPool, nullptr);
    if (c.device) vkDestroyDevice(c.device, nullptr);
    if (c.instance) vkDestroyInstance(c.instance, nullptr);
    c.cmdPool = VK_NULL_HANDLE;
    c.device = VK_NULL_HANDLE;
    c.instance = VK_NULL_HANDLE;
}

// index of a memory type in `bits` that has all of `want`, or -1
static int pickMemType(const VkPhysicalDeviceMemoryProperties& mp, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (!(bits & (1u << i))) continue;
        if ((mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    }
    return -1;
}

static const VkBufferUsageFlags kProbeBufferUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

// ---------------------------------------------------------------------------
// GPU touch: prove the shared allocation survives a real GPU access
//
// The GPU copies `readRegion` into a private staging buffer (so a mismatch means
// the GPU could not read what the host/peer wrote) and fills `fillRegion` with a
// known word (so the caller can check, through whichever mapping it is testing,
// that a GPU write lands in the shared pages).  Without this an OK verdict would
// only prove that a map call returned a pointer.
// ---------------------------------------------------------------------------

struct GpuTouch {
    bool ran = false;
    VkResult submitResult = VK_NOT_READY;
    int64_t readMismatch = -3;  // -1 match, -3 never ran
    std::string fail;
};

static GpuTouch gpuTouch(VkCtx& c, VkBuffer buf, int readRegion, uint32_t readSeed, int fillRegion,
                         uint32_t fillWord) {
    GpuTouch g;
    if (buf == VK_NULL_HANDLE || c.cmdPool == VK_NULL_HANDLE || c.queue == VK_NULL_HANDLE) {
        g.fail = "no buffer/queue/command pool for the GPU touch";
        return g;
    }

    // private host-visible staging target for the read-back
    VkBufferCreateInfo sbi{};
    sbi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    sbi.size = kRegion;
    sbi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    VkResult r = vkCreateBuffer(c.device, &sbi, nullptr, &staging);
    if (r != VK_SUCCESS) {
        g.fail = "staging vkCreateBuffer=" + vkStr(r);
        return g;
    }
    VkMemoryRequirements sreq{};
    vkGetBufferMemoryRequirements(c.device, staging, &sreq);
    int sType = pickMemType(c.memProps, sreq.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (sType < 0) {
        vkDestroyBuffer(c.device, staging, nullptr);
        g.fail = fmt("no HOST_VISIBLE|HOST_COHERENT staging type in bits=0x%x", sreq.memoryTypeBits);
        return g;
    }
    VkMemoryAllocateInfo smai{};
    smai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    smai.allocationSize = sreq.size;
    smai.memoryTypeIndex = (uint32_t)sType;
    VkDeviceMemory smem = VK_NULL_HANDLE;
    r = vkAllocateMemory(c.device, &smai, nullptr, &smem);
    if (r != VK_SUCCESS) {
        vkDestroyBuffer(c.device, staging, nullptr);
        g.fail = "staging vkAllocateMemory=" + vkStr(r);
        return g;
    }
    vkBindBufferMemory(c.device, staging, smem, 0);

    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = c.cmdPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    r = vkAllocateCommandBuffers(c.device, &cai, &cmd);
    if (r != VK_SUCCESS) {
        vkFreeMemory(c.device, smem, nullptr);
        vkDestroyBuffer(c.device, staging, nullptr);
        g.fail = "vkAllocateCommandBuffers=" + vkStr(r);
        return g;
    }

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    // host writes are made visible to the device by the queue submit itself for
    // HOST_COHERENT memory, but the shared allocation may be imported and
    // non-coherent, so ask for it explicitly
    VkMemoryBarrier pre{};
    pre.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    pre.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    pre.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &pre, 0, nullptr, 0,
                         nullptr);

    VkBufferCopy copy{};
    copy.srcOffset = (VkDeviceSize)regOff(readRegion);
    copy.dstOffset = 0;
    copy.size = kRegion;
    vkCmdCopyBuffer(cmd, buf, staging, 1, &copy);
    vkCmdFillBuffer(cmd, buf, (VkDeviceSize)regOff(fillRegion), (VkDeviceSize)kRegion, fillWord);

    // device writes must be made visible to the host explicitly
    VkMemoryBarrier post{};
    post.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    post.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    post.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &post, 0, nullptr, 0,
                         nullptr);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    g.submitResult = vkQueueSubmit(c.queue, 1, &si, VK_NULL_HANDLE);
    if (g.submitResult == VK_SUCCESS) {
        VkResult wr = vkQueueWaitIdle(c.queue);
        if (wr != VK_SUCCESS) {
            g.fail = "vkQueueWaitIdle=" + vkStr(wr);
        } else {
            void* sp = nullptr;
            VkResult mr = vkMapMemory(c.device, smem, 0, VK_WHOLE_SIZE, 0, &sp);
            if (mr == VK_SUCCESS && sp) {
                g.readMismatch = checkPattern(sp, kRegion, readSeed);
                vkUnmapMemory(c.device, smem);
                g.ran = true;
                if (g.readMismatch != -1)
                    g.fail = fmt("GPU copy out of the shared allocation mismatched at byte %lld",
                                 (long long)g.readMismatch);
            } else {
                g.fail = "staging vkMapMemory=" + vkStr(mr);
            }
        }
    } else {
        g.fail = "vkQueueSubmit=" + vkStr(g.submitResult);
    }

    vkFreeCommandBuffers(c.device, c.cmdPool, 1, &cmd);
    vkFreeMemory(c.device, smem, nullptr);
    vkDestroyBuffer(c.device, staging, nullptr);
    return g;
}

// ---------------------------------------------------------------------------
// exportable HOST_VISIBLE|HOST_COHERENT allocation + fd
// ---------------------------------------------------------------------------

struct ExportAlloc {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* host = nullptr;
    uint64_t allocationSize = 0;
    uint64_t bufferSize = 0;
    uint32_t memoryTypeIndex = 0;
    uint32_t memoryTypeBits = 0;
    bool dedicated = false;
    int fd = -1;
    bool advertisedExportable = false;
    bool advertisedImportable = false;
    VkResult bindResult = VK_SUCCESS;
    std::string fail;  // empty on success
};

// A failure on a handle type the driver advertised as EXPORTABLE is a driver
// bug (FAIL); the same failure on one it never advertised is simply the route
// not being there (UNSUPPORTED).  One rule, used at every export failure site.
static const char* exportFailStatus(const ExportAlloc& a) {
    return a.advertisedExportable ? "FAIL" : "UNSUPPORTED";
}

static bool exportHostVisible(VkCtx& c, VkExternalMemoryHandleTypeFlagBits ht, uint64_t size, const char* tag,
                              ExportAlloc& a) {
    a.bufferSize = size;

    VkPhysicalDeviceExternalBufferInfo ebi{};
    ebi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
    ebi.usage = kProbeBufferUsage;
    ebi.handleType = ht;
    VkExternalBufferProperties ebp{};
    ebp.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
    vkGetPhysicalDeviceExternalBufferProperties(c.phys, &ebi, &ebp);
    a.advertisedExportable =
        (ebp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
    a.advertisedImportable =
        (ebp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
    a.dedicated =
        (ebp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
    pr("%s advertisedExportable=%d importable=%d dedicatedOnly=%d", tag, (int)a.advertisedExportable,
       (int)a.advertisedImportable, (int)a.dedicated);

    VkExternalMemoryBufferCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    ext.handleTypes = ht;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.pNext = &ext;
    bci.size = size;
    bci.usage = kProbeBufferUsage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult r = vkCreateBuffer(c.device, &bci, nullptr, &a.buf);
    if (r != VK_SUCCESS) {
        a.buf = VK_NULL_HANDLE;
        a.fail = "vkCreateBuffer(external)=" + vkStr(r);
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(c.device, a.buf, &req);
    a.allocationSize = req.size;
    a.memoryTypeBits = req.memoryTypeBits;
    int typeIdx = pickMemType(c.memProps, req.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (typeIdx < 0) {
        a.fail = fmt("no HOST_VISIBLE|HOST_COHERENT memory type in bits=0x%x", req.memoryTypeBits);
        return false;
    }
    a.memoryTypeIndex = (uint32_t)typeIdx;
    pr("%s memReq size=%llu align=%llu typeBits=0x%x -> type %d", tag, (unsigned long long)req.size,
       (unsigned long long)req.alignment, req.memoryTypeBits, typeIdx);

    VkExportMemoryAllocateInfo exportInfo{};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exportInfo.handleTypes = ht;
    VkMemoryDedicatedAllocateInfo ded{};
    ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    ded.buffer = a.buf;
    if (a.dedicated) exportInfo.pNext = &ded;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &exportInfo;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = a.memoryTypeIndex;

    r = vkAllocateMemory(c.device, &mai, nullptr, &a.mem);
    if (r != VK_SUCCESS) {
        a.mem = VK_NULL_HANDLE;
        a.fail = fmt("vkAllocateMemory(export)=%s (advertisedExportable=%d)", vkStr(r).c_str(),
                     (int)a.advertisedExportable);
        return false;
    }
    a.bindResult = vkBindBufferMemory(c.device, a.buf, a.mem, 0);
    if (a.bindResult != VK_SUCCESS) pr("%s vkBindBufferMemory=%s (continuing)", tag, vkStr(a.bindResult).c_str());

    r = vkMapMemory(c.device, a.mem, 0, VK_WHOLE_SIZE, 0, &a.host);
    if (r != VK_SUCCESS || !a.host) {
        a.host = nullptr;
        a.fail = "exporter-side vkMapMemory=" + vkStr(r);
        return false;
    }

    VkMemoryGetFdInfoKHR gfi{};
    gfi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    gfi.memory = a.mem;
    gfi.handleType = ht;
    r = c.pGetMemoryFdKHR(c.device, &gfi, &a.fd);
    if (r != VK_SUCCESS || a.fd < 0) {
        a.fd = -1;
        a.fail = fmt("vkGetMemoryFdKHR=%s (advertisedExportable=%d)", vkStr(r).c_str(), (int)a.advertisedExportable);
        return false;
    }
    pr("%s exported fd=%d -> %s", tag, a.fd, describeFd(a.fd).c_str());
    return true;
}

static void freeExportAlloc(VkCtx& c, ExportAlloc& a) {
    if (a.fd >= 0) close(a.fd);
    if (a.host) vkUnmapMemory(c.device, a.mem);
    if (a.mem) vkFreeMemory(c.device, a.mem, nullptr);
    if (a.buf) vkDestroyBuffer(c.device, a.buf, nullptr);
    a.fd = -1;
    a.host = nullptr;
    a.mem = VK_NULL_HANDLE;
    a.buf = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// GLES / EGL context
// ---------------------------------------------------------------------------

struct GlCtx {
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface surf = EGL_NO_SURFACE;
    std::vector<std::string> glExts;
    std::vector<std::string> eglExts;
    std::string vendor, renderer, version;

    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC pGetNativeClientBuffer = nullptr;
    PFNGLBUFFERSTORAGEEXTERNALEXTPROC pBufferStorageExternal = nullptr;

    // GL_EXT_memory_object / GL_EXT_memory_object_fd
    PFNGLCREATEMEMORYOBJECTSEXTPROC pCreateMemoryObjects = nullptr;
    PFNGLDELETEMEMORYOBJECTSEXTPROC pDeleteMemoryObjects = nullptr;
    PFNGLMEMORYOBJECTPARAMETERIVEXTPROC pMemoryObjectParameteriv = nullptr;
    PFNGLBUFFERSTORAGEMEMEXTPROC pBufferStorageMem = nullptr;
    PFNGLIMPORTMEMORYFDEXTPROC pImportMemoryFd = nullptr;
    PFNGLGETUNSIGNEDBYTEI_VEXTPROC pGetUnsignedBytei_v = nullptr;
    void (GL_APIENTRYP pMemoryBarrier)(GLbitfield) = nullptr;

    bool hasGl(const char* n) const {
        for (auto& s : glExts)
            if (s == n) return true;
        return false;
    }
    bool hasEgl(const char* n) const {
        for (auto& s : eglExts)
            if (s == n) return true;
        return false;
    }
    // everything the T1 GLES leg needs
    bool canImportFd() const {
        return hasGl("GL_EXT_memory_object") && hasGl("GL_EXT_memory_object_fd") && pCreateMemoryObjects &&
               pImportMemoryFd && pBufferStorageMem;
    }
    std::string missingForImportFd() const {
        return fmt("GL_EXT_memory_object=%d GL_EXT_memory_object_fd=%d GL_EXT_buffer_storage=%d "
                   "glCreateMemoryObjectsEXT=%d glImportMemoryFdEXT=%d glBufferStorageMemEXT=%d",
                   (int)hasGl("GL_EXT_memory_object"), (int)hasGl("GL_EXT_memory_object_fd"),
                   (int)hasGl("GL_EXT_buffer_storage"), (int)(pCreateMemoryObjects != nullptr),
                   (int)(pImportMemoryFd != nullptr), (int)(pBufferStorageMem != nullptr));
    }
};

static void splitExts(const char* s, std::vector<std::string>& out) {
    if (!s) return;
    std::string cur;
    for (const char* p = s; *p; ++p) {
        if (*p == ' ') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(*p);
        }
    }
    if (!cur.empty()) out.push_back(cur);
}

static bool glCtxInit(GlCtx& g) {
    g.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g.dpy == EGL_NO_DISPLAY) {
        pr("eglGetDisplay failed");
        return false;
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(g.dpy, &major, &minor)) {
        pr("eglInitialize failed 0x%04x", eglGetError());
        return false;
    }
    pr("EGL %d.%d vendor=%s", major, minor, eglQueryString(g.dpy, EGL_VENDOR));
    splitExts(eglQueryString(g.dpy, EGL_EXTENSIONS), g.eglExts);
    const char* clientExts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    splitExts(clientExts, g.eglExts);

    const EGLint cfgAttr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                              EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                              EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                              EGL_NONE};
    EGLConfig cfg = nullptr;
    EGLint numCfg = 0;
    if (!eglChooseConfig(g.dpy, cfgAttr, &cfg, 1, &numCfg) || numCfg == 0) {
        pr("eglChooseConfig failed 0x%04x", eglGetError());
        return false;
    }
    const EGLint pbAttr[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    g.surf = eglCreatePbufferSurface(g.dpy, cfg, pbAttr);
    if (g.surf == EGL_NO_SURFACE) {
        pr("eglCreatePbufferSurface failed 0x%04x", eglGetError());
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint versions[][2] = {{3, 2}, {3, 1}, {3, 0}};
    for (auto& v : versions) {
        const EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION, v[0], EGL_CONTEXT_MINOR_VERSION, v[1], EGL_NONE};
        g.ctx = eglCreateContext(g.dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
        if (g.ctx != EGL_NO_CONTEXT) break;
    }
    if (g.ctx == EGL_NO_CONTEXT) {
        pr("eglCreateContext failed 0x%04x", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(g.dpy, g.surf, g.surf, g.ctx)) {
        pr("eglMakeCurrent failed 0x%04x", eglGetError());
        return false;
    }

    const char* vd = (const char*)glGetString(GL_VENDOR);
    const char* rd = (const char*)glGetString(GL_RENDERER);
    const char* vr = (const char*)glGetString(GL_VERSION);
    g.vendor = vd ? vd : "";
    g.renderer = rd ? rd : "";
    g.version = vr ? vr : "";

    GLint numExt = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &numExt);
    for (GLint i = 0; i < numExt; ++i) {
        const char* e = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e) g.glExts.push_back(e);
    }
    if (g.glExts.empty()) splitExts((const char*)glGetString(GL_EXTENSIONS), g.glExts);

    g.pGetNativeClientBuffer =
        (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    g.pBufferStorageExternal =
        (PFNGLBUFFERSTORAGEEXTERNALEXTPROC)eglGetProcAddress("glBufferStorageExternalEXT");
    g.pCreateMemoryObjects = (PFNGLCREATEMEMORYOBJECTSEXTPROC)eglGetProcAddress("glCreateMemoryObjectsEXT");
    g.pDeleteMemoryObjects = (PFNGLDELETEMEMORYOBJECTSEXTPROC)eglGetProcAddress("glDeleteMemoryObjectsEXT");
    g.pMemoryObjectParameteriv =
        (PFNGLMEMORYOBJECTPARAMETERIVEXTPROC)eglGetProcAddress("glMemoryObjectParameterivEXT");
    g.pBufferStorageMem = (PFNGLBUFFERSTORAGEMEMEXTPROC)eglGetProcAddress("glBufferStorageMemEXT");
    g.pImportMemoryFd = (PFNGLIMPORTMEMORYFDEXTPROC)eglGetProcAddress("glImportMemoryFdEXT");
    g.pGetUnsignedBytei_v = (PFNGLGETUNSIGNEDBYTEI_VEXTPROC)eglGetProcAddress("glGetUnsignedBytei_vEXT");
    // ES 3.1 core, but loaded dynamically so an ES 3.0 context still links
    g.pMemoryBarrier = (void(GL_APIENTRYP)(GLbitfield))eglGetProcAddress("glMemoryBarrier");
    return true;
}

static void glCtxDestroy(GlCtx& g) {
    if (g.dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(g.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g.ctx != EGL_NO_CONTEXT) eglDestroyContext(g.dpy, g.ctx);
        if (g.surf != EGL_NO_SURFACE) eglDestroySurface(g.dpy, g.surf);
        eglTerminate(g.dpy);
    }
    g.dpy = EGL_NO_DISPLAY;
}

// GL_DEVICE_UUID_EXT must equal the Vulkan deviceUUID for an fd import to be
// legal; a mismatch is the usual reason glImportMemoryFdEXT declines.
static std::string glDeviceUuidReport(GlCtx& g, const uint8_t* vkUuid, bool* matched) {
    if (matched) *matched = false;
    if (!g.hasGl("GL_EXT_memory_object") || !g.pGetUnsignedBytei_v) return "unavailable";
    GLint n = 0;
    glDrain();
    glGetIntegerv(GL_NUM_DEVICE_UUIDS_EXT, &n);
    if (glDrain() != GL_NO_ERROR || n <= 0) return "GL_NUM_DEVICE_UUIDS_EXT unreadable";
    std::string out;
    for (GLint i = 0; i < n; ++i) {
        GLubyte uuid[GL_UUID_SIZE_EXT] = {0};
        g.pGetUnsignedBytei_v(GL_DEVICE_UUID_EXT, (GLuint)i, uuid);
        char hex[2 * GL_UUID_SIZE_EXT + 1] = {0};
        for (int k = 0; k < GL_UUID_SIZE_EXT; ++k) snprintf(hex + k * 2, 3, "%02x", uuid[k]);
        if (!out.empty()) out += ",";
        out += hex;
        if (!memcmp(uuid, vkUuid, GL_UUID_SIZE_EXT) && matched) *matched = true;
    }
    return out;
}

// ---------------------------------------------------------------------------
// GL side of T1: import an exported fd as GL buffer storage and map it
// ---------------------------------------------------------------------------

struct GlImport {
    bool memObjOk = false;
    bool storageOk = false;
    bool mapOk = false;
    bool persistentCoherent = false;  // the PERSISTENT|COHERENT map is what AcquirePersistentMap needs
    GLenum errImport = GL_NO_ERROR;
    GLenum errStorage = GL_NO_ERROR;
    GLenum errMap = GL_NO_ERROR;   // error of the PERSISTENT|COHERENT attempt
    GLenum errMap2 = GL_NO_ERROR;  // error of the plain MAP_READ|MAP_WRITE fallback
    GLuint memObj = 0;
    GLuint buf = 0;
    void* ptr = nullptr;
    uint64_t mappedSize = 0;
    std::string variant;  // which phrasing the driver accepted
    std::string ladder;   // every phrasing tried, with its error
    std::string fail;
};

// Borrows `fd` (dups it per attempt; the caller keeps ownership).
//
// Drivers disagree about how this call has to be phrased -- whether the memory
// object must be flagged dedicated, and whether the buffer may be smaller than
// the imported allocation -- and a probe that tried only one phrasing would
// report a driver preference as a missing capability.  So walk the ladder and
// report which rung the driver accepted.
static void glImportFdBuffer(GlCtx& g, int fd, uint64_t allocationSize, uint64_t bufferSize, bool dedicatedHint,
                             GlImport& o) {
    struct Attempt {
        bool dedicated;
        uint64_t importSize;
        uint64_t storageSize;
    };
    std::vector<Attempt> attempts;
    // some drivers validate the imported size against the fd's own size rather
    // than against the exporter's VkMemoryRequirements::size
    off_t fdSize = lseek(fd, 0, SEEK_END);
    if (fdSize > 0) lseek(fd, 0, SEEK_SET);
    std::vector<uint64_t> importSizes{allocationSize};
    if (fdSize > 0 && (uint64_t)fdSize != allocationSize) importSizes.push_back((uint64_t)fdSize);
    for (uint64_t imp : importSizes) {
        for (bool ded : {dedicatedHint, !dedicatedHint}) {
            attempts.push_back(Attempt{ded, imp, bufferSize});
            if (imp != bufferSize) attempts.push_back(Attempt{ded, imp, imp});
        }
    }
    glGenBuffers(1, &o.buf);

    for (const Attempt& at : attempts) {
        std::string tag = fmt("[ded=%d imp=%llu store=%llu]", (int)at.dedicated, (unsigned long long)at.importSize,
                              (unsigned long long)at.storageSize);
        glDrain();
        GLuint mo = 0;
        g.pCreateMemoryObjects(1, &mo);
        if (at.dedicated && g.pMemoryObjectParameteriv) {
            GLint yes = GL_TRUE;
            g.pMemoryObjectParameteriv(mo, GL_DEDICATED_MEMORY_OBJECT_EXT, &yes);
            glDrain();
        }
        int dupFd = dup(fd);
        g.pImportMemoryFd(mo, (GLuint64)at.importSize, GL_HANDLE_TYPE_OPAQUE_FD_EXT, (GLint)dupFd);
        GLenum eImport = glDrain();
        if (eImport != GL_NO_ERROR) {
            // Deliberately NOT closed: EXT_memory_object_fd transfers ownership of
            // the fd to the implementation and does not say whether that still
            // happens when the import fails, and Mesa closes it either way.  A
            // double close would land on whatever fd the allocator handed out
            // next -- the socket, in this program.  At most a handful of rungs
            // run, so leaking the dup is the cheap, safe side of that trade.
            (void)dupFd;
            if (g.pDeleteMemoryObjects) g.pDeleteMemoryObjects(1, &mo);
            glDrain();
            if (!o.memObjOk) o.errImport = eImport;
            o.ladder += tag + "import=" + glErrStr(eImport) + " ";
            continue;
        }
        o.memObjOk = true;
        o.errImport = GL_NO_ERROR;

        glBindBuffer(GL_ARRAY_BUFFER, o.buf);
        glDrain();
        g.pBufferStorageMem(GL_ARRAY_BUFFER, (GLsizeiptr)at.storageSize, mo, 0);
        GLenum eStorage = glDrain();
        o.ladder += tag + "storage=" + glErrStr(eStorage) + " ";
        if (eStorage != GL_NO_ERROR) {
            o.errStorage = eStorage;
            if (g.pDeleteMemoryObjects) g.pDeleteMemoryObjects(1, &mo);
            glDrain();
            // storage is immutable once it takes, so a failed attempt needs a
            // fresh buffer name before the next rung
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glDeleteBuffers(1, &o.buf);
            glGenBuffers(1, &o.buf);
            continue;
        }
        o.memObj = mo;
        o.errStorage = GL_NO_ERROR;
        o.storageOk = true;
        o.mappedSize = at.storageSize;
        o.variant = tag;
        break;
    }

    if (!o.memObjOk) {
        o.fail = "glImportMemoryFdEXT -> " + glErrStr(o.errImport) + " (ladder: " + o.ladder + ")";
        return;
    }
    if (!o.storageOk) {
        o.fail = "glBufferStorageMemEXT -> " + glErrStr(o.errStorage) + " (ladder: " + o.ladder + ")";
        return;
    }
    bufferSize = o.mappedSize;

    o.ptr = glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bufferSize,
                             GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT |
                                 GL_MAP_COHERENT_BIT_EXT);
    o.errMap = glDrain();
    if (o.ptr) {
        o.mapOk = true;
        o.persistentCoherent = true;
        return;
    }
    // A driver may back the storage but refuse the persistent/coherent flags --
    // that is exactly the T1/T2 distinction for DirectGLES, so it is reported
    // separately rather than folded into one failure.
    o.ptr = glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bufferSize, GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
    o.errMap2 = glDrain();
    if (o.ptr) {
        o.mapOk = true;
        o.fail = "PERSISTENT|COHERENT map refused (" + glErrStr(o.errMap) + "), only a scoped map works";
    } else {
        o.fail = "glMapBufferRange persistent -> " + glErrStr(o.errMap) + ", plain -> " + glErrStr(o.errMap2);
    }
}

static void glImportPublish(GlCtx& g, GlImport& o) {
    if (!o.mapOk) return;
    if (o.persistentCoherent) {
        if (g.pMemoryBarrier) g.pMemoryBarrier(GL_ALL_BARRIER_BITS);
    } else {
        glUnmapBuffer(GL_ARRAY_BUFFER);
        o.ptr = nullptr;
    }
    glFinish();
}

static void glImportRelease(GlCtx& g, GlImport& o) {
    if (o.buf) {
        glBindBuffer(GL_ARRAY_BUFFER, o.buf);
        if (o.ptr) glUnmapBuffer(GL_ARRAY_BUFFER);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDeleteBuffers(1, &o.buf);
    }
    if (o.memObj && g.pDeleteMemoryObjects) g.pDeleteMemoryObjects(1, &o.memObj);
    glDrain();
    o.buf = 0;
    o.memObj = 0;
    o.ptr = nullptr;
}

// ---------------------------------------------------------------------------
// child spawn
// ---------------------------------------------------------------------------

// Spawns /proc/self/exe --child=<route>; the child gets `sock` on fd 3.
static pid_t spawnChild(const char* route, int* parentSock) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        pr("socketpair failed errno=%d", errno);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        pr("fork failed errno=%d", errno);
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    if (pid == 0) {
        close(sv[0]);
        if (sv[1] != 3) {
            dup2(sv[1], 3);
            close(sv[1]);
        }
        char arg[64];
        snprintf(arg, sizeof(arg), "--child=%s", route);
        char base[64];
        snprintf(base, sizeof(base), "--region-base=%llu", (unsigned long long)gRegionBase);
        char self[512];
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n <= 0) _exit(90);
        self[n] = 0;
        char hold[64];
        snprintf(hold, sizeof(hold), "--hold-ms=%u", gHoldMs);
        char* argv[] = {self, arg, base, hold, nullptr};
        execv(self, argv);
        _exit(91);
    }
    close(sv[1]);
    *parentSock = sv[0];
    setRecvTimeout(sv[0], 60);
    return pid;
}

// Bounded: a child wedged inside a driver call must not hold the probe (and the
// device) forever.  Poll for a few seconds, then kill it and report that.
static std::string reapChild(pid_t pid) {
    int status = 0;
    bool killed = false;
    for (int i = 0; i < 100; ++i) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            if (WIFEXITED(status)) return fmt("child exit=%d%s", WEXITSTATUS(status), killed ? " (killed)" : "");
            if (WIFSIGNALED(status)) return fmt("child signal=%d%s", WTERMSIG(status), killed ? " (killed)" : "");
            return "child ?";
        }
        if (w < 0) return fmt("waitpid errno=%d", errno);
        if (i == 60 && !killed) {
            kill(pid, SIGKILL);
            killed = true;
        }
        usleep(50000);
    }
    kill(pid, SIGKILL);
    if (waitpid(pid, &status, 0) < 0) return fmt("child unreaped, waitpid errno=%d", errno);
    return fmt("child killed after hang (signal=%d)", WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

// ---------------------------------------------------------------------------
// wire payloads
// ---------------------------------------------------------------------------

struct T1Offer {
    uint64_t allocationSize;
    uint64_t bufferSize;
    uint32_t handleType;   // VkExternalMemoryHandleTypeFlagBits used to export
    uint32_t seedA;        // pattern the parent wrote in REG_A
    uint32_t seedB;        // pattern the child must write in REG_B (via mmap)
    uint32_t seedC;        // pattern the child must write in REG_C (via imported vkMapMemory)
    uint32_t memoryTypeIndex;
    uint32_t memoryTypeBits;
    uint32_t gpuWord;      // word the GPU filled REG_E with
    uint32_t gpuRan;       // 0 -> REG_E carries nothing, do not check it
};

struct T1Result {
    int32_t gotFd;
    int32_t mmapOk;
    int32_t mmapErrno;
    int64_t mmapMismatch;       // -1 == data matched
    int64_t mmapPatternOffset;  // where the exporter's payload really starts in the mapping, -1 = not found
    int64_t mmapGpuMismatch;    // REG_E through the plain mapping (-2 = not checked)
    int32_t vkInitOk;
    int32_t fdPropsResult;  // VkResult of vkGetMemoryFdPropertiesKHR
    uint32_t fdMemoryTypeBits;
    int32_t importResult;   // VkResult of vkAllocateMemory with the import struct
    int32_t bindResult;
    int32_t mapResult;
    int64_t vkMismatch;     // -1 == data matched
    int64_t vkGpuMismatch;  // REG_E through the imported mapping (-2 = not checked)
    int32_t wroteB;
    int32_t wroteC;
    char note[384];
};

struct T1GlOffer {
    uint64_t allocationSize;
    uint64_t bufferSize;
    uint32_t seedA;
    uint32_t seedD;   // the child writes REG_D through the imported GL mapping
    uint32_t gpuWord;
    uint32_t gpuRan;
    uint32_t dedicated;
};

struct T1GlResult {
    int32_t glInitOk;
    int32_t haveExts;
    int32_t memObjOk;
    int32_t storageOk;
    int32_t mapOk;
    int32_t persistentCoherent;
    uint32_t errImport, errStorage, errMap, errMap2;
    int64_t mismatchA;
    int64_t mismatchGpu;
    int32_t wroteD;
    char note[640];
};

struct T0Request {
    uint64_t size;
    uint32_t seedA;  // pattern the child writes through AHardwareBuffer_lock
};

struct T0Alloc {
    int32_t allocOk;
    int32_t allocErr;
    uint64_t size;
    uint32_t stride;
    char note[192];
};

struct T0Verify {
    uint32_t seedB;        // parent wrote REG_B through the imported vkMapMemory
    uint32_t seedC;        // parent wrote REG_C through the imported GL mapping
    uint32_t seedD;        // parent wrote REG_D through AHardwareBuffer_lock
    uint32_t gpuWord;      // the GPU filled REG_E with this
    uint32_t writtenMask;  // bit0=B bit1=C bit2=D bit3=E(gpu)
};

struct T0Result {
    int32_t lockOk;
    int32_t lockErr;
    int64_t mismatchB;
    int64_t mismatchC;
    int64_t mismatchD;
    int64_t mismatchE;
    char note[192];
};

// T0S (sustained lock): the client locks the AHB once and never unlocks it while the
// server's GPU reads what the client's CPU wrote and writes back, round after round.
struct T0SRequest {
    uint64_t size;
    uint32_t rounds;
    uint32_t seedBase;
};

struct T0SRound {
    uint32_t round;
    uint32_t seedA;  // the client CPU wrote REG_A with this pattern through its held lock
};

struct T0SGpuDone {
    uint32_t round;
    uint32_t vkWord;  // the server's Vulkan GPU filled REG_E with this word
    uint32_t glWord;  // the server's GL compute dispatch filled REG_F with this word
    uint32_t mask;    // bit0 = vk fill ran, bit1 = gl fill ran
};

struct T0SCheck {
    uint32_t round;
    int32_t pad;
    int64_t mismatchE;  // through the client's still-held lock pointer, -2 = not checked
    int64_t mismatchF;
};

struct T0SDone {
    int32_t lockRc;
    int32_t unlockRc;
    int32_t relockRc;
    int32_t pad;
    int64_t relockMismatchE;  // after unlock + a fresh lock: the last GPU writes, -2 = not checked
    int64_t relockMismatchF;
    char note[160];
};

struct T3Offer {
    uint64_t size;
    uint32_t seedA;
    uint32_t seedB;
    uint32_t gpuWord;
    uint32_t gpuRan;
};

struct T3Result {
    int32_t mmapOk;
    int32_t mmapErrno;
    int64_t mismatch;
    int64_t gpuMismatch;
    char note[192];
};

// T3 in the direction that makes it a tier: the CLIENT allocates, the SERVER
// imports the client's host pointer.
struct T3cRequest {
    uint64_t size;   // must be a multiple of minImportedHostPointerAlignment
    uint32_t seedA;  // the child writes REG_A
};

struct T3cReady {
    int32_t ok;
    int32_t err;
    uint64_t size;
    char note[160];
};

struct T3cVerify {
    uint32_t seedB;    // the parent wrote REG_B through the imported VkDeviceMemory
    uint32_t gpuWord;  // the parent's GPU filled REG_E
    uint32_t mask;     // bit0=B bit1=E
};

struct T3cResult {
    int64_t mismatchB;
    int64_t mismatchE;
    char note[160];
};

// ---------------------------------------------------------------------------
// Phase A: enumeration
// ---------------------------------------------------------------------------

static std::string memFlagStr(VkMemoryPropertyFlags f) {
    std::string s;
    if (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) s += "DEVICE_LOCAL ";
    if (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) s += "HOST_VISIBLE ";
    if (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) s += "HOST_COHERENT ";
    if (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) s += "HOST_CACHED ";
    if (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) s += "LAZY ";
    if (f & VK_MEMORY_PROPERTY_PROTECTED_BIT) s += "PROTECTED ";
    return s;
}

// The whole point of this probe is what the driver does in the process MobileGL
// runs in.  `adb shell` is not that process: it is the `shell` SELinux domain,
// which has access to device nodes and ashmem/dmabuf rules that `untrusted_app`
// does not necessarily share.  Print the domain we actually got so a later app
// run can be compared against it.
static const char* kDomainCaveat =
    "run context is `adb shell` (SELinux domain u:r:shell:s0), NOT the untrusted_app "
    "domain MobileGL runs in; per-domain SELinux rules can reject a route that works here";

static void printRunContext() {
    std::string sec = readSmallFile("/proc/self/attr/current");
    pr("=== run context ===");
    pr("uid=%d gid=%d pid=%d selinux=%s", (int)getuid(), (int)getgid(), (int)getpid(), sec.c_str());
    pr("CAVEAT: %s", kDomainCaveat);
    pr("        to answer the question for the real domain, run this binary from the app "
       "process (spike A's trace-app hook) rather than from adb shell -- see README.md");
}

static void reportExternalBufferCaps(VkCtx& c, VkExternalMemoryHandleTypeFlagBits ht, const char* name) {
    VkPhysicalDeviceExternalBufferInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
    info.usage = kProbeBufferUsage;
    info.handleType = ht;
    VkExternalBufferProperties out{};
    out.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
    vkGetPhysicalDeviceExternalBufferProperties(c.phys, &info, &out);
    const VkExternalMemoryProperties& p = out.externalMemoryProperties;
    std::string feat;
    if (p.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) feat += "DEDICATED_ONLY ";
    if (p.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) feat += "EXPORTABLE ";
    if (p.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) feat += "IMPORTABLE ";
    if (feat.empty()) feat = "<none>";
    pr("  externalBuffer[%s]: features=%s exportFrom=0x%x compatible=0x%x", name, feat.c_str(),
       p.exportFromImportedHandleTypes, p.compatibleHandleTypes);
}

static void phaseEnumerate(VkCtx& c, GlCtx& g, bool glOk) {
    pr("=== phase A: capability enumeration ===");

    char model[PROP_VALUE_MAX] = {0}, dev[PROP_VALUE_MAX] = {0}, rel[PROP_VALUE_MAX] = {0};
    getProp("ro.product.model", model, sizeof(model));
    getProp("ro.product.device", dev, sizeof(dev));
    getProp("ro.build.version.release", rel, sizeof(rel));
    pr("device: model=%s device=%s android=%s", model, dev, rel);

    pr("vulkan: %s (api %u.%u.%u, driver 0x%08x, vendor 0x%04x)", c.props.deviceName,
       VK_VERSION_MAJOR(c.props.apiVersion), VK_VERSION_MINOR(c.props.apiVersion),
       VK_VERSION_PATCH(c.props.apiVersion), c.props.driverVersion, c.props.vendorID);

    struct {
        const char* name;
        bool present;
    } probe[] = {
        {VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, c.hasExt(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME)},
        {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, c.hasExtMemFd},
        {VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME, c.hasDmaBuf},
        {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, c.hasExtMemHost},
        {"VK_ANDROID_external_memory_android_hardware_buffer", c.hasAhb},
        {VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME, c.hasQueueFamilyForeign},
        {VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, c.hasExt(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME)},
    };
    for (auto& e : probe) pr("  VK ext %-58s %s", e.name, e.present ? "YES" : "no");

    pr("memory types (%u):", c.memProps.memoryTypeCount);
    for (uint32_t i = 0; i < c.memProps.memoryTypeCount; ++i) {
        const VkMemoryType& mt = c.memProps.memoryTypes[i];
        pr("  [%u] heap=%u size=%lluMiB flags=%s", i, mt.heapIndex,
           (unsigned long long)(c.memProps.memoryHeaps[mt.heapIndex].size >> 20),
           memFlagStr(mt.propertyFlags).c_str());
    }

    // Only query handle types the driver actually claims: a handle type whose
    // extension is absent is not required to be understood by this entry point.
    reportExternalBufferCaps(c, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, "OPAQUE_FD");
    if (c.hasDmaBuf) reportExternalBufferCaps(c, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, "DMA_BUF");
    if (c.hasExtMemHost)
        reportExternalBufferCaps(c, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, "HOST_ALLOCATION");
    if (c.hasAhb)
        reportExternalBufferCaps(c, VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
                                 "AHARDWAREBUFFER");

    if (!glOk) {
        pr("gles: context unavailable, GL extension probe skipped");
        record("A-gles-context", "FAIL", "no headless EGL context; every GLES leg is unanswered");
        return;
    }
    pr("gles: vendor=%s renderer=%s version=%s", g.vendor.c_str(), g.renderer.c_str(), g.version.c_str());
    const char* glWanted[] = {
        "GL_EXT_memory_object",       "GL_EXT_memory_object_fd", "GL_EXT_external_buffer",
        "GL_EXT_buffer_storage",      "GL_OES_EGL_image",        "GL_OES_EGL_image_external",
        "GL_OES_EGL_image_external_essl3", "GL_EXT_memory_object_win32",
    };
    for (const char* n : glWanted) pr("  GL ext %-40s %s", n, g.hasGl(n) ? "YES" : "no");
    const char* eglWanted[] = {
        "EGL_ANDROID_get_native_client_buffer", "EGL_KHR_image_base", "EGL_ANDROID_image_native_buffer",
        "EGL_EXT_image_dma_buf_import",         "EGL_KHR_gl_texture_2D_image",
    };
    for (const char* n : eglWanted) pr("  EGL ext %-40s %s", n, g.hasEgl(n) ? "YES" : "no");
    pr("  eglGetNativeClientBufferANDROID=%p glBufferStorageExternalEXT=%p", (void*)g.pGetNativeClientBuffer,
       (void*)g.pBufferStorageExternal);
    pr("  glCreateMemoryObjectsEXT=%p glImportMemoryFdEXT=%p glBufferStorageMemEXT=%p glMemoryObjectParameterivEXT=%p",
       (void*)g.pCreateMemoryObjects, (void*)g.pImportMemoryFd, (void*)g.pBufferStorageMem,
       (void*)g.pMemoryObjectParameteriv);

    bool uuidMatch = false;
    std::string glUuid = glDeviceUuidReport(g, c.deviceUUID, &uuidMatch);
    char vkUuid[2 * VK_UUID_SIZE + 1] = {0};
    for (uint32_t i = 0; i < VK_UUID_SIZE; ++i) snprintf(vkUuid + i * 2, 3, "%02x", c.deviceUUID[i]);
    pr("  GL_DEVICE_UUID_EXT=%s vkDeviceUUID=%s match=%d", glUuid.c_str(), vkUuid, (int)uuidMatch);
}

// ---------------------------------------------------------------------------
// T1 parent: server exports its own allocation
// ---------------------------------------------------------------------------

static void runT1Parent(VkCtx& c, VkExternalMemoryHandleTypeFlagBits handleType, const char* routeName,
                        uint64_t size) {
    if (!c.hasExtMemFd || !c.pGetMemoryFdKHR) {
        record(routeName, "UNSUPPORTED", "VK_KHR_external_memory_fd absent");
        return;
    }
    if (handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT && !c.hasDmaBuf) {
        record(routeName, "UNSUPPORTED", "VK_EXT_external_memory_dma_buf absent");
        return;
    }

    ExportAlloc a;
    std::string tag = fmt("T1[%s]", routeName);
    if (!exportHostVisible(c, handleType, size, tag.c_str(), a)) {
        // one rule for every export-path failure, advertised or not
        record(routeName, exportFailStatus(a), a.fail);
        freeExportAlloc(c, a);
        return;
    }

    const uint32_t seedA = 0xA5A50001u, seedB = 0xB0B00002u, seedC = 0xC0C00003u;
    const uint32_t gpuWord = 0x5EED1234u;
    memset(a.host, 0, (size_t)size);
    writeRegion(a.host, REG_A, seedA);

    // real GPU access on the shared allocation, before the handover: the GPU
    // reads REG_A (host-written) and writes REG_E, which the importer then has
    // to see through its own mapping.
    GpuTouch gt = gpuTouch(c, a.buf, REG_A, seedA, REG_E, gpuWord);
    int64_t gpuFillSeenHere = gt.ran ? checkFillWord(a.host, REG_E, gpuWord) : -3;
    pr("%s gpuTouch ran=%d submit=%s readMismatch=%lld fillSeenByExporter=%lld %s", tag.c_str(), (int)gt.ran,
       vkStr(gt.submitResult).c_str(), (long long)gt.readMismatch, (long long)gpuFillSeenHere, gt.fail.c_str());

    int sock = -1;
    pid_t pid = spawnChild("t1", &sock);
    if (pid < 0) {
        record(routeName, "FAIL", "spawnChild failed");
        freeExportAlloc(c, a);
        return;
    }

    T1Offer offer{};
    offer.allocationSize = a.allocationSize;
    offer.bufferSize = size;
    offer.handleType = (uint32_t)handleType;
    offer.seedA = seedA;
    offer.seedB = seedB;
    offer.seedC = seedC;
    offer.memoryTypeIndex = a.memoryTypeIndex;
    offer.memoryTypeBits = a.memoryTypeBits;
    offer.gpuWord = gpuWord;
    offer.gpuRan = (gt.ran && gpuFillSeenHere == -1) ? 1u : 0u;

    // For OPAQUE_FD the raw mmap leg is informational only: the Vulkan spec
    // explicitly forbids interpreting an opaque fd payload outside the driver,
    // so a driver that refuses it is conformant and MobileGL would never take
    // that route.  For DMA_BUF a CPU mapping is the point of the handle type,
    // so there it is decisive.
    const bool mmapDecisive = (handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

    std::string detail;
    const char* status = "FAIL";
    if (!sendMsg(sock, MSG_T1_OFFER, &offer, sizeof(offer), a.fd)) {
        detail = fmt("sendMsg(offer) errno=%d", errno);
        record(routeName, "FAIL", detail);
        sendMsg(sock, MSG_BYE, nullptr, 0, -1);
        reapChild(pid);
        close(sock);
        freeExportAlloc(c, a);
        return;
    }
    close(a.fd);
    a.fd = -1;

    uint32_t tag2 = 0;
    T1Result res{};
    size_t got = 0;
    if (!recvMsg(sock, &tag2, &res, sizeof(res), &got, nullptr) || tag2 != MSG_T1_RESULT || got != sizeof(res)) {
        detail = fmt("no T1 result from child (errno=%d, %s)", errno, reapChild(pid).c_str());
        record(routeName, "FAIL", detail);
        close(sock);
        freeExportAlloc(c, a);
        return;
    }

    // The child wrote REG_B (mmap) and REG_C (imported vkMapMemory); check that
    // the writes are visible through the *server's* own mapping.
    int64_t backB = res.wroteB ? checkRegion(a.host, REG_B, seedB) : -2;
    int64_t backC = res.wroteC ? checkRegion(a.host, REG_C, seedC) : -2;

    std::vector<Leg> legs;
    {
        Leg l;
        l.name = "rawmmap";
        l.decisive = mmapDecisive;
        l.attempted = res.mmapOk != 0;
        l.readOk = res.mmapOk && res.mmapMismatch == -1 && (!offer.gpuRan || res.mmapGpuMismatch == -1);
        l.writeOk = res.wroteB && backB == -1;
        if (!l.attempted)
            l.fail = fmt("mmap failed errno=%d(%s)", res.mmapErrno, strerror(res.mmapErrno));
        else if (!l.readOk)
            l.fail = fmt("exporter payload not at offset 0 (cmp=%lld payloadAt=%lld gpuCmp=%lld)",
                         (long long)res.mmapMismatch, (long long)res.mmapPatternOffset,
                         (long long)res.mmapGpuMismatch);
        else if (!l.writeOk)
            l.fail = fmt("importer write not visible to exporter (back=%lld)", (long long)backB);
        legs.push_back(l);
    }
    {
        Leg l;
        l.name = "vkimport";
        l.decisive = true;
        l.attempted = res.importResult == VK_SUCCESS;
        l.readOk = res.importResult == VK_SUCCESS && res.mapResult == VK_SUCCESS && res.vkMismatch == -1 &&
                   (!offer.gpuRan || res.vkGpuMismatch == -1);
        l.writeOk = res.wroteC && backC == -1;
        if (!res.vkInitOk)
            l.fail = "child Vulkan init failed";
        else if (res.importResult != VK_SUCCESS)
            l.fail = "vkAllocateMemory(import)=" + vkStr((VkResult)res.importResult);
        else if (res.mapResult != VK_SUCCESS)
            l.fail = "importer vkMapMemory=" + vkStr((VkResult)res.mapResult);
        else if (!l.readOk)
            l.fail = fmt("payload mismatch cmp=%lld gpuCmp=%lld", (long long)res.vkMismatch,
                         (long long)res.vkGpuMismatch);
        else if (!l.writeOk)
            l.fail = fmt("importer write not visible to exporter (back=%lld)", (long long)backC);
        legs.push_back(l);
    }
    {
        Leg l;
        l.name = "gpu";
        l.decisive = true;
        l.attempted = gt.ran;
        l.readOk = gt.readMismatch == -1;
        l.writeOk = gpuFillSeenHere == -1;
        if (!gt.ran)
            l.fail = "GPU touch did not run: " + gt.fail;
        else if (!l.readOk)
            l.fail = fmt("GPU read of the shared allocation mismatched at %lld", (long long)gt.readMismatch);
        else if (!l.writeOk)
            l.fail = fmt("GPU write not visible through the exporter's map (at %lld)", (long long)gpuFillSeenHere);
        legs.push_back(l);
    }

    std::string why;
    status = legVerdict(legs, &why);
    detail = fmt(
        "%s | mmap=%s(errno=%d,cmp=%lld,payloadAt=%lld,gpu=%lld,back=%lld) vkimport=%s(fdProps=%s bits=0x%x "
        "bind=%s map=%s cmp=%lld gpu=%lld back=%lld) gpuTouch(submit=%s read=%lld fill=%lld) %s%s%s",
        legTrace(legs).c_str(), res.mmapOk ? (res.mmapOk == 2 ? "ok-buffersize" : "ok") : "fail", res.mmapErrno,
        (long long)res.mmapMismatch, (long long)res.mmapPatternOffset, (long long)res.mmapGpuMismatch,
        (long long)backB, res.importResult == VK_SUCCESS ? "ok" : vkStr((VkResult)res.importResult).c_str(),
        vkStr((VkResult)res.fdPropsResult).c_str(), res.fdMemoryTypeBits, vkStr((VkResult)res.bindResult).c_str(),
        vkStr((VkResult)res.mapResult).c_str(), (long long)res.vkMismatch, (long long)res.vkGpuMismatch,
        (long long)backC, vkStr(gt.submitResult).c_str(), (long long)gt.readMismatch, (long long)gpuFillSeenHere,
        res.note, why.empty() ? "" : " | why: ", why.c_str());
    if (!mmapDecisive)
        detail += " | rawmmap informational: an opaque fd is not required to be mmap-able";

    sendMsg(sock, MSG_BYE, nullptr, 0, -1);
    detail += " ";
    detail += reapChild(pid);
    close(sock);
    freeExportAlloc(c, a);
    record(routeName, status, detail);
}

// ---------------------------------------------------------------------------
// T1 child
// ---------------------------------------------------------------------------

static int childT1(int sock) {
    setRecvTimeout(sock, 30);
    T1Offer offer{};
    uint32_t tag = 0;
    size_t got = 0;
    int fd = -1;
    if (!recvMsg(sock, &tag, &offer, sizeof(offer), &got, &fd) || tag != MSG_T1_OFFER) {
        pr("child: bad offer (errno=%d)", errno);
        return 2;
    }
    T1Result res{};
    res.mmapMismatch = -3;
    res.vkMismatch = -3;
    res.mmapGpuMismatch = -2;
    res.vkGpuMismatch = -2;
    res.gotFd = fd;
    if (fd < 0) {
        snprintf(res.note, sizeof(res.note), "no fd received over SCM_RIGHTS");
        sendMsg(sock, MSG_T1_RESULT, &res, sizeof(res), -1);
        return 3;
    }
    pr("child: got fd=%d -> %s", fd, describeFd(fd).c_str());
    std::string note = describeFd(fd);

    // (1) plain mmap of the exported fd
    size_t mappedLen = (size_t)offer.allocationSize;
    int firstErrno = 0;
    void* p = mmap(nullptr, mappedLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        firstErrno = errno;
        res.mmapOk = 0;
        res.mmapErrno = firstErrno;
        pr("child: mmap(MAP_SHARED, allocationSize) failed errno=%d (%s)", firstErrno, strerror(firstErrno));
        // second chance: some allocators only allow the buffer size, not the padded size
        mappedLen = (size_t)offer.bufferSize;
        p = mmap(nullptr, mappedLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p != MAP_FAILED) {
            res.mmapOk = 2;
            res.mmapErrno = 0;  // the mapping succeeded; the first errno is history, not the verdict
            note += fmt(" [mmap needed bufferSize not allocationSize; allocationSize errno=%d]", firstErrno);
        } else {
            res.mmapErrno = errno;
        }
    } else {
        res.mmapOk = 1;
    }
    // Read through the plain mapping, but do NOT write through it yet: if this
    // mapping is offset-shifted relative to the driver's view of the same
    // allocation, an early write here lands on top of the exporter's payload and
    // poisons the Vulkan-import read below.  Reads first, writes afterwards.
    res.mmapPatternOffset = -1;
    if (p != MAP_FAILED) {
        res.mmapMismatch = checkRegion(p, REG_A, offer.seedA);
        if (offer.gpuRan) res.mmapGpuMismatch = checkFillWord(p, REG_E, offer.gpuWord);
        if (res.mmapMismatch != -1) {
            // Locate the exporter's payload: an fd that maps at a fixed offset from
            // the driver's base is still usable, but only if that offset is
            // discoverable, which opaque-fd does not promise.  Report it either way.
            uint8_t want[64];
            fillPattern(want, sizeof(want), offer.seedA);
            const uint8_t* hay = (const uint8_t*)p;
            for (uint64_t off = 0; off + sizeof(want) <= mappedLen; ++off) {
                if (!memcmp(hay + off, want, sizeof(want))) {
                    res.mmapPatternOffset = (int64_t)off;
                    break;
                }
            }
        }
    }

    // writes through the plain mapping, deferred until every read is done
    auto writeThroughMmap = [&]() {
        if (p == MAP_FAILED) return;
        writeRegion(p, REG_B, offer.seedB);
        res.wroteB = 1;
        msync(p, mappedLen, MS_SYNC);
    };

    // (2) import the same fd into a child-side VkDeviceMemory and map it
    VkCtx c;
    if (!vkCtxInit(c, false)) {
        writeThroughMmap();
        snprintf(res.note, sizeof(res.note), "%s | child vulkan init failed", note.c_str());
        sendMsg(sock, MSG_T1_RESULT, &res, sizeof(res), -1);
        return 4;
    }
    res.vkInitOk = 1;
    VkExternalMemoryHandleTypeFlagBits ht = (VkExternalMemoryHandleTypeFlagBits)offer.handleType;

    uint32_t fdTypeBits = 0xFFFFFFFFu;
    if (c.pGetMemoryFdPropertiesKHR) {
        VkMemoryFdPropertiesKHR fdProps{};
        fdProps.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
        VkResult fr = c.pGetMemoryFdPropertiesKHR(c.device, ht, fd, &fdProps);
        res.fdPropsResult = (int32_t)fr;
        res.fdMemoryTypeBits = fdProps.memoryTypeBits;
        // OPAQUE_FD does not permit vkGetMemoryFdPropertiesKHR; only DMA_BUF does.
        if (fr == VK_SUCCESS && ht == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
            fdTypeBits = fdProps.memoryTypeBits;
    } else {
        res.fdPropsResult = (int32_t)VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    VkExternalMemoryBufferCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    ext.handleTypes = ht;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.pNext = &ext;
    bci.size = offer.bufferSize;
    bci.usage = kProbeBufferUsage;
    VkBuffer buf = VK_NULL_HANDLE;
    VkResult r = vkCreateBuffer(c.device, &bci, nullptr, &buf);
    if (r != VK_SUCCESS) {
        res.importResult = (int32_t)r;
        writeThroughMmap();
        snprintf(res.note, sizeof(res.note), "%s | child vkCreateBuffer=%s", note.c_str(), vkStr(r).c_str());
        sendMsg(sock, MSG_T1_RESULT, &res, sizeof(res), -1);
        vkCtxDestroy(c);
        return 5;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(c.device, buf, &req);
    uint32_t bits = req.memoryTypeBits & fdTypeBits;
    // For OPAQUE_FD the spec requires the importer to name the *same* memory type
    // index the exporter allocated from; only DMA_BUF lets the importer choose
    // from vkGetMemoryFdPropertiesKHR.
    int typeIdx;
    if (ht == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) {
        typeIdx = (int)offer.memoryTypeIndex;
    } else {
        typeIdx = pickMemType(c.memProps, bits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (typeIdx < 0) typeIdx = (int)offer.memoryTypeIndex;  // fall back to the exporter's choice
    }

    // the import consumes the fd on success, so hand over a duplicate
    int importFd = dup(fd);
    VkImportMemoryFdInfoKHR imp{};
    imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    imp.handleType = ht;
    imp.fd = importFd;
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.buffer = buf;
    imp.pNext = &dedicated;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &imp;
    mai.allocationSize = offer.allocationSize;
    mai.memoryTypeIndex = (uint32_t)typeIdx;

    VkDeviceMemory mem = VK_NULL_HANDLE;
    r = vkAllocateMemory(c.device, &mai, nullptr, &mem);
    res.importResult = (int32_t)r;
    if (r != VK_SUCCESS) {
        // retry without the dedicated-allocation chain -- some drivers reject it
        close(importFd);
        importFd = dup(fd);
        imp.fd = importFd;
        imp.pNext = nullptr;
        r = vkAllocateMemory(c.device, &mai, nullptr, &mem);
        if (r == VK_SUCCESS) {
            note += " [import needed no dedicated info]";
            res.importResult = (int32_t)r;
        } else {
            close(importFd);
            writeThroughMmap();
            snprintf(res.note, sizeof(res.note), "%s | import=%s type=%d bits=0x%x", note.c_str(), vkStr(r).c_str(),
                     typeIdx, bits);
            vkDestroyBuffer(c.device, buf, nullptr);
            sendMsg(sock, MSG_T1_RESULT, &res, sizeof(res), -1);
            vkCtxDestroy(c);
            return 0;
        }
    }
    res.bindResult = (int32_t)vkBindBufferMemory(c.device, buf, mem, 0);
    void* host = nullptr;
    r = vkMapMemory(c.device, mem, 0, VK_WHOLE_SIZE, 0, &host);
    res.mapResult = (int32_t)r;
    if (r == VK_SUCCESS && host) {
        res.vkMismatch = checkRegion(host, REG_A, offer.seedA);
        if (offer.gpuRan) res.vkGpuMismatch = checkFillWord(host, REG_E, offer.gpuWord);
        writeRegion(host, REG_C, offer.seedC);
        res.wroteC = 1;
        vkUnmapMemory(c.device, mem);
    }
    // now that both mappings have been read, write through the plain one too
    writeThroughMmap();
    snprintf(res.note, sizeof(res.note), "%s | childType=%d bits=0x%x", note.c_str(), typeIdx, bits);
    vkFreeMemory(c.device, mem, nullptr);
    vkDestroyBuffer(c.device, buf, nullptr);
    sendMsg(sock, MSG_T1_RESULT, &res, sizeof(res), -1);
    vkCtxDestroy(c);
    if (p != MAP_FAILED) munmap(p, mappedLen);
    close(fd);
    return 0;
}

// ---------------------------------------------------------------------------
// T1 for DirectGLES ("Espryt"): the exported fd imported as GL buffer storage
//
// AcquirePersistentMap on the GLES backend is glBufferStorageEXT +
// glMapBufferRange(PERSISTENT|COHERENT), not a VkDeviceMemory map, so the
// Vulkan T1 answer above does not decide the tier for that backend.  The GL
// route to the same question is GL_EXT_memory_object{,_fd}: import the fd as a
// memory object, back a buffer with it, and map that buffer persistently.
// Tried in-process first (isolates "GL can import this fd at all" from
// "the fd survives a process boundary"), then cross-process.
// ---------------------------------------------------------------------------

static const char* kT1GlSameProc = "T1-gles-memobj-fd-same-proc";
static const char* kT1GlCrossProc = "T1-gles-memobj-fd-cross-proc";

static int childT1Gl(int sock) {
    setRecvTimeout(sock, 30);
    T1GlOffer offer{};
    uint32_t tag = 0;
    size_t got = 0;
    int fd = -1;
    if (!recvMsg(sock, &tag, &offer, sizeof(offer), &got, &fd) || tag != MSG_T1GL_OFFER) {
        pr("child: bad T1GL offer errno=%d", errno);
        return 2;
    }
    T1GlResult res{};
    res.mismatchA = -3;
    res.mismatchGpu = -2;
    if (fd < 0) {
        snprintf(res.note, sizeof(res.note), "no fd over SCM_RIGHTS");
        sendMsg(sock, MSG_T1GL_RESULT, &res, sizeof(res), -1);
        return 3;
    }
    std::string note = describeFd(fd);

    GlCtx g;
    if (!glCtxInit(g)) {
        snprintf(res.note, sizeof(res.note), "%s | child EGL/GLES init failed", note.c_str());
        sendMsg(sock, MSG_T1GL_RESULT, &res, sizeof(res), -1);
        close(fd);
        return 4;
    }
    res.glInitOk = 1;
    if (!g.canImportFd()) {
        snprintf(res.note, sizeof(res.note), "%s | %s", note.c_str(), g.missingForImportFd().c_str());
        sendMsg(sock, MSG_T1GL_RESULT, &res, sizeof(res), -1);
        glCtxDestroy(g);
        close(fd);
        return 0;
    }
    res.haveExts = 1;

    GlImport imp;
    glImportFdBuffer(g, fd, offer.allocationSize, offer.bufferSize, offer.dedicated != 0, imp);
    res.memObjOk = imp.memObjOk;
    res.storageOk = imp.storageOk;
    res.mapOk = imp.mapOk;
    res.persistentCoherent = imp.persistentCoherent;
    res.errImport = imp.errImport;
    res.errStorage = imp.errStorage;
    res.errMap = imp.errMap;
    res.errMap2 = imp.errMap2;
    if (imp.mapOk && imp.ptr) {
        res.mismatchA = checkRegion(imp.ptr, REG_A, offer.seedA);
        if (offer.gpuRan) res.mismatchGpu = checkFillWord(imp.ptr, REG_E, offer.gpuWord);
        writeRegion(imp.ptr, REG_D, offer.seedD);
        res.wroteD = 1;
        glImportPublish(g, imp);
    }
    snprintf(res.note, sizeof(res.note), "%s | accepted=%s | ladder: %s| %s", note.c_str(),
             imp.variant.empty() ? "none" : imp.variant.c_str(), imp.ladder.c_str(), imp.fail.c_str());
    glImportRelease(g, imp);
    sendMsg(sock, MSG_T1GL_RESULT, &res, sizeof(res), -1);
    glCtxDestroy(g);
    close(fd);
    return 0;
}

static void runT1GlesParent(VkCtx& c, GlCtx& g, bool glOk, uint64_t size) {
    if (!glOk) {
        record(kT1GlSameProc, "SKIP", "no headless GLES context");
        record(kT1GlCrossProc, "SKIP", "no headless GLES context");
        return;
    }
    bool uuidMatch = false;
    std::string glUuid = glDeviceUuidReport(g, c.deviceUUID, &uuidMatch);
    std::string uuidNote = fmt("glDeviceUUID=%s vkMatch=%d", glUuid.c_str(), (int)uuidMatch);

    if (!g.canImportFd()) {
        std::string d = g.missingForImportFd() + " | " + uuidNote;
        record(kT1GlSameProc, "UNSUPPORTED", d);
        record(kT1GlCrossProc, "UNSUPPORTED", d);
        return;
    }
    if (!c.hasExtMemFd || !c.pGetMemoryFdKHR) {
        record(kT1GlSameProc, "UNSUPPORTED", "VK_KHR_external_memory_fd absent, nothing to import");
        record(kT1GlCrossProc, "UNSUPPORTED", "VK_KHR_external_memory_fd absent, nothing to import");
        return;
    }

    ExportAlloc a;
    if (!exportHostVisible(c, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, size, "T1-gles", a)) {
        std::string d = a.fail + " | " + uuidNote;
        record(kT1GlSameProc, exportFailStatus(a), d);
        record(kT1GlCrossProc, exportFailStatus(a), d);
        freeExportAlloc(c, a);
        return;
    }

    const uint32_t seedA = 0x61510001u, seedD = 0x61510004u, seedF = 0x61510006u;
    const uint32_t gpuWord = 0x6C651234u;
    memset(a.host, 0, (size_t)size);
    writeRegion(a.host, REG_A, seedA);
    GpuTouch gt = gpuTouch(c, a.buf, REG_A, seedA, REG_E, gpuWord);
    int64_t gpuFillSeenHere = gt.ran ? checkFillWord(a.host, REG_E, gpuWord) : -3;
    const bool gpuUsable = gt.ran && gt.readMismatch == -1 && gpuFillSeenHere == -1;
    pr("T1-gles gpuTouch ran=%d submit=%s read=%lld fill=%lld %s", (int)gt.ran, vkStr(gt.submitResult).c_str(),
       (long long)gt.readMismatch, (long long)gpuFillSeenHere, gt.fail.c_str());

    // ---- (a) same process ----------------------------------------------------
    {
        GlImport imp;
        glImportFdBuffer(g, a.fd, a.allocationSize, size, a.dedicated, imp);
        int64_t cmpA = -3, cmpGpu = -2, backF = -2;
        if (imp.mapOk && imp.ptr) {
            cmpA = checkRegion(imp.ptr, REG_A, seedA);
            if (gpuUsable) cmpGpu = checkFillWord(imp.ptr, REG_E, gpuWord);
            writeRegion(imp.ptr, REG_F, seedF);
            glImportPublish(g, imp);
            backF = checkRegion(a.host, REG_F, seedF);
        }

        std::vector<Leg> legs;
        Leg l;
        l.name = "gl-import";
        l.decisive = true;
        l.attempted = imp.storageOk;
        l.readOk = imp.mapOk && cmpA == -1 && (!gpuUsable || cmpGpu == -1);
        l.writeOk = imp.mapOk && backF == -1;
        if (!imp.memObjOk)
            l.fail = "glImportMemoryFdEXT -> " + glErrStr(imp.errImport);
        else if (!imp.storageOk)
            l.fail = "glBufferStorageMemEXT -> " + glErrStr(imp.errStorage);
        else if (!imp.mapOk)
            l.fail = "glMapBufferRange persistent -> " + glErrStr(imp.errMap) + ", plain -> " + glErrStr(imp.errMap2);
        else if (!l.readOk)
            l.fail = fmt("Vulkan-written payload not visible through the GL map (cmp=%lld gpuCmp=%lld)",
                         (long long)cmpA, (long long)cmpGpu);
        else if (!l.writeOk)
            l.fail = fmt("GL-map write not visible through the Vulkan map (back=%lld)", (long long)backF);
        legs.push_back(l);
        // The tier needs a *persistent coherent* mapping, not a scoped one: a
        // driver that only grants the scoped map cannot host AcquirePersistentMap.
        Leg pc;
        pc.name = "persistent-coherent";
        pc.decisive = true;
        pc.attempted = imp.mapOk;
        pc.readOk = imp.persistentCoherent;
        pc.writeOk = imp.persistentCoherent;
        if (!imp.mapOk)
            pc.fail = "no mapping at all";
        else if (!imp.persistentCoherent)
            pc.fail = "PERSISTENT|COHERENT refused (" + glErrStr(imp.errMap) + "), only a scoped map works";
        legs.push_back(pc);

        std::string why;
        const char* status = legVerdict(legs, &why);
        record(kT1GlSameProc, status,
               fmt("%s | memObj=%d(%s) storage=%d(%s) accepted=%s map=%d persistentCoherent=%d(%s/%s) cmpA=%lld "
                   "cmpGpu=%lld backF=%lld | ladder: %s| %s | %s",
                   legTrace(legs).c_str(), (int)imp.memObjOk, glErrStr(imp.errImport).c_str(), (int)imp.storageOk,
                   glErrStr(imp.errStorage).c_str(), imp.variant.empty() ? "none" : imp.variant.c_str(),
                   (int)imp.mapOk, (int)imp.persistentCoherent, glErrStr(imp.errMap).c_str(),
                   glErrStr(imp.errMap2).c_str(), (long long)cmpA, (long long)cmpGpu, (long long)backF,
                   imp.ladder.c_str(), uuidNote.c_str(), why.c_str()));
        glImportRelease(g, imp);
    }

    // ---- (b) cross process ---------------------------------------------------
    {
        int sock = -1;
        pid_t pid = spawnChild("t1gl", &sock);
        if (pid < 0) {
            record(kT1GlCrossProc, "FAIL", "spawnChild failed");
            freeExportAlloc(c, a);
            return;
        }
        T1GlOffer offer{};
        offer.allocationSize = a.allocationSize;
        offer.bufferSize = size;
        offer.seedA = seedA;
        offer.seedD = seedD;
        offer.gpuWord = gpuWord;
        offer.gpuRan = gpuUsable ? 1u : 0u;
        offer.dedicated = a.dedicated ? 1u : 0u;

        if (!sendMsg(sock, MSG_T1GL_OFFER, &offer, sizeof(offer), a.fd)) {
            record(kT1GlCrossProc, "FAIL", fmt("sendMsg(offer) errno=%d", errno));
            sendMsg(sock, MSG_BYE, nullptr, 0, -1);
            reapChild(pid);
            close(sock);
            freeExportAlloc(c, a);
            return;
        }
        T1GlResult res{};
        uint32_t tag = 0;
        size_t got = 0;
        if (!recvMsg(sock, &tag, &res, sizeof(res), &got, nullptr) || tag != MSG_T1GL_RESULT || got != sizeof(res)) {
            record(kT1GlCrossProc, "FAIL", fmt("no reply errno=%d %s", errno, reapChild(pid).c_str()));
            close(sock);
            freeExportAlloc(c, a);
            return;
        }
        int64_t backD = res.wroteD ? checkRegion(a.host, REG_D, seedD) : -2;

        std::vector<Leg> legs;
        Leg l;
        l.name = "gl-import";
        l.decisive = true;
        l.attempted = res.storageOk != 0;
        l.readOk = res.mapOk && res.mismatchA == -1 && (!offer.gpuRan || res.mismatchGpu == -1);
        l.writeOk = res.wroteD && backD == -1;
        if (!res.glInitOk)
            l.fail = "child EGL/GLES init failed";
        else if (!res.haveExts)
            l.fail = "child lacks GL_EXT_memory_object{,_fd}";
        else if (!res.memObjOk)
            l.fail = "glImportMemoryFdEXT -> " + glErrStr(res.errImport);
        else if (!res.storageOk)
            l.fail = "glBufferStorageMemEXT -> " + glErrStr(res.errStorage);
        else if (!res.mapOk)
            l.fail = "glMapBufferRange persistent -> " + glErrStr(res.errMap) + ", plain -> " + glErrStr(res.errMap2);
        else if (!l.readOk)
            l.fail = fmt("exporter payload not visible through the child's GL map (cmp=%lld gpuCmp=%lld)",
                         (long long)res.mismatchA, (long long)res.mismatchGpu);
        else if (!l.writeOk)
            l.fail = fmt("child GL-map write not visible to the exporter (back=%lld)", (long long)backD);
        legs.push_back(l);
        Leg pc;
        pc.name = "persistent-coherent";
        pc.decisive = true;
        pc.attempted = res.mapOk != 0;
        pc.readOk = res.persistentCoherent != 0;
        pc.writeOk = res.persistentCoherent != 0;
        if (!res.mapOk)
            pc.fail = "no mapping at all";
        else if (!res.persistentCoherent)
            pc.fail = "PERSISTENT|COHERENT refused (" + glErrStr(res.errMap) + "), only a scoped map works";
        legs.push_back(pc);

        std::string why;
        const char* status = legVerdict(legs, &why);
        std::string detail =
            fmt("%s | glInit=%d exts=%d memObj=%d(%s) storage=%d(%s) map=%d persistentCoherent=%d(%s/%s) "
                "cmpA=%lld cmpGpu=%lld backD=%lld | %s | %s [%s] ",
                legTrace(legs).c_str(), res.glInitOk, res.haveExts, res.memObjOk, glErrStr(res.errImport).c_str(),
                res.storageOk, glErrStr(res.errStorage).c_str(), res.mapOk, res.persistentCoherent,
                glErrStr(res.errMap).c_str(), glErrStr(res.errMap2).c_str(), (long long)res.mismatchA,
                (long long)res.mismatchGpu, (long long)backD, uuidNote.c_str(), why.c_str(), res.note);
        sendMsg(sock, MSG_BYE, nullptr, 0, -1);
        detail += reapChild(pid);
        close(sock);
        record(kT1GlCrossProc, status, detail);
    }

    freeExportAlloc(c, a);
}

// ---------------------------------------------------------------------------
// T0: child allocates an AHardwareBuffer BLOB, parent imports it
// ---------------------------------------------------------------------------

#if PROBE_HAVE_AHB

static int childT0(int sock) {
    setRecvTimeout(sock, 30);
    T0Request rq{};
    uint32_t tag = 0;
    size_t got = 0;
    if (!recvMsg(sock, &tag, &rq, sizeof(rq), &got, nullptr) || tag != MSG_T0_REQUEST) {
        pr("child: bad T0 request errno=%d", errno);
        return 2;
    }
    AHardwareBuffer_Desc desc{};
    desc.width = (uint32_t)rq.size;
    desc.height = 1;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_BLOB;
    desc.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                 AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;
    AHardwareBuffer* ahb = nullptr;
    int rc = AHardwareBuffer_allocate(&desc, &ahb);
    T0Alloc alloc{};
    alloc.allocOk = (rc == 0 && ahb) ? 1 : 0;
    alloc.allocErr = rc;
    alloc.size = rq.size;
    if (!alloc.allocOk) {
        snprintf(alloc.note, sizeof(alloc.note), "AHardwareBuffer_allocate rc=%d errno=%d", rc, errno);
        sendMsg(sock, MSG_T0_ALLOC, &alloc, sizeof(alloc), -1);
        return 3;
    }
    AHardwareBuffer_Desc back{};
    AHardwareBuffer_describe(ahb, &back);
    alloc.stride = back.stride;
    snprintf(alloc.note, sizeof(alloc.note), "desc w=%u h=%u layers=%u fmt=0x%x usage=0x%llx stride=%u", back.width,
             back.height, back.layers, back.format, (unsigned long long)back.usage, back.stride);

    void* p = nullptr;
    rc = AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &p);
    if (rc != 0 || !p) {
        alloc.allocOk = 2;
        snprintf(alloc.note + strlen(alloc.note), sizeof(alloc.note) - strlen(alloc.note), " | lock rc=%d", rc);
        sendMsg(sock, MSG_T0_ALLOC, &alloc, sizeof(alloc), -1);
        return 4;
    }
    memset(p, 0, (size_t)rq.size);
    writeRegion(p, REG_A, rq.seedA);
    AHardwareBuffer_unlock(ahb, nullptr);

    if (!sendMsg(sock, MSG_T0_ALLOC, &alloc, sizeof(alloc), -1)) return 5;
    int sendRc = AHardwareBuffer_sendHandleToUnixSocket(ahb, sock);
    pr("child: AHardwareBuffer_sendHandleToUnixSocket rc=%d", sendRc);
    if (sendRc != 0) return 6;

    T0Verify ver{};
    if (!recvMsg(sock, &tag, &ver, sizeof(ver), &got, nullptr) || tag != MSG_T0_VERIFY) {
        pr("child: no T0 verify errno=%d", errno);
        AHardwareBuffer_release(ahb);
        return 7;
    }
    T0Result res{};
    res.mismatchB = res.mismatchC = res.mismatchD = res.mismatchE = -2;
    void* q = nullptr;
    rc = AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &q);
    res.lockOk = (rc == 0 && q) ? 1 : 0;
    res.lockErr = rc;
    if (res.lockOk) {
        if (ver.writtenMask & 1) res.mismatchB = checkRegion(q, REG_B, ver.seedB);
        if (ver.writtenMask & 2) res.mismatchC = checkRegion(q, REG_C, ver.seedC);
        if (ver.writtenMask & 4) res.mismatchD = checkRegion(q, REG_D, ver.seedD);
        if (ver.writtenMask & 8) res.mismatchE = checkFillWord(q, REG_E, ver.gpuWord);
        AHardwareBuffer_unlock(ahb, nullptr);
    }
    snprintf(res.note, sizeof(res.note), "mask=0x%x", ver.writtenMask);
    sendMsg(sock, MSG_T0_RESULT, &res, sizeof(res), -1);
    AHardwareBuffer_release(ahb);
    return 0;
}

static void runT0Parent(VkCtx& c, GlCtx& g, bool glOk, uint64_t size) {
    const uint32_t seedA = 0x0A0A0011u, seedB = 0x0B0B0022u, seedC = 0x0C0C0033u, seedD = 0x0D0D0044u;
    const uint32_t gpuWord = 0x70701234u;

    // The composite row is recorded at the very end from the full
    // import + map + compare + write-back chain; the handoff alone is only a
    // diagnostic and gets its own informational row.
    auto failAll = [&](const std::string& why) {
        record("T0-ahb-handoff", "FAIL", why);
        record("T0-ahb-blob-transfer", "FAIL", "handoff failed, nothing to import: " + why);
    };

    int sock = -1;
    pid_t pid = spawnChild("t0", &sock);
    if (pid < 0) {
        failAll("spawnChild failed");
        return;
    }
    T0Request rq{};
    rq.size = size;
    rq.seedA = seedA;
    if (!sendMsg(sock, MSG_T0_REQUEST, &rq, sizeof(rq), -1)) {
        failAll(fmt("sendMsg errno=%d", errno));
        close(sock);
        reapChild(pid);
        return;
    }
    T0Alloc alloc{};
    uint32_t tag = 0;
    size_t got = 0;
    if (!recvMsg(sock, &tag, &alloc, sizeof(alloc), &got, nullptr) || tag != MSG_T0_ALLOC) {
        failAll(fmt("no alloc reply errno=%d %s", errno, reapChild(pid).c_str()));
        close(sock);
        return;
    }
    if (alloc.allocOk != 1) {
        failAll(fmt("child alloc failed rc=%d %s", alloc.allocErr, alloc.note));
        close(sock);
        reapChild(pid);
        return;
    }
    pr("T0 child allocated: %s", alloc.note);

    AHardwareBuffer* ahb = nullptr;
    int rc = AHardwareBuffer_recvHandleFromUnixSocket(sock, &ahb);
    if (rc != 0 || !ahb) {
        failAll(fmt("recvHandleFromUnixSocket rc=%d errno=%d", rc, errno));
        close(sock);
        reapChild(pid);
        return;
    }
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(ahb, &desc);
    pr("T0 parent received AHB: w=%u h=%u fmt=0x%x usage=0x%llx stride=%u", desc.width, desc.height, desc.format,
       (unsigned long long)desc.usage, desc.stride);
    record("T0-ahb-handoff", "OK",
           fmt("socket handoff of a %llu-byte BLOB works (%s) -- handoff only, see T0-ahb-blob-transfer for the tier",
               (unsigned long long)size, alloc.note));

    uint32_t writtenMask = 0;
    int64_t cpuCmp = -3, vkCmp = -3, glCmp = -3, vkGpuCmp = -2;
    bool vkMapped = false, glMapped = false, glPersistent = false;
    GpuTouch gt;
    std::string vkFail, glFail, cpuFail, gpuFail;

    // (a) CPU path: AHardwareBuffer_lock on the receiving side
    {
        void* p = nullptr;
        rc = AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
                                  -1, nullptr, &p);
        if (rc != 0 || !p) {
            cpuFail = fmt("AHardwareBuffer_lock rc=%d errno=%d", rc, errno);
            record("T0-ahb-cpu-lock", "FAIL", cpuFail);
        } else {
            cpuCmp = checkRegion(p, REG_A, seedA);
            writeRegion(p, REG_D, seedD);
            writtenMask |= 4;
            AHardwareBuffer_unlock(ahb, nullptr);
            if (cpuCmp != -1) cpuFail = fmt("payload mismatch at %lld", (long long)cpuCmp);
            record("T0-ahb-cpu-lock", cpuCmp == -1 ? "OK" : "FAIL",
                   fmt("cross-process CPU read of the client's payload, mismatch=%lld", (long long)cpuCmp));
        }
    }

    // (b) Vulkan import (+ a real GPU access on the imported memory)
    if (!c.hasAhb || !c.pGetAhbProps) {
        vkFail = "VK_ANDROID_external_memory_android_hardware_buffer absent";
        record("T0-ahb-vulkan-import", "UNSUPPORTED", vkFail);
    } else {
        VkAndroidHardwareBufferPropertiesANDROID props{};
        props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
        VkResult r = c.pGetAhbProps(c.device, ahb, &props);
        if (r != VK_SUCCESS) {
            vkFail = "vkGetAndroidHardwareBufferPropertiesANDROID=" + vkStr(r);
            record("T0-ahb-vulkan-import", "FAIL", vkFail);
        } else {
            pr("T0 AHB props: allocationSize=%llu memoryTypeBits=0x%x", (unsigned long long)props.allocationSize,
               props.memoryTypeBits);
            VkExternalMemoryBufferCreateInfo ext{};
            ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
            ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
            VkBufferCreateInfo bci{};
            bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bci.pNext = &ext;
            bci.size = size;
            bci.usage = kProbeBufferUsage;
            VkBuffer buf = VK_NULL_HANDLE;
            r = vkCreateBuffer(c.device, &bci, nullptr, &buf);
            if (r != VK_SUCCESS) {
                vkFail = "vkCreateBuffer(AHB external)=" + vkStr(r);
                record("T0-ahb-vulkan-import", "FAIL", vkFail);
            } else {
                int typeIdx = pickMemType(c.memProps, props.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                bool hostVisible = typeIdx >= 0;
                if (typeIdx < 0) typeIdx = pickMemType(c.memProps, props.memoryTypeBits, 0);
                VkImportAndroidHardwareBufferInfoANDROID imp{};
                imp.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
                imp.buffer = ahb;
                VkMemoryDedicatedAllocateInfo ded{};
                ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
                ded.buffer = buf;
                imp.pNext = &ded;
                VkMemoryAllocateInfo mai{};
                mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                mai.pNext = &imp;
                mai.allocationSize = props.allocationSize;
                mai.memoryTypeIndex = (uint32_t)(typeIdx < 0 ? 0 : typeIdx);
                VkDeviceMemory mem = VK_NULL_HANDLE;
                r = vkAllocateMemory(c.device, &mai, nullptr, &mem);
                if (r != VK_SUCCESS) {
                    vkFail = fmt("vkAllocateMemory(import AHB)=%s typeIdx=%d bits=0x%x", vkStr(r).c_str(), typeIdx,
                                 props.memoryTypeBits);
                    record("T0-ahb-vulkan-import", "FAIL", vkFail);
                } else {
                    VkResult br = vkBindBufferMemory(c.device, buf, mem, 0);
                    // GPU access on the client's allocation -- the part that makes
                    // T0 a tier rather than a successful mmap
                    if (br == VK_SUCCESS) {
                        gt = gpuTouch(c, buf, REG_A, seedA, REG_E, gpuWord);
                        if (gt.ran) writtenMask |= 8;
                        gpuFail = gt.fail;
                    } else {
                        gpuFail = "vkBindBufferMemory=" + vkStr(br);
                    }
                    void* host = nullptr;
                    VkResult mr = vkMapMemory(c.device, mem, 0, VK_WHOLE_SIZE, 0, &host);
                    if (mr == VK_SUCCESS && host) {
                        vkMapped = true;
                        vkCmp = checkRegion(host, REG_A, seedA);
                        if (gt.ran) vkGpuCmp = checkFillWord(host, REG_E, gpuWord);
                        writeRegion(host, REG_B, seedB);
                        writtenMask |= 1;
                        vkUnmapMemory(c.device, mem);
                        if (vkCmp != -1) vkFail = fmt("payload mismatch at %lld", (long long)vkCmp);
                        record("T0-ahb-vulkan-import", vkCmp == -1 ? "OK" : "PARTIAL",
                               fmt("imported+mapped (hostVisibleType=%d bind=%s) payload mismatch=%lld gpuFill=%lld",
                                   (int)hostVisible, vkStr(br).c_str(), (long long)vkCmp, (long long)vkGpuCmp));
                    } else {
                        vkFail = fmt("vkMapMemory=%s (bind=%s hostVisibleType=%d bits=0x%x)", vkStr(mr).c_str(),
                                     vkStr(br).c_str(), (int)hostVisible, props.memoryTypeBits);
                        record("T0-ahb-vulkan-import", "PARTIAL", "import ok, " + vkFail);
                    }
                    vkFreeMemory(c.device, mem, nullptr);
                }
                vkDestroyBuffer(c.device, buf, nullptr);
            }
        }
    }
    record("T0-ahb-gpu-access", gt.ran && gt.readMismatch == -1 ? "OK" : (gt.ran ? "FAIL" : "SKIP"),
           fmt("vkCmdCopyBuffer out of the client AHB + vkCmdFillBuffer into it: ran=%d submit=%s read=%lld "
               "fillSeenByServerMap=%lld %s",
               (int)gt.ran, vkStr(gt.submitResult).c_str(), (long long)gt.readMismatch, (long long)vkGpuCmp,
               gpuFail.c_str()));

    // (c) GL import through EGL_ANDROID_get_native_client_buffer + EXT_external_buffer
    //     -- this is the DirectGLES ("Espryt") form of T0: the server backs a GL
    //     buffer with the client's allocation and maps it persistent/coherent.
    if (!glOk) {
        glFail = "no GL context";
        record("T0-ahb-gl-import", "SKIP", glFail);
    } else if (!g.hasGl("GL_EXT_external_buffer") || !g.pBufferStorageExternal || !g.pGetNativeClientBuffer) {
        glFail = fmt("GL_EXT_external_buffer=%d GL_EXT_buffer_storage=%d eglGetNativeClientBufferANDROID=%d "
                     "glBufferStorageExternalEXT=%d",
                     (int)g.hasGl("GL_EXT_external_buffer"), (int)g.hasGl("GL_EXT_buffer_storage"),
                     (int)(g.pGetNativeClientBuffer != nullptr), (int)(g.pBufferStorageExternal != nullptr));
        record("T0-ahb-gl-import", "UNSUPPORTED", glFail);
    } else {
        EGLClientBuffer cb = g.pGetNativeClientBuffer(ahb);
        if (!cb) {
            glFail = fmt("eglGetNativeClientBufferANDROID=NULL egl=0x%04x", eglGetError());
            record("T0-ahb-gl-import", "FAIL", glFail);
        } else {
            GLuint b = 0;
            glGenBuffers(1, &b);
            glBindBuffer(GL_ARRAY_BUFFER, b);
            glDrain();
            g.pBufferStorageExternal(GL_ARRAY_BUFFER, 0, (GLsizeiptr)size, cb,
                                     GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT |
                                         GL_MAP_COHERENT_BIT_EXT | GL_DYNAMIC_STORAGE_BIT_EXT);
            GLenum errStorage = glDrain();
            if (errStorage != GL_NO_ERROR) {
                glFail = "glBufferStorageExternalEXT -> " + glErrStr(errStorage);
                record("T0-ahb-gl-import", "FAIL", glFail);
            } else {
                void* m = glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr)size,
                                           GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT |
                                               GL_MAP_COHERENT_BIT_EXT);
                GLenum errMap = glDrain();
                GLenum errMap2 = GL_NO_ERROR;
                glPersistent = m != nullptr;
                if (!m) {
                    m = glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr)size, GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
                    errMap2 = glDrain();
                }
                if (!m) {
                    glFail = "glMapBufferRange persistent -> " + glErrStr(errMap) + ", plain -> " + glErrStr(errMap2);
                    record("T0-ahb-gl-import", "FAIL", "storage ok, " + glFail);
                } else {
                    glMapped = true;
                    glCmp = checkRegion(m, REG_A, seedA);
                    writeRegion(m, REG_C, seedC);
                    writtenMask |= 2;
                    glUnmapBuffer(GL_ARRAY_BUFFER);
                    glFinish();
                    if (glCmp != -1) glFail = fmt("payload mismatch at %lld", (long long)glCmp);
                    if (!glPersistent) glFail += " [PERSISTENT|COHERENT refused: " + glErrStr(errMap) + "]";
                    record("T0-ahb-gl-import", (glCmp == -1 && glPersistent) ? "OK" : "PARTIAL",
                           fmt("GL map of the client AHB: persistentCoherent=%d (persistent err=%s, plain err=%s) "
                               "payload mismatch=%lld",
                               (int)glPersistent, glErrStr(errMap).c_str(), glErrStr(errMap2).c_str(),
                               (long long)glCmp));
                }
            }
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glDeleteBuffers(1, &b);
        }
    }

    // (d) ask the client to verify everything the server wrote
    T0Verify ver{};
    ver.seedB = seedB;
    ver.seedC = seedC;
    ver.seedD = seedD;
    ver.gpuWord = gpuWord;
    ver.writtenMask = writtenMask;
    T0Result res{};
    res.mismatchB = res.mismatchC = res.mismatchD = res.mismatchE = -3;
    bool gotVerify = false;
    std::string wbDetail;
    const char* wbStatus = "FAIL";
    if (!sendMsg(sock, MSG_T0_VERIFY, &ver, sizeof(ver), -1)) {
        wbDetail = fmt("sendMsg(verify) errno=%d", errno);
    } else if (!recvMsg(sock, &tag, &res, sizeof(res), &got, nullptr) || tag != MSG_T0_RESULT) {
        wbDetail = fmt("no verify reply errno=%d", errno);
    } else {
        gotVerify = true;
        bool anyChecked = false, allOk = true;
        auto acc = [&](int64_t v) {
            if (v == -2 || v == -3) return;
            anyChecked = true;
            if (v != -1) allOk = false;
        };
        acc(res.mismatchB);
        acc(res.mismatchC);
        acc(res.mismatchD);
        acc(res.mismatchE);
        wbStatus = !anyChecked ? "SKIP" : (allOk ? "OK" : "FAIL");
        wbDetail = fmt("mask=0x%x vkWrite=%lld glWrite=%lld cpuWrite=%lld gpuFill=%lld (clientLock=%d rc=%d)",
                       writtenMask, (long long)res.mismatchB, (long long)res.mismatchC, (long long)res.mismatchD,
                       (long long)res.mismatchE, res.lockOk, res.lockErr);
    }
    record("T0-ahb-writeback-to-client", wbStatus, wbDetail);

    // composite tier verdict: handoff alone is not the tier
    std::vector<Leg> legs;
    {
        Leg l;
        l.name = "vk-import";
        l.decisive = true;
        l.attempted = vkMapped;
        l.readOk = vkMapped && vkCmp == -1;
        l.writeOk = gotVerify && (writtenMask & 1) && res.mismatchB == -1;
        if (!l.attempted)
            l.fail = vkFail.empty() ? "not attempted" : vkFail;
        else if (!l.readOk)
            l.fail = "server could not read the client payload: " + vkFail;
        else if (!l.writeOk)
            l.fail = fmt("server write not visible to the client (back=%lld)", (long long)res.mismatchB);
        legs.push_back(l);
    }
    {
        Leg l;
        l.name = "gl-import";
        l.decisive = true;
        l.attempted = glMapped;
        l.readOk = glMapped && glCmp == -1 && glPersistent;
        l.writeOk = gotVerify && (writtenMask & 2) && res.mismatchC == -1;
        if (!l.attempted)
            l.fail = glFail.empty() ? "not attempted" : glFail;
        else if (!l.readOk)
            l.fail = "GL side: " + glFail;
        else if (!l.writeOk)
            l.fail = fmt("server GL write not visible to the client (back=%lld)", (long long)res.mismatchC);
        legs.push_back(l);
    }
    {
        Leg l;
        l.name = "gpu";
        l.decisive = true;
        l.attempted = gt.ran;
        l.readOk = gt.readMismatch == -1;
        l.writeOk = gotVerify && (writtenMask & 8) && res.mismatchE == -1;
        if (!gt.ran)
            l.fail = "GPU touch did not run: " + gpuFail;
        else if (!l.readOk)
            l.fail = fmt("GPU read of the client allocation mismatched at %lld", (long long)gt.readMismatch);
        else if (!l.writeOk)
            l.fail = fmt("GPU write not visible to the client (back=%lld)", (long long)res.mismatchE);
        legs.push_back(l);
    }
    {
        Leg l;
        l.name = "cpu-lock";
        l.decisive = false;  // informational: proves the handle, not the tier
        l.attempted = cpuCmp != -3;
        l.readOk = cpuCmp == -1;
        l.writeOk = gotVerify && (writtenMask & 4) && res.mismatchD == -1;
        l.fail = cpuFail;
        legs.push_back(l);
    }
    std::string why;
    const char* status = legVerdict(legs, &why);
    record("T0-ahb-blob-transfer", status,
           fmt("%s | full chain handoff+import+map+compare+writeback | cpuCmp=%lld vkCmp=%lld glCmp=%lld "
               "glPersistentCoherent=%d gpuRan=%d | %s",
               legTrace(legs).c_str(), (long long)cpuCmp, (long long)vkCmp, (long long)glCmp, (int)glPersistent,
               (int)gt.ran, why.c_str()));

    sendMsg(sock, MSG_BYE, nullptr, 0, -1);
    std::string reap = reapChild(pid);
    pr("T0 %s", reap.c_str());
    AHardwareBuffer_release(ahb);
    close(sock);
}

// ---------------------------------------------------------------------------
// T0S: sustained lock. The T0 row above locks, writes, unlocks, and only then lets
// the GPU in - the one ordering AHardwareBuffer_lock/unlock is specified for. A
// persistent map is the opposite shape: the client keeps its pointer (= the lock)
// for the life of the store and writes through it while the server's GPU reads and
// writes the same pages. So here the client locks ONCE, never unlocks while the
// rounds run, and per round: CPU-writes REG_A, the server's GPU reads REG_A (Vulkan
// vkCmdCopyBuffer and a GL compute dispatch on the glBufferStorageExternalEXT
// buffer) and fills REG_E (Vulkan) / REG_F (GL), and the client reads both fills
// back through the still-held pointer. Only after the last round does it unlock and
// lock again, which separates "the GPU writes never landed" from "the held CPU
// mapping kept stale lines".
// ---------------------------------------------------------------------------

static int t0sClientRounds(int sock, AHardwareBuffer* ahb, void* p, int lockRc, uint32_t rounds,
                           uint32_t seedBase);

static int childT0Sustained(int sock) {
    setRecvTimeout(sock, 60);
    T0SRequest rq{};
    uint32_t tag = 0;
    size_t got = 0;
    if (!recvMsg(sock, &tag, &rq, sizeof(rq), &got, nullptr) || tag != MSG_T0S_REQUEST) {
        pr("child: bad T0S request errno=%d", errno);
        return 2;
    }
    AHardwareBuffer_Desc desc{};
    desc.width = (uint32_t)rq.size;
    desc.height = 1;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_BLOB;
    desc.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                 AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;
    AHardwareBuffer* ahb = nullptr;
    int rc = AHardwareBuffer_allocate(&desc, &ahb);
    T0Alloc alloc{};
    alloc.allocOk = (rc == 0 && ahb) ? 1 : 0;
    alloc.allocErr = rc;
    alloc.size = rq.size;
    if (!alloc.allocOk) {
        snprintf(alloc.note, sizeof(alloc.note), "AHardwareBuffer_allocate rc=%d errno=%d", rc, errno);
        sendMsg(sock, MSG_T0S_ALLOC, &alloc, sizeof(alloc), -1);
        return 3;
    }
    AHardwareBuffer_Desc back{};
    AHardwareBuffer_describe(ahb, &back);
    alloc.stride = back.stride;
    void* p = nullptr;
    const int lockRc = AHardwareBuffer_lock(
        ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &p);
    snprintf(alloc.note, sizeof(alloc.note), "desc w=%u usage=0x%llx held-lock rc=%d ptr=%p", back.width,
             (unsigned long long)back.usage, lockRc, p);
    if (lockRc != 0 || !p) {
        alloc.allocOk = 2;
        sendMsg(sock, MSG_T0S_ALLOC, &alloc, sizeof(alloc), -1);
        AHardwareBuffer_release(ahb);
        return 4;
    }
    memset(p, 0, (size_t)rq.size);
    if (!sendMsg(sock, MSG_T0S_ALLOC, &alloc, sizeof(alloc), -1)) return 5;
    int sendRc = AHardwareBuffer_sendHandleToUnixSocket(ahb, sock);
    pr("child: T0S held lock ptr=%p, sendHandle rc=%d", p, sendRc);
    if (sendRc != 0) return 6;
    const int rc2 = t0sClientRounds(sock, ahb, p, lockRc, rq.rounds, rq.seedBase);
    AHardwareBuffer_release(ahb);
    return rc2;
}

// The client half of the T0S rounds, after the handle has been sent: per round the CPU writes
// REG_A through the held lock, the server's GPU reads it and fills REG_E / REG_F, and the client
// checks both fills through the still-held pointer; then unlock + relock. The caller releases.
static int t0sClientRounds(int sock, AHardwareBuffer* ahb, void* p, int lockRc, uint32_t rounds,
                           uint32_t seedBase) {
    uint32_t tag = 0;
    size_t got = 0;
    uint32_t lastVk = 0, lastGl = 0, lastMask = 0;
    for (uint32_t i = 0; i < rounds; ++i) {
        T0SRound r{};
        r.round = i;
        r.seedA = seedBase + i * 0x01010101u;
        writeRegion(p, REG_A, r.seedA);  // through the held lock, no unlock
        if (!sendMsg(sock, MSG_T0S_ROUND, &r, sizeof(r), -1)) return 7;
        T0SGpuDone gd{};
        if (!recvMsg(sock, &tag, &gd, sizeof(gd), &got, nullptr) || tag != MSG_T0S_GPUDONE) {
            pr("child: T0S no gpu-done in round %u errno=%d", i, errno);
            return 8;
        }
        T0SCheck ck{};
        ck.round = i;
        ck.mismatchE = (gd.mask & 1) ? checkFillWord(p, REG_E, gd.vkWord) : -2;
        ck.mismatchF = (gd.mask & 2) ? checkFillWord(p, REG_F, gd.glWord) : -2;
        lastVk = gd.vkWord;
        lastGl = gd.glWord;
        lastMask = gd.mask;
        if (!sendMsg(sock, MSG_T0S_CHECK, &ck, sizeof(ck), -1)) return 9;
    }
    T0SDone dn{};
    dn.lockRc = lockRc;
    if (gHoldMs) {
        pr("child: holding the locked AHB (%p) for %u ms", p, gHoldMs);
        usleep(static_cast<useconds_t>(gHoldMs) * 1000u);
    }
    dn.unlockRc = AHardwareBuffer_unlock(ahb, nullptr);
    void* q = nullptr;
    dn.relockRc = AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &q);
    dn.relockMismatchE = dn.relockMismatchF = -2;
    if (dn.relockRc == 0 && q) {
        if (lastMask & 1) dn.relockMismatchE = checkFillWord(q, REG_E, lastVk);
        if (lastMask & 2) dn.relockMismatchF = checkFillWord(q, REG_F, lastGl);
        AHardwareBuffer_unlock(ahb, nullptr);
    }
    snprintf(dn.note, sizeof(dn.note), "held ptr=%p relock ptr=%p", p, q);
    sendMsg(sock, MSG_T0S_DONE, &dn, sizeof(dn), -1);
    recvMsg(sock, &tag, nullptr, 0, &got, nullptr);  // BYE
    return 0;
}

// A GL compute pass over the imported buffer: copies `readWord`.. (1024 words) into
// `out` and fills `fillWord`.. with `fill`. Word offsets are relative to the bound range.
static const char* kT0SComputeSrc =
    "#version 310 es\n"
    "layout(local_size_x = 64) in;\n"
    "layout(std430, binding = 0) buffer Ext { uint w[]; } ext;\n"
    "layout(std430, binding = 1) buffer Out { uint w[]; } outb;\n"
    "uniform uint uRead;\n"
    "uniform uint uFillAt;\n"
    "uniform uint uFill;\n"
    "void main() {\n"
    "  uint i = gl_GlobalInvocationID.x;\n"
    "  outb.w[i] = ext.w[uRead + i];\n"
    "  ext.w[uFillAt + i] = uFill;\n"
    "}\n";

static GLuint buildT0SCompute(std::string* fail) {
    GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(sh, 1, &kT0SComputeSrc, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
        *fail = std::string("compute compile failed: ") + log;
        glDeleteShader(sh);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, sh);
    glLinkProgram(prog);
    glDeleteShader(sh);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
        *fail = std::string("compute link failed: ") + log;
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static void t0sServe(VkCtx& c, GlCtx& g, bool glOk, int sock, AHardwareBuffer* ahb, uint64_t size,
                     uint32_t rounds, const char* rowPrefix, pid_t pid);
static void printSummary();

static void runT0SustainedParent(VkCtx& c, GlCtx& g, bool glOk, uint64_t size, uint32_t rounds) {
    const uint32_t seedBase = 0x5A000001u;
    int sock = -1;
    pid_t pid = spawnChild("t0s", &sock);
    if (pid < 0) {
        record("T0S-sustained-lock", "FAIL", "spawnChild failed");
        return;
    }
    T0SRequest rq{};
    rq.size = size;
    rq.rounds = rounds;
    rq.seedBase = seedBase;
    T0Alloc alloc{};
    uint32_t tag = 0;
    size_t got = 0;
    if (!sendMsg(sock, MSG_T0S_REQUEST, &rq, sizeof(rq), -1) ||
        !recvMsg(sock, &tag, &alloc, sizeof(alloc), &got, nullptr) || tag != MSG_T0S_ALLOC || alloc.allocOk != 1) {
        record("T0S-sustained-lock", "FAIL",
               fmt("client alloc/held lock failed: ok=%d rc=%d %s errno=%d", alloc.allocOk, alloc.allocErr, alloc.note,
                   errno));
        close(sock);
        reapChild(pid);
        return;
    }
    pr("T0S child allocated and holds its lock: %s", alloc.note);
    AHardwareBuffer* ahb = nullptr;
    int rc = AHardwareBuffer_recvHandleFromUnixSocket(sock, &ahb);
    if (rc != 0 || !ahb) {
        record("T0S-sustained-lock", "FAIL", fmt("recvHandleFromUnixSocket rc=%d errno=%d", rc, errno));
        close(sock);
        reapChild(pid);
        return;
    }
    t0sServe(c, g, glOk, sock, ahb, size, rounds, "T0S-sustained-lock", pid);
    AHardwareBuffer_release(ahb);
    close(sock);
}

// The server half of the T0S rounds on a received (still client-locked) AHB: import it into
// Vulkan and GL, run `rounds` rounds against the client on `sock`, take its DONE, send BYE, and
// record <rowPrefix>-vulkan / -gles / <rowPrefix>. `pid` >= 0 is a spawned child to reap. The
// caller releases the AHB and closes `sock`.
static void t0sServe(VkCtx& c, GlCtx& g, bool glOk, int sock, AHardwareBuffer* ahb, uint64_t size,
                     uint32_t rounds, const char* rowPrefix, pid_t pid) {
    uint32_t tag = 0;
    size_t got = 0;
    // --- Vulkan import of the (still client-locked) AHB ---
    VkBuffer vbuf = VK_NULL_HANDLE;
    VkDeviceMemory vmem = VK_NULL_HANDLE;
    void* vhost = nullptr;
    std::string vkFail;
    if (!c.hasAhb || !c.pGetAhbProps) {
        vkFail = "VK_ANDROID_external_memory_android_hardware_buffer absent";
    } else {
        VkAndroidHardwareBufferPropertiesANDROID props{};
        props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
        VkResult r = c.pGetAhbProps(c.device, ahb, &props);
        VkExternalMemoryBufferCreateInfo ext{};
        ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.pNext = &ext;
        bci.size = size;
        bci.usage = kProbeBufferUsage;
        if (r != VK_SUCCESS) {
            vkFail = "vkGetAndroidHardwareBufferPropertiesANDROID=" + vkStr(r);
        } else if ((r = vkCreateBuffer(c.device, &bci, nullptr, &vbuf)) != VK_SUCCESS) {
            vkFail = "vkCreateBuffer(AHB external)=" + vkStr(r);
            vbuf = VK_NULL_HANDLE;
        } else {
            int typeIdx = pickMemType(c.memProps, props.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (typeIdx < 0) typeIdx = pickMemType(c.memProps, props.memoryTypeBits, 0);
            VkImportAndroidHardwareBufferInfoANDROID imp{};
            imp.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
            imp.buffer = ahb;
            VkMemoryDedicatedAllocateInfo ded{};
            ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
            ded.buffer = vbuf;
            imp.pNext = &ded;
            VkMemoryAllocateInfo mai{};
            mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.pNext = &imp;
            mai.allocationSize = props.allocationSize;
            mai.memoryTypeIndex = (uint32_t)(typeIdx < 0 ? 0 : typeIdx);
            if ((r = vkAllocateMemory(c.device, &mai, nullptr, &vmem)) != VK_SUCCESS) {
                vkFail = "vkAllocateMemory(import AHB)=" + vkStr(r);
                vmem = VK_NULL_HANDLE;
            } else if ((r = vkBindBufferMemory(c.device, vbuf, vmem, 0)) != VK_SUCCESS) {
                vkFail = "vkBindBufferMemory=" + vkStr(r);
            } else {
                // informational: the server's own host view of the same pages
                if (vkMapMemory(c.device, vmem, 0, VK_WHOLE_SIZE, 0, &vhost) != VK_SUCCESS) vhost = nullptr;
            }
        }
    }
    const bool vkReady = vkFail.empty() && vbuf != VK_NULL_HANDLE && vmem != VK_NULL_HANDLE;

    // --- GL import + compute program ---
    std::string glFail;
    GLuint gbuf = 0, gout = 0, prog = 0;
    GLint locRead = -1, locFillAt = -1, locFill = -1;
    if (!glOk) {
        glFail = "no GL context";
    } else if (!g.hasGl("GL_EXT_external_buffer") || !g.pBufferStorageExternal || !g.pGetNativeClientBuffer) {
        glFail = "GL_EXT_external_buffer / eglGetNativeClientBufferANDROID absent";
    } else {
        EGLClientBuffer cb = g.pGetNativeClientBuffer(ahb);
        if (!cb) {
            glFail = fmt("eglGetNativeClientBufferANDROID=NULL egl=0x%04x", eglGetError());
        } else {
            glDrain();
            glGenBuffers(1, &gbuf);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, gbuf);
            g.pBufferStorageExternal(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)size, cb,
                                     GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT |
                                         GL_MAP_COHERENT_BIT_EXT | GL_DYNAMIC_STORAGE_BIT_EXT);
            GLenum e = glDrain();
            if (e != GL_NO_ERROR) {
                glFail = "glBufferStorageExternalEXT -> " + glErrStr(e);
            } else {
                glGenBuffers(1, &gout);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, gout);
                glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)kRegion, nullptr, GL_DYNAMIC_READ);
                prog = buildT0SCompute(&glFail);
                if (prog) {
                    locRead = glGetUniformLocation(prog, "uRead");
                    locFillAt = glGetUniformLocation(prog, "uFillAt");
                    locFill = glGetUniformLocation(prog, "uFill");
                }
                e = glDrain();
                if (glFail.empty() && e != GL_NO_ERROR) glFail = "GL setup -> " + glErrStr(e);
            }
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        }
    }
    const bool glReady = glFail.empty() && prog != 0;

    uint32_t vkRead = 0, vkWrite = 0, glRead = 0, glWrite = 0, vkHostRead = 0, roundsRun = 0;
    std::string vkFirst, glFirst, hostFirst;
    uint32_t lastVk = 0, lastGl = 0;
    for (uint32_t i = 0; i < rounds; ++i) {
        T0SRound r{};
        if (!recvMsg(sock, &tag, &r, sizeof(r), &got, nullptr) || tag != MSG_T0S_ROUND) {
            if (vkFirst.empty()) vkFirst = fmt("round %u: no client round message errno=%d", i, errno);
            break;
        }
        T0SGpuDone gd{};
        gd.round = i;
        gd.vkWord = 0x7E000000u + i;
        gd.glWord = 0x6C000000u + i;
        lastVk = gd.vkWord;
        lastGl = gd.glWord;
        if (vhost) {
            int64_t h = checkRegion(vhost, REG_A, r.seedA);
            if (h == -1)
                ++vkHostRead;
            else if (hostFirst.empty())
                hostFirst = fmt("round %u host mismatch at %lld", i, (long long)h);
        }
        if (vkReady) {
            GpuTouch gt = gpuTouch(c, vbuf, REG_A, r.seedA, REG_E, gd.vkWord);
            if (gt.ran) gd.mask |= 1;
            if (gt.ran && gt.readMismatch == -1)
                ++vkRead;
            else if (vkFirst.empty())
                vkFirst = fmt("round %u GPU read: %s", i, gt.fail.c_str());
        }
        if (glReady) {
            glUseProgram(prog);
            glUniform1ui(locRead, (GLuint)(REG_A * kRegion / 4));
            glUniform1ui(locFillAt, (GLuint)(REG_F * kRegion / 4));
            glUniform1ui(locFill, gd.glWord);
            glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, gbuf, (GLintptr)gRegionBase,
                              (GLsizeiptr)(kRegionCount * kRegion));
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gout);
            glDispatchCompute((GLuint)(kRegion / 4 / 64), 1, 1);
            glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
            glFinish();
            GLenum e = glDrain();
            if (e == GL_NO_ERROR) gd.mask |= 2;
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, gout);
            void* m = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)kRegion, GL_MAP_READ_BIT);
            int64_t mm = m ? checkPattern(m, kRegion, r.seedA) : -3;
            if (m) glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            if (e == GL_NO_ERROR && mm == -1)
                ++glRead;
            else if (glFirst.empty())
                glFirst = fmt("round %u GL compute read: err=%s mismatch=%lld", i, glErrStr(e).c_str(), (long long)mm);
        }
        if (!sendMsg(sock, MSG_T0S_GPUDONE, &gd, sizeof(gd), -1)) break;
        T0SCheck ck{};
        if (!recvMsg(sock, &tag, &ck, sizeof(ck), &got, nullptr) || tag != MSG_T0S_CHECK) break;
        ++roundsRun;
        if (ck.mismatchE == -1)
            ++vkWrite;
        else if ((gd.mask & 1) && vkFirst.empty())
            vkFirst = fmt("round %u: GPU fill not seen through the client's held lock (mismatch=%lld)", i,
                          (long long)ck.mismatchE);
        if (ck.mismatchF == -1)
            ++glWrite;
        else if ((gd.mask & 2) && glFirst.empty())
            glFirst = fmt("round %u: GL compute fill not seen through the client's held lock (mismatch=%lld)", i,
                          (long long)ck.mismatchF);
    }
    T0SDone dn{};
    dn.relockMismatchE = dn.relockMismatchF = -3;
    bool gotDone = recvMsg(sock, &tag, &dn, sizeof(dn), &got, nullptr) && tag == MSG_T0S_DONE;
    sendMsg(sock, MSG_BYE, nullptr, 0, -1);
    std::string reap = pid >= 0 ? reapChild(pid) : std::string("no child");
    const std::string rowVk = std::string(rowPrefix) + "-vulkan";
    const std::string rowGl = std::string(rowPrefix) + "-gles";

    auto rowStatus = [&](bool ready, uint32_t rd, uint32_t wr) -> const char* {
        if (!ready) return "FAIL";
        if (rd == rounds && wr == rounds && roundsRun == rounds) return "OK";
        if (rd > 0 || wr > 0) return "PARTIAL";
        return "FAIL";
    };
    record(rowVk.c_str(), rowStatus(vkReady, vkRead, vkWrite),
           fmt("rounds=%u/%u gpuReadOfHeldCpuWrite=%u gpuFillSeenByHeldLock=%u serverHostMapRead=%u%s | after "
               "unlock+relock: fill=%lld | %s%s",
               roundsRun, rounds, vkRead, vkWrite, vkHostRead, vhost ? "" : "(no host map)",
               (long long)dn.relockMismatchE, vkReady ? vkFirst.c_str() : vkFail.c_str(),
               hostFirst.empty() ? "" : (" | " + hostFirst).c_str()));
    record(rowGl.c_str(), rowStatus(glReady, glRead, glWrite),
           fmt("rounds=%u/%u computeReadOfHeldCpuWrite=%u computeFillSeenByHeldLock=%u | after unlock+relock: "
               "fill=%lld | %s",
               roundsRun, rounds, glRead, glWrite, (long long)dn.relockMismatchF,
               glReady ? glFirst.c_str() : glFail.c_str()));
    std::vector<Leg> legs;
    {
        Leg l;
        l.name = "vk-gpu";
        l.decisive = true;
        l.attempted = vkReady && roundsRun > 0;
        l.readOk = vkReady && vkRead == rounds;
        l.writeOk = vkReady && vkWrite == rounds;
        l.fail = vkReady ? vkFirst : vkFail;
        legs.push_back(l);
    }
    {
        Leg l;
        l.name = "gl-gpu";
        l.decisive = true;
        l.attempted = glReady && roundsRun > 0;
        l.readOk = glReady && glRead == rounds;
        l.writeOk = glReady && glWrite == rounds;
        l.fail = glReady ? glFirst : glFail;
        legs.push_back(l);
    }
    std::string why;
    const char* status = legVerdict(legs, &why);
    record(rowPrefix, status,
           fmt("%s | size=%llu regionBase=%llu rounds=%u client lock rc=%d, unlock rc=%d, relock rc=%d (%s) %s | %s",
               legTrace(legs).c_str(), (unsigned long long)size, (unsigned long long)gRegionBase, rounds,
               gotDone ? dn.lockRc : -99, gotDone ? dn.unlockRc : -99, gotDone ? dn.relockRc : -99,
               gotDone ? dn.note : "no done message", reap.c_str(), why.c_str()));

    if (prog) glDeleteProgram(prog);
    if (gout) glDeleteBuffers(1, &gout);
    if (gbuf) glDeleteBuffers(1, &gbuf);
    if (vhost) vkUnmapMemory(c.device, vmem);
    if (vmem) vkFreeMemory(c.device, vmem, nullptr);
    if (vbuf) vkDestroyBuffer(c.device, vbuf, nullptr);
    (void)lastVk;
    (void)lastGl;
}

// ---------------------------------------------------------------------------
// P11 B2 step 1: does a client's AHardwareBuffer cross the REAL routes into the server app?
//
//   server  `libMobileGLServer.so <endpoint> --serve`: the probe packaged in the render server's
//           place (-Pmobilegl.extmemProbeAsServer=ON), so MobileGLServerService execs it where the
//           supervisor would run - in the server app's own untrusted_app domain - and B1's broker
//           connects to it, writes its PairBind pair and hands the connected pair to the helper.
//           Each accepted pair: skip the PairBind frame on both connections (told apart by the
//           frame length: control 64 bytes, aux 68 - PairBindFrames.java), then serve the client.
//   client  `extmem_probe --route-client[=<endpoint>]`: exec'd by the B1 helper with
//           MOBILEGL_IPC_CONTROL=fd:<control>,<aux> (routes a/b), or connecting to <endpoint>
//           itself (same app only - another domain is refused `connectto`, B0).
//
// Per variant the client allocates a BLOB AHB, locks it ONCE for the whole run, and hands it over
//   direct  AHardwareBuffer_sendHandleToUnixSocket on the aux connection itself;
//   hop     on a fresh socketpair whose other end travels over aux by SCM_RIGHTS (the server
//           then receives the handle from that socket).
// Then the T0S rounds run on the control connection: the server imports the handle into Vulkan
// and GL, its GPU reads what the client's CPU wrote through the held lock and writes fills the
// client checks through the same held pointer. Rows: ROUTE-<variant>-<size>{,-vulkan,-gles}.
// ---------------------------------------------------------------------------

enum : uint32_t { MSG_RT_HELLO = 40, MSG_RT_HOP = 41, MSG_RT_SERVER = 42 };

struct RtHello {
    uint32_t variant;  // 0 direct, 1 hop
    uint32_t rounds;
    uint64_t size;
    uint64_t regionBase;
    uint32_t seedBase;
    int32_t pad;
    char client[192];
    char note[160];
};

struct RtServer {
    int32_t received;  // 1 = the handle arrived and describes as a BLOB of `size`
    int32_t pad;
    char server[192];
    char note[192];
};

static std::string selfContext() {
    std::string sec = readSmallFile("/proc/self/attr/current");
    while (!sec.empty() && (sec.back() == '\n' || sec.back() == '\0')) sec.pop_back();
    return fmt("%s uid=%d pid=%d", sec.c_str(), (int)getuid(), (int)getpid());
}

static const char* kRtControlPrefixHex =
    "100000004d474c4308000e0007000800080000000000000f0c0000000000060008000400060000000400000010000000";
static const char* kRtAuxPrefixHex =
    "100000004d474c4308000c0007000800080000000000000f0c00000008000c000800070008000000000000010400000010000000";

static std::vector<uint8_t> rtPairBindFrame(bool aux, const uint8_t nonce[16]) {
    const char* hex = aux ? kRtAuxPrefixHex : kRtControlPrefixHex;
    std::vector<uint8_t> payload;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
        char b[3] = {hex[i], hex[i + 1], 0};
        payload.push_back((uint8_t)strtoul(b, nullptr, 16));
    }
    payload.insert(payload.end(), nonce, nonce + 16);
    std::vector<uint8_t> out(8);
    const uint32_t magic = 0x464C474Du, len = (uint32_t)payload.size();
    memcpy(out.data(), &magic, 4);
    memcpy(out.data() + 4, &len, 4);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// Reads one framed control message's header and payload; returns the payload length or -1.
static int rtSkipFrame(int fd) {
    uint32_t hdr[2] = {0, 0};
    if (!readAll(fd, hdr, sizeof(hdr))) return -1;
    if (hdr[0] != 0x464C474Du || hdr[1] > 4096) return -1;
    std::vector<uint8_t> skip(hdr[1]);
    if (hdr[1] && !readAll(fd, skip.data(), hdr[1])) return -1;
    return (int)hdr[1];
}

static int rtListen(const char* endpoint) {
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    socklen_t len;
    if (endpoint[0] == '@') {
        strncpy(a.sun_path + 1, endpoint + 1, sizeof(a.sun_path) - 2);
        len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(endpoint + 1));
    } else {
        unlink(endpoint);
        strncpy(a.sun_path, endpoint, sizeof(a.sun_path) - 1);
        len = (socklen_t)sizeof(a);
    }
    if (bind(s, (struct sockaddr*)&a, len) != 0 || listen(s, 16) != 0) {
        pr("route server: bind/listen %s failed errno=%d", endpoint, errno);
        close(s);
        return -1;
    }
    return s;
}

static int rtConnect(const char* endpoint) {
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    socklen_t len;
    if (endpoint[0] == '@') {
        strncpy(a.sun_path + 1, endpoint + 1, sizeof(a.sun_path) - 2);
        len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(endpoint + 1));
    } else {
        strncpy(a.sun_path, endpoint, sizeof(a.sun_path) - 1);
        len = (socklen_t)sizeof(a);
    }
    if (connect(s, (struct sockaddr*)&a, len) != 0) {
        pr("route client: connect %s failed errno=%d", endpoint, errno);
        close(s);
        return -1;
    }
    return s;
}

static void rtServePair(VkCtx& c, GlCtx& g, bool glOk, int ctl, int aux) {
    setRecvTimeout(ctl, 60);
    setRecvTimeout(aux, 60);
    for (;;) {
        RtHello h{};
        uint32_t tag = 0;
        size_t got = 0;
        if (!recvMsg(ctl, &tag, &h, sizeof(h), &got, nullptr) || tag == MSG_BYE) break;
        if (tag != MSG_RT_HELLO) {
            pr("route server: unexpected tag %u", tag);
            break;
        }
        h.client[sizeof(h.client) - 1] = 0;
        h.note[sizeof(h.note) - 1] = 0;
        gRegionBase = h.regionBase;
        const char* vname = h.variant ? "hop" : "direct";
        pr("route server: client [%s] variant=%s size=%llu rounds=%u (%s)", h.client, vname,
           (unsigned long long)h.size, h.rounds, h.note);
        AHardwareBuffer* ahb = nullptr;
        int rc = -1;
        std::string how;
        if (h.variant == 0) {
            rc = AHardwareBuffer_recvHandleFromUnixSocket(aux, &ahb);
            how = fmt("recvHandleFromUnixSocket(aux) rc=%d errno=%d", rc, errno);
        } else {
            int hopFd = -1;
            uint32_t t2 = 0;
            if (!recvMsg(aux, &t2, nullptr, 0, &got, &hopFd) || t2 != MSG_RT_HOP || hopFd < 0) {
                how = fmt("hop socket did not arrive over aux (tag=%u fd=%d errno=%d)", t2, hopFd, errno);
            } else {
                rc = AHardwareBuffer_recvHandleFromUnixSocket(hopFd, &ahb);
                how = fmt("hop %s; recvHandleFromUnixSocket(hop) rc=%d errno=%d", describeFd(hopFd).c_str(), rc,
                          errno);
                close(hopFd);
            }
        }
        RtServer sv{};
        snprintf(sv.server, sizeof(sv.server), "%s", selfContext().c_str());
        if (rc == 0 && ahb) {
            AHardwareBuffer_Desc d{};
            AHardwareBuffer_describe(ahb, &d);
            sv.received = (d.format == AHARDWAREBUFFER_FORMAT_BLOB && d.width == h.size) ? 1 : 0;
            snprintf(sv.note, sizeof(sv.note), "%s | desc w=%u fmt=0x%x usage=0x%llx", how.c_str(), d.width,
                     d.format, (unsigned long long)d.usage);
        } else {
            snprintf(sv.note, sizeof(sv.note), "%s", how.c_str());
        }
        pr("route server: %s", sv.note);
        sendMsg(ctl, MSG_RT_SERVER, &sv, sizeof(sv), -1);
        const std::string row = fmt("ROUTE-%s-%llu", vname, (unsigned long long)h.size);
        if (!sv.received) {
            record(row.c_str(), "FAIL", fmt("client [%s] -> server [%s]: %s", h.client, sv.server, sv.note));
            if (ahb) AHardwareBuffer_release(ahb);
            break;
        }
        pr("route server: client [%s] -> server [%s]", h.client, sv.server);
        t0sServe(c, g, glOk, ctl, ahb, h.size, h.rounds, row.c_str(), -1);
        AHardwareBuffer_release(ahb);
    }
}

static int routeServe(const char* endpoint) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    gRole = "route-server";
    signal(SIGPIPE, SIG_IGN);
    pr("extmem route server: %s", selfContext().c_str());
    VkCtx c;
    const bool vkOk = vkCtxInit(c, false);
    GlCtx g;
    const bool glOk = glCtxInit(g);
    pr("extmem route server: vulkan=%d gl=%d", (int)vkOk, (int)glOk);
    if (!vkOk) return 1;
    int ls = rtListen(endpoint);
    if (ls < 0) return 2;
    // The service's readiness line (srv.sh waits for it) - the real supervisor says the same.
    pr("extmem route server: listening on %s", endpoint);
    int pendingCtl = -1, pendingAux = -1;
    for (;;) {
        int s = accept4(ls, nullptr, nullptr, SOCK_CLOEXEC);
        if (s < 0) {
            if (errno == EINTR) continue;
            pr("route server: accept errno=%d", errno);
            return 3;
        }
        setRecvTimeout(s, 10);
        const int n = rtSkipFrame(s);
        if (n == 64) {
            if (pendingCtl >= 0) close(pendingCtl);
            pendingCtl = s;
        } else if (n == 68) {
            if (pendingAux >= 0) close(pendingAux);
            pendingAux = s;
        } else {
            pr("route server: connection without a PairBind (frame=%d) closed", n);
            close(s);
            continue;
        }
        if (pendingCtl >= 0 && pendingAux >= 0) {
            pr("route server: paired control=%d aux=%d", pendingCtl, pendingAux);
            rtServePair(c, g, glOk, pendingCtl, pendingAux);
            close(pendingCtl);
            close(pendingAux);
            pendingCtl = pendingAux = -1;
            gResults.clear();
        }
    }
}

static int rtClientVariant(int ctl, int aux, uint32_t variant, uint64_t size, uint32_t rounds, bool atEnd) {
    AHardwareBuffer_Desc desc{};
    desc.width = (uint32_t)size;
    desc.height = 1;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_BLOB;
    desc.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                 AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;
    AHardwareBuffer* ahb = nullptr;
    const int arc = AHardwareBuffer_allocate(&desc, &ahb);
    const char* vname = variant ? "hop" : "direct";
    const std::string row = fmt("ROUTE-%s-%llu-client", vname, (unsigned long long)size);
    if (arc != 0 || !ahb) {
        record(row.c_str(), "FAIL", fmt("AHardwareBuffer_allocate rc=%d", arc));
        return 3;
    }
    void* p = nullptr;
    const int lockRc = AHardwareBuffer_lock(
        ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &p);
    if (lockRc != 0 || !p) {
        record(row.c_str(), "FAIL", fmt("held lock rc=%d", lockRc));
        AHardwareBuffer_release(ahb);
        return 4;
    }
    memset(p, 0, (size_t)size);
    gRegionBase = atEnd ? size - kRegionCount * kRegion : 0;
    RtHello h{};
    h.variant = variant;
    h.rounds = rounds;
    h.size = size;
    h.regionBase = gRegionBase;
    h.seedBase = 0x3B000001u + variant * 0x100u;
    snprintf(h.client, sizeof(h.client), "%s", selfContext().c_str());
    snprintf(h.note, sizeof(h.note), "held lock ptr=%p", p);
    int sendRc = -1;
    if (!sendMsg(ctl, MSG_RT_HELLO, &h, sizeof(h), -1)) {
        record(row.c_str(), "FAIL", fmt("hello send errno=%d", errno));
    } else if (variant == 0) {
        sendRc = AHardwareBuffer_sendHandleToUnixSocket(ahb, aux);
    } else {
        int sp[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp) == 0) {
            sendRc = AHardwareBuffer_sendHandleToUnixSocket(ahb, sp[0]);
            close(sp[0]);
            if (sendRc == 0 && !sendMsg(aux, MSG_RT_HOP, nullptr, 0, sp[1])) sendRc = -errno;
            close(sp[1]);
        }
    }
    RtServer sv{};
    uint32_t tag = 0;
    size_t got = 0;
    const bool haveServer = recvMsg(ctl, &tag, &sv, sizeof(sv), &got, nullptr) && tag == MSG_RT_SERVER;
    sv.server[sizeof(sv.server) - 1] = 0;
    sv.note[sizeof(sv.note) - 1] = 0;
    int rrc = 10;
    if (sendRc != 0 || !haveServer || !sv.received) {
        record(row.c_str(), "FAIL",
               fmt("sendRc=%d serverAnswered=%d received=%d server [%s]: %s", sendRc, (int)haveServer,
                   (int)sv.received, sv.server, sv.note));
    } else {
        rrc = t0sClientRounds(ctl, ahb, p, lockRc, rounds, h.seedBase);
        record(row.c_str(), rrc == 0 ? "OK" : "FAIL",
               fmt("client [%s] -> server [%s] | %s | rounds rc=%d (the server's rows carry the GPU verdict)",
                   h.client, sv.server, sv.note, rrc));
    }
    AHardwareBuffer_release(ahb);
    return rrc;
}

static int routeClient(const char* endpoint, uint64_t size, uint32_t rounds, bool atEnd) {
    gRole = "route-client";
    signal(SIGPIPE, SIG_IGN);
    int ctl = -1, aux = -1;
    const char* env = getenv("MOBILEGL_IPC_CONTROL");
    bool paired = false;
    if (endpoint && *endpoint) {
        ctl = rtConnect(endpoint);
        aux = rtConnect(endpoint);
    } else if (env && !strncmp(env, "fd:", 3) && sscanf(env + 3, "%d,%d", &ctl, &aux) == 2) {
        const char* pe = getenv("MOBILEGL_IPC_FD_PAIRED");
        paired = pe && !strcmp(pe, "1");
    } else {
        pr("route client: no endpoint and MOBILEGL_IPC_CONTROL is not fd:<c>,<a> (%s)", env ? env : "unset");
        return 2;
    }
    if (ctl < 0 || aux < 0) return 2;
    pr("route client: %s control=%d aux=%d %s aux=%s", selfContext().c_str(), ctl, aux,
       paired ? "paired at hand-off" : "presenting its own PairBind pair", describeFd(aux).c_str());
    if (!paired) {
        uint8_t nonce[16];
        FILE* r = fopen("/dev/urandom", "rb");
        if (!r || fread(nonce, 1, 16, r) != 16) memset(nonce, 0x5A, 16);
        if (r) fclose(r);
        const std::vector<uint8_t> fc = rtPairBindFrame(false, nonce), fa = rtPairBindFrame(true, nonce);
        if (!writeAll(ctl, fc.data(), fc.size()) || !writeAll(aux, fa.data(), fa.size())) {
            pr("route client: PairBind write failed errno=%d", errno);
            return 3;
        }
    }
    setRecvTimeout(ctl, 60);
    int worst = 0;
    for (uint32_t v = 0; v < 2; ++v) {
        const int rc = rtClientVariant(ctl, aux, v, size, rounds, atEnd);
        if (rc != 0) worst = rc;
    }
    sendMsg(ctl, MSG_BYE, nullptr, 0, -1);
    printSummary();
    close(ctl);
    close(aux);
    return worst;
}

#else  // !PROBE_HAVE_AHB

static int childT0(int) {
    pr("T0 is Android-only");
    return 1;
}
static void runT0Parent(VkCtx&, GlCtx&, bool, uint64_t) {
    record("T0-ahb-blob-transfer", "SKIP", "AHardwareBuffer is Android-only; host build cannot run T0");
}
static int childT0Sustained(int) {
    pr("T0S is Android-only");
    return 1;
}
static void runT0SustainedParent(VkCtx&, GlCtx&, bool, uint64_t, uint32_t) {
    record("T0S-sustained-lock", "SKIP", "AHardwareBuffer is Android-only; host build cannot run T0S");
}
static int routeServe(const char*) {
    pr("the AHB route server is Android-only");
    return 1;
}
static int routeClient(const char*, uint64_t, uint32_t, bool) {
    pr("the AHB route client is Android-only");
    return 1;
}

#endif  // PROBE_HAVE_AHB

// ---------------------------------------------------------------------------
// T3: VK_EXT_external_memory_host over a memfd-backed mapping
// ---------------------------------------------------------------------------

// Reserves an alignment-corrected window and places `fd` inside it.  Returns the
// aligned pointer, or nullptr; `*reserveOut` must be munmap'ed with
// `mapSize + align` bytes.
static void* mapAlignedFd(int fd, uint64_t mapSize, uint64_t align, void** reserveOut, std::string* fail) {
    *reserveOut = mmap(nullptr, (size_t)(mapSize + align), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (*reserveOut == MAP_FAILED) {
        *reserveOut = nullptr;
        *fail = fmt("reserve mmap errno=%d(%s)", errno, strerror(errno));
        return nullptr;
    }
    uintptr_t base = ((uintptr_t)*reserveOut + align - 1) & ~(uintptr_t)(align - 1);
    void* host = mmap((void*)base, (size_t)mapSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (host == MAP_FAILED) {
        *fail = fmt("mmap(fd, MAP_FIXED) errno=%d(%s)", errno, strerror(errno));
        munmap(*reserveOut, (size_t)(mapSize + align));
        *reserveOut = nullptr;
        return nullptr;
    }
    return host;
}

// Imports `host` as VkDeviceMemory and binds a buffer to it.
struct HostImport {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* mapped = nullptr;
    int typeIdx = -1;
    VkResult hostPtrProps = VK_NOT_READY;
    VkResult createResult = VK_NOT_READY;
    VkResult allocResult = VK_NOT_READY;
    VkResult bindResult = VK_NOT_READY;
    VkResult mapResult = VK_NOT_READY;
    uint32_t bits = 0;
    std::string fail;
};

static bool importHostPointer(VkCtx& c, void* host, uint64_t mapSize, HostImport& o) {
    VkMemoryHostPointerPropertiesEXT hp{};
    hp.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
    o.hostPtrProps = c.pGetHostPtrProps(c.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &hp);
    if (o.hostPtrProps != VK_SUCCESS) {
        o.fail = "vkGetMemoryHostPointerPropertiesEXT=" + vkStr(o.hostPtrProps);
        return false;
    }
    VkExternalMemoryBufferCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.pNext = &ext;
    bci.size = mapSize;
    bci.usage = kProbeBufferUsage;
    o.createResult = vkCreateBuffer(c.device, &bci, nullptr, &o.buf);
    VkMemoryRequirements req{};
    if (o.createResult == VK_SUCCESS) {
        vkGetBufferMemoryRequirements(c.device, o.buf, &req);
    } else {
        o.buf = VK_NULL_HANDLE;
        req.memoryTypeBits = 0xFFFFFFFFu;
    }
    o.bits = hp.memoryTypeBits & req.memoryTypeBits;
    o.typeIdx = pickMemType(c.memProps, o.bits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (o.typeIdx < 0) o.typeIdx = pickMemType(c.memProps, o.bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (o.typeIdx < 0) {
        o.fail = fmt("no host-visible memory type in hostPtrBits=0x%x & reqBits=0x%x", hp.memoryTypeBits,
                     req.memoryTypeBits);
        return false;
    }
    VkImportMemoryHostPointerInfoEXT imp{};
    imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = host;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &imp;
    mai.allocationSize = mapSize;
    mai.memoryTypeIndex = (uint32_t)o.typeIdx;
    o.allocResult = vkAllocateMemory(c.device, &mai, nullptr, &o.mem);
    if (o.allocResult != VK_SUCCESS) {
        o.mem = VK_NULL_HANDLE;
        o.fail = fmt("vkAllocateMemory(import host ptr)=%s type=%d bits=0x%x", vkStr(o.allocResult).c_str(), o.typeIdx,
                     o.bits);
        return false;
    }
    o.bindResult = (o.buf != VK_NULL_HANDLE) ? vkBindBufferMemory(c.device, o.buf, o.mem, 0) : VK_SUCCESS;
    o.mapResult = vkMapMemory(c.device, o.mem, 0, VK_WHOLE_SIZE, 0, &o.mapped);
    if (o.mapResult != VK_SUCCESS) {
        o.mapped = nullptr;
        o.fail = "vkMapMemory=" + vkStr(o.mapResult);
        return false;
    }
    return true;
}

static void releaseHostImport(VkCtx& c, HostImport& o) {
    if (o.mapped) vkUnmapMemory(c.device, o.mem);
    if (o.mem) vkFreeMemory(c.device, o.mem, nullptr);
    if (o.buf) vkDestroyBuffer(c.device, o.buf, nullptr);
    o.mapped = nullptr;
    o.mem = VK_NULL_HANDLE;
    o.buf = VK_NULL_HANDLE;
}

static int childT3(int sock) {
    setRecvTimeout(sock, 30);
    T3Offer offer{};
    uint32_t tag = 0;
    size_t got = 0;
    int fd = -1;
    if (!recvMsg(sock, &tag, &offer, sizeof(offer), &got, &fd) || tag != MSG_T3_OFFER) return 2;
    T3Result res{};
    res.mismatch = -3;
    res.gpuMismatch = -2;
    if (fd < 0) {
        snprintf(res.note, sizeof(res.note), "no fd");
        sendMsg(sock, MSG_T3_RESULT, &res, sizeof(res), -1);
        return 3;
    }
    snprintf(res.note, sizeof(res.note), "%s", describeFd(fd).c_str());
    void* p = mmap(nullptr, (size_t)offer.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        res.mmapOk = 0;
        res.mmapErrno = errno;
    } else {
        res.mmapOk = 1;
        res.mismatch = checkRegion(p, REG_A, offer.seedA);
        if (offer.gpuRan) res.gpuMismatch = checkFillWord(p, REG_E, offer.gpuWord);
        writeRegion(p, REG_B, offer.seedB);
        msync(p, (size_t)offer.size, MS_SYNC);
        munmap(p, (size_t)offer.size);
    }
    sendMsg(sock, MSG_T3_RESULT, &res, sizeof(res), -1);
    close(fd);
    return 0;
}

// The direction that makes T3 a tier: the CLIENT allocates the memory and the
// SERVER imports the client's host pointer.  The child is the client here.
static int childT3Client(int sock) {
    setRecvTimeout(sock, 30);
    T3cRequest rq{};
    uint32_t tag = 0;
    size_t got = 0;
    if (!recvMsg(sock, &tag, &rq, sizeof(rq), &got, nullptr) || tag != MSG_T3C_REQUEST) return 2;

    T3cReady ready{};
    ready.size = rq.size;
    int memfd = memfd_create("extmem_probe_client", 0);
    if (memfd < 0) {
        ready.err = errno;
        snprintf(ready.note, sizeof(ready.note), "memfd_create errno=%d(%s)", errno, strerror(errno));
        sendMsg(sock, MSG_T3C_READY, &ready, sizeof(ready), -1);
        return 3;
    }
    if (ftruncate(memfd, (off_t)rq.size) != 0) {
        ready.err = errno;
        snprintf(ready.note, sizeof(ready.note), "ftruncate errno=%d(%s)", errno, strerror(errno));
        sendMsg(sock, MSG_T3C_READY, &ready, sizeof(ready), -1);
        close(memfd);
        return 4;
    }
    void* p = mmap(nullptr, (size_t)rq.size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (p == MAP_FAILED) {
        ready.err = errno;
        snprintf(ready.note, sizeof(ready.note), "mmap errno=%d(%s)", errno, strerror(errno));
        sendMsg(sock, MSG_T3C_READY, &ready, sizeof(ready), -1);
        close(memfd);
        return 5;
    }
    memset(p, 0, (size_t)rq.size);
    writeRegion(p, REG_A, rq.seedA);
    ready.ok = 1;
    snprintf(ready.note, sizeof(ready.note), "client memfd %s", describeFd(memfd).c_str());
    if (!sendMsg(sock, MSG_T3C_READY, &ready, sizeof(ready), memfd)) {
        munmap(p, (size_t)rq.size);
        close(memfd);
        return 6;
    }

    T3cVerify ver{};
    T3cResult res{};
    res.mismatchB = res.mismatchE = -2;
    if (!recvMsg(sock, &tag, &ver, sizeof(ver), &got, nullptr) || tag != MSG_T3C_VERIFY) {
        munmap(p, (size_t)rq.size);
        close(memfd);
        return 7;
    }
    if (ver.mask & 1) res.mismatchB = checkRegion(p, REG_B, ver.seedB);
    if (ver.mask & 2) res.mismatchE = checkFillWord(p, REG_E, ver.gpuWord);
    snprintf(res.note, sizeof(res.note), "mask=0x%x", ver.mask);
    sendMsg(sock, MSG_T3C_RESULT, &res, sizeof(res), -1);
    munmap(p, (size_t)rq.size);
    close(memfd);
    return 0;
}

static void runT3ClientAllocParent(VkCtx& c, uint64_t size) {
    const char* route = "T3-client-memfd-server-import";
    if (!c.hasExtMemHost || !c.pGetHostPtrProps) {
        record(route, "UNSUPPORTED", "VK_EXT_external_memory_host absent");
        return;
    }
    uint64_t align = c.minImportedHostPointerAlignment ? c.minImportedHostPointerAlignment : 4096;
    uint64_t mapSize = (size + align - 1) & ~(align - 1);
    const uint32_t seedA = 0x3C3C0001u, seedB = 0x3C3C0002u, gpuWord = 0x3C3C1234u;

    int sock = -1;
    pid_t pid = spawnChild("t3c", &sock);
    if (pid < 0) {
        record(route, "FAIL", "spawnChild failed");
        return;
    }
    T3cRequest rq{};
    rq.size = mapSize;
    rq.seedA = seedA;
    if (!sendMsg(sock, MSG_T3C_REQUEST, &rq, sizeof(rq), -1)) {
        record(route, "FAIL", fmt("sendMsg(request) errno=%d", errno));
        close(sock);
        reapChild(pid);
        return;
    }
    T3cReady ready{};
    uint32_t tag = 0;
    size_t got = 0;
    int fd = -1;
    if (!recvMsg(sock, &tag, &ready, sizeof(ready), &got, &fd) || tag != MSG_T3C_READY) {
        record(route, "FAIL", fmt("no ready reply errno=%d %s", errno, reapChild(pid).c_str()));
        close(sock);
        return;
    }
    if (!ready.ok || fd < 0) {
        record(route, "FAIL", fmt("client could not allocate: %s (fd=%d)", ready.note, fd));
        if (fd >= 0) close(fd);
        sendMsg(sock, MSG_BYE, nullptr, 0, -1);
        reapChild(pid);
        close(sock);
        return;
    }
    pr("T3c server received the client's memfd: %s", describeFd(fd).c_str());

    void* reserve = nullptr;
    std::string mapFail;
    void* host = mapAlignedFd(fd, mapSize, align, &reserve, &mapFail);
    if (!host) {
        record(route, "FAIL", "server could not map the client's memfd: " + mapFail);
        close(fd);
        sendMsg(sock, MSG_BYE, nullptr, 0, -1);
        reapChild(pid);
        close(sock);
        return;
    }

    HostImport hi;
    bool imported = importHostPointer(c, host, mapSize, hi);
    int64_t cmpA = -3, gpuFillSeen = -3;
    GpuTouch gt;
    if (imported) {
        cmpA = checkRegion(hi.mapped, REG_A, seedA);   // server reads what the client wrote
        writeRegion(hi.mapped, REG_B, seedB);          // server writes back
        gt = gpuTouch(c, hi.buf, REG_A, seedA, REG_E, gpuWord);
        gpuFillSeen = gt.ran ? checkFillWord(hi.mapped, REG_E, gpuWord) : -3;
    }

    T3cVerify ver{};
    ver.seedB = seedB;
    ver.gpuWord = gpuWord;
    ver.mask = (imported ? 1u : 0u) | ((gt.ran && gpuFillSeen == -1) ? 2u : 0u);
    T3cResult res{};
    res.mismatchB = res.mismatchE = -3;
    bool gotVerify = false;
    if (sendMsg(sock, MSG_T3C_VERIFY, &ver, sizeof(ver), -1) &&
        recvMsg(sock, &tag, &res, sizeof(res), &got, nullptr) && tag == MSG_T3C_RESULT) {
        gotVerify = true;
    }

    std::vector<Leg> legs;
    {
        Leg l;
        l.name = "server-import";
        l.decisive = true;
        l.attempted = imported;
        l.readOk = imported && cmpA == -1;
        l.writeOk = gotVerify && (ver.mask & 1) && res.mismatchB == -1;
        if (!imported)
            l.fail = hi.fail;
        else if (!l.readOk)
            l.fail = fmt("server could not read the client's payload (cmp=%lld)", (long long)cmpA);
        else if (!l.writeOk)
            l.fail = fmt("server write not visible to the client (back=%lld)", (long long)res.mismatchB);
        legs.push_back(l);
    }
    {
        Leg l;
        l.name = "gpu";
        l.decisive = true;
        l.attempted = gt.ran;
        l.readOk = gt.readMismatch == -1;
        l.writeOk = gotVerify && (ver.mask & 2) && res.mismatchE == -1;
        if (!gt.ran)
            l.fail = "GPU touch did not run: " + gt.fail;
        else if (!l.readOk)
            l.fail = fmt("GPU read of the client's memory mismatched at %lld", (long long)gt.readMismatch);
        else if (!l.writeOk)
            l.fail = fmt("GPU write not visible to the client (serverMap=%lld clientMap=%lld)",
                         (long long)gpuFillSeen, (long long)res.mismatchE);
        legs.push_back(l);
    }
    std::string why;
    const char* status = legVerdict(legs, &why);
    std::string detail =
        fmt("%s | client allocates, server imports: align=%llu size=%llu hostPtrProps=%s create=%s alloc=%s bind=%s "
            "map=%s type=%d bits=0x%x | serverReadOfClient=%lld clientReadOfServer=%lld gpuRead=%lld "
            "gpuFill(server=%lld,client=%lld) | %s [%s] ",
            legTrace(legs).c_str(), (unsigned long long)align, (unsigned long long)mapSize,
            vkStr(hi.hostPtrProps).c_str(), vkStr(hi.createResult).c_str(), vkStr(hi.allocResult).c_str(),
            vkStr(hi.bindResult).c_str(), vkStr(hi.mapResult).c_str(), hi.typeIdx, hi.bits, (long long)cmpA,
            (long long)res.mismatchB, (long long)gt.readMismatch, (long long)gpuFillSeen, (long long)res.mismatchE,
            why.c_str(), ready.note);

    releaseHostImport(c, hi);
    munmap(host, (size_t)mapSize);
    if (reserve) munmap(reserve, (size_t)(mapSize + align));
    close(fd);
    sendMsg(sock, MSG_BYE, nullptr, 0, -1);
    detail += reapChild(pid);
    close(sock);
    record(route, status, detail);
}

static void runT3Parent(VkCtx& c, uint64_t size) {
    if (!c.hasExtMemHost || !c.pGetHostPtrProps) {
        record("T3-external-memory-host", "UNSUPPORTED", "VK_EXT_external_memory_host absent");
        record("T3-memfd-cross-process", "UNSUPPORTED", "VK_EXT_external_memory_host absent");
        return;
    }
    uint64_t align = c.minImportedHostPointerAlignment ? c.minImportedHostPointerAlignment : 4096;
    uint64_t mapSize = (size + align - 1) & ~(align - 1);

    int memfd = memfd_create("extmem_probe", 0);
    if (memfd < 0) {
        record("T3-external-memory-host", "FAIL", fmt("memfd_create errno=%d", errno));
        return;
    }
    if (ftruncate(memfd, (off_t)mapSize) != 0) {
        record("T3-external-memory-host", "FAIL", fmt("ftruncate errno=%d", errno));
        close(memfd);
        return;
    }
    void* reserve = nullptr;
    std::string mapFail;
    void* host = mapAlignedFd(memfd, mapSize, align, &reserve, &mapFail);
    if (!host) {
        record("T3-external-memory-host", "FAIL", mapFail);
        close(memfd);
        return;
    }
    const uint32_t seedA = 0x33330001u, seedB = 0x33330002u, gpuWord = 0x33331234u;
    memset(host, 0, (size_t)mapSize);
    writeRegion(host, REG_A, seedA);

    HostImport hi;
    bool imported = importHostPointer(c, host, mapSize, hi);
    int64_t cmpA = -3, gpuFillSeen = -3;
    GpuTouch gt;
    if (imported) {
        cmpA = checkRegion(hi.mapped, REG_A, seedA);
        gt = gpuTouch(c, hi.buf, REG_A, seedA, REG_E, gpuWord);
        gpuFillSeen = gt.ran ? checkFillWord(host, REG_E, gpuWord) : -3;
    }
    {
        std::vector<Leg> legs;
        Leg l;
        l.name = "import-map";
        l.decisive = true;
        l.attempted = imported;
        // one process on both ends here, so the "write back" direction is the
        // imported mapping seeing the original mmap's bytes
        l.readOk = imported && cmpA == -1;
        l.writeOk = imported && cmpA == -1;
        l.fail = imported ? (cmpA == -1 ? "" : fmt("payload mismatch at %lld", (long long)cmpA)) : hi.fail;
        legs.push_back(l);
        Leg gl;
        gl.name = "gpu";
        gl.decisive = true;
        gl.attempted = gt.ran;
        gl.readOk = gt.readMismatch == -1;
        gl.writeOk = gpuFillSeen == -1;
        if (!gt.ran)
            gl.fail = "GPU touch did not run: " + gt.fail;
        else if (!gl.readOk)
            gl.fail = fmt("GPU read mismatched at %lld", (long long)gt.readMismatch);
        else if (!gl.writeOk)
            gl.fail = fmt("GPU write not visible through the host mapping (at %lld)", (long long)gpuFillSeen);
        legs.push_back(gl);
        std::string why;
        record("T3-external-memory-host", legVerdict(legs, &why),
               fmt("%s | align=%llu type=%d bits=0x%x hostPtrProps=%s alloc=%s bind=%s map=%s mismatch=%lld "
                   "gpuRead=%lld gpuFill=%lld %s",
                   legTrace(legs).c_str(), (unsigned long long)align, hi.typeIdx, hi.bits,
                   vkStr(hi.hostPtrProps).c_str(), vkStr(hi.allocResult).c_str(), vkStr(hi.bindResult).c_str(),
                   vkStr(hi.mapResult).c_str(), (long long)cmpA, (long long)gt.readMismatch,
                   (long long)gpuFillSeen, why.c_str()));
    }

    // the same memfd handed to another process
    int sock = -1;
    pid_t pid = spawnChild("t3", &sock);
    if (pid < 0) {
        record("T3-memfd-cross-process", "FAIL", "spawnChild failed");
    } else {
        T3Offer off{};
        off.size = mapSize;
        off.seedA = seedA;
        off.seedB = seedB;
        off.gpuWord = gpuWord;
        off.gpuRan = (gt.ran && gpuFillSeen == -1) ? 1u : 0u;
        if (!sendMsg(sock, MSG_T3_OFFER, &off, sizeof(off), memfd)) {
            record("T3-memfd-cross-process", "FAIL", fmt("sendMsg errno=%d", errno));
        } else {
            T3Result res{};
            uint32_t tag = 0;
            size_t got = 0;
            if (!recvMsg(sock, &tag, &res, sizeof(res), &got, nullptr) || tag != MSG_T3_RESULT) {
                record("T3-memfd-cross-process", "FAIL", fmt("no reply errno=%d", errno));
            } else {
                int64_t back = res.mmapOk ? checkRegion(host, REG_B, seedB) : -3;
                std::vector<Leg> legs;
                Leg l;
                l.name = "peer-mmap";
                l.decisive = true;
                l.attempted = res.mmapOk != 0;
                l.readOk = res.mmapOk && res.mismatch == -1 && (!off.gpuRan || res.gpuMismatch == -1);
                l.writeOk = res.mmapOk && back == -1;
                if (!l.attempted)
                    l.fail = fmt("peer mmap failed errno=%d(%s)", res.mmapErrno, strerror(res.mmapErrno));
                else if (!l.readOk)
                    l.fail = fmt("peer could not read (cmp=%lld gpuCmp=%lld)", (long long)res.mismatch,
                                 (long long)res.gpuMismatch);
                else if (!l.writeOk)
                    l.fail = fmt("peer write not visible here (back=%lld)", (long long)back);
                legs.push_back(l);
                std::string why;
                record("T3-memfd-cross-process", legVerdict(legs, &why),
                       fmt("%s | child mmap=%d errno=%d cmp=%lld gpuCmp=%lld writeback=%lld %s [%s]",
                           legTrace(legs).c_str(), res.mmapOk, res.mmapErrno, (long long)res.mismatch,
                           (long long)res.gpuMismatch, (long long)back, why.c_str(), res.note));
            }
        }
        sendMsg(sock, MSG_BYE, nullptr, 0, -1);
        reapChild(pid);
        close(sock);
    }

    releaseHostImport(c, hi);
    munmap(host, (size_t)mapSize);
    if (reserve) munmap(reserve, (size_t)(mapSize + align));
    close(memfd);
}

// ---------------------------------------------------------------------------
// T4: the image the peer allocated OUTSIDE Android
//
// The {Owner=Platform, Storage=OfferedImage} cell of an Anland-style Wayland host: a
// glibc peer in a Droidspaces container creates the GBM buffer on the device the
// container owns and offers it here as a dma-buf, and this side -- the one holding the
// live EGL/GLES context -- has to be able to READ it and to DRAW into it.
//
// The topology is the reverse of every other leg on purpose.  The peer is not a re-exec
// of /proc/self/exe: nothing here needs a second driver instance (the only reason the
// other legs exec is that driver threads and device state do not survive fork), and the
// peer has to be glibc to create a GBM buffer at all.  So this side LISTENS on an
// abstract unix socket and the peer dials it, and because a Droidspaces container runs
// with net_mode=host, one network namespace holds both ends.  The peer is therefore not
// this process's child: there is nothing to reapChild(), and the frozen protocol has no
// MSG_BYE for T4, so the socket close is the whole teardown.
//
// Two failures must never be reported the same way:
//   capability  (fourcc, modifier) is not in the driver's own eglQueryDmaBufFormatsEXT /
//               eglQueryDmaBufModifiersEXT answer.  Refusing a pair the driver never
//               advertised is the driver being right, and no import attempt can change it.
//   driver bug  the pair IS advertised and the import still fails.
// The capability question is asked first and its answer travels in the row.
//
// Both paths are read at (0,0) AND at the far corner: a wrong stride or offset shifts the
// corner texel, which a one-pixel read cannot see.
//
// T4 is two rows over that one connection, and they answer different halves:
//   T4-image-import  the container's dma-buf, imported here.  Gated on
//                    EGL_EXT_image_dma_buf_import: a driver without it gets a capability
//                    answer recorded before the ladder is entered, never an import
//                    failure dressed up as a driver bug.
//   T4-ahb-image     an AHardwareBuffer minted on THIS side and imported in the container,
//                    which is the direction that still answers on such a driver.
//
// GL hands back channels (R,G,B,A) where the peer's seed is memory bytes, and the fourcc the
// peer allocates -- GBM's ARGB8888 -- is B,G,R,A in memory.  A working import therefore reads
// back the seed's bytes in CHANNEL order rather than verbatim, so the row carries both
// readings: the verbatim one the protocol specifies, and the channel-order one beside it, so
// the byte-order convention can never be mistaken for a broken import (and vice versa).
//
// The host build compiles this section, but its runT4 is a one-line SKIP row: the only way
// this side can be dialed is rtListen(), which lives inside the AHardwareBuffer guard with
// the rest of the T0/route block, and a host build has neither a listener nor a container
// peer.  T4 itself needs no AHardwareBuffer -- PROBE_HAVE_AHB is simply the macro that
// carries rtListen() -- so the guard is spelled out here with the stub beside it rather
// than left to look like an Android-only leg.
// ---------------------------------------------------------------------------


#if PROBE_HAVE_AHB

// The peer spells the same literal, so neither side depends on the other's byte order.
static const uint32_t kT4Magic = 0x5434474DU;

enum : uint32_t {
    MSG_T4_OFFER = 50,
    MSG_T4_READ = 51,
    MSG_T4_WROTE = 52,
    MSG_T4_ACK = 53,
    MSG_T4_RESULT = 54,
    // The Android-minted half of T4, appended so that no existing tag moves.  The
    // AHardwareBuffer handle travels beside the offer exactly the way the T0 legs
    // send theirs -- AHardwareBuffer_sendHandleToUnixSocket into one end of a
    // socketpair, and that end's peer over SCM_RIGHTS with the message.
    MSG_T4_AHB_OFFER = 55,
    MSG_T4_AHB_ACK = 56,
};

// Byte for byte the peer's struct in peer/t4_gbm_peer.c: every field is fixed width, so the
// two compilers cannot disagree about padding even though only one of them is bionic.
struct T4Layout {
    uint32_t magic, version, width, height, fourcc, stride, offset, reserved;
    uint64_t modifier;  // UINT64_MAX = the peer has none, so rung 0 has nothing to name
    uint8_t seed[4];    // what the peer filled the image with, in the buffer's memory order
    char peer[128];
    char note[128];
};

struct T4Read {
    int32_t texOk, rbOk, rung, pad;
    uint8_t texWord[4], texCorner[4], rbWord[4], rbCorner[4];
    char importNote[160];
};

// path 0 = renderbuffer, 1 = texture.  `color` is the byte sequence the peer should find in
// the buffer, so it is in memory order and not in GL channel order.
struct T4Wrote {
    int32_t path;
    uint8_t color[4];
};

struct T4Ack {
    int32_t ok;
    uint8_t observed[4];
    char note[128];
};

// MSG_T4_AHB_OFFER: the buffer THIS side minted, described so the container can
// import it without guessing any part of the layout.  The fourcc is the DRM name
// whose memory order is the order AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM names --
// DRM_FORMAT_ABGR8888, fourcc_code('A','B','2','4') = 0x34324241, whose
// little-endian [31:0] A:B:G:R layout puts R,G,B,A in bytes 0..3, the same pairing
// gralloc itself uses for HAL_PIXEL_FORMAT_RGBA_8888 -- so the bytes the peer
// compares are the bytes glClearColor() produced, and no channel-renaming step
// sits between the two sides where a convention could pass for a working image.
// The stride is the stride AHardwareBuffer_describe() reported, never width*4.
struct T4AhbOffer {
    uint32_t magic, version, width, height, stride, fourcc;
    uint8_t seed[4];  // what this side cleared into the buffer, in memory order
    char note[128];
};

// MSG_T4_AHB_ACK: what the container's GBM import made of the fd, what both pixels
// of its mapping held BEFORE it wrote anything, and the pattern it then wrote over
// the whole mapping.  `matched` is the peer's own comparison -- bit0 for (0,0),
// bit1 for the far corner -- and this side recomputes it from the two pixel fields
// instead of trusting it, so a disagreement between the bits and the bytes is
// visible in the row.
struct T4AhbAck {
    int32_t imported;  // the peer's gbm_bo_import returned a bo
    int32_t matched;   // 1 = (0,0) held the announced seed, 2 = the far corner did
    uint32_t stride;   // the stride the peer's own mapping reports
    uint8_t observed[4];
    uint8_t observedCorner[4];
    uint8_t pattern[4];  // zero when the peer could not write anything
    char note[128];
};

// The dma-buf import entry points stay local to T4 rather than joining GlCtx: T1-gles asks
// the same question through GL_EXT_memory_object, and widening GlCtx would move that leg
// for no reason.
struct T4Egl {
    PFNEGLCREATEIMAGEKHRPROC pCreateImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC pDestroyImage = nullptr;
    PFNEGLQUERYDMABUFFORMATSEXTPROC pFormats = nullptr;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC pModifiers = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pTargetTexture = nullptr;
    PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC pTargetRb = nullptr;
};

// The two-pixel read only means anything byte-exact: a wrong stride or offset shows up as
// differing bytes, which a decimal rendering would hide.
static std::string t4Hex4(const uint8_t* b) {
    return fmt("%02x%02x%02x%02x", b[0], b[1], b[2], b[3]);
}

// glClientWaitSync answers in its own vocabulary, which glErrStr does not cover.
static std::string t4WaitStr(GLenum w) {
    switch (w) {
        case GL_ALREADY_SIGNALED: return "GL_ALREADY_SIGNALED";
        case GL_CONDITION_SATISFIED: return "GL_CONDITION_SATISFIED";
        case GL_TIMEOUT_EXPIRED: return "GL_TIMEOUT_EXPIRED";
        case GL_WAIT_FAILED: return "GL_WAIT_FAILED";
        default: return fmt("GL(0x%04x)", (unsigned)w);
    }
}
// The driver's own answer to "do you take this (fourcc, modifier) pair", asked before any
// import so that a pair the driver never advertised can never be reported as an import
// failure.  `asked` says whether the answer means anything at all.
struct T4Cap {
    bool asked = false;
    bool formatListed = false;
    bool listed = false;
    int formats = 0;
    int modifiers = 0;
    std::string note;
};

static T4Cap t4Capability(EGLDisplay dpy, const T4Egl& e, uint32_t fourcc, uint64_t modifier) {
    T4Cap cap;
    if (!e.pFormats || !e.pModifiers) {
        cap.note = "EGL_EXT_image_dma_buf_import_modifiers absent, or its two entry points are: the driver "
                   "publishes no (fourcc, modifier) list, so an import failure cannot be attributed either way";
        return cap;
    }
    EGLint total = 0;
    if (!e.pFormats(dpy, 0, nullptr, &total) || total <= 0) {
        cap.note = fmt("eglQueryDmaBufFormatsEXT(list) -> 0x%04x num=%d", (unsigned)eglGetError(), (int)total);
        return cap;
    }
    std::vector<EGLint> formats((size_t)total);
    EGLint filled = 0;
    if (!e.pFormats(dpy, total, formats.data(), &filled)) {
        cap.note = fmt("eglQueryDmaBufFormatsEXT(%d) -> 0x%04x", (int)total, (unsigned)eglGetError());
        return cap;
    }
    cap.asked = true;
    cap.formats = (int)filled;
    for (EGLint i = 0; i < filled; ++i) {
        if ((uint32_t)formats[(size_t)i] == fourcc) cap.formatListed = true;
    }
    if (!cap.formatListed) {
        cap.note = fmt("fourcc=0x%08x is not among the %d formats the driver lists: this is a capability answer, "
                       "not an import failure",
                       fourcc, (int)filled);
        return cap;
    }
    EGLint nmods = 0;
    if (!e.pModifiers(dpy, (EGLint)fourcc, 0, nullptr, nullptr, &nmods)) {
        cap.note = fmt("fourcc=0x%08x is listed, eglQueryDmaBufModifiersEXT(list) -> 0x%04x", fourcc,
                       (unsigned)eglGetError());
        return cap;
    }
    cap.modifiers = (int)nmods;
    if (modifier == UINT64_MAX) {
        // Nothing to look up: the implicit modifier is the only one rung 1 can name, and it
        // is legal for every format the driver lists.
        cap.listed = true;
        cap.note = fmt("fourcc=0x%08x is listed; the driver lists %d explicit modifier(s) but the peer offered "
                       "none, so rung 1 is the only rung that can be spelled",
                       fourcc, (int)nmods);
        return cap;
    }
    if (nmods <= 0) {
        cap.note = fmt("fourcc=0x%08x is listed with no explicit modifiers and the peer offered modifier=0x%016llx: "
                       "the driver never advertised that pair",
                       fourcc, (unsigned long long)modifier);
        return cap;
    }
    std::vector<EGLuint64KHR> mods((size_t)nmods);
    EGLint modFilled = 0;
    if (!e.pModifiers(dpy, (EGLint)fourcc, nmods, mods.data(), nullptr, &modFilled)) {
        cap.note = fmt("eglQueryDmaBufModifiersEXT(fourcc=0x%08x, %d) -> 0x%04x", fourcc, (int)nmods,
                       (unsigned)eglGetError());
        return cap;
    }
    for (EGLint i = 0; i < modFilled; ++i) {
        if ((uint64_t)mods[(size_t)i] == modifier) cap.listed = true;
    }
    cap.note = cap.listed
                   ? fmt("fourcc=0x%08x modifier=0x%016llx is in the driver's own list (%d formats, %d modifiers "
                         "for this one), so any import failure below is the driver refusing its own pair",
                         fourcc, (unsigned long long)modifier, (int)filled, (int)modFilled)
                   : fmt("fourcc=0x%08x is listed but modifier=0x%016llx is not among its %d modifiers", fourcc,
                         (unsigned long long)modifier, (int)modFilled);
    return cap;
}

// The ladder EGL_EXT_image_dma_buf_import{,_modifiers} defines.  Rung 0 names the modifier
// through EGL_DMA_BUF_PLANE0_MODIFIER_*_EXT, rung 1 is the original four-integer form.  A
// peer with no modifier has no rung-0 spelling at all, and the ladder records that as
// "skipped" rather than as a rejection, so "rung 1 accepted" cannot be misread as the
// driver having refused rung 0.
struct T4Import {
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    int rung = -1;
    std::string accepted = "none";
    std::string ladder;
    std::string fail;
};

static T4Import t4ImportImage(EGLDisplay dpy, const T4Egl& e, int fd, const T4Layout& lay) {
    T4Import imp;
    for (int rung = 0; rung < 2; ++rung) {
        if (rung == 0 && lay.modifier == UINT64_MAX) {
            imp.ladder += "rung0=skipped(peer offered no modifier) ";
            continue;
        }
        EGLint attrs[24];
        int n = 0;
        attrs[n++] = EGL_WIDTH;
        attrs[n++] = (EGLint)lay.width;
        attrs[n++] = EGL_HEIGHT;
        attrs[n++] = (EGLint)lay.height;
        attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
        attrs[n++] = (EGLint)lay.fourcc;
        attrs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;
        attrs[n++] = fd;
        attrs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        attrs[n++] = (EGLint)lay.offset;
        attrs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        attrs[n++] = (EGLint)lay.stride;
        if (rung == 0) {
            attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
            attrs[n++] = (EGLint)(lay.modifier & 0xFFFFFFFFu);
            attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
            attrs[n++] = (EGLint)(lay.modifier >> 32);
        }
        attrs[n++] = EGL_NONE;
        EGLImageKHR img = e.pCreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
        // read the error either way: it is this call's, and one left in the queue would be
        // blamed on the next one
        const EGLint err = eglGetError();
        if (img != EGL_NO_IMAGE_KHR) {
            imp.image = img;
            imp.rung = rung;
            imp.accepted = rung == 0 ? "rung0-modifier" : "rung1-legacy";
            imp.ladder += fmt("rung%d=accepted ", rung);
            return imp;
        }
        imp.ladder += fmt("rung%d=0x%04x ", rung, (unsigned)err);
        imp.fail = fmt("eglCreateImageKHR(rung%d) -> 0x%04x", rung, (unsigned)err);
    }
    return imp;
}
// ---------------------------------------------------------------------------
// T4's second route to an EGLImage: wrap the peer's fd as an AHardwareBuffer
// ---------------------------------------------------------------------------
//
// WHY THIS EXISTS.  EGL_LINUX_DMA_BUF_EXT is the portable spelling and this driver does
// not have it (measured), which left the cross-process image story depending on the HOST
// allocating the buffer instead.  But the device ships AHardwareBuffer_createFromHandle
// in libnativewindow.so: the NDK does not declare it because it is a SystemApi, and plain
// C has no opinion about that.  If it accepts a container's bare dma-buf, the detour
// disappears -- the compositor keeps minting its buffers exactly as it does today and the
// render server wraps the fd it is already sent, with no host change at all.
//
// handleType is tried BOTH ways (1 for a handle from AHardwareBuffer_getNativeHandle, 2
// for a dma-buf) because that enum is not in the NDK here and guessing one would turn a
// two-line answer into an unattributable FAIL.  Nothing but the integer reaches the
// platform.
//
// WHAT A SUCCESS HERE WOULD AND WOULD NOT SAY.  Wrap plus EGLImage means the driver takes
// the image.  It does NOT say the driver will render into memory gralloc never described
// -- that is what the two-pixel round trip at the end of this row measures, and it is the
// only thing that settles it.

struct T4NativeHandle {  // native_handle_t, which the NDK does not declare
    int version;
    int numFds;
    int numInts;
    int data[1];
};

struct T4WrapApi {
    void* lib = nullptr;
    int (*createFromHandle)(const void* desc, const T4NativeHandle* handle, int handleType, void** out) = nullptr;
};

static T4WrapApi t4WrapApi() {
    T4WrapApi a;
    // libnativewindow.so first: that is where the measured device exports it.
    const char* libs[] = {"libnativewindow.so", "libandroid.so", "libui.so"};
    for (const char* name : libs) {
        a.lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (!a.lib) continue;
        a.createFromHandle = (int (*)(const void*, const T4NativeHandle*, int, void**))dlsym(
            a.lib, "AHardwareBuffer_createFromHandle");
        if (a.createFromHandle) return a;
        dlclose(a.lib);
        a.lib = nullptr;
    }
    return a;
}

// AHardwareBuffer_Format for the fourcc the peer named, or 0 for a pairing this row has
// never seen.  The memory order decides it: 'AB24' holds R,G,B,A and 'AR24' holds B,G,R,A,
// and the two numbers below say exactly that in Android's vocabulary.  They are written
// out because they are the ABI of an enum the NDK does not carry.
static uint32_t t4AhbFormatFor(uint32_t fourcc) {
    switch (fourcc) {
        case 0x34324241u: return 1u;  // 'AB24' DRM_FORMAT_ABGR8888 -> R8G8B8A8_UNORM
        case 0x34325241u: return 5u;  // 'AR24' DRM_FORMAT_ARGB8888 -> B8G8R8A8_UNORM
        default: return 0u;
    }
}

// The AHB the row wrapped.  The EGLImage keeps using it until the row ends, and the row
// ends at process exit, so the reference is deliberately never dropped: releasing a buffer
// out from under a live image would be a use-after-free, which is a worse answer than a
// leak in a program that is about to print its summary and exit.
static void* gT4WrappedAhb = nullptr;

static T4Import t4ImportViaWrap(GlCtx& g, const T4Egl& e, int fd, const T4Layout& lay) {
    T4Import imp;
    const T4WrapApi api = t4WrapApi();
    if (!api.createFromHandle) {
        imp.ladder = "wrap-api=absent ";
        imp.fail = "no library here exports AHardwareBuffer_createFromHandle";
        return imp;
    }
    // ahbFmt, not fmt: the file's fmt() is the printf helper, and a local named the same
    // would shadow it for the whole of this function.
    const uint32_t ahbFmt = t4AhbFormatFor(lay.fourcc);
    if (ahbFmt == 0) {
        imp.ladder = "wrap-api=present format=unknown ";
        imp.fail = fmt("fourcc=0x%08x has no AHardwareBuffer_Format on this row", lay.fourcc);
        return imp;
    }
    if (!g.pGetNativeClientBuffer) {
        imp.ladder = "wrap-api=present clientBuffer=absent ";
        imp.fail = "eglGetNativeClientBufferANDROID is not loadable, so an AHardwareBuffer cannot become an "
                   "EGLClientBuffer";
        return imp;
    }

    // AHardwareBuffer_Desc::stride is in PIXELS while the peer's layout is in bytes -- the
    // same trap that cost the Android-minted row a device run, hence the division.
    struct Desc {
        uint32_t width, height, layers, format;
        uint64_t usage;
        uint32_t stride, rfu0;
        uint64_t rfu1;
    } desc{};
    desc.width = lay.width;
    desc.height = lay.height;
    desc.layers = 1;
    desc.format = ahbFmt;
    desc.usage = (1u << 8) | (1u << 9);  // GPU_SAMPLED_IMAGE | GPU_COLOR_OUTPUT
    desc.stride = lay.stride / 4u;

    uint8_t hbuf[sizeof(T4NativeHandle) + 2 * sizeof(int)] = {0};
    T4NativeHandle* h = (T4NativeHandle*)hbuf;
    h->version = (int)sizeof(hbuf);
    h->numFds = 1;
    h->numInts = 0;
    h->data[0] = fd;

    for (int type = 1; type <= 2; ++type) {
        void* ahb = nullptr;
        const int rc = api.createFromHandle(&desc, h, type, &ahb);
        imp.ladder += fmt("handleType%d=rc%d ", type, rc);
        if (rc != 0 || !ahb) continue;
        EGLClientBuffer cb = g.pGetNativeClientBuffer((const struct AHardwareBuffer*)ahb);
        const EGLint cbErr = eglGetError();
        if (!cb) {
            imp.ladder += fmt("clientBuffer(type%d)=null(0x%04x) ", type, (unsigned)cbErr);
            continue;
        }
        const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        EGLImageKHR img = e.pCreateImage(g.dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, attrs);
        const EGLint imgErr = eglGetError();
        if (img == EGL_NO_IMAGE_KHR) {
            imp.ladder += fmt("image(type%d)=0x%04x ", type, (unsigned)imgErr);
            imp.fail = fmt("eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID) on an fd wrapped as an AHardwareBuffer "
                           "(handleType=%d) -> 0x%04x",
                           type, (unsigned)imgErr);
            continue;
        }
        gT4WrappedAhb = ahb;
        imp.image = img;
        imp.rung = 100 + type;  // off the two extension rungs, so the row cannot confuse them
        imp.accepted = fmt("wrap-handleType%d", type);
        imp.ladder += fmt("accepted(type%d) ", type);
        return imp;
    }
    if (imp.fail.empty()) imp.fail = "no handleType produced an EGLImage";
    return imp;
}

// The memory order of the 32-bit layouts the peer can plausibly hand over.  A DRM fourcc is
// a name, not a layout rule -- 'AR24' is [31:0] A:R:G:B, so a little-endian buffer holds
// B,G,R,A -- and a derived-then-wrong order would silently invert the peer's byte compare,
// so the layouts are written out instead of computed.
struct T4MemOrder {
    uint32_t fourcc;
    const char* mem;  // channels in memory order, byte 0 first
};

static const T4MemOrder kT4MemOrders[] = {
    {0x34325241u, "BGRA"},  // 'AR24' DRM_FORMAT_ARGB8888
    {0x34324241u, "RGBA"},  // 'AB24' DRM_FORMAT_ABGR8888
    {0x34324152u, "ABGR"},  // 'RA24' DRM_FORMAT_RGBA8888
    {0x34324142u, "ARGB"},  // 'BA24' DRM_FORMAT_BGRA8888
    {0x34325258u, "BGRX"},  // 'XR24' DRM_FORMAT_XRGB8888
    {0x34324258u, "RGBX"},  // 'XB24' DRM_FORMAT_XBGR8888
    {0x34325852u, "XBGR"},  // 'RX24' DRM_FORMAT_RGBX8888
    {0x34325842u, "XRGB"},  // 'BX24' DRM_FORMAT_BGRX8888
};

struct T4ColorPlan {
    bool pinned = false;
    GLfloat clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    std::string note;
};

static T4ColorPlan t4PlanColor(uint32_t fourcc, const uint8_t want[4]) {
    T4ColorPlan plan;
    const char* mem = nullptr;
    for (const T4MemOrder& m : kT4MemOrders) {
        if (m.fourcc == fourcc) mem = m.mem;
    }
    if (!mem) {
        // GL channel order is all this side can honestly claim here, and the row says the
        // compare the peer is about to make was not pinned by the format.
        for (int i = 0; i < 4; ++i) plan.clear[i] = want[i] / 255.0f;
        plan.note = fmt("fourcc=0x%08x is not one of the 32-bit layouts whose memory order this side knows, so "
                        "the clear goes out in GL channel order and the peer's byte compare is not pinned by "
                        "the format",
                        fourcc);
        return plan;
    }
    plan.pinned = true;
    for (int i = 0; i < 4; ++i) {
        switch (mem[i]) {
            case 'R': plan.clear[0] = want[i] / 255.0f; break;
            case 'G': plan.clear[1] = want[i] / 255.0f; break;
            case 'B': plan.clear[2] = want[i] / 255.0f; break;
            case 'A': plan.clear[3] = want[i] / 255.0f; break;
            default: break;  // 'X' is written as 1.0, which want[3] = 0xFF matches
        }
    }
    plan.note = fmt("fourcc=0x%08x holds %s in memory", fourcc, mem);
    return plan;
}

// GL hands back channels while the peer's seed is memory bytes; when the fourcc's memory
// order is known the seed can be re-read in channel order, and the row reports that
// comparison next to the verbatim one so a byte-order convention can never pass for a
// broken import.  Returns false, leaving `out` alone, when the order is not known.
static bool t4SeedAsChannels(uint32_t fourcc, const uint8_t seed[4], uint8_t out[4]) {
    const char* mem = nullptr;
    for (const T4MemOrder& m : kT4MemOrders) {
        if (m.fourcc == fourcc) mem = m.mem;
    }
    if (!mem) return false;
    const char* channels = "RGBA";
    for (int ch = 0; ch < 4; ++ch) {
        out[ch] = 0xFF;  // an 'X' byte carries nothing; the driver writes 1.0 there
        for (int i = 0; i < 4; ++i) {
            if (mem[i] == channels[ch]) out[ch] = seed[i];
        }
    }
    return true;
}

// The 1x1 sampling program the texture read goes through.  The leg's own FBO holds the
// image, so the draw has to land somewhere else, and the probe's surface is a 1x1 pbuffer --
// exactly the target this needs.
static const char* kT4ReadVs =
    "#version 300 es\n"
    "void main() {\n"
    "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

// highp on the texel centre: (w - 0.5) / w is not representable in mediump for a real image,
// and a rounded coordinate would sample the neighbouring texel -- exactly the mistake the
// corner read exists to catch.
static const char* kT4ReadFs =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp sampler2D;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec2 uTexel;\n"
    "out vec4 oColor;\n"
    "void main() { oColor = texture(uTex, uTexel); }\n";
static GLuint t4BuildReadProgram(std::string* fail) {
    auto compile = [&](GLenum kind, const char* src) -> GLuint {
        GLuint sh = glCreateShader(kind);
        glShaderSource(sh, 1, &src, nullptr);
        glCompileShader(sh);
        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512] = {0};
            glGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
            *fail = std::string("shader compile failed: ") + log;
            glDeleteShader(sh);
            return 0;
        }
        return sh;
    };
    GLuint vs = compile(GL_VERTEX_SHADER, kT4ReadVs);
    if (!vs) return 0;
    GLuint fs = compile(GL_FRAGMENT_SHADER, kT4ReadFs);
    if (!fs) {
        glDeleteShader(vs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
        *fail = std::string("link failed: ") + log;
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static bool t4ReadTexel(GLuint prog, GLint locTex, GLint locTexel, GLuint tex, uint32_t w, uint32_t h, uint32_t x,
                        uint32_t y, uint8_t out[4], std::string* fail) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 1, 1);
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(locTex, 0);
    glUniform2f(locTexel, ((float)x + 0.5f) / (float)w, ((float)y + 0.5f) / (float)h);
    glDrain();
    glDrawArrays(GL_TRIANGLES, 0, 3);
    const GLenum drawErr = glDrain();
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
    const GLenum readErr = glDrain();
    if (drawErr != GL_NO_ERROR || readErr != GL_NO_ERROR) {
        *fail = fmt("1x1 sampling draw -> %s, glReadPixels -> %s", glErrStr(drawErr).c_str(),
                    glErrStr(readErr).c_str());
        return false;
    }
    return true;
}

static bool t4ReadRb(GLuint fbo, uint32_t x, uint32_t y, uint8_t out[4], std::string* fail) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glDrain();
    glReadPixels((GLint)x, (GLint)y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
    const GLenum e = glDrain();
    if (e != GL_NO_ERROR) {
        *fail = "glReadPixels off the image FBO -> " + glErrStr(e);
        return false;
    }
    return true;
}

// Clears one path to `color`, then glFinish *and* an explicit fence whose wait result is what
// comes back: the peer is a different process on a different driver stack, so "the drawing
// landed" needs evidence this side can show, and glFinish alone only says the commands were
// accepted.
static std::string t4WritePath(GLuint fbo, uint32_t w, uint32_t h, const GLfloat color[4]) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, (GLsizei)w, (GLsizei)h);
    glClearColor(color[0], color[1], color[2], color[3]);
    glDrain();
    glClear(GL_COLOR_BUFFER_BIT);
    const GLenum clearErr = glDrain();
    glFinish();
    const GLenum finishErr = glDrain();
    const GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    const GLenum fenceErr = glDrain();
    std::string wait = "no sync object";
    if (sync) {
        // bounded: an unsignalled fence has to read as GL_TIMEOUT_EXPIRED, never as a hang
        wait = t4WaitStr(glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull));
        glDeleteSync(sync);
        glDrain();
    }
    return fmt("clear=%s finish=%s fence=%s wait=%s", glErrStr(clearErr).c_str(), glErrStr(finishErr).c_str(),
               glErrStr(fenceErr).c_str(), wait.c_str());
}

// Per-path state: the import attempt, the two reads, and the peer's answer to the clear.
struct T4PathRun {
    const char* name = "?";
    bool targetOk = false;  // glEGLImageTarget*OES took the image
    std::string targetFail;
    bool fboOk = false;  // and the image is a complete COLOR_ATTACHMENT0
    std::string fboFail;
    bool readDone = false;
    uint8_t word[4] = {0, 0, 0, 0};
    uint8_t corner[4] = {0, 0, 0, 0};
    std::string readFail;
    bool wrote = false;
    bool ackGot = false;
    int32_t ackOk = 0;
    uint8_t observed[4] = {0, 0, 0, 0};
    std::string ackNote;
    std::string writeFail;
};

struct T4GL {
    GLuint tex = 0;
    GLuint rb = 0;
    GLuint fboTex = 0;
    GLuint fboRb = 0;
    GLuint prog = 0;
    GLint locTex = -1;
    GLint locTexel = -1;
};

// Waits for `want`, stepping over -- and naming -- any other frame.  The frozen protocol
// gives MSG_T4_RESULT no payload contract, so a frame carrying it is skipped rather than
// parsed, which keeps the stream aligned for the ack that follows.  Skipping assumes the
// unexpected frame is no larger than the one being awaited, which is the only size the
// protocol defines.
static bool t4RecvTag(int sock, uint32_t want, void* payload, size_t maxLen, size_t* gotOut, int* fdOut,
                      std::string* skipped, std::string* fail) {
    for (int i = 0; i < 8; ++i) {
        uint32_t tag = 0;
        size_t got = 0;
        int fd = -1;
        if (!recvMsg(sock, &tag, payload, maxLen, &got, &fd)) {
            *fail = fmt("recvMsg -> errno=%d(%s)", errno, strerror(errno));
            if (fd >= 0) close(fd);
            return false;
        }
        if (tag == want) {
            if (gotOut) {
                *gotOut = got;
            }
            if (fdOut) {
                *fdOut = fd;
            } else if (fd >= 0) {
                close(fd);
            }
            return true;
        }
        if (fd >= 0) close(fd);
        if (skipped) {
            if (!skipped->empty()) *skipped += ",";
            *skipped += fmt("tag%u(len=%zu)", tag, got);
        }
    }
    *fail = "too many unexpected frames before the expected one";
    return false;
}

// The Android-minted row, defined at the end of this section: runT4 records the
// container-minted direction first and this one second, and the second is the row
// the first cannot stand in for.
static void runT4AhbRow(GlCtx& g, const T4Egl& e, int sock, const T4Layout& lay);

static void runT4(GlCtx& g, bool glOk, const char* endpoint) {
    // Capability gates come before the listen: with no context, or without the extensions
    // and entry points the import is spelled in, there is nothing this side could import,
    // and saying so here beats leaving the peer to discover it through its own timeout.
    if (!glOk) {
        record(kT4Row, "SKIP", "no headless GLES context: nothing on this side could import the peer's dma-buf");
        return;
    }
    T4Egl e;
    e.pCreateImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    e.pDestroyImage = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    e.pFormats = (PFNEGLQUERYDMABUFFORMATSEXTPROC)eglGetProcAddress("eglQueryDmaBufFormatsEXT");
    e.pModifiers = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    e.pTargetTexture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    e.pTargetRb =
        (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");

    if (!g.hasEgl("EGL_KHR_image_base") || !e.pCreateImage || !e.pDestroyImage) {
        record(kT4Row, "UNSUPPORTED",
               fmt("EGL_KHR_image_base=%d and of that extension eglCreateImageKHR=%d eglDestroyImageKHR=%d",
                   (int)g.hasEgl("EGL_KHR_image_base"), (int)(e.pCreateImage != nullptr),
                   (int)(e.pDestroyImage != nullptr)));
        return;
    }
    if (!g.hasGl("GL_OES_EGL_image") || (!e.pTargetTexture && !e.pTargetRb)) {
        record(kT4Row, "UNSUPPORTED",
               fmt("GL_OES_EGL_image=%d and of that extension glEGLImageTargetTexture2DOES=%d "
                   "glEGLImageTargetRenderbufferStorageOES=%d",
                   (int)g.hasGl("GL_OES_EGL_image"), (int)(e.pTargetTexture != nullptr),
                   (int)(e.pTargetRb != nullptr)));
        return;
    }

    int ls = rtListen(endpoint);
    if (ls < 0) {
        record(kT4Row, "FAIL", fmt("could not listen on %s for the container peer: errno=%d(%s)", endpoint, errno,
                                   strerror(errno)));
        return;
    }
    setRecvTimeout(ls, 60);
    pr("T4: listening on %s for the Droidspaces peer (abstract socket; net_mode=host keeps both ends in one "
       "namespace)",
       endpoint);
    int sock = accept(ls, nullptr, nullptr);
    close(ls);
    if (sock < 0) {
        record(kT4Row, "FAIL", fmt("no peer dialed %s within the accept timeout: errno=%d(%s)", endpoint, errno,
                                   strerror(errno)));
        return;
    }
    setRecvTimeout(sock, 60);

    // ---- 1. the peer's offer -------------------------------------------------
    T4Layout lay{};
    uint32_t tag = 0;
    size_t got = 0;
    int fd = -1;
    if (!recvMsg(sock, &tag, &lay, sizeof(lay), &got, &fd) || tag != MSG_T4_OFFER || got != sizeof(lay)) {
        record(kT4Row, "FAIL", fmt("no T4 offer: tag=%u len=%zu errno=%d(%s)", tag, got, errno, strerror(errno)));
        if (fd >= 0) close(fd);
        close(sock);
        return;
    }
    lay.peer[sizeof(lay.peer) - 1] = 0;
    lay.note[sizeof(lay.note) - 1] = 0;
    const std::string fdDesc = describeFd(fd);
    const std::string modifierStr =
        lay.modifier == UINT64_MAX ? std::string("none") : fmt("0x%016llx", (unsigned long long)lay.modifier);
    pr("T4 peer [%s] %s", lay.peer, lay.note);
    pr("T4 offer: v%u %ux%u fourcc=0x%08x stride=%u offset=%u modifier=%s seed=%s fd[%s]", lay.version, lay.width,
       lay.height, lay.fourcc, lay.stride, lay.offset, modifierStr.c_str(), t4Hex4(lay.seed).c_str(),
       fdDesc.c_str());

    if (lay.magic != kT4Magic) {
        record(kT4Row, "FAIL", fmt("offer magic=0x%08x, expected 0x%08x: the two sides do not speak the same "
                                   "protocol | peer [%s] %s",
                                   lay.magic, kT4Magic, lay.peer, lay.note));
        if (fd >= 0) close(fd);
        close(sock);
        return;
    }
    if (fd < 0) {
        record(kT4Row, "FAIL", fmt("the offer carried no fd over SCM_RIGHTS, so the peer could not create the GBM "
                                   "buffer | %ux%u fourcc=0x%08x | peer [%s] %s",
                                   lay.width, lay.height, lay.fourcc, lay.peer, lay.note));
        close(sock);
        return;
    }
    if (lay.width == 0 || lay.height == 0 || lay.stride == 0) {
        record(kT4Row, "FAIL", fmt("degenerate layout %ux%u stride=%u: nothing can be imported from it | fd[%s]",
                                   lay.width, lay.height, lay.stride, fdDesc.c_str()));
        close(fd);
        close(sock);
        return;
    }

    // ---- the capability gate: EGL_EXT_image_dma_buf_import -------------------
    // Every rung of the EGL_LINUX_DMA_BUF_EXT ladder is defined by that extension,
    // so a driver that does not advertise it does not take a container's dma-buf at
    // all.  Walking the ladder anyway would report a capability answer as an import
    // failure -- the two things this row exists to keep apart -- so the row is
    // recorded here, naming the extension, and the ladder is not entered.  The peer
    // stays connected: the Android-minted row below is the half of T4 that can still
    // be measured on such a driver, and it needs this same connection.
    // ---- 2/3. into an EGLImage: the driver's own ladder, or the wrap that replaces it
    // A driver without EGL_EXT_image_dma_buf_import cannot be asked the ladder question at
    // all, so the row goes to the wrap route instead of reporting a capability answer it
    // has not finished establishing.  Only when BOTH routes fail is the offer recorded as
    // one this driver cannot take up.
    T4Import imp;
    // The capability probe only means anything on the ladder route, and the row prints it
    // either way: a default-constructed T4Cap says "never asked" (asked=0), which is the
    // truth when the extension is not there to ask about.
    T4Cap cap;
    if (!g.hasEgl("EGL_EXT_image_dma_buf_import")) {
        pr("T4: EGL_EXT_image_dma_buf_import is not advertised: the EGL_LINUX_DMA_BUF_EXT ladder is not walked");
        imp = t4ImportViaWrap(g, e, fd, lay);
        pr("T4 wrap: accepted=%s | ladder: %s| %s", imp.accepted.c_str(), imp.ladder.c_str(),
           imp.fail.empty() ? "-" : imp.fail.c_str());
        if (imp.image == EGL_NO_IMAGE_KHR) {
            record(kT4Row, "UNSUPPORTED",
                   fmt("neither route to an EGLImage exists here: EGL_EXT_image_dma_buf_import is not advertised "
                       "(EGL_KHR_image_base=%d, GL_OES_EGL_image=%d) so EGL_LINUX_DMA_BUF_EXT has no spelling, and "
                       "wrapping the fd as an AHardwareBuffer did not produce one either (%s): the container's %ux%u "
                       "fourcc=0x%08x stride=%u offset=%u modifier=%s fd[%s] and peer [%s] %s are recorded as an "
                       "offer this driver cannot take up",
                       (int)g.hasEgl("EGL_KHR_image_base"), (int)g.hasGl("GL_OES_EGL_image"),
                       imp.fail.empty() ? imp.ladder.c_str() : imp.fail.c_str(), lay.width, lay.height, lay.fourcc,
                       lay.stride, lay.offset, modifierStr.c_str(), fdDesc.c_str(), lay.peer, lay.note));
            runT4AhbRow(g, e, sock, lay);
            close(fd);
            close(sock);
            return;
        }
    } else {
        // ---- the driver's own question, before any import ---------------------
        cap = t4Capability(g.dpy, e, lay.fourcc, lay.modifier);
        pr("T4 capability: asked=%d formatListed=%d pairListed=%d formats=%d modifiers=%d | %s", (int)cap.asked,
           (int)cap.formatListed, (int)cap.listed, cap.formats, cap.modifiers, cap.note.c_str());
        imp = t4ImportImage(g.dpy, e, fd, lay);
        pr("T4 import: accepted=%s | ladder: %s| %s", imp.accepted.c_str(), imp.ladder.c_str(),
           imp.fail.empty() ? "-" : imp.fail.c_str());
    }

    // ---- 4. read both paths, at two pixels each ------------------------------
    T4GL gl;
    T4PathRun tex, rb;
    tex.name = "tex";
    rb.name = "rb";
    uint8_t seedCh[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    const bool seedChKnown = t4SeedAsChannels(lay.fourcc, lay.seed, seedCh);

    if (imp.image != EGL_NO_IMAGE_KHR) {
        if (e.pTargetTexture) {
            glGenFramebuffers(1, &gl.fboTex);
            glGenTextures(1, &gl.tex);
            glBindTexture(GL_TEXTURE_2D, gl.tex);
            // NEAREST and no mip chain: the read has to land on the texel asked for and not
            // on an average of its neighbours
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glDrain();
            e.pTargetTexture(GL_TEXTURE_2D, (GLeglImageOES)imp.image);
            const GLenum texErr = glDrain();
            if (texErr != GL_NO_ERROR) {
                tex.targetFail = "glEGLImageTargetTexture2DOES -> " + glErrStr(texErr);
            } else {
                tex.targetOk = true;
                glBindFramebuffer(GL_FRAMEBUFFER, gl.fboTex);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl.tex, 0);
                const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                const GLenum attachErr = glDrain();
                if (st != GL_FRAMEBUFFER_COMPLETE || attachErr != GL_NO_ERROR) {
                    tex.fboFail = fmt("the image texture attached but the FBO is 0x%04x (%s)", (unsigned)st,
                                      glErrStr(attachErr).c_str());
                } else {
                    tex.fboOk = true;
                }
            }
        } else {
            tex.targetFail = "glEGLImageTargetTexture2DOES is absent, so the texture path was not exercised";
        }

        if (e.pTargetRb) {
            glGenFramebuffers(1, &gl.fboRb);
            glGenRenderbuffers(1, &gl.rb);
            glBindRenderbuffer(GL_RENDERBUFFER, gl.rb);
            glDrain();
            e.pTargetRb(GL_RENDERBUFFER, (GLeglImageOES)imp.image);
            const GLenum rbErr = glDrain();
            if (rbErr != GL_NO_ERROR) {
                rb.targetFail = "glEGLImageTargetRenderbufferStorageOES -> " + glErrStr(rbErr);
            } else {
                rb.targetOk = true;
                glBindFramebuffer(GL_FRAMEBUFFER, gl.fboRb);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, gl.rb);
                const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                const GLenum attachErr = glDrain();
                if (st != GL_FRAMEBUFFER_COMPLETE || attachErr != GL_NO_ERROR) {
                    rb.fboFail = fmt("the image renderbuffer attached but the FBO is 0x%04x (%s)", (unsigned)st,
                                     glErrStr(attachErr).c_str());
                } else {
                    rb.fboOk = true;
                }
            }
        } else {
            rb.targetFail =
                "glEGLImageTargetRenderbufferStorageOES is absent, so the renderbuffer path was not exercised";
        }
    } else {
        tex.targetFail = fmt("no EGLImage to attach: %s (ladder: %s)",
                             imp.fail.empty() ? "the import ladder was not entered" : imp.fail.c_str(),
                             imp.ladder.c_str());
        rb.targetFail = tex.targetFail;
    }

    const uint32_t farX = lay.width - 1, farY = lay.height - 1;
    if (tex.targetOk) {
        std::string progFail;
        gl.prog = t4BuildReadProgram(&progFail);
        if (!gl.prog) {
            tex.readFail = progFail;
        } else {
            gl.locTex = glGetUniformLocation(gl.prog, "uTex");
            gl.locTexel = glGetUniformLocation(gl.prog, "uTexel");
            std::string failA, failB;
            const bool okA = t4ReadTexel(gl.prog, gl.locTex, gl.locTexel, gl.tex, lay.width, lay.height, 0, 0,
                                         tex.word, &failA);
            const bool okB = t4ReadTexel(gl.prog, gl.locTex, gl.locTexel, gl.tex, lay.width, lay.height, farX, farY,
                                         tex.corner, &failB);
            tex.readDone = okA && okB;
            if (!okA) tex.readFail = "pixel(0,0): " + failA;
            if (!okB) tex.readFail += (tex.readFail.empty() ? "" : "; ") + std::string("pixel(w-1,h-1): ") + failB;
        }
    } else {
        tex.readFail = "not attempted: " + tex.targetFail;
    }

    if (rb.targetOk && rb.fboOk) {
        std::string failA, failB;
        const bool okA = t4ReadRb(gl.fboRb, 0, 0, rb.word, &failA);
        const bool okB = t4ReadRb(gl.fboRb, farX, farY, rb.corner, &failB);
        rb.readDone = okA && okB;
        if (!okA) rb.readFail = "pixel(0,0): " + failA;
        if (!okB) rb.readFail += (rb.readFail.empty() ? "" : "; ") + std::string("pixel(w-1,h-1): ") + failB;
    } else if (rb.targetOk) {
        rb.readFail = "not attempted: " + rb.fboFail;
    } else {
        rb.readFail = "not attempted: " + rb.targetFail;
    }

    // The protocol's read comparison is verbatim and the verdict keeps it that way.  GL hands
    // back channels while the peer's seed is memory bytes, so when the fourcc's memory order
    // is known the same comparison is also made against the seed re-read in channel order:
    // the two disagreeing is a fact about byte order, not about the import, and the row shows
    // both rather than letting the verbatim test alone imply a broken driver.
    const bool texReadOk = tex.targetOk && tex.readDone && !memcmp(tex.word, lay.seed, 4) &&
                           !memcmp(tex.corner, lay.seed, 4);
    const bool rbReadOk = rb.targetOk && rb.readDone && !memcmp(rb.word, lay.seed, 4) &&
                          !memcmp(rb.corner, lay.seed, 4);
    const bool texOrderOk = seedChKnown && !memcmp(tex.word, seedCh, 4) && !memcmp(tex.corner, seedCh, 4);
    const bool rbOrderOk = seedChKnown && !memcmp(rb.word, seedCh, 4) && !memcmp(rb.corner, seedCh, 4);

    // ---- 5. what the two reads produced, back to the peer --------------------
    T4Read rep{};
    rep.texOk = texReadOk ? 1 : 0;
    rep.rbOk = rbReadOk ? 1 : 0;
    rep.rung = imp.rung;
    memcpy(rep.texWord, tex.word, 4);
    memcpy(rep.texCorner, tex.corner, 4);
    memcpy(rep.rbWord, rb.word, 4);
    memcpy(rep.rbCorner, rb.corner, 4);
    snprintf(rep.importNote, sizeof(rep.importNote), "%s | ladder: %s| %s", imp.accepted.c_str(), imp.ladder.c_str(),
             cap.note.c_str());
    const bool readSent = sendMsg(sock, MSG_T4_READ, &rep, sizeof(rep), -1);
    if (!readSent) pr("T4: MSG_T4_READ did not go out: errno=%d(%s)", errno, strerror(errno));

    // ---- 6/7. draw into each path, ack by ack --------------------------------
    // Both palettes end in 0xFF so an ignored 'X' channel and a real 'A' agree, and the two
    // clears differ in every byte so a swapped path cannot pass unnoticed.
    const uint8_t rbWant[4] = {0x11, 0x22, 0x33, 0xFF};
    const uint8_t texWant[4] = {0x44, 0x55, 0x66, 0xFF};
    const T4ColorPlan rbPlan = t4PlanColor(lay.fourcc, rbWant);
    const T4ColorPlan texPlan = t4PlanColor(lay.fourcc, texWant);
    std::string rbFence = "not attempted", texFence = "not attempted";
    std::string skipped;

    if (rb.fboOk) {
        rbFence = t4WritePath(gl.fboRb, lay.width, lay.height, rbPlan.clear);
        T4Wrote w{};
        w.path = 0;
        memcpy(w.color, rbWant, 4);
        if (!sendMsg(sock, MSG_T4_WROTE, &w, sizeof(w), -1)) {
            rb.writeFail = fmt("sendMsg(MSG_T4_WROTE path=0) errno=%d(%s)", errno, strerror(errno));
        } else {
            rb.wrote = true;
            T4Ack ack{};
            std::string fail;
            if (!t4RecvTag(sock, MSG_T4_ACK, &ack, sizeof(ack), nullptr, nullptr, &skipped, &fail)) {
                rb.writeFail = "no ack for the renderbuffer write: " + fail;
            } else {
                ack.note[sizeof(ack.note) - 1] = 0;
                rb.ackGot = true;
                rb.ackOk = ack.ok;
                memcpy(rb.observed, ack.observed, 4);
                rb.ackNote = ack.note;
                if (!ack.ok) {
                    rb.writeFail = fmt("peer ack ok=0 observed=%s want=%s note=[%s]", t4Hex4(ack.observed).c_str(),
                                       t4Hex4(rbWant).c_str(), ack.note);
                }
            }
        }
    } else {
        const std::string whyNot = rb.targetOk ? rb.fboFail : rb.targetFail;
        rb.writeFail = "no clear was attempted: " + whyNot;
    }

    if (tex.fboOk) {
        texFence = t4WritePath(gl.fboTex, lay.width, lay.height, texPlan.clear);
        T4Wrote w{};
        w.path = 1;
        memcpy(w.color, texWant, 4);
        if (!sendMsg(sock, MSG_T4_WROTE, &w, sizeof(w), -1)) {
            tex.writeFail = fmt("sendMsg(MSG_T4_WROTE path=1) errno=%d(%s)", errno, strerror(errno));
        } else {
            tex.wrote = true;
            T4Ack ack{};
            std::string fail;
            if (!t4RecvTag(sock, MSG_T4_ACK, &ack, sizeof(ack), nullptr, nullptr, &skipped, &fail)) {
                tex.writeFail = "no ack for the texture write: " + fail;
            } else {
                ack.note[sizeof(ack.note) - 1] = 0;
                tex.ackGot = true;
                tex.ackOk = ack.ok;
                memcpy(tex.observed, ack.observed, 4);
                tex.ackNote = ack.note;
                if (!ack.ok) {
                    tex.writeFail = fmt("peer ack ok=0 observed=%s want=%s note=[%s]", t4Hex4(ack.observed).c_str(),
                                        t4Hex4(texWant).c_str(), ack.note);
                }
            }
        }
    } else {
        const std::string whyNot = tex.targetOk ? tex.fboFail : tex.targetFail;
        tex.writeFail = "no clear was attempted: " + whyNot;
    }
    pr("T4 clear: renderbuffer %s ack=%d | texture %s ack=%d", rbFence.c_str(), rb.ackGot ? rb.ackOk : -1,
       texFence.c_str(), tex.ackGot ? tex.ackOk : -1);

    // ---- 8. the peer's last word ---------------------------------------------
    // MSG_T4_RESULT has no payload contract in the frozen protocol, so it is taken raw: the
    // length and the leading bytes go into the row and nothing is parsed out of it.
    uint8_t lastWord[256];
    memset(lastWord, 0, sizeof(lastWord));
    std::string resultNote;
    {
        setRecvTimeout(sock, 2);  // bounded: a peer that says nothing must not hold the row up
        uint32_t rtag = 0;
        size_t rgot = 0;
        if (recvMsg(sock, &rtag, lastWord, sizeof(lastWord), &rgot, nullptr)) {
            if (rtag == MSG_T4_RESULT) {
                std::string hex;
                for (size_t i = 0; i < rgot && i < 16; ++i) hex += fmt("%02x", lastWord[i]);
                resultNote = fmt("MSG_T4_RESULT len=%zu bytes=%s", rgot, hex.c_str());
            } else {
                resultNote = fmt("the peer's last frame was tag=%u len=%zu, not MSG_T4_RESULT", rtag, rgot);
            }
        } else {
            resultNote = fmt("no MSG_T4_RESULT frame (the peer closed, timed out, or sent more than %zu bytes): "
                             "errno=%d(%s)",
                             sizeof(lastWord), errno, strerror(errno));
        }
    }

    // ---- the verdict ---------------------------------------------------------
    std::vector<Leg> legs;
    {
        Leg l;
        l.name = tex.name;
        l.decisive = true;
        l.attempted = tex.targetOk;
        l.readOk = texReadOk;
        l.writeOk = tex.ackGot && tex.ackOk != 0;
        if (!tex.targetOk)
            l.fail = tex.targetFail;
        else if (!tex.readDone)
            l.fail = "texture read: " + tex.readFail;
        else if (!l.readOk)
            l.fail = fmt("texture read is not the peer's seed verbatim: word=%s corner=%s seed(memory)=%s "
                         "seed(channels)=%s channelMatch=%d -- GL hands back channels while the peer fills "
                         "memory bytes, so a fourcc whose memory order is not RGBA differs here on a working "
                         "import",
                         t4Hex4(tex.word).c_str(), t4Hex4(tex.corner).c_str(), t4Hex4(lay.seed).c_str(),
                         seedChKnown ? t4Hex4(seedCh).c_str() : "fourcc order unknown", (int)texOrderOk);
        else if (!l.writeOk)
            l.fail = tex.writeFail;
        legs.push_back(l);
    }
    {
        Leg l;
        l.name = rb.name;
        l.decisive = true;
        l.attempted = rb.targetOk;
        l.readOk = rbReadOk;
        l.writeOk = rb.ackGot && rb.ackOk != 0;
        if (!rb.targetOk)
            l.fail = rb.targetFail;
        else if (!rb.readDone)
            l.fail = "renderbuffer read: " + rb.readFail;
        else if (!l.readOk)
            l.fail = fmt("renderbuffer read is not the peer's seed verbatim: word=%s corner=%s seed(memory)=%s "
                         "seed(channels)=%s channelMatch=%d -- GL hands back channels while the peer fills "
                         "memory bytes, so a fourcc whose memory order is not RGBA differs here on a working "
                         "import",
                         t4Hex4(rb.word).c_str(), t4Hex4(rb.corner).c_str(), t4Hex4(lay.seed).c_str(),
                         seedChKnown ? t4Hex4(seedCh).c_str() : "fourcc order unknown", (int)rbOrderOk);
        else if (!l.writeOk)
            l.fail = rb.writeFail;
        legs.push_back(l);
    }
    std::string why;
    const char* status = legVerdict(legs, &why);

    // every failure string already names its step, so an empty one leaves no marker
    auto join = [](const std::string& a, const std::string& b) {
        if (a.empty()) return b;
        if (b.empty()) return a;
        return a + "; " + b;
    };

    std::string detail = fmt(
        "%s | %ux%u fourcc=0x%08x stride=%u offset=%u modifier=%s seed=%s | capability[asked=%d format=%d pair=%d "
        "formats=%d modifiers=%d: %s] | import[accepted=%s ladder: %s| %s] | fd[%s]",
        legTrace(legs).c_str(), lay.width, lay.height, lay.fourcc, lay.stride, lay.offset, modifierStr.c_str(),
        t4Hex4(lay.seed).c_str(), (int)cap.asked, (int)cap.formatListed, (int)cap.listed, cap.formats, cap.modifiers,
        cap.note.c_str(), imp.accepted.c_str(), imp.ladder.c_str(), imp.fail.empty() ? "-" : imp.fail.c_str(),
        fdDesc.c_str());
    detail += fmt(" | tex[target=%d fbo=%d word=%s corner=%s read=%d orderMapped=%d(%s) fail=[%s]]",
                  (int)tex.targetOk, (int)tex.fboOk, t4Hex4(tex.word).c_str(), t4Hex4(tex.corner).c_str(),
                  (int)tex.readDone, (int)texOrderOk, seedChKnown ? t4Hex4(seedCh).c_str() : "fourcc order unknown",
                  join(tex.targetFail, join(tex.readFail, tex.writeFail)).c_str());
    detail += fmt(" rb[target=%d fbo=%d word=%s corner=%s read=%d orderMapped=%d(%s) fail=[%s]]",
                  (int)rb.targetOk, (int)rb.fboOk, t4Hex4(rb.word).c_str(), t4Hex4(rb.corner).c_str(),
                  (int)rb.readDone, (int)rbOrderOk, seedChKnown ? t4Hex4(seedCh).c_str() : "fourcc order unknown",
                  join(rb.targetFail, join(rb.readFail, rb.writeFail)).c_str());
    detail += fmt(" | clear[rb fence=%s want=%s ack=%d observed=%s note=[%s]] [tex fence=%s want=%s ack=%d "
                  "observed=%s note=[%s]] skipped=[%s]",
                  rbFence.c_str(), t4Hex4(rbWant).c_str(), rb.ackGot ? rb.ackOk : -1, t4Hex4(rb.observed).c_str(),
                  rb.ackNote.c_str(), texFence.c_str(), t4Hex4(texWant).c_str(), tex.ackGot ? tex.ackOk : -1,
                  t4Hex4(tex.observed).c_str(), tex.ackNote.c_str(), skipped.c_str());
    detail += fmt(" | colourOrder[rb pinned=%d %s] [tex pinned=%d %s] | readSent=%d | %s | peer [%s] %s | %s",
                  (int)rbPlan.pinned, rbPlan.note.c_str(), (int)texPlan.pinned, texPlan.note.c_str(),
                  (int)readSent, resultNote.c_str(), lay.peer, lay.note, why.c_str());
    record(kT4Row, status, detail);

    // The other half of T4, over the connection that is still open: this side mints
    // the image and the container imports it.  See runT4AhbRow for what it measures.
    runT4AhbRow(g, e, sock, lay);

    // The EGLImage outlives every GL object that wraps it, and the fd is the peer's only
    // handle on the buffer: tear down in that order.
    if (gl.prog) glDeleteProgram(gl.prog);
    if (gl.fboTex) glDeleteFramebuffers(1, &gl.fboTex);
    if (gl.fboRb) glDeleteFramebuffers(1, &gl.fboRb);
    if (gl.tex) glDeleteTextures(1, &gl.tex);
    if (gl.rb) glDeleteRenderbuffers(1, &gl.rb);
    if (imp.image != EGL_NO_IMAGE_KHR) e.pDestroyImage(g.dpy, imp.image);
    close(fd);
    close(sock);
}

// ---------------------------------------------------------------------------
// T4-ahb-image: the same image, minted on the Android side
//
// T4-image-import above asks whether an image allocated in the container can be
// imported HERE.  This row asks the other half of the same question, and it is the
// half that survives on a driver with no EGL_EXT_image_dma_buf_import.  An app
// cannot adopt the container's fd as an AHardwareBuffer (createFromHandle is a
// SystemApi, not NDK), so the image is born on this side and travels the other way:
// the buffer is allocated in this process, handed over as an fd over a socketpair --
// the mechanism T0 hands its BLOB across -- and imported in the container as a GBM bo.
//
// Both directions are measured, and neither is measured by this side alone:
//   read   the peer must find, at (0,0) AND at the far corner of its own mapping,
//          the four bytes this side cleared into the image.
//   write  the peer fills its mapping with a pattern of its own and this side reads
//          it back through the GPU -- glEGLImageTargetTexture2DOES plus the same 1x1
//          sampling program the row above uses, not a second CPU mapping of a buffer
//          this process already owns, which would prove nothing about the peer.
// A refusal anywhere is recorded where it happened with its EGL or GL error named,
// and can never come out as OK.
// ---------------------------------------------------------------------------

static const char* kT4AhbRow = "T4-ahb-image";

// The DRM fourcc this row offers, and why it is that one: an AHardwareBuffer does
// not carry a fourcc, so the two sides must agree on memory order in words.
// AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM is R,G,B,A in memory, and the DRM name with
// that same little-endian layout is DRM_FORMAT_ABGR8888, fourcc_code('A','B','2','4')
// = 0x34324241, whose [31:0] A:B:G:R puts R in byte 0 -- the pairing gralloc itself
// uses for HAL_PIXEL_FORMAT_RGBA_8888.  Choosing it is what makes the comparison
// below mean something: t4PlanColor() derives the clear colour from the same table
// entry the peer's bytes are compared through, so a channel mistake cannot cancel
// itself out between the two sides.
static const uint32_t kT4AhbFourcc = 0x34324241u;  // DRM_FORMAT_ABGR8888, memory order R,G,B,A

// AHardwareBuffer_Desc::stride IS IN PIXELS -- the platform's own unit -- while every
// consumer on the wire wants the row stride in BYTES: gbm_bo_import is told it, and the
// peer's mapping and both of its pixel offsets follow from it.  Announcing the pixel
// count as if it were bytes cost a device run (the peer imported a 64-byte row for a
// 256-byte-row image), so the conversion lives here, next to the fourcc that fixes the
// bytes per pixel: DRM_FORMAT_ABGR8888 is four bytes per pixel, and the AHB format this
// row allocates (AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM) is the same memory order.
static const uint32_t kT4AhbBytesPerPixel = 4u;

// The colour this side clears into the image before offering it: four bytes in that
// memory order, all four distinct, so a permuted or repeated byte shows up as a
// different sequence instead of hiding inside the colour itself.
static const uint8_t kT4AhbSeed[4] = {0x2A, 0x5B, 0x8C, 0xFF};

static void runT4AhbRow(GlCtx& g, const T4Egl& e, int sock, const T4Layout& lay) {
    // The row is sized by the leg's only size knob: the peer dialled with --width/
    // --height (64x64 by default) and the same two numbers size this allocation, so a
    // single option moves both sides and they cannot silently disagree about the
    // layout.  The stride, by contrast, is never assumed to be width*4: it is what
    // AHardwareBuffer_describe() reports in PIXELS, converted to the BYTES every consumer
    // on the wire wants, and it is what the peer is told.
    const uint32_t w = lay.width, h = lay.height;
    const uint32_t farX = w - 1, farY = h - 1;
    const T4ColorPlan plan = t4PlanColor(kT4AhbFourcc, kT4AhbSeed);

    // ---- the entry points this row is spelled in -----------------------------
    if (!g.hasEgl("EGL_ANDROID_get_native_client_buffer") || !g.pGetNativeClientBuffer) {
        record(kT4AhbRow, "UNSUPPORTED",
               fmt("EGL_ANDROID_get_native_client_buffer=%d and of that extension eglGetNativeClientBufferANDROID=%d: "
                   "an AHardwareBuffer cannot become an EGLClientBuffer here, so there is no image to offer",
                   (int)g.hasEgl("EGL_ANDROID_get_native_client_buffer"),
                   (int)(g.pGetNativeClientBuffer != nullptr)));
        return;
    }
    if (!e.pTargetRb || !e.pTargetTexture) {
        record(kT4AhbRow, "UNSUPPORTED",
               fmt("GL_OES_EGL_image=%d but glEGLImageTargetRenderbufferStorageOES=%d glEGLImageTargetTexture2DOES=%d: "
                   "this row clears through a renderbuffer and reads back through the texture path, and one of the "
                   "two is absent",
                   (int)g.hasGl("GL_OES_EGL_image"), (int)(e.pTargetRb != nullptr),
                   (int)(e.pTargetTexture != nullptr)));
        return;
    }

    // ---- 1. the buffer, allocated here, described verbatim -------------------
    AHardwareBuffer_Desc desc{};
    desc.width = w;
    desc.height = h;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                 AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    AHardwareBuffer* ahb = nullptr;
    const int allocRc = AHardwareBuffer_allocate(&desc, &ahb);
    if (allocRc != 0 || !ahb) {
        record(kT4AhbRow, "UNSUPPORTED",
               fmt("AHardwareBuffer_allocate(width=%u height=%u layers=1 format=AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM "
                   "usage=0x%llx) rc=%d errno=%d(%s): this side cannot mint the image at all, so the "
                   "Android-minted direction has nothing to measure",
                   w, h, (unsigned long long)desc.usage, allocRc, errno, strerror(errno)));
        return;
    }
    AHardwareBuffer_Desc back{};
    AHardwareBuffer_describe(ahb, &back);
    pr("T4-ahb: AHardwareBuffer_allocate rc=%d, AHardwareBuffer_describe -> width=%u height=%u layers=%u "
       "format=0x%x usage=0x%llx stride=%u rfu0=%u rfu1=%llu",
       allocRc, back.width, back.height, back.layers, (unsigned)back.format, (unsigned long long)back.usage,
       back.stride, back.rfu0, (unsigned long long)back.rfu1);
    if (back.width == 0 || back.height == 0 || back.stride == 0) {
        record(kT4AhbRow, "FAIL",
               fmt("AHardwareBuffer_describe returned a degenerate layout: %ux%u layers=%u stride=%u", back.width,
                   back.height, back.layers, back.stride));
        AHardwareBuffer_release(ahb);
        return;
    }

    // ---- 2. import it HERE: client buffer -> EGLImage -> renderbuffer --------
    EGLClientBuffer cb = g.pGetNativeClientBuffer(ahb);
    const EGLint cbErr = eglGetError();
    pr("T4-ahb: eglGetNativeClientBufferANDROID(ahb=%p) -> %p, eglGetError=0x%04x", (void*)ahb, (void*)cb,
       (unsigned)cbErr);
    if (!cb) {
        record(kT4AhbRow, "FAIL",
               fmt("eglGetNativeClientBufferANDROID(ahb) returned NULL with eglGetError=0x%04x, for an "
                   "AHardwareBuffer this same driver allocated one call earlier (%ux%u stride=%u)",
                   (unsigned)cbErr, back.width, back.height, back.stride));
        AHardwareBuffer_release(ahb);
        return;
    }
    const EGLint imgAttrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR img = e.pCreateImage(g.dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, imgAttrs);
    const EGLint imgErr = eglGetError();
    pr("T4-ahb: eglCreateImageKHR(EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, clientBuffer=%p) -> %p, "
       "eglGetError=0x%04x",
       (void*)cb, (void*)img, (unsigned)imgErr);
    if (img == EGL_NO_IMAGE_KHR) {
        record(kT4AhbRow, "FAIL",
               fmt("eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID) -> EGL_NO_IMAGE_KHR with eglGetError=0x%04x: the "
                   "driver took the client buffer (%p) and then refused the image",
                   (unsigned)imgErr, (void*)cb));
        AHardwareBuffer_release(ahb);
        return;
    }

    GLuint rb = 0, fboRb = 0, tex = 0, fboTex = 0, prog = 0;
    std::string stepFail;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glDrain();
    e.pTargetRb(GL_RENDERBUFFER, (GLeglImageOES)img);
    const GLenum rbErr = glDrain();
    pr("T4-ahb: glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, image) -> %s", glErrStr(rbErr).c_str());
    if (rbErr != GL_NO_ERROR) stepFail = "glEGLImageTargetRenderbufferStorageOES -> " + glErrStr(rbErr);
    glGenFramebuffers(1, &fboRb);
    glBindFramebuffer(GL_FRAMEBUFFER, fboRb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    const GLenum fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    const GLenum attachErr = glDrain();
    pr("T4-ahb: image renderbuffer FBO completeness=0x%04x (%s), glGetError=%s", (unsigned)fboStatus,
       fboStatus == GL_FRAMEBUFFER_COMPLETE ? "GL_FRAMEBUFFER_COMPLETE" : "not complete",
       glErrStr(attachErr).c_str());
    if (stepFail.empty() && (fboStatus != GL_FRAMEBUFFER_COMPLETE || attachErr != GL_NO_ERROR)) {
        stepFail = fmt("the image renderbuffer attached but the FBO is 0x%04x (%s)", (unsigned)fboStatus,
                       glErrStr(attachErr).c_str());
    }
    if (!stepFail.empty()) {
        record(kT4AhbRow, "FAIL",
               fmt("%s | AHB[%ux%u layers=%u format=0x%x usage=0x%llx stride=%u] | fourcc=0x%08x seed=%s | image=%p "
                   "egl=0x%04x clientBuffer=%p egl=0x%04x | nothing was drawn and nothing was offered",
                   stepFail.c_str(), back.width, back.height, back.layers, (unsigned)back.format,
                   (unsigned long long)back.usage, back.stride, kT4AhbFourcc, t4Hex4(kT4AhbSeed).c_str(),
                   (void*)img, (unsigned)imgErr, (void*)cb, (unsigned)cbErr));
        glDeleteFramebuffers(1, &fboRb);
        glDeleteRenderbuffers(1, &rb);
        e.pDestroyImage(g.dpy, img);
        AHardwareBuffer_release(ahb);
        return;
    }

    // The clear is what the peer's two pixels are compared against, so it goes
    // through the same clear + glFinish + fence path the row above uses, and that
    // fence's wait result is reported verbatim.
    const std::string clearFence = t4WritePath(fboRb, w, h, plan.clear);
    pr("T4-ahb: clear %s into the renderbuffer FBO: %s | %s", t4Hex4(kT4AhbSeed).c_str(), clearFence.c_str(),
       plan.note.c_str());

    // ---- 3. hand it over: socketpair + SCM_RIGHTS, exactly as T0 does -------
    T4AhbOffer off{};
    off.magic = kT4Magic;
    off.version = 1;
    off.width = back.width;
    off.height = back.height;
    off.stride = back.stride * kT4AhbBytesPerPixel;  // describe() reports pixels; the wire wants bytes
    off.fourcc = kT4AhbFourcc;
    memcpy(off.seed, kT4AhbSeed, 4);
    snprintf(off.note, sizeof(off.note), "clear=[%s] fence=[%s] modifier=DRM_FORMAT_MOD_LINEAR assumed",
             t4Hex4(kT4AhbSeed).c_str(), clearFence.c_str());
    off.note[sizeof(off.note) - 1] = 0;
    int sp[2] = {-1, -1};
    int sendRc = -1;
    std::string spDesc = "no socketpair";
    std::string sendFail;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp) != 0) {
        sendRc = -errno;
        sendFail = fmt("socketpair -> errno=%d(%s)", errno, strerror(errno));
    } else {
        spDesc = describeFd(sp[1]);
        sendRc = AHardwareBuffer_sendHandleToUnixSocket(ahb, sp[0]);
        if (sendRc != 0) {
            sendFail = fmt("AHardwareBuffer_sendHandleToUnixSocket(ahb, socketpair[0]) -> %d errno=%d(%s)", sendRc,
                           errno, strerror(errno));
        }
        close(sp[0]);
        if (sendRc == 0 && !sendMsg(sock, MSG_T4_AHB_OFFER, &off, sizeof(off), sp[1])) {
            sendRc = -errno;
            sendFail = fmt("sendMsg(MSG_T4_AHB_OFFER, fd=socketpair[1]) -> errno=%d(%s)", errno, strerror(errno));
        }
        close(sp[1]);
    }
    const bool offered = sendRc == 0;
    pr("T4-ahb: MSG_T4_AHB_OFFER %ux%u stride=%u B (%u px x %u B/px) fourcc=0x%08x seed=%s note=[%s] "
       "handle-send=%d fd[%s]",
       off.width, off.height, off.stride, back.stride, kT4AhbBytesPerPixel, off.fourcc,
       t4Hex4(off.seed).c_str(), off.note, sendRc, spDesc.c_str());

    // ---- 4. the peer's answer ------------------------------------------------
    T4AhbAck ack{};
    size_t ackGot = 0;
    std::string ackFail;
    std::string ackSkipped;
    bool ackRecv = false;
    if (offered) {
        // The dma-buf row's last step shortens the socket timeout to 2s to bound its
        // wait for a frame the peer never sends; this wait is a real exchange and
        // takes the leg's own budget back.
        setRecvTimeout(sock, 60);
        ackRecv = t4RecvTag(sock, MSG_T4_AHB_ACK, &ack, sizeof(ack), &ackGot, nullptr, &ackSkipped, &ackFail);
    } else {
        ackFail = sendFail;
    }
    if (ackRecv) {
        ack.note[sizeof(ack.note) - 1] = 0;
        pr("T4-ahb: MSG_T4_AHB_ACK imported=%d matched=0x%x stride=%u observed=%s observedCorner=%s pattern=%s "
           "note=[%s] skipped=[%s]",
           (int)ack.imported, (int)ack.matched, ack.stride, t4Hex4(ack.observed).c_str(),
           t4Hex4(ack.observedCorner).c_str(), t4Hex4(ack.pattern).c_str(), ack.note, ackSkipped.c_str());
    } else if (offered) {
        pr("T4-ahb: no MSG_T4_AHB_ACK: %s", ackFail.c_str());
    }

    // ---- 5. this side's own read: the texture path, at both pixels -----------
    // Taken AFTER the peer's ack, so what comes back is its answer and not the
    // colour this side cleared a moment ago.
    std::string readFail;
    bool readDone = false;
    uint8_t texWord[4] = {0, 0, 0, 0}, texCorner[4] = {0, 0, 0, 0};
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glDrain();
    e.pTargetTexture(GL_TEXTURE_2D, (GLeglImageOES)img);
    const GLenum texErr = glDrain();
    pr("T4-ahb: glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image) -> %s", glErrStr(texErr).c_str());
    if (texErr != GL_NO_ERROR) {
        readFail = "glEGLImageTargetTexture2DOES -> " + glErrStr(texErr);
    } else {
        glGenFramebuffers(1, &fboTex);
        glBindFramebuffer(GL_FRAMEBUFFER, fboTex);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        const GLenum texFboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        const GLenum texAttachErr = glDrain();
        if (texFboStatus != GL_FRAMEBUFFER_COMPLETE || texAttachErr != GL_NO_ERROR) {
            readFail = fmt("the image texture attached but the FBO is 0x%04x (%s)", (unsigned)texFboStatus,
                           glErrStr(texAttachErr).c_str());
        }
    }
    if (readFail.empty()) {
        std::string progFail;
        prog = t4BuildReadProgram(&progFail);
        if (!prog) {
            readFail = progFail;
        } else {
            const GLint locTex = glGetUniformLocation(prog, "uTex");
            const GLint locTexel = glGetUniformLocation(prog, "uTexel");
            std::string failA, failB;
            const bool okA = t4ReadTexel(prog, locTex, locTexel, tex, w, h, 0, 0, texWord, &failA);
            const bool okB = t4ReadTexel(prog, locTex, locTexel, tex, w, h, farX, farY, texCorner, &failB);
            readDone = okA && okB;
            if (!okA) readFail = "pixel(0,0): " + failA;
            if (!okB) readFail += (readFail.empty() ? "" : "; ") + std::string("pixel(w-1,h-1): ") + failB;
            const std::string rf = readFail.empty() ? std::string("-") : readFail;
            pr("T4-ahb: texture-path read word=%s corner=%s readDone=%d fail=[%s]", t4Hex4(texWord).c_str(),
               t4Hex4(texCorner).c_str(), (int)readDone, rf.c_str());
        }
    }

    // ---- 6. the row's verdict: one decisive leg, both halves named -----------
    Leg l;
    l.name = "ahb";
    l.decisive = true;
    l.attempted = offered;
    l.readOk = ackRecv && ack.imported && (ack.matched & 3) == 3 && !memcmp(ack.observed, kT4AhbSeed, 4) &&
               !memcmp(ack.observedCorner, kT4AhbSeed, 4);
    l.writeOk = ackRecv && readDone && !memcmp(texWord, ack.pattern, 4) && !memcmp(texCorner, ack.pattern, 4);
    if (!offered) {
        l.fail = "the offer never went out: " + sendFail;
    } else if (!ackRecv) {
        l.fail = "no MSG_T4_AHB_ACK: " + ackFail;
    } else if (!ack.imported) {
        l.fail = fmt("the container's gbm_bo_import refused the fd: peer note=[%s]", ack.note);
    } else if (!l.readOk) {
        l.fail = fmt("the peer did not see the clear colour at both pixels: matched=0x%x observed=%s corner=%s "
                     "want=%s",
                     (unsigned)ack.matched, t4Hex4(ack.observed).c_str(), t4Hex4(ack.observedCorner).c_str(),
                     t4Hex4(kT4AhbSeed).c_str());
    } else if (!readDone) {
        l.fail = "texture-path read of the same image: " + readFail;
    } else if (!l.writeOk) {
        l.fail = fmt("texture-path read is not the peer's pattern: word=%s corner=%s pattern=%s",
                     t4Hex4(texWord).c_str(), t4Hex4(texCorner).c_str(), t4Hex4(ack.pattern).c_str());
    }
    std::vector<Leg> legs{l};
    std::string why;
    const char* status = legVerdict(legs, &why);

    // Every raw value goes into the row, in the shape the other legs use: the leg
    // trace first, then what each step actually saw, then the failure.
    std::string detail = fmt(
        "%s | AHB[%ux%u layers=%u format=0x%x usage=0x%llx stride=%u rfu0=%u rfu1=%llu] | fourcc=0x%08x [%s] | "
        "seed=%s | egl[clientBuffer=%p err=0x%04x image=%p err=0x%04x] | rbFbo[status=0x%04x glErr=%s] | "
        "clear[%s] | offer[sent=%d handleSend=%d fd[%s] note=[%s]] | ack[len=%zu got=%d imported=%d matched=0x%x "
        "stride=%u observed=%s observedCorner=%s pattern=%s note=[%s] skipped=[%s]] | texRead[done=%d word=%s "
        "corner=%s fail=[%s]] | %s | peer [%s] %s",
        legTrace(legs).c_str(), back.width, back.height, back.layers, (unsigned)back.format,
        (unsigned long long)back.usage, back.stride, back.rfu0, (unsigned long long)back.rfu1, kT4AhbFourcc,
        plan.note.c_str(), t4Hex4(kT4AhbSeed).c_str(), (void*)cb, (unsigned)cbErr, (void*)img, (unsigned)imgErr,
        (unsigned)fboStatus, glErrStr(attachErr).c_str(), clearFence.c_str(), (int)offered, sendRc, spDesc.c_str(),
        off.note, ackGot, (int)ackRecv, ackRecv ? (int)ack.imported : -1, ackRecv ? (int)ack.matched : 0,
        ack.stride, t4Hex4(ack.observed).c_str(), t4Hex4(ack.observedCorner).c_str(), t4Hex4(ack.pattern).c_str(),
        ackRecv ? ack.note : ackFail.c_str(), ackSkipped.c_str(), (int)readDone, t4Hex4(texWord).c_str(),
        t4Hex4(texCorner).c_str(), readFail.empty() ? "-" : readFail.c_str(), why.c_str(), lay.peer, lay.note);
    record(kT4AhbRow, status, detail);

    // The EGLImage outlives every GL object that wraps it, and the AHardwareBuffer is
    // what the image was created from, so the teardown order is the one the row above
    // already uses.
    if (prog) glDeleteProgram(prog);
    if (fboTex) glDeleteFramebuffers(1, &fboTex);
    if (fboRb) glDeleteFramebuffers(1, &fboRb);
    if (tex) glDeleteTextures(1, &tex);
    if (rb) glDeleteRenderbuffers(1, &rb);
    e.pDestroyImage(g.dpy, img);
    AHardwareBuffer_release(ahb);
}

// ---------------------------------------------------------------------------
// T4, the other end: the buffer the DISPLAY HOST is about to scan out
// ---------------------------------------------------------------------------
//
// WHY THIS LEG IS THE ONE THE PRODUCT TURNS ON.  The rows above ask what a driver will take
// FROM a container.  This one asks the question the architecture actually depends on: can
// the render server draw into the buffer the display host is about to put on the glass?  On
// an Anland-style host that is not a matter of taste -- the host dequeues its scanout
// buffers from its own SurfaceView, and nothing the server allocates for itself will ever be
// scanned out.  What makes it possible is that such a buffer is a real gralloc allocation,
// and the platform hands back an AHardwareBuffer for that handle (measured: handle type 2
// accepts it, on the device, in the untrusted_app domain).
//
// THE ROUND TRIP IS CLOSED FROM THIS SIDE, because only the host can say whether the drawing
// arrived where the screen will look.  The host offers; this side imports the image, clears a
// colour through it behind a fence, and answers; the host reads both pixels back through its
// own view of the same buffer and returns them; this side decides.  A row that said OK on
// "an EGLImage was created" would be reporting that the platform took the handle, which is
// not the question.


// Mirrors struct ahb_offer / ahb_ack / ahb_seen in the display host's ahb_bridge.h.  The
// handle travels ahead of the sideband on the same socket, which is the order both ends read.
struct T4HostOffer {
    uint32_t magic, version, index, width, height, stride, format, usage_lo, usage_hi;
    char note[64];
} __attribute__((packed));

struct T4HostAck {
    uint32_t magic, version, index, imported, drawn, fence_ok;
    uint8_t color[4];
    char note[64];
} __attribute__((packed));

struct T4HostSeen {
    uint32_t magic, version, index, locked;
    uint8_t observed[4];
    uint8_t observed_corner[4];
    char note[64];
} __attribute__((packed));

static int t4Pending(int fd)
{
    int n = -1;
    return ioctl(fd, FIONREAD, &n) == 0 ? n : -1;
}


// The magic the display host's ahb_bridge.h stamps on all three of its messages.  It is NOT
// kT4Magic: that one belongs to the dma-buf protocol between this probe and the container
// peer, and using it here failed a good round trip while reporting the wrong reason -- the
// handle had arrived, the description had arrived, and only the comparison was wrong.
static const uint32_t kT4HostMagic = 0x4D474C41u;  // "ALGM" in memory order

static void runT4Host(GlCtx& g, bool glOk, const char* path) {
    if (!glOk) {
        record(kT4HostRow, "SKIP", "no headless GLES context: nothing on this side could take the host's image");
        return;
    }
    // The entry points this leg is spelled in, loaded here rather than shared with runT4:
    // the two legs are run on their own (--only-t4 / --only-t4-host), and a load that one of
    // them needs must not depend on the other having run first.
    T4Egl e;
    e.pCreateImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    e.pDestroyImage = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    e.pTargetRb =
        (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
    if (!g.hasEgl("EGL_KHR_image_base") || !e.pCreateImage || !e.pDestroyImage || !e.pTargetRb) {
        record(kT4HostRow, "UNSUPPORTED",
               fmt("EGL_KHR_image_base=%d eglCreateImageKHR=%d eglDestroyImageKHR=%d "
                   "glEGLImageTargetRenderbufferStorageOES=%d: the host's image cannot be taken and drawn into",
                   (int)g.hasEgl("EGL_KHR_image_base"), (int)(e.pCreateImage != nullptr),
                   (int)(e.pDestroyImage != nullptr), (int)(e.pTargetRb != nullptr)));
        return;
    }
    if (!g.pGetNativeClientBuffer) {
        record(kT4HostRow, "UNSUPPORTED",
               "EGL_ANDROID_get_native_client_buffer is not advertised, so an AHardwareBuffer cannot become an "
               "EGLClientBuffer and the host's image cannot be taken at all");
        return;
    }
    const int lfd = rtListen(path);
    if (lfd < 0) {
        record(kT4HostRow, "FAIL", fmt("could not listen on %s: errno=%d(%s)", path, errno, strerror(errno)));
        return;
    }
    setRecvTimeout(lfd, 60);
    const int fd = accept(lfd, nullptr, nullptr);
    const int acceptErr = errno;
    close(lfd);
    if (fd < 0) {
        record(kT4HostRow, "FAIL",
               fmt("no display host dialled %s within the accept timeout: errno=%d(%s)", path, acceptErr,
                   strerror(acceptErr)));
        return;
    }
    setRecvTimeout(fd, 60);
    pr("T4-host: a display host dialled %s; waiting for its buffers", path);

    const T4ColorPlan plan = t4PlanColor(kT4AhbFourcc, kT4AhbSeed);
    int offered = 0, imported = 0, drew = 0, seen = 0, agreed = 0;
    std::string trace, fail;

    for (int i = 0; i < 16; ++i) {
        const int pendBefore = t4Pending(fd);
        AHardwareBuffer* ahb = nullptr;
        const int hrc = AHardwareBuffer_recvHandleFromUnixSocket(fd, &ahb);
        const int pendAfter = t4Pending(fd);
        /* The handle message footprint, measured rather than assumed: a receiver that leaves
         * bytes behind hands the description reader whatever was left. */
        pr("T4-host: recvHandle rc=%d pending %d -> %d", hrc, pendBefore, pendAfter);
        if (hrc != 0 || !ahb)
            break;
        T4HostOffer of{};
        const bool ofRead = readAll(fd, &of, sizeof(of));
        if (!ofRead || of.magic != kT4HostMagic) {
            pr("T4-host: sideband read=%d magic=0x%08x first=%02x %02x %02x %02x", (int)ofRead, of.magic,
               ((const unsigned char *)&of)[0], ((const unsigned char *)&of)[1],
               ((const unsigned char *)&of)[2], ((const unsigned char *)&of)[3]);
            char why[160];
            snprintf(why, sizeof(why), "handle rc=%d (pending %d -> %d) then sideband read=%d magic=0x%08x",
                     hrc, pendBefore, pendAfter, (int)ofRead, of.magic);
            fail = why;
            AHardwareBuffer_release(ahb);
            break;
        }
        ++offered;
        AHardwareBuffer_Desc d{};
        AHardwareBuffer_describe(ahb, &d);

        T4HostAck ack{};
        ack.magic = kT4HostMagic;
        ack.version = 1;
        ack.index = of.index;

        EGLClientBuffer cb = g.pGetNativeClientBuffer((const struct AHardwareBuffer*)ahb);
        const EGLint cbErr = eglGetError();
        EGLImageKHR img = EGL_NO_IMAGE_KHR;
        if (cb) {
            const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
            img = e.pCreateImage(g.dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, attrs);
        }
        const EGLint imgErr = eglGetError();
        if (img == EGL_NO_IMAGE_KHR) {
            trace += fmt("buf%u=image(0x%04x/0x%04x) ", of.index, (unsigned)cbErr, (unsigned)imgErr);
            snprintf(ack.note, sizeof(ack.note), "clientBuffer=0x%04x image=0x%04x", (unsigned)cbErr,
                     (unsigned)imgErr);
            (void)writeAll(fd, &ack, sizeof(ack));
            AHardwareBuffer_release(ahb);
            continue;
        }
        ++imported;
        ack.imported = 1;

        GLuint rb = 0, fbo = 0;
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glDrain();
        e.pTargetRb(GL_RENDERBUFFER, (GLeglImageOES)img);
        const GLenum rbErr = glDrain();
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        const GLenum attachErr = glDrain();

        if (rbErr == GL_NO_ERROR && status == GL_FRAMEBUFFER_COMPLETE && attachErr == GL_NO_ERROR) {
            const std::string fence = t4WritePath(fbo, d.width, d.height, plan.clear);
            ack.drawn = 1;
            ack.fence_ok = fence.find("GL_TIMEOUT_EXPIRED") == std::string::npos &&
                           fence.find("GL_WAIT_FAILED") == std::string::npos;
            memcpy(ack.color, kT4AhbSeed, 4);
            snprintf(ack.note, sizeof(ack.note), "%ux%u stride=%u(px) cleared %s | %s", d.width, d.height, d.stride,
                     t4Hex4(kT4AhbSeed).c_str(), fence.c_str());
            ++drew;
        } else {
            snprintf(ack.note, sizeof(ack.note),
                     "rb=0x%04x fbo=0x%04x attach=0x%04x: not drawable",
                     (unsigned)rbErr, (unsigned)status, (unsigned)attachErr);
        }
        (void)writeAll(fd, &ack, sizeof(ack));

        T4HostSeen back{};
        if (readAll(fd, &back, sizeof(back)) && back.magic == kT4HostMagic) {
            ++seen;
            const bool first = !memcmp(back.observed, kT4AhbSeed, 4);
            const bool corner = !memcmp(back.observed_corner, kT4AhbSeed, 4);
            if (back.locked && first && corner)
                ++agreed;
            trace += fmt("buf%u=%s/%s ", of.index, first ? "first-ok" : "first-bad", corner ? "corner-ok" : "corner-bad");
        } else {
            trace += fmt("buf%u=no-readback ", of.index);
        }

        if (fbo)
            glDeleteFramebuffers(1, &fbo);
        if (rb)
            glDeleteRenderbuffers(1, &rb);
        e.pDestroyImage(g.dpy, img);
        AHardwareBuffer_release(ahb);
    }
    close(fd);

    if (offered == 0) {
        record(kT4HostRow, "FAIL",
               fmt("the display host dialled %s but offered no buffer: %s", path,
                   fail.empty() ? "the connection closed first" : fail.c_str()));
        return;
    }
    std::vector<Leg> legs;
    Leg leg;
    leg.name = "host";
    leg.decisive = true;
    leg.attempted = true;
    leg.readOk = imported == offered && drew == imported;  // the server got in and drew
    leg.writeOk = seen == offered && agreed == seen;       // and the host saw it at both pixels
    if (!leg.writeOk)
        leg.fail = fmt("drawn=%d/%d hostReadback=%d/%d agreed=%d", drew, offered, seen, offered, agreed);
    legs.push_back(leg);
    std::string why;
    const char* status = legVerdict(legs, &why);
    record(kT4HostRow, status,
           fmt("%s | offered=%d imported=%d drawn=%d hostReadback=%d agreed=%d | %s | seed=%s [%s] | server=%s | "
               "trace: %s",
               legTrace(legs).c_str(), offered, imported, drew, seen, agreed, why.c_str(),
               t4Hex4(kT4AhbSeed).c_str(), plan.note.c_str(), selfContext().c_str(), trace.c_str()));
}

#else  // !PROBE_HAVE_AHB

// The host build has neither a listener nor an AHardwareBuffer, so both T4 legs report
// exactly that instead of inventing a device answer they cannot have.  PROBE_HAVE_AHB is
// simply the macro that carries rtListen() and the AHardwareBuffer entry points.
static void runT4(GlCtx&, bool, const char*) {
    record(kT4Row, "SKIP",
           "host build has no listener: rtListen() is compiled with the AHardwareBuffer block, so the "
           "container peer has nothing to dial");
}

static void runT4Host(GlCtx&, bool, const char*) {
    record(kT4HostRow, "SKIP",
           "host build has no listener and no AHardwareBuffer: the display host has nothing to dial");
}

#endif  // PROBE_HAVE_AHB

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void printSummary() {
    char model[PROP_VALUE_MAX] = {0};
    getProp("ro.product.model", model, sizeof(model));
    std::string sec = readSmallFile("/proc/self/attr/current");
    printf("\n=== extmem_probe summary (model=%s selinux=%s) ===\n", model, sec.c_str());
    printf("NOTE: %s\n", kDomainCaveat);
    printf("%-34s %-12s %s\n", "ROUTE", "STATUS", "DETAIL");
    for (const RouteResult& r : gResults)
        printf("%-34s %-12s %s\n", r.route.c_str(), r.status.c_str(), r.detail.c_str());
    printf("=== end ===\n");
    fflush(stdout);
}

// The trace APK's exec hook (spawn_spike.cpp) runs `<lib> <markerPath>` from the app
// process and cannot pass options. So a positional argument is taken as that marker
// path: options are read from `<markerPath>.args` (whitespace-separated, optional), and
// at the end a one-line verdict is written to the marker so the hook's report reads OK.
static std::vector<std::string> readArgsFile(const std::string& path) {
    std::vector<std::string> out;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return out;
    char tok[256];
    while (fscanf(f, "%255s", tok) == 1) out.push_back(tok);
    fclose(f);
    return out;
}

int main(int argc, char** argv) {
    // P11 B2 step 1: exec'd by MobileGLServerService as `libMobileGLServer.so <endpoint> --serve`.
    if (argc >= 3 && !strcmp(argv[2], "--serve")) return routeServe(argv[1]);
    uint64_t size = kDefaultSize;
    bool routeClientMode = false;
    std::string routeEndpoint;
    std::string t4Endpoint = "@mgl-t4";
    // The other end of T4: the buffer the display host is about to scan out.  A path rather
    // than an abstract name, because the host is the one that dials and that is where its
    // bridge looks for a listener.
    bool doT4Host = false;
    std::string t4HostPath = "/data/local/tmp/mobilegl_bridge.sock";
    const char* childRoute = nullptr;
    bool doT1 = true, doT0 = true, doT3 = true, doGles = true;
    // T4 waits for a peer that only exists when it is asked for, so it is off unless
    // --only-t4 turns it on.
    bool doT4 = false;
    bool regionsAtEnd = false;
    uint32_t sustainedRounds = 0;
    std::string markerPath;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-' && markerPath.empty()) {
            markerPath = argv[i];
        } else {
            args.push_back(argv[i]);
        }
    }
    std::vector<std::string> fileArgs;
    if (!markerPath.empty()) {
        fileArgs = readArgsFile(markerPath + ".args");
        args.insert(args.end(), fileArgs.begin(), fileArgs.end());
    }
    for (const std::string& s : args) {
        const char* a = s.c_str();
        if (!strncmp(a, "--child=", 8)) {
            static std::string route;  // outlives `args`
            route = a + 8;
            childRoute = route.c_str();
        } else if (!strcmp(a, "--route-client")) {
            routeClientMode = true;
        } else if (!strncmp(a, "--route-client=", 15)) {
            routeClientMode = true;
            routeEndpoint = a + 15;
        } else if (!strncmp(a, "--size=", 7)) {
            size = strtoull(a + 7, nullptr, 0);
        } else if (!strncmp(a, "--hold-ms=", 10)) {
            gHoldMs = (uint32_t)strtoul(a + 10, nullptr, 0);
        } else if (!strncmp(a, "--region-base=", 14)) {
            gRegionBase = strtoull(a + 14, nullptr, 0);
        } else if (!strcmp(a, "--regions-at-end")) {
            regionsAtEnd = true;
        } else if (!strncmp(a, "--sustained-lock=", 17)) {
            sustainedRounds = (uint32_t)strtoul(a + 17, nullptr, 0);
        } else if (!strcmp(a, "--sustained-lock")) {
            sustainedRounds = 8;
        } else if (!strcmp(a, "--only-t1")) {
            doT0 = doT3 = doT4 = false;
        } else if (!strcmp(a, "--only-t0")) {
            doT1 = doT3 = doGles = doT4 = false;
        } else if (!strcmp(a, "--only-sustained")) {
            doT1 = doT0 = doT3 = doGles = doT4 = false;
            if (!sustainedRounds) sustainedRounds = 8;
        } else if (!strcmp(a, "--only-t3")) {
            doT1 = doT0 = doGles = doT4 = false;
        } else if (!strcmp(a, "--only-gles")) {
            doT1 = doT0 = doT3 = doT4 = false;
        } else if (!strcmp(a, "--only-t4")) {
            doT1 = doT0 = doT3 = doGles = false;
            doT4 = true;
        } else if (!strncmp(a, "--only-t4=", 10)) {
            doT1 = doT0 = doT3 = doGles = false;
            doT4 = true;
            t4Endpoint = a + 10;
        } else if (!strcmp(a, "--only-t4-host")) {
            doT1 = doT0 = doT3 = doGles = doT4 = false;
            doT4Host = true;
        } else if (!strncmp(a, "--only-t4-host=", 15)) {
            doT1 = doT0 = doT3 = doGles = doT4 = false;
            doT4Host = true;
            t4HostPath = a + 15;
        } else if (!strcmp(a, "--no-gles")) {
            doGles = false;
        } else if (!strcmp(a, "--help")) {
            printf("usage: extmem_probe [markerPath] [--size=BYTES] [--regions-at-end] [--sustained-lock[=ROUNDS]]\n"
                   "       extmem_probe --route-client[=@endpoint] [--size=BYTES] [--sustained-lock=ROUNDS] [--regions-at-end]\n"
                   "       extmem_probe <endpoint> --serve   (packaged as libMobileGLServer.so)\n"
                   "       [--only-t0|--only-t1|--only-t3|--only-gles|--only-sustained|--only-t4[=@endpoint]\n"
                   "        |--only-t4-host[=PATH]] [--no-gles]\n"
                   "  --only-t4-host[=PATH]: listen on /data/local/tmp/mobilegl_bridge.sock (default) for a\n"
                   "                         display host offering the buffer it is about to scan out; the round\n"
                   "                         trip is closed by that host reading its own pixels back.\n"
                   "  --only-t4[=@endpoint]: listen on @mgl-t4 (default) for the droidspaces container peer;\n"
                   "                         one connection carries both T4 rows: T4-image-import (its dma-buf\n"
                   "                         into this side) and T4-ahb-image (this side's AHardwareBuffer into\n"
                   "                         the container, sent as an fd over SCM_RIGHTS)\n"
                   "  markerPath: exec-hook mode; options are also read from <markerPath>.args\n");
            return 0;
        }
    }
    if (size < kRegionCount * kRegion) size = kRegionCount * kRegion;
    size = (size + kRegion - 1) / kRegion * kRegion;
    if (regionsAtEnd && !childRoute) gRegionBase = size - kRegionCount * kRegion;
    if (routeClientMode)
        return routeClient(routeEndpoint.c_str(), size, sustainedRounds ? sustainedRounds : 4, regionsAtEnd);

    // A peer that has already exited must not take this process down with it.
    signal(SIGPIPE, SIG_IGN);

    if (childRoute) {
        static char roleBuf[32];
        snprintf(roleBuf, sizeof(roleBuf), "child:%s", childRoute);
        gRole = roleBuf;
        int sock = 3;
        if (!strcmp(childRoute, "t1")) return childT1(sock);
        if (!strcmp(childRoute, "t1gl")) return childT1Gl(sock);
        if (!strcmp(childRoute, "t0")) return childT0(sock);
        if (!strcmp(childRoute, "t0s")) return childT0Sustained(sock);
        if (!strcmp(childRoute, "t3")) return childT3(sock);
        if (!strcmp(childRoute, "t3c")) return childT3Client(sock);
        pr("unknown child route %s", childRoute);
        return 1;
    }

    std::string joined;
    for (const std::string& s : args) joined += " " + s;
    pr("extmem_probe: MobileGL disaggregation spike B, size=%llu bytes regionBase=%llu sustainedRounds=%u "
       "marker=%s args=[%s ]",
       (unsigned long long)size, (unsigned long long)gRegionBase, sustainedRounds,
       markerPath.empty() ? "-" : markerPath.c_str(), joined.c_str());
    printRunContext();

    VkCtx c;
    bool vkOk = vkCtxInit(c, true);
    GlCtx g;
    bool glOk = glCtxInit(g);

    if (!vkOk) {
        record("vulkan-init", "FAIL", "no usable Vulkan device");
        printSummary();
        return 1;
    }
    phaseEnumerate(c, g, glOk);

    pr("=== phase T1: server-exported allocation (opaque fd / dma-buf) ===");
    if (doT1) {
        runT1Parent(c, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, "T1-opaque-fd", size);
        runT1Parent(c, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, "T1-dma-buf", size);
    }
    pr("=== phase T1-gles: the same export imported as GL buffer storage ===");
    if (doGles) runT1GlesParent(c, g, glOk, size);
    pr("=== phase T0: client-allocated AHardwareBuffer BLOB ===");
    if (doT0) runT0Parent(c, g, glOk, size);
    pr("=== phase T0S: client-allocated AHardwareBuffer, lock held across GPU rounds ===");
    if (sustainedRounds) runT0SustainedParent(c, g, glOk, size, sustainedRounds);
    pr("=== phase T3: VK_EXT_external_memory_host ===");
    if (doT3) {
        runT3Parent(c, size);
        runT3ClientAllocParent(c, size);
    }
    pr("=== phase T4: an image allocated outside Android (dma-buf from the container peer) ===");
    if (doT4) runT4(g, glOk, t4Endpoint.c_str());
    if (doT4Host) {
        pr("T4-host: listening on %s for the display host buffers", t4HostPath.c_str());
        runT4Host(g, glOk, t4HostPath.c_str());
    }

    glCtxDestroy(g);
    vkCtxDestroy(c);
    printSummary();
    if (!markerPath.empty()) {
        int ok = 0, partial = 0, fail = 0, unsup = 0, skip = 0;
        for (const RouteResult& r : gResults) {
            if (r.status == "OK") ++ok;
            else if (r.status == "PARTIAL") ++partial;
            else if (r.status == "FAIL") ++fail;
            else if (r.status == "UNSUPPORTED") ++unsup;
            else ++skip;
        }
        FILE* m = fopen(markerPath.c_str(), "w");
        if (m) {
            fprintf(m, "extmem_probe done selinux=%s rows=%zu OK=%d PARTIAL=%d FAIL=%d UNSUPPORTED=%d SKIP=%d\n",
                    readSmallFile("/proc/self/attr/current").c_str(), gResults.size(), ok, partial, fail, unsup,
                    skip);
            fclose(m);
        }
    }
    return 0;
}
