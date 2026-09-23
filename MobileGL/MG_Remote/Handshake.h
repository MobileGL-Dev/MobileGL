// P6.5 handshake policy, shared by both roles. Rejections are returned, never aborted.
#pragma once
#include "CapsCodec.h"
#include "Protocol/generated/protocol_generated.h"
#include "Transport/AuthToken.h"
#include "Transport/ITransport.h"
#include <MG_Util/Debug/Log.h>
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace MobileGL::MG_Remote {
    inline MobileGLResult RefuseHandshake(Transport::ITransport& transport,
                                          ::MobileGL::Wire::RefuseCode code,
                                          const char* detail, Uint64 expected = 0,
                                          Uint64 actual = 0, const char* peerValue = nullptr) {
        MGLOG_E("MGPipe: Refuse{%s} %s expected=%llu actual=%llu peer=%s",
                ::MobileGL::Wire::EnumNameRefuseCode(code), detail,
                static_cast<unsigned long long>(expected), static_cast<unsigned long long>(actual),
                peerValue == nullptr ? "" : peerValue);
        ::flatbuffers::FlatBufferBuilder builder(256);
        auto refusal = ::MobileGL::Wire::CreateRefuseDirect(builder, code, detail, expected, actual, peerValue);
        auto envelope = ::MobileGL::Wire::CreateCtrlEnvelope(builder, ::MobileGL::Wire::CtrlMsg::Refuse,
                                                            refusal.Union());
        ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, envelope);
        transport.SendFrame({builder.GetBufferPointer(), builder.GetSize()});
        return MOBILEGL_ERR_PROTOCOL_MISMATCH;
    }

    inline bool LogPeerRefusal(const ::MobileGL::Wire::CtrlEnvelope* envelope) {
        if (envelope == nullptr || envelope->msg_type() != ::MobileGL::Wire::CtrlMsg::Refuse) return false;
        const auto* refusal = envelope->msg_as_Refuse();
        if (refusal == nullptr) return false;
        MGLOG_E("MGPipe: peer Refuse{%s} %s expected=%llu actual=%llu",
                ::MobileGL::Wire::EnumNameRefuseCode(refusal->code()),
                refusal->detail() == nullptr ? "" : refusal->detail()->c_str(),
                static_cast<unsigned long long>(refusal->expected()),
                static_cast<unsigned long long>(refusal->actual()));
        return true;
    }

    // PH-7 (1)(2), ID-P7-3. THE TOKEN POLICY, ONCE, FOR BOTH SERVER-SIDE SITES.
    //
    // ServerSession::Accept and ServerMain::RunSession each had their own reading of it (see
    // Transport/AuthToken.h for what the three readings were). They now call this, so the answer
    // to "is this peer allowed to open a session" is one function with one comparison, and it is
    // the constant-time one.
    //
    // THE FOUR STATES, stated here because they were implicit and therefore different at each
    // site:
    //   in-process  - there is no peer; the "connection" is this process. Never authenticated.
    //   no token    - not an empty credential: the LISTEN policy has already confined this
    //                 server to loopback (SocketTransport.cpp's ListenTcp), so there is nothing
    //                 left for a token to add. A peer that sends one anyway is not refused for
    //                 it; ServerMain used to do that and no document ever said so.
    //   token, match    - welcomed.
    //   token, mismatch - `Refuse{Authentication}`, detail "token mismatch", and the value is
    //                 not in the line. A missing token field is a mismatch, not a separate case.
    inline MobileGLResult AuthenticatePeerToken(Transport::ITransport& transport,
                                                const ::flatbuffers::String* presented) {
        if (transport.Role() == Transport::TransportRole::InProcess) return MOBILEGL_OK;
        const char* expected = Transport::ConfiguredAuthToken();
        if (expected == nullptr) return MOBILEGL_OK;
        const char* bytes = presented == nullptr ? "" : presented->c_str();
        const std::size_t size = presented == nullptr ? 0u : presented->size();
        if (Transport::ConstantTimeTokenMatch(expected, bytes, size)) return MOBILEGL_OK;
        return RefuseHandshake(transport, ::MobileGL::Wire::RefuseCode::Authentication,
                               "token mismatch");
    }

    inline MobileGLResult ValidatePeerHandshake(Transport::ITransport& transport,
                                                Uint32 major, Uint32 minor, Uint64 fingerprint,
                                                const char* stamp, ::MobileGL::Wire::DialMode dial) {
        using ::MobileGL::Wire::RefuseCode;
        if (major != MOBILEGL_PROTOCOL_ABI_MAJOR || minor != MOBILEGL_PROTOCOL_ABI_MINOR)
            return RefuseHandshake(transport, RefuseCode::ProtocolVersion, "protocol version",
                MOBILEGL_ABI_VERSION(MOBILEGL_PROTOCOL_ABI_MAJOR, MOBILEGL_PROTOCOL_ABI_MINOR),
                MOBILEGL_ABI_VERSION(major, minor));
        if (fingerprint != WireFingerprint())
            return RefuseHandshake(transport, RefuseCode::WireFingerprint, "wire layout",
                                   WireFingerprint(), fingerprint);
        const bool sameBuild = BuildFingerprintPresent() && stamp != nullptr && stamp[0] != '\0' &&
                               std::strcmp(stamp, BuildFingerprint()) == 0;
        const char* required = std::getenv("MOBILEGL_IPC_REQUIRE_SAME_BUILD");
        const bool requireSame = dial == ::MobileGL::Wire::DialMode::Fork ||
                                 (required != nullptr && std::strcmp(required, "1") == 0);
        if (!sameBuild && requireSame)
            return RefuseHandshake(transport, RefuseCode::BuildFingerprint, "build identity", 0, 0,
                                   stamp == nullptr ? "<missing>" : stamp);
        if (!sameBuild && dial == ::MobileGL::Wire::DialMode::Connect)
            MGLOG_W("MGPipe: compatible wire with different build: local=%s peer=%s",
                    BuildFingerprint(), stamp == nullptr ? "<missing>" : stamp);
        return MOBILEGL_OK;
    }
} // namespace MobileGL::MG_Remote
