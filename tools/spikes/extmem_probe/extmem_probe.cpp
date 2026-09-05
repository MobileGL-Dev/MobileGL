// extmem_probe -- MobileGL disaggregation P0 spike B (plan-B §8.3, §11 P0).
//
// Question this program answers, per device:
//   Can a server-allocated HOST_VISIBLE|HOST_COHERENT VkDeviceMemory be shared
//   with another process and mapped there, and by which route?
//
//   T1  server exports its own allocation      (VkExportMemoryAllocateInfo +
//       vkGetMemoryFdKHR, opaque-fd and dma-buf, handed over SCM_RIGHTS; the
//       importer tries plain mmap() *and* a Vulkan import + vkMapMemory)
//   T0  server imports a client allocation     (AHardwareBuffer BLOB sent over a
//       unix socket, imported into VkDeviceMemory via
//       VK_ANDROID_external_memory_android_hardware_buffer and into a GL buffer
//       via EGL_ANDROID_get_native_client_buffer + glBufferStorageExternalEXT)
//   T3  server imports a client host mapping   (VK_EXT_external_memory_host over
//       a memfd-backed mmap region)
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
// The probe is an Android deliverable; the host build exists only so the T1/T3
// harness itself can be validated against a driver that is known to implement
// those routes (lavapipe), which is what makes a device-side FAIL attributable
// to the driver rather than to this program.  T0 is Android-only by nature.
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

static std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return std::string(buf);
}

static const char* vkStr(VkResult r) {
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
        default: {
            static char tmp[32];
            snprintf(tmp, sizeof(tmp), "VkResult(%d)", (int)r);
            return tmp;
        }
    }
}

// ---------------------------------------------------------------------------
// payload patterns
// ---------------------------------------------------------------------------

static const uint64_t kRegion = 4096;      // bytes per verification region
static const uint64_t kDefaultSize = 65536;

// region indices inside the shared allocation
enum {
    REG_A = 0,  // first writer's payload
    REG_B = 1,  // importer write through the plain host mapping (mmap / AHB lock)
    REG_C = 2,  // importer write through the imported Vulkan mapping
    REG_D = 3,  // importer write through the imported GL mapping
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
        pr("vkCreateInstance failed: %s", vkStr(r));
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
        pr("vkCreateDevice failed: %s", vkStr(r));
        return false;
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
        pr("deviceUUID=%s minImportedHostPointerAlignment=%llu", uuid,
           (unsigned long long)c.minImportedHostPointerAlignment);
    }
    return true;
}

static void vkCtxDestroy(VkCtx& c) {
    if (c.device) vkDestroyDevice(c.device, nullptr);
    if (c.instance) vkDestroyInstance(c.instance, nullptr);
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
// T1 payloads
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
};

struct T1Result {
    int32_t gotFd;
    int32_t mmapOk;
    int32_t mmapErrno;
    int64_t mmapMismatch;       // -1 == data matched
    int64_t mmapPatternOffset;  // where the exporter's payload really starts in the mapping, -1 = not found
    int32_t vkInitOk;
    int32_t fdPropsResult;  // VkResult of vkGetMemoryFdPropertiesKHR
    uint32_t fdMemoryTypeBits;
    int32_t importResult;   // VkResult of vkAllocateMemory with the import struct
    int32_t bindResult;
    int32_t mapResult;
    int64_t vkMismatch;     // -1 == data matched
    int32_t wroteB;
    int32_t wroteC;
    char note[384];
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
    uint32_t seedB;      // parent wrote REG_B through the imported vkMapMemory
    uint32_t seedC;      // parent wrote REG_C through the imported GL mapping
    uint32_t seedD;      // parent wrote REG_D through AHardwareBuffer_lock
    uint32_t writtenMask;  // bit0=B bit1=C bit2=D
};

struct T0Result {
    int32_t lockOk;
    int32_t lockErr;
    int64_t mismatchB;
    int64_t mismatchC;
    int64_t mismatchD;
    char note[192];
};

struct T3Offer {
    uint64_t size;
    uint32_t seedA;
    uint32_t seedB;
};

struct T3Result {
    int32_t mmapOk;
    int32_t mmapErrno;
    int64_t mismatch;
    char note[192];
};

// ---------------------------------------------------------------------------
// Phase A: enumeration
// ---------------------------------------------------------------------------

static const char* memFlagStr(VkMemoryPropertyFlags f) {
    static char b[128];
    b[0] = 0;
    if (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) strcat(b, "DEVICE_LOCAL ");
    if (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) strcat(b, "HOST_VISIBLE ");
    if (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) strcat(b, "HOST_COHERENT ");
    if (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) strcat(b, "HOST_CACHED ");
    if (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) strcat(b, "LAZY ");
    if (f & VK_MEMORY_PROPERTY_PROTECTED_BIT) strcat(b, "PROTECTED ");
    return b;
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
    char feat[96];
    feat[0] = 0;
    if (p.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) strcat(feat, "DEDICATED_ONLY ");
    if (p.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) strcat(feat, "EXPORTABLE ");
    if (p.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) strcat(feat, "IMPORTABLE ");
    if (!feat[0]) strcat(feat, "<none>");
    pr("  externalBuffer[%s]: features=%s exportFrom=0x%x compatible=0x%x", name, feat,
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
           (unsigned long long)(c.memProps.memoryHeaps[mt.heapIndex].size >> 20), memFlagStr(mt.propertyFlags));
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
        record("A-gles-context", "FAIL", "no headless EGL context");
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
    pr("  eglGetNativeClientBufferANDROID=%p glBufferStorageExternalEXT=%p",
       (void*)g.pGetNativeClientBuffer, (void*)g.pBufferStorageExternal);
}

// ---------------------------------------------------------------------------
// T1 parent
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

    // exportability report first -- a driver that says "not exportable" here and
    // still returns an fd is a driver bug we want on the record.
    VkPhysicalDeviceExternalBufferInfo ebi{};
    ebi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
    ebi.usage = kProbeBufferUsage;
    ebi.handleType = handleType;
    VkExternalBufferProperties ebp{};
    ebp.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
    vkGetPhysicalDeviceExternalBufferProperties(c.phys, &ebi, &ebp);
    bool advertisedExportable =
        (ebp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
    pr("T1[%s] advertisedExportable=%d importable=%d", routeName, (int)advertisedExportable,
       (int)((ebp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0));

    VkExternalMemoryBufferCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    ext.handleTypes = handleType;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.pNext = &ext;
    bci.size = size;
    bci.usage = kProbeBufferUsage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buf = VK_NULL_HANDLE;
    VkResult r = vkCreateBuffer(c.device, &bci, nullptr, &buf);
    if (r != VK_SUCCESS) {
        record(routeName, "FAIL", fmt("vkCreateBuffer(external)=%s", vkStr(r)));
        return;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(c.device, buf, &req);
    int typeIdx = pickMemType(c.memProps, req.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (typeIdx < 0) {
        vkDestroyBuffer(c.device, buf, nullptr);
        record(routeName, "FAIL", fmt("no HOST_VISIBLE|HOST_COHERENT type in bits=0x%x", req.memoryTypeBits));
        return;
    }
    pr("T1[%s] memReq size=%llu align=%llu typeBits=0x%x -> type %d", routeName,
       (unsigned long long)req.size, (unsigned long long)req.alignment, req.memoryTypeBits, typeIdx);

    VkExportMemoryAllocateInfo exportInfo{};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exportInfo.handleTypes = handleType;
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.buffer = buf;
    bool needDedicated =
        (ebp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
    if (needDedicated) exportInfo.pNext = &dedicated;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &exportInfo;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)typeIdx;

    VkDeviceMemory mem = VK_NULL_HANDLE;
    r = vkAllocateMemory(c.device, &mai, nullptr, &mem);
    if (r != VK_SUCCESS) {
        vkDestroyBuffer(c.device, buf, nullptr);
        record(routeName, advertisedExportable ? "FAIL" : "UNSUPPORTED",
               fmt("vkAllocateMemory(export)=%s (advertisedExportable=%d)", vkStr(r), (int)advertisedExportable));
        return;
    }
    r = vkBindBufferMemory(c.device, buf, mem, 0);
    if (r != VK_SUCCESS) pr("T1[%s] vkBindBufferMemory=%s (continuing)", routeName, vkStr(r));

    void* host = nullptr;
    r = vkMapMemory(c.device, mem, 0, VK_WHOLE_SIZE, 0, &host);
    if (r != VK_SUCCESS) {
        vkFreeMemory(c.device, mem, nullptr);
        vkDestroyBuffer(c.device, buf, nullptr);
        record(routeName, "FAIL", fmt("server-side vkMapMemory=%s", vkStr(r)));
        return;
    }
    const uint32_t seedA = 0xA5A50001u, seedB = 0xB0B00002u, seedC = 0xC0C00003u;
    memset(host, 0, (size_t)size);
    writeRegion(host, REG_A, seedA);

    VkMemoryGetFdInfoKHR gfi{};
    gfi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    gfi.memory = mem;
    gfi.handleType = handleType;
    int fd = -1;
    r = c.pGetMemoryFdKHR(c.device, &gfi, &fd);
    if (r != VK_SUCCESS || fd < 0) {
        vkUnmapMemory(c.device, mem);
        vkFreeMemory(c.device, mem, nullptr);
        vkDestroyBuffer(c.device, buf, nullptr);
        record(routeName, "UNSUPPORTED", fmt("vkGetMemoryFdKHR=%s fd=%d (advertisedExportable=%d)", vkStr(r), fd,
                                             (int)advertisedExportable));
        return;
    }
    pr("T1[%s] exported fd=%d -> %s", routeName, fd, describeFd(fd).c_str());

    int sock = -1;
    pid_t pid = spawnChild("t1", &sock);
    if (pid < 0) {
        close(fd);
        vkUnmapMemory(c.device, mem);
        vkFreeMemory(c.device, mem, nullptr);
        vkDestroyBuffer(c.device, buf, nullptr);
        record(routeName, "FAIL", "spawnChild failed");
        return;
    }

    T1Offer offer{};
    offer.allocationSize = req.size;
    offer.bufferSize = size;
    offer.handleType = (uint32_t)handleType;
    offer.seedA = seedA;
    offer.seedB = seedB;
    offer.seedC = seedC;
    offer.memoryTypeIndex = (uint32_t)typeIdx;
    offer.memoryTypeBits = req.memoryTypeBits;

    std::string detail;
    const char* status = "FAIL";
    if (!sendMsg(sock, MSG_T1_OFFER, &offer, sizeof(offer), fd)) {
        detail = fmt("sendMsg(offer) errno=%d", errno);
    } else {
        close(fd);
        fd = -1;
        uint32_t tag = 0;
        T1Result res{};
        size_t got = 0;
        if (!recvMsg(sock, &tag, &res, sizeof(res), &got, nullptr) || tag != MSG_T1_RESULT ||
            got != sizeof(res)) {
            detail = fmt("no T1 result from child (errno=%d, %s)", errno, reapChild(pid).c_str());
            pid = -1;
        } else {
            // The child wrote REG_B (mmap) and REG_C (imported vkMapMemory); check
            // that the writes are visible through the *server's* own mapping.
            int64_t backB = res.wroteB ? checkRegion(host, REG_B, seedB) : -2;
            int64_t backC = res.wroteC ? checkRegion(host, REG_C, seedC) : -2;

            detail = fmt(
                "mmap=%s(errno=%d,cmp=%lld,payloadAt=%lld,back=%lld) vkimport=%s(fdProps=%s bits=0x%x bind=%s "
                "map=%s cmp=%lld back=%lld) %s",
                res.mmapOk ? "ok" : "fail", res.mmapErrno, (long long)res.mmapMismatch,
                (long long)res.mmapPatternOffset, (long long)backB,
                res.importResult == VK_SUCCESS ? "ok" : vkStr((VkResult)res.importResult),
                vkStr((VkResult)res.fdPropsResult), res.fdMemoryTypeBits, vkStr((VkResult)res.bindResult),
                vkStr((VkResult)res.mapResult), (long long)res.vkMismatch, (long long)backC, res.note);

            bool mmapPath = res.mmapOk && res.mmapMismatch == -1 && backB == -1;
            bool vkPath = res.importResult == VK_SUCCESS && res.mapResult == VK_SUCCESS && res.vkMismatch == -1 &&
                          backC == -1;
            if (mmapPath && vkPath) {
                status = "OK";
            } else if (mmapPath || vkPath) {
                status = "PARTIAL";
            } else if (!res.mmapOk && res.importResult != VK_SUCCESS) {
                status = "FAIL";
            } else {
                status = "PARTIAL";
            }
        }
    }
    if (pid > 0) {
        sendMsg(sock, MSG_BYE, nullptr, 0, -1);
        detail += " ";
        detail += reapChild(pid);
    }
    close(sock);
    if (fd >= 0) close(fd);
    vkUnmapMemory(c.device, mem);
    vkFreeMemory(c.device, mem, nullptr);
    vkDestroyBuffer(c.device, buf, nullptr);
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
    void* p = mmap(nullptr, mappedLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        res.mmapOk = 0;
        res.mmapErrno = errno;
        pr("child: mmap(MAP_SHARED) failed errno=%d (%s)", errno, strerror(errno));
        // second chance: some allocators only allow the buffer size, not the padded size
        mappedLen = (size_t)offer.bufferSize;
        p = mmap(nullptr, mappedLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p != MAP_FAILED) {
            res.mmapOk = 2;
            note += " [mmap needed bufferSize not allocationSize]";
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

    // (2) import the same fd into a child-side VkDeviceMemory and map it
    VkCtx c;
    if (!vkCtxInit(c, false)) {
        if (p != MAP_FAILED) {
            writeRegion(p, REG_B, offer.seedB);
            res.wroteB = 1;
            msync(p, (size_t)mappedLen, MS_SYNC);
        }
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
        if (p != MAP_FAILED) {
            writeRegion(p, REG_B, offer.seedB);
            res.wroteB = 1;
            msync(p, (size_t)mappedLen, MS_SYNC);
        }
        snprintf(res.note, sizeof(res.note), "%s | child vkCreateBuffer=%s", note.c_str(), vkStr(r));
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
            if (p != MAP_FAILED) {
                writeRegion(p, REG_B, offer.seedB);
                res.wroteB = 1;
                msync(p, (size_t)mappedLen, MS_SYNC);
            }
            snprintf(res.note, sizeof(res.note), "%s | import=%s type=%d bits=0x%x", note.c_str(), vkStr(r),
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
        writeRegion(host, REG_C, offer.seedC);
        res.wroteC = 1;
        vkUnmapMemory(c.device, mem);
    }
    // now that both mappings have been read, write through the plain one too
    if (p != MAP_FAILED) {
        writeRegion(p, REG_B, offer.seedB);
        res.wroteB = 1;
        msync(p, (size_t)mappedLen, MS_SYNC);
    }
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
    res.mismatchB = res.mismatchC = res.mismatchD = -2;
    void* q = nullptr;
    rc = AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &q);
    res.lockOk = (rc == 0 && q) ? 1 : 0;
    res.lockErr = rc;
    if (res.lockOk) {
        if (ver.writtenMask & 1) res.mismatchB = checkRegion(q, REG_B, ver.seedB);
        if (ver.writtenMask & 2) res.mismatchC = checkRegion(q, REG_C, ver.seedC);
        if (ver.writtenMask & 4) res.mismatchD = checkRegion(q, REG_D, ver.seedD);
        AHardwareBuffer_unlock(ahb, nullptr);
    }
    snprintf(res.note, sizeof(res.note), "mask=0x%x", ver.writtenMask);
    sendMsg(sock, MSG_T0_RESULT, &res, sizeof(res), -1);
    AHardwareBuffer_release(ahb);
    return 0;
}

static void runT0Parent(VkCtx& c, GlCtx& g, bool glOk, uint64_t size) {
    const uint32_t seedA = 0x0A0A0011u, seedB = 0x0B0B0022u, seedC = 0x0C0C0033u, seedD = 0x0D0D0044u;

    int sock = -1;
    pid_t pid = spawnChild("t0", &sock);
    if (pid < 0) {
        record("T0-ahb-blob-transfer", "FAIL", "spawnChild failed");
        return;
    }
    T0Request rq{};
    rq.size = size;
    rq.seedA = seedA;
    if (!sendMsg(sock, MSG_T0_REQUEST, &rq, sizeof(rq), -1)) {
        record("T0-ahb-blob-transfer", "FAIL", fmt("sendMsg errno=%d", errno));
        close(sock);
        reapChild(pid);
        return;
    }
    T0Alloc alloc{};
    uint32_t tag = 0;
    size_t got = 0;
    if (!recvMsg(sock, &tag, &alloc, sizeof(alloc), &got, nullptr) || tag != MSG_T0_ALLOC) {
        record("T0-ahb-blob-transfer", "FAIL", fmt("no alloc reply errno=%d %s", errno, reapChild(pid).c_str()));
        close(sock);
        return;
    }
    if (alloc.allocOk != 1) {
        record("T0-ahb-blob-transfer", "FAIL", fmt("child alloc failed rc=%d %s", alloc.allocErr, alloc.note));
        close(sock);
        reapChild(pid);
        return;
    }
    pr("T0 child allocated: %s", alloc.note);

    AHardwareBuffer* ahb = nullptr;
    int rc = AHardwareBuffer_recvHandleFromUnixSocket(sock, &ahb);
    if (rc != 0 || !ahb) {
        record("T0-ahb-blob-transfer", "FAIL", fmt("recvHandleFromUnixSocket rc=%d errno=%d", rc, errno));
        close(sock);
        reapChild(pid);
        return;
    }
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(ahb, &desc);
    pr("T0 parent received AHB: w=%u h=%u fmt=0x%x usage=0x%llx stride=%u", desc.width, desc.height, desc.format,
       (unsigned long long)desc.usage, desc.stride);
    record("T0-ahb-blob-transfer", "OK", fmt("socket handoff of a %llu-byte BLOB works (%s)",
                                             (unsigned long long)size, alloc.note));

    // (a) CPU path: AHardwareBuffer_lock on the receiving side
    uint32_t writtenMask = 0;
    {
        void* p = nullptr;
        rc = AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
                                  -1, nullptr, &p);
        if (rc != 0 || !p) {
            record("T0-ahb-cpu-lock", "FAIL", fmt("AHardwareBuffer_lock rc=%d errno=%d", rc, errno));
        } else {
            int64_t cmp = checkRegion(p, REG_A, seedA);
            writeRegion(p, REG_D, seedD);
            writtenMask |= 4;
            AHardwareBuffer_unlock(ahb, nullptr);
            record("T0-ahb-cpu-lock", cmp == -1 ? "OK" : "FAIL",
                   fmt("cross-process CPU read of the child's payload, mismatch=%lld", (long long)cmp));
        }
    }

    // (b) Vulkan import
    if (!c.hasAhb || !c.pGetAhbProps) {
        record("T0-ahb-vulkan-import", "UNSUPPORTED",
               "VK_ANDROID_external_memory_android_hardware_buffer absent");
    } else {
        VkAndroidHardwareBufferPropertiesANDROID props{};
        props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
        VkResult r = c.pGetAhbProps(c.device, ahb, &props);
        if (r != VK_SUCCESS) {
            record("T0-ahb-vulkan-import", "FAIL", fmt("vkGetAndroidHardwareBufferPropertiesANDROID=%s", vkStr(r)));
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
                record("T0-ahb-vulkan-import", "FAIL", fmt("vkCreateBuffer(AHB external)=%s", vkStr(r)));
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
                    record("T0-ahb-vulkan-import", "FAIL",
                           fmt("vkAllocateMemory(import AHB)=%s typeIdx=%d bits=0x%x", vkStr(r), typeIdx,
                               props.memoryTypeBits));
                } else {
                    VkResult br = vkBindBufferMemory(c.device, buf, mem, 0);
                    void* host = nullptr;
                    VkResult mr = vkMapMemory(c.device, mem, 0, VK_WHOLE_SIZE, 0, &host);
                    if (mr == VK_SUCCESS && host) {
                        int64_t cmp = checkRegion(host, REG_A, seedA);
                        writeRegion(host, REG_B, seedB);
                        writtenMask |= 1;
                        vkUnmapMemory(c.device, mem);
                        record("T0-ahb-vulkan-import", cmp == -1 ? "OK" : "PARTIAL",
                               fmt("imported+mapped (hostVisibleType=%d bind=%s) payload mismatch=%lld",
                                   (int)hostVisible, vkStr(br), (long long)cmp));
                    } else {
                        record("T0-ahb-vulkan-import", "PARTIAL",
                               fmt("import ok, vkMapMemory=%s (bind=%s hostVisibleType=%d bits=0x%x)", vkStr(mr),
                                   vkStr(br), (int)hostVisible, props.memoryTypeBits));
                    }
                    vkFreeMemory(c.device, mem, nullptr);
                }
                vkDestroyBuffer(c.device, buf, nullptr);
            }
        }
    }

    // (c) GL import through EGL_ANDROID_get_native_client_buffer + EXT_external_buffer
    if (!glOk) {
        record("T0-ahb-gl-import", "SKIP", "no GL context");
    } else if (!g.hasGl("GL_EXT_external_buffer") || !g.pBufferStorageExternal || !g.pGetNativeClientBuffer) {
        record("T0-ahb-gl-import", "UNSUPPORTED",
               fmt("GL_EXT_external_buffer=%d GL_EXT_buffer_storage=%d eglGetNativeClientBufferANDROID=%d "
                   "glBufferStorageExternalEXT=%d",
                   (int)g.hasGl("GL_EXT_external_buffer"), (int)g.hasGl("GL_EXT_buffer_storage"),
                   (int)(g.pGetNativeClientBuffer != nullptr), (int)(g.pBufferStorageExternal != nullptr)));
    } else {
        EGLClientBuffer cb = g.pGetNativeClientBuffer(ahb);
        if (!cb) {
            record("T0-ahb-gl-import", "FAIL", fmt("eglGetNativeClientBufferANDROID=NULL egl=0x%04x", eglGetError()));
        } else {
            GLuint b = 0;
            glGenBuffers(1, &b);
            glBindBuffer(GL_ARRAY_BUFFER, b);
            while (glGetError() != GL_NO_ERROR) {}
            g.pBufferStorageExternal(GL_ARRAY_BUFFER, 0, (GLsizeiptr)size, cb,
                                     GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT |
                                         GL_MAP_COHERENT_BIT_EXT | GL_DYNAMIC_STORAGE_BIT_EXT);
            GLenum err = glGetError();
            if (err != GL_NO_ERROR) {
                record("T0-ahb-gl-import", "FAIL", fmt("glBufferStorageExternalEXT -> GL error 0x%04x", err));
            } else {
                void* m = glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr)size,
                                           GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT |
                                               GL_MAP_COHERENT_BIT_EXT);
                GLenum merr = glGetError();
                if (!m) {
                    record("T0-ahb-gl-import", "PARTIAL",
                           fmt("storage ok, glMapBufferRange returned NULL (GL error 0x%04x)", merr));
                } else {
                    int64_t cmp = checkRegion(m, REG_A, seedA);
                    writeRegion(m, REG_C, seedC);
                    writtenMask |= 2;
                    glUnmapBuffer(GL_ARRAY_BUFFER);
                    glFinish();
                    record("T0-ahb-gl-import", cmp == -1 ? "OK" : "PARTIAL",
                           fmt("persistent-coherent GL map of the client AHB, payload mismatch=%lld",
                               (long long)cmp));
                }
            }
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glDeleteBuffers(1, &b);
        }
    }

    // (d) ask the child to verify everything the parent wrote
    T0Verify ver{};
    ver.seedB = seedB;
    ver.seedC = seedC;
    ver.seedD = seedD;
    ver.writtenMask = writtenMask;
    std::string wbDetail;
    const char* wbStatus = "FAIL";
    if (!sendMsg(sock, MSG_T0_VERIFY, &ver, sizeof(ver), -1)) {
        wbDetail = fmt("sendMsg(verify) errno=%d", errno);
    } else {
        T0Result res{};
        if (!recvMsg(sock, &tag, &res, sizeof(res), &got, nullptr) || tag != MSG_T0_RESULT) {
            wbDetail = fmt("no verify reply errno=%d", errno);
        } else {
            bool anyChecked = false, allOk = true;
            auto acc = [&](int64_t v) {
                if (v == -2) return;
                anyChecked = true;
                if (v != -1) allOk = false;
            };
            acc(res.mismatchB);
            acc(res.mismatchC);
            acc(res.mismatchD);
            wbStatus = !anyChecked ? "SKIP" : (allOk ? "OK" : "FAIL");
            wbDetail = fmt("mask=0x%x vkWrite=%lld glWrite=%lld cpuWrite=%lld (lock=%d)", writtenMask,
                           (long long)res.mismatchB, (long long)res.mismatchC, (long long)res.mismatchD,
                           res.lockOk);
        }
    }
    record("T0-ahb-writeback-to-client", wbStatus, wbDetail);

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

static int childT3(int sock) {
    setRecvTimeout(sock, 30);
    T3Offer offer{};
    uint32_t tag = 0;
    size_t got = 0;
    int fd = -1;
    if (!recvMsg(sock, &tag, &offer, sizeof(offer), &got, &fd) || tag != MSG_T3_OFFER) return 2;
    T3Result res{};
    res.mismatch = -3;
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
        writeRegion(p, REG_B, offer.seedB);
        munmap(p, (size_t)offer.size);
    }
    sendMsg(sock, MSG_T3_RESULT, &res, sizeof(res), -1);
    close(fd);
    return 0;
}

static void runT3Parent(VkCtx& c, uint64_t size) {
    if (!c.hasExtMemHost || !c.pGetHostPtrProps) {
        record("T3-external-memory-host", "UNSUPPORTED", "VK_EXT_external_memory_host absent");
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
    // reserve an aligned window, then place the memfd inside it
    void* reserve = mmap(nullptr, (size_t)(mapSize + align), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reserve == MAP_FAILED) {
        record("T3-external-memory-host", "FAIL", fmt("reserve mmap errno=%d", errno));
        close(memfd);
        return;
    }
    uintptr_t base = ((uintptr_t)reserve + align - 1) & ~(uintptr_t)(align - 1);
    void* host = mmap((void*)base, (size_t)mapSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, memfd, 0);
    if (host == MAP_FAILED) {
        record("T3-external-memory-host", "FAIL", fmt("mmap(memfd, MAP_FIXED) errno=%d", errno));
        munmap(reserve, (size_t)(mapSize + align));
        close(memfd);
        return;
    }
    const uint32_t seedA = 0x33330001u, seedB = 0x33330002u;
    memset(host, 0, (size_t)mapSize);
    writeRegion(host, REG_A, seedA);

    VkMemoryHostPointerPropertiesEXT hp{};
    hp.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
    VkResult r = c.pGetHostPtrProps(c.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &hp);
    if (r != VK_SUCCESS) {
        record("T3-external-memory-host", "FAIL", fmt("vkGetMemoryHostPointerPropertiesEXT=%s align=%llu", vkStr(r),
                                                      (unsigned long long)align));
    } else {
        VkExternalMemoryBufferCreateInfo ext{};
        ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.pNext = &ext;
        bci.size = mapSize;
        bci.usage = kProbeBufferUsage;
        VkBuffer buf = VK_NULL_HANDLE;
        VkResult cr = vkCreateBuffer(c.device, &bci, nullptr, &buf);
        VkMemoryRequirements req{};
        if (cr == VK_SUCCESS) vkGetBufferMemoryRequirements(c.device, buf, &req);
        uint32_t bits = hp.memoryTypeBits & (cr == VK_SUCCESS ? req.memoryTypeBits : 0xFFFFFFFFu);
        int typeIdx = pickMemType(c.memProps, bits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (typeIdx < 0) typeIdx = pickMemType(c.memProps, bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        if (typeIdx < 0) {
            record("T3-external-memory-host", "FAIL",
                   fmt("no host-visible type in hostPtrBits=0x%x & reqBits=0x%x", hp.memoryTypeBits,
                       req.memoryTypeBits));
        } else {
            VkImportMemoryHostPointerInfoEXT imp{};
            imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
            imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            imp.pHostPointer = host;
            VkMemoryAllocateInfo mai{};
            mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.pNext = &imp;
            mai.allocationSize = mapSize;
            mai.memoryTypeIndex = (uint32_t)typeIdx;
            VkDeviceMemory mem = VK_NULL_HANDLE;
            VkResult ar = vkAllocateMemory(c.device, &mai, nullptr, &mem);
            if (ar != VK_SUCCESS) {
                record("T3-external-memory-host", "FAIL",
                       fmt("vkAllocateMemory(import host ptr)=%s type=%d bits=0x%x align=%llu", vkStr(ar), typeIdx,
                           bits, (unsigned long long)align));
            } else {
                VkResult br = (cr == VK_SUCCESS) ? vkBindBufferMemory(c.device, buf, mem, 0) : VK_SUCCESS;
                void* mapped = nullptr;
                VkResult mr = vkMapMemory(c.device, mem, 0, VK_WHOLE_SIZE, 0, &mapped);
                int64_t cmp = -3;
                if (mr == VK_SUCCESS && mapped) cmp = checkRegion(mapped, REG_A, seedA);
                if (mr == VK_SUCCESS) vkUnmapMemory(c.device, mem);
                record("T3-external-memory-host", (mr == VK_SUCCESS && cmp == -1) ? "OK" : "PARTIAL",
                       fmt("import ok (align=%llu type=%d bind=%s) vkMapMemory=%s mismatch=%lld",
                           (unsigned long long)align, typeIdx, vkStr(br), vkStr(mr), (long long)cmp));
                vkFreeMemory(c.device, mem, nullptr);
            }
        }
        if (cr == VK_SUCCESS) vkDestroyBuffer(c.device, buf, nullptr);
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
                record("T3-memfd-cross-process", (res.mmapOk && res.mismatch == -1 && back == -1) ? "OK" : "FAIL",
                       fmt("child mmap=%d errno=%d cmp=%lld writeback=%lld [%s]", res.mmapOk, res.mmapErrno,
                           (long long)res.mismatch, (long long)back, res.note));
            }
        }
        sendMsg(sock, MSG_BYE, nullptr, 0, -1);
        reapChild(pid);
        close(sock);
    }

    munmap(host, (size_t)mapSize);
    munmap(reserve, (size_t)(mapSize + align));
    close(memfd);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void printSummary() {
    char model[PROP_VALUE_MAX] = {0};
    getProp("ro.product.model", model, sizeof(model));
    printf("\n=== extmem_probe summary (model=%s) ===\n", model);
    printf("%-34s %-12s %s\n", "ROUTE", "STATUS", "DETAIL");
    for (const RouteResult& r : gResults)
        printf("%-34s %-12s %s\n", r.route.c_str(), r.status.c_str(), r.detail.c_str());
    printf("=== end ===\n");
    fflush(stdout);
}

int main(int argc, char** argv) {
    uint64_t size = kDefaultSize;
    const char* childRoute = nullptr;
    bool doT1 = true, doT0 = true, doT3 = true;
    for (int i = 1; i < argc; ++i) {
        if (!strncmp(argv[i], "--child=", 8)) {
            childRoute = argv[i] + 8;
        } else if (!strncmp(argv[i], "--size=", 7)) {
            size = strtoull(argv[i] + 7, nullptr, 0);
        } else if (!strcmp(argv[i], "--only-t1")) {
            doT0 = doT3 = false;
        } else if (!strcmp(argv[i], "--only-t0")) {
            doT1 = doT3 = false;
        } else if (!strcmp(argv[i], "--only-t3")) {
            doT1 = doT0 = false;
        } else if (!strcmp(argv[i], "--help")) {
            printf("usage: extmem_probe [--size=BYTES] [--only-t0|--only-t1|--only-t3]\n");
            return 0;
        }
    }
    if (size < 4 * kRegion) size = 4 * kRegion;

    // A peer that has already exited must not take this process down with it.
    signal(SIGPIPE, SIG_IGN);

    if (childRoute) {
        static char roleBuf[32];
        snprintf(roleBuf, sizeof(roleBuf), "child:%s", childRoute);
        gRole = roleBuf;
        int sock = 3;
        if (!strcmp(childRoute, "t1")) return childT1(sock);
        if (!strcmp(childRoute, "t0")) return childT0(sock);
        if (!strcmp(childRoute, "t3")) return childT3(sock);
        pr("unknown child route %s", childRoute);
        return 1;
    }

    pr("extmem_probe: MobileGL disaggregation spike B, size=%llu bytes", (unsigned long long)size);

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
    pr("=== phase T0: client-allocated AHardwareBuffer BLOB ===");
    if (doT0) runT0Parent(c, g, glOk, size);
    pr("=== phase T3: VK_EXT_external_memory_host ===");
    if (doT3) runT3Parent(c, size);

    glCtxDestroy(g);
    vkCtxDestroy(c);
    printSummary();
    return 0;
}
