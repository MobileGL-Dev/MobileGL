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

#if PROBE_HAVE_AHB
#  include <android/hardware_buffer.h>
#  include <sys/system_properties.h>
#else
#  define PROP_VALUE_MAX 92
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

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

static void writeRegion(void* base, int region, uint32_t seed) {
    fillPattern((uint8_t*)base + region * kRegion, kRegion, seed);
}
static int64_t checkRegion(const void* base, int region, uint32_t seed) {
    return checkPattern((const uint8_t*)base + region * kRegion, kRegion, seed);
}

// vkCmdFillBuffer writes a repeating 32-bit word; -1 on match, else the first
// mismatching word index * 4
static int64_t checkFillWord(const void* base, int region, uint32_t word) {
    const uint32_t* w = (const uint32_t*)((const uint8_t*)base + region * kRegion);
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
    copy.srcOffset = (VkDeviceSize)(readRegion * kRegion);
    copy.dstOffset = 0;
    copy.size = kRegion;
    vkCmdCopyBuffer(cmd, buf, staging, 1, &copy);
    vkCmdFillBuffer(cmd, buf, (VkDeviceSize)(fillRegion * kRegion), (VkDeviceSize)kRegion, fillWord);

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
        char self[512];
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n <= 0) _exit(90);
        self[n] = 0;
        char* argv[] = {self, arg, nullptr};
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

#else  // !PROBE_HAVE_AHB

static int childT0(int) {
    pr("T0 is Android-only");
    return 1;
}
static void runT0Parent(VkCtx&, GlCtx&, bool, uint64_t) {
    record("T0-ahb-blob-transfer", "SKIP", "AHardwareBuffer is Android-only; host build cannot run T0");
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

int main(int argc, char** argv) {
    uint64_t size = kDefaultSize;
    const char* childRoute = nullptr;
    bool doT1 = true, doT0 = true, doT3 = true, doGles = true;
    for (int i = 1; i < argc; ++i) {
        if (!strncmp(argv[i], "--child=", 8)) {
            childRoute = argv[i] + 8;
        } else if (!strncmp(argv[i], "--size=", 7)) {
            size = strtoull(argv[i] + 7, nullptr, 0);
        } else if (!strcmp(argv[i], "--only-t1")) {
            doT0 = doT3 = false;
        } else if (!strcmp(argv[i], "--only-t0")) {
            doT1 = doT3 = doGles = false;
        } else if (!strcmp(argv[i], "--only-t3")) {
            doT1 = doT0 = doGles = false;
        } else if (!strcmp(argv[i], "--only-gles")) {
            doT1 = doT0 = doT3 = false;
        } else if (!strcmp(argv[i], "--no-gles")) {
            doGles = false;
        } else if (!strcmp(argv[i], "--help")) {
            printf("usage: extmem_probe [--size=BYTES] [--only-t0|--only-t1|--only-t3|--only-gles] [--no-gles]\n");
            return 0;
        }
    }
    if (size < kRegionCount * kRegion) size = kRegionCount * kRegion;

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
        if (!strcmp(childRoute, "t3")) return childT3(sock);
        if (!strcmp(childRoute, "t3c")) return childT3Client(sock);
        pr("unknown child route %s", childRoute);
        return 1;
    }

    pr("extmem_probe: MobileGL disaggregation spike B, size=%llu bytes", (unsigned long long)size);
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
    pr("=== phase T3: VK_EXT_external_memory_host ===");
    if (doT3) {
        runT3Parent(c, size);
        runT3ClientAllocParent(c, size);
    }

    glCtxDestroy(g);
    vkCtxDestroy(c);
    printSummary();
    return 0;
}
