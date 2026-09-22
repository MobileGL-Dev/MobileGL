// MobileGL - MobileGL/MG_IntegrationTest/Harness/TextureEmitPeek.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "TextureEmitPeek.h"

#if !defined(__ANDROID__)
#include <MG_Pipe/MGPipe.h>
#if MOBILEGL_PIPE_PUSH
#include <MG_Impl/Pipe/TextureEmit.h>
#define MGITEST_TEXTURE_EMIT_PEEK_LIVE 1
#endif
#endif

namespace MGITest {

#if defined(MGITEST_TEXTURE_EMIT_PEEK_LIVE)

    bool PeekTextureEmitCounters(TextureEmitCountersPeek* out) {
        if (out == nullptr) return false;
        // A SEPARATE TRANSLATION UNIT for PipeSlotPeek.h's reason, verbatim: the scenario sources
        // include the GL headers with prototypes and MobileGL's umbrella header is not meant to
        // meet them in one file.
        auto& emitter = MobileGL::MG_Pipe::MGPipeTextureEmitterInstance();
        out->SubDataCount = static_cast<unsigned long long>(emitter.SubDataCount());
        out->RefusedSubDataCount = static_cast<unsigned long long>(emitter.RefusedSubDataCount());
        out->CreateCount = static_cast<unsigned long long>(emitter.CreateCount());
        out->RespecifyCount = static_cast<unsigned long long>(emitter.RespecifyCount());
        out->DrainListSize = static_cast<unsigned long long>(emitter.DrainListSize());
        return true;
    }

    const char* TextureEmitPeekSkipReason() { return ""; }

#else

    bool PeekTextureEmitCounters(TextureEmitCountersPeek*) { return false; }

    const char* TextureEmitPeekSkipReason() {
#if defined(__ANDROID__)
        return "this module links the shipping libMobileGL.so on Android, built "
               "-fvisibility=hidden, so the client emitter's counters do not resolve";
#else
        return "this is a PULL build: MGPipeTextureEmitter is compiled only under "
               "MOBILEGL_PIPE_PUSH, so there is no emission cursor to read";
#endif
    }

#endif

} // namespace MGITest
