#pragma once

#include "ITransport.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace MobileGL::MG_Remote::Transport {
    // The control reader remains runnable while the GL thread waits on data.
    // Log forwarding can therefore never fill a socket and block the applier.
    class ControlInbox {
    public:
        explicit ControlInbox(ITransport& transport);
        ~ControlInbox();
        void Stop();
        MobileGLResult Receive(std::vector<std::uint8_t>& frame, std::uint32_t timeoutMs);
        std::uint64_t Peek();
        bool WaitClosed(std::uint32_t timeoutMs);
        // TEST-ONLY (P9 W3, fault injection F2). While paused the reader takes NOTHING off the
        // socket - not a LogLine, not a reply - which is exactly a client that stopped reading its
        // control connection, the peer the server's log forward (LogForward.h) must not be held by.
        // Frames already queued stay queued; resuming picks up where the socket is.
        void PauseReadingForTest(bool paused);
    private:
        void Read();
        ITransport& m_transport;
        std::mutex m_mutex;
        std::condition_variable m_cv;
        std::deque<std::vector<std::uint8_t>> m_frames;
        std::uint64_t m_bytes = 0;
        bool m_stop = false;
        bool m_pausedForTest = false;
        MobileGLResult m_result = MOBILEGL_OK;
        std::thread m_reader;
    };
}
