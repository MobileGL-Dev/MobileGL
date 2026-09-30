// MobileGL - MobileGL/MG_Backend/HostFrameBridge.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "HostFrameBridge.h"

#if MOBILEGL_BUILD_DISAGGREGATED && defined(__ANDROID__)

#include <android/hardware_buffer.h>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace MobileGL::MG_Backend {
    namespace {
        Bool ReadExactly(Int fd, void* out, SizeT n) {
            Uint8* at = static_cast<Uint8*>(out);
            while (n > 0) {
                const ssize_t got = ::read(fd, at, n);
                if (got <= 0) {
                    if (got < 0 && errno == EINTR) continue;
                    return false;
                }
                at += got;
                n -= static_cast<SizeT>(got);
            }
            return true;
        }

        Bool WriteExactly(Int fd, const void* in, SizeT n) {
            const Uint8* at = static_cast<const Uint8*>(in);
            while (n > 0) {
                const ssize_t put = ::write(fd, at, n);
                if (put <= 0) {
                    if (put < 0 && errno == EINTR) continue;
                    return false;
                }
                at += put;
                n -= static_cast<SizeT>(put);
            }
            return true;
        }
    }

    HostFrameBridge::~HostFrameBridge() { Close(); }

    Bool HostFrameBridge::Open(const char* path, Int acceptTimeoutMs, String& why) {
        Close();
        const char* name = (path != nullptr && path[0] != 0) ? path : kHostFrameSocket;

        m_listen = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (m_listen < 0) {
            why = std::format("socket(AF_UNIX): {}", std::strerror(errno));
            return false;
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (std::strlen(name) >= sizeof(address.sun_path)) {
            why = std::format("the socket name is longer than the kernel takes: {}", name);
            Close();
            return false;
        }
        // "@name" is the abstract namespace: sun_path[0] is NUL and the name follows it.
        // Nothing is created on disk, so nothing is left behind and no file permission stands
        // between two apps that may not write each other's directories.
        if (name[0] == '@') {
            address.sun_path[0] = 0;
            std::strncpy(address.sun_path + 1, name + 1, sizeof(address.sun_path) - 2);
        } else {
            std::strncpy(address.sun_path, name, sizeof(address.sun_path) - 1);
            // A leftover socket file from a server that died is not a reason to refuse the host.
            ::unlink(name);
        }
        if (::bind(m_listen, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            why = std::format("bind({}): {}", name, std::strerror(errno));
            Close();
            return false;
        }
        if (::listen(m_listen, 1) != 0) {
            why = std::format("listen({}): {}", name, std::strerror(errno));
            Close();
            return false;
        }

        pollfd waiting{};
        waiting.fd = m_listen;
        waiting.events = POLLIN;
        const Int ready = ::poll(&waiting, 1, acceptTimeoutMs);
        if (ready <= 0) {
            why = (ready == 0) ? std::format("no display host dialled {} within {} ms", name, acceptTimeoutMs)
                               : std::format("poll({}): {}", name, std::strerror(errno));
            Close();
            return false;
        }
        m_fd = ::accept(m_listen, nullptr, nullptr);
        if (m_fd < 0) {
            why = std::format("accept({}): {}", name, std::strerror(errno));
            Close();
            return false;
        }
        MGLOG_I("host frame bridge: a display host is on %s", name);
        return true;
    }

    Bool HostFrameBridge::Acquire(struct AHardwareBuffer** buffer, HostFrameOffer& offer, String& why) {
        *buffer = nullptr;
        offer = HostFrameOffer{};
        if (m_fd < 0) {
            why = "the bridge is not open";
            return false;
        }
        AHardwareBuffer* received = nullptr;
        if (AHardwareBuffer_recvHandleFromUnixSocket(m_fd, &received) != 0 || received == nullptr) {
            why = "the host stopped offering frames, or the handle did not cross";
            return false;
        }
        if (!ReadExactly(m_fd, &offer, sizeof(offer)) || offer.Magic != kHostFrameMagic) {
            // The handle is already this side's by the time this fails, so it goes back before
            // the failure does: a reference held past the connection starves the host's pool.
            AHardwareBuffer_release(received);
            why = std::format("the frame's description did not follow its handle (magic 0x{:08x})", offer.Magic);
            return false;
        }
        *buffer = received;
        return true;
    }

    Bool HostFrameBridge::Complete(const HostFrameOffer& offer, Bool imported, Bool drawn, Bool fenceOk, String& why) {
        if (m_fd < 0) {
            why = "the bridge is not open";
            return false;
        }
        HostFrameAck ack{};
        ack.Magic = kHostFrameMagic;
        ack.Version = kHostFrameVersion;
        ack.Index = offer.Index;
        ack.Imported = imported ? 1u : 0u;
        ack.Drawn = drawn ? 1u : 0u;
        ack.FenceOk = fenceOk ? 1u : 0u;
        if (!WriteExactly(m_fd, &ack, sizeof(ack))) {
            why = std::format("the answer to frame {} did not go out: {}", offer.Index, std::strerror(errno));
            return false;
        }
        // The host answers with its own view of the pixels.  It is what closes the round trip, so
        // it is read here and kept rather than left in the socket for the next frame to trip over.
        m_lastSeen = HostFrameSeen{};
        if (!ReadExactly(m_fd, &m_lastSeen, sizeof(m_lastSeen)) || m_lastSeen.Magic != kHostFrameMagic) {
            why = std::format("frame {} was answered but the host's readback did not arrive", offer.Index);
            m_lastSeen = HostFrameSeen{};
            return false;
        }
        return true;
    }

    void HostFrameBridge::Close() {
        if (m_fd >= 0) ::close(m_fd);
        if (m_listen >= 0) ::close(m_listen);
        m_fd = -1;
        m_listen = -1;
    }
}

#else  // !(MOBILEGL_BUILD_DISAGGREGATED && __ANDROID__)

namespace MobileGL::MG_Backend {
    HostFrameBridge::~HostFrameBridge() = default;
    Bool HostFrameBridge::Open(const char*, Int, String& why) {
        why = "host frames are a disaggregated Android path";
        return false;
    }
    Bool HostFrameBridge::Acquire(struct AHardwareBuffer**, HostFrameOffer&, String& why) {
        why = "host frames are a disaggregated Android path";
        return false;
    }
    Bool HostFrameBridge::Complete(const HostFrameOffer&, Bool, Bool, Bool, String& why) {
        why = "host frames are a disaggregated Android path";
        return false;
    }
    void HostFrameBridge::Close() {}
}

#endif
