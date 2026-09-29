// MobileGL - MobileGL/MG_Remote/Transport/PairBind.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P11 PAIR. The PairBind frame (protocol.fbs), both directions of it: SocketTransport::ConnectTo
// writes one as the first frame of each of a client's two connections, and Server/PairAcceptor
// reads them and pairs the two halves by the nonce. Here, not in Handshake.h beside the DataBind
// codec, because the transport itself writes it and Handshake.h brings the logger's umbrella
// header with it (WireLog.h says why Transport keeps that out).

#pragma once

#include "../Protocol/generated/protocol_generated.h"
#include "AuthToken.h" // kDataNonceBytes

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace MobileGL::MG_Remote::Transport {

    // 128 bits from the client's CSPRNG, the width of Welcome.dataNonce, so the nonce comparison
    // (ConstantTimeNonceMatch) is the same one.
    inline constexpr std::size_t kPairNonceBytes = kDataNonceBytes;

    inline std::vector<std::uint8_t> EncodePairBind(const std::uint8_t (&nonce)[kPairNonceBytes], bool aux) {
        ::flatbuffers::FlatBufferBuilder builder(64);
        auto bind = ::MobileGL::Wire::CreatePairBind(builder, builder.CreateVector(nonce, kPairNonceBytes), aux);
        auto envelope = ::MobileGL::Wire::CreateCtrlEnvelope(builder, ::MobileGL::Wire::CtrlMsg::PairBind,
                                                            bind.Union());
        ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, envelope);
        return std::vector<std::uint8_t>(builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize());
    }

    // True when `frame` is a verifiable CtrlEnvelope carrying a PairBind whose nonce is exactly
    // kPairNonceBytes; the nonce and the role are copied out. Anything else is false.
    inline bool DecodePairBind(const std::vector<std::uint8_t>& frame, std::uint8_t (&nonce)[kPairNonceBytes],
                               bool* outAux) {
        if (outAux == nullptr || frame.size() < 8 || !::MobileGL::Wire::CtrlEnvelopeBufferHasIdentifier(frame.data()))
            return false;
        ::flatbuffers::Verifier verifier(frame.data(), frame.size());
        if (!::MobileGL::Wire::VerifyCtrlEnvelopeBuffer(verifier)) return false;
        const auto* bind = ::MobileGL::Wire::GetCtrlEnvelope(frame.data())->msg_as_PairBind();
        if (bind == nullptr || bind->nonce() == nullptr || bind->nonce()->size() != kPairNonceBytes) return false;
        std::memcpy(nonce, bind->nonce()->data(), kPairNonceBytes);
        *outAux = bind->aux();
        return true;
    }

} // namespace MobileGL::MG_Remote::Transport
