// MobileGL - MobileGL/MG_State/GLState/ErrorState/Error.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Error.h"
#include <algorithm>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/MGToGL/ErrorCodeConverter.h>

namespace MobileGL::MG_State::GLState {
    void ErrorState::RecordError(ErrorCode code, UniquePtr<ErrorInfo> info) {
        if (code == ErrorCode::NoError) {
            MGLOG_E("Recording Non-OpenGL error:\n%s", info->toString().c_str());
            m_nonGLErrors.push_back(MakeUnique<Error>(code, Move(info)));
        } else {
            MGLOG_E("Recording OpenGL error (%s):\n%s",
                    MG_Util::ConvertGLEnumToString(MG_Util::ConvertErrorCodeToGLEnum(code)).c_str(),
                    info->toString().c_str());
            // GL error semantics are sticky flags, not a queue (GL 3.3 core §2.5): with multiple
            // error flags, each is set only while currently unset — repeated errors of the same
            // code are discarded until glGetError reads the flag. Unbounded accumulation leaked
            // stale errors into later, unrelated glGetError checks (GL CTS deinit noise).
            const Bool alreadyPending = std::any_of(m_errors.begin(), m_errors.end(),
                                                    [code](const auto& e) { return e->code == code; });
            if (!alreadyPending) {
                m_errors.push_back(MakeUnique<Error>(code, Move(info)));
            }
        }
    }

    Bool ErrorState::HasNonGLError() const {
        return !m_nonGLErrors.empty();
    }

    Optional<const Error*> ErrorState::PeekNonGLError() const {
        if (m_nonGLErrors.empty()) return Nullopt;
        return Optional<const Error*>{m_nonGLErrors.front().get()};
    }

    Optional<UniquePtr<Error>> ErrorState::PopNonGLError() {
        if (m_nonGLErrors.empty()) return Nullopt;
        auto error = Move(m_nonGLErrors.front());
        m_nonGLErrors.erase(m_nonGLErrors.begin());
        return Move(error);
    }

    Bool ErrorState::HasGLError() const {
        return !m_errors.empty();
    }

    Optional<const Error*> ErrorState::PeekGLError() const {
        if (m_errors.empty()) return Optional<const Error*>{};
        return Optional<const Error*>{m_errors.front().get()};
    }

    Optional<UniquePtr<Error>> ErrorState::PopGLError() {
        if (m_errors.empty()) return Nullopt;
        auto error = Move(m_errors.front());
        m_errors.erase(m_errors.begin());
        return Move(error);
    }

    void ErrorState::Clear() {
        m_errors.clear();
        m_nonGLErrors.clear();
    }
} // namespace MobileGL::MG_State::GLState
