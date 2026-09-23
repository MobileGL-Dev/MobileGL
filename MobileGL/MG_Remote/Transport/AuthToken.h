// MobileGL - MobileGL/MG_Remote/Transport/AuthToken.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// PH-7 (1)(2)(3), ID-P7-3. THE ONE PLACE THAT KNOWS WHAT `MOBILEGL_IPC_TOKEN` MEANS.
//
// It was three places, and the three did not agree:
//
//   SocketTransport.cpp's ListenTcp asked only "is it non-empty", and a one-byte token
//     therefore unlocked a `tcp://0.0.0.0` listen.
//   ServerSession.cpp compared with `std::strcmp` under "the role is not InProcess AND a token
//     is configured".
//   ServerMain.cpp compared with `std::string::operator==` under "the control connection is
//     TCP", which additionally demanded that a peer present an EMPTY token to a server that had
//     configured none - a rule the other site did not have and no document states.
//
// Two of the three compared byte-at-a-time and returned at the first differing byte. That is the
// textbook remote timing oracle: an attacker who can retry learns the token one byte at a time
// instead of guessing 2^128. Whether it is exploitable over a LAN at these timings is beside the
// point; the comparison costs the same either way, and a security property nobody has to
// re-derive is worth more than a measurement.
//
// WHY A MINIMUM LENGTH IS A LISTEN-TIME REFUSAL AND NOT A WARNING. Ph's threat model (ID-P7-3)
// is "anybody who can reach the port has a remote code path". The answer to it is the token, so
// a token an attacker can enumerate is not an answer - it is the appearance of one, which is
// worse than no token at all, because `no token` is already handled: it confines the listener to
// loopback. A four-byte token, before this file, did not confine anything. 16 bytes is the
// floor, it is checked where the listener is opened, and a short token FAILS THE LISTEN rather
// than silently degrading to loopback-only: an operator who set a token and got a loopback
// server would have no way to tell that from success, which is the exact silence Ph exists to
// remove.
//
// The token is never logged, at any level, on any path. The refusals name the policy, not the
// secret (CONTRACT-P65: "错令牌返回 Refuse，不打印令牌值").

#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace MobileGL::MG_Remote::Transport {

    // 128 bits of secret, in the encoding an environment variable can carry. The same width as
    // `Welcome.dataNonce` (PH-7 (4)) for the same reason: below it, guessing is cheaper than
    // attacking anything else in this design.
    inline constexpr std::size_t kMinimumAuthTokenBytes = 16;

    // The configured token, or nullptr when none is configured. An EMPTY value is "none": every
    // TCP lane in this tree sets `MOBILEGL_IPC_TOKEN=` precisely to say so (see
    // MG_IntegrationTest/CMakeLists.txt's three arm environments), and an empty string is not a
    // secret under any reading.
    inline const char* ConfiguredAuthToken() {
        const char* value = std::getenv("MOBILEGL_IPC_TOKEN");
        return value != nullptr && value[0] != '\0' ? value : nullptr;
    }

    // Long enough to be a credential. Asked only of a CONFIGURED token; "none" is a separate
    // state with its own policy (loopback-only), not a zero-length token.
    inline bool AuthTokenIsLongEnough(const char* token) {
        return token != nullptr && std::strlen(token) >= kMinimumAuthTokenBytes;
    }

    // Constant time in the CONTENTS of both operands.
    //
    // The loop runs over the expected token's length and never leaves early, so the time depends
    // on that length - which is this process's own configuration, fixed before any peer connects
    // and not attacker-controlled - and on nothing the peer sent. The two lengths are folded into
    // the accumulator rather than branched on, so a peer cannot learn the token's length from a
    // faster answer either. Past the presented token's end the comparison reads a zero, which is
    // a value, not a short-circuit: `presented` is never indexed out of its own bounds.
    inline bool ConstantTimeTokenMatch(const char* expected, const char* presented,
                                       std::size_t presentedSize) {
        if (expected == nullptr) return false;
        const std::size_t expectedSize = std::strlen(expected);
        std::size_t difference = expectedSize ^ presentedSize;
        for (std::size_t index = 0; index < expectedSize; ++index) {
            const unsigned char theirs =
                index < presentedSize ? static_cast<unsigned char>(presented[index]) : 0u;
            difference |= static_cast<unsigned char>(
                static_cast<unsigned char>(expected[index]) ^ theirs);
        }
        return difference == 0;
    }

} // namespace MobileGL::MG_Remote::Transport
