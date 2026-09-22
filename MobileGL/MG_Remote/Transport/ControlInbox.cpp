#include "ControlInbox.h"
#include "Doorbell.h"
#include "../Protocol/SurfaceOpCodec.h"
#include <MG_Util/Debug/Log.h>
#include <chrono>

namespace MobileGL::MG_Remote::Transport {
    ControlInbox::ControlInbox(ITransport& transport) : m_transport(transport), m_reader([this] { Read(); }) {}
    ControlInbox::~ControlInbox() { Stop(); }

    void ControlInbox::Stop() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
            m_cv.notify_all();
        }
        // Receive uses a short bounded wait, so no shutdown of the data plane is needed.
        if (m_reader.joinable()) m_reader.join();
    }

    void ControlInbox::Read() {
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stop) return;
            }
            std::uint64_t size = 0;
            auto result = m_transport.ReceiveFrame({nullptr, 0}, &size, 100);
            if (result == MOBILEGL_ERR_TIMEOUT) continue;
            std::vector<std::uint8_t> frame;
            if (result == MOBILEGL_ERR_BUFFER_TOO_SMALL && size <= 64ull * 1024 * 1024) {
                frame.resize(static_cast<std::size_t>(size));
                result = m_transport.ReceiveFrame({frame.data(), frame.size()}, &size, 0);
            }
            if (result == MOBILEGL_OK) {
                flatbuffers::Verifier verifier(frame.data(), frame.size());
                if (!::MobileGL::Wire::VerifyCtrlEnvelopeBuffer(verifier)) result = MOBILEGL_ERR_PROTOCOL_MISMATCH;
                else {
                    const auto* envelope = ::MobileGL::Wire::GetCtrlEnvelope(frame.data());
                    if (const auto* log = envelope->msg_as_LogLine()) {
                        if (log->text()) MG_Util::Debug::WritePeerLog(log->text()->c_str());
                        continue;
                    }
                }
            }
            std::unique_lock<std::mutex> lock(m_mutex);
            if (result != MOBILEGL_OK) {
                m_result = result;
                m_cv.notify_all();
                return;
            }
            m_cv.wait(lock, [&] { return m_stop || (m_frames.size() < 256 && m_bytes < 64ull * 1024 * 1024); });
            if (m_stop) return;
            m_bytes += frame.size();
            m_frames.push_back(std::move(frame));
            m_cv.notify_all();
        }
    }

    MobileGLResult ControlInbox::Receive(std::vector<std::uint8_t>& frame, std::uint32_t timeoutMs) {
        std::unique_lock<std::mutex> lock(m_mutex);
        const auto ready = [&] { return m_stop || !m_frames.empty() || m_result != MOBILEGL_OK; };
        if (timeoutMs == kWaitForever) m_cv.wait(lock, ready);
        else if (!m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready)) return MOBILEGL_ERR_TIMEOUT;
        if (m_frames.empty()) return m_result == MOBILEGL_OK ? MOBILEGL_ERR_TRANSPORT_CLOSED : m_result;
        frame = std::move(m_frames.front());
        m_frames.pop_front();
        m_bytes -= frame.size();
        m_cv.notify_all();
        return MOBILEGL_OK;
    }

    std::uint64_t ControlInbox::Peek() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_frames.empty() ? 0 : m_frames.front().size();
    }
}
