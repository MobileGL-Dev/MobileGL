// MobileGL - MobileGL/MG_Remote/Transport/AuthToken.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The one definition of what MOBILEGL_IPC_TOKEN means; listeners and handshake checks use these
// helpers (ARCHITECTURE.md §15.1). No token confines the listener to loopback. A configured
// token shorter than kMinimumAuthTokenBytes fails the listen rather than falling back to
// loopback, since an operator who set a token could not tell that fallback from success.
// Comparisons are constant time: a byte-at-a-time compare lets a peer that can retry learn the
// token one byte at a time. The token is never logged; refusals name the policy, not the secret.

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

    // The comparison's width. Up to this many bytes the loop's trip count is a constant; a
    // configured token longer than it is compared in full (the loop extends to its length).
    inline constexpr std::size_t kAuthTokenCompareBytes = 64;

    // Constant time in the CONTENTS of both operands, and in their LENGTHS up to
    // kAuthTokenCompareBytes.
    //
    // The first shape of this loop ran over the configured token's length. That length is not the
    // peer's to vary, but it is part of the operator's secret, and an answer whose time was
    // proportional to it told a peer how long a token to guess (F fix round, review of slices
    // 1-3). The loop now runs a fixed 64 iterations - or the configured token's length past that,
    // so a longer token is still compared to its last byte and reveals only that it is longer than
    // 64 - and reads BOTH operands as zero past their own ends: a zero is a value, not a
    // short-circuit, and neither operand is ever indexed out of its own bounds. The two lengths
    // are folded into the accumulator rather than branched on, and nothing leaves early. The
    // presented length does not extend the loop: a peer's own bytes are the one thing it already
    // knows, and letting it choose the trip count would let it choose our work.
    inline bool ConstantTimeTokenMatch(const char* expected, const char* presented,
                                       std::size_t presentedSize) {
        if (expected == nullptr) return false;
        const std::size_t expectedSize = std::strlen(expected);
        const std::size_t width = expectedSize > kAuthTokenCompareBytes ? expectedSize : kAuthTokenCompareBytes;
        std::size_t difference = expectedSize ^ presentedSize;
        for (std::size_t index = 0; index < width; ++index) {
            const unsigned char ours =
                index < expectedSize ? static_cast<unsigned char>(expected[index]) : 0u;
            const unsigned char theirs =
                index < presentedSize ? static_cast<unsigned char>(presented[index]) : 0u;
            difference |= static_cast<unsigned char>(ours ^ theirs);
        }
        return difference == 0;
    }

    // PH-7 (4), ID-P7-3. The width of `Welcome.dataNonce`: 128 bits from the CSPRNG, the same
    // floor as the token for the same reason.
    inline constexpr std::size_t kDataNonceBytes = 16;

    // The nonce comparison. Both operands are exactly kDataNonceBytes by the time this is asked
    // (a presented nonce of any other length is refused before it gets here), so the loop has
    // one length and no early exit.
    inline bool ConstantTimeNonceMatch(const unsigned char* expected, const unsigned char* presented) {
        if (expected == nullptr || presented == nullptr) return false;
        unsigned difference = 0;
        for (std::size_t index = 0; index < kDataNonceBytes; ++index)
            difference |= static_cast<unsigned>(expected[index] ^ presented[index]);
        return difference == 0;
    }

} // namespace MobileGL::MG_Remote::Transport
