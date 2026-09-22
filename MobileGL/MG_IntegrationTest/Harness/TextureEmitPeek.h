// MobileGL - MobileGL/MG_IntegrationTest/Harness/TextureEmitPeek.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// THE CLIENT TEXTURE EMITTER'S OWN COUNTERS AND ITS DRAIN LIST, read from a scenario.
//
// WHY A WHITE-BOX READING EXISTS HERE AT ALL, and it is the same argument PipeApplyPeek.h makes
// for G9. The owner-keyed drain cursor (TextureEmit.h's "the drain list (D-D4)") is a statement
// about WHICH emission cursor an upload through a texture VIEW lands on. Its public-GL
// observable - the texels a later sample returns - is repaired by every other arm of the
// system: a level that never made it onto the drain list is still uploaded by the NEXT whole-
// level respecify, by a GenerateMipmap, or by the monolith sync path, and the picture comes out
// right one frame later than it should have. A scenario that only looked at pixels would
// therefore go green over a two-cursor regression on most trees and red on none of them
// reliably. The counters are where the claim actually lives:
//
//   SubDataCount()   how many resource_subdata records the drain put on the wire. A drain that
//                    ran and emitted NOTHING is the silent failure this peek exists to make
//                    loud - it looks exactly like a healthy drain from outside.
//   DrainListSize()  what is still owed after the drain. Non-zero after a validate point means
//                    a level bailed (no storage, no shadow, an empty box) and stayed on the
//                    list, which is the safe direction but not the expected one for a plain
//                    glTexSubImage of a texture with storage.
//
// EVERY ENTRY POINT RETURNS false, TOUCHING NOTHING, WHERE IT CANNOT LOOK, and a caller that
// gets false has learned NOTHING. Out of reach means: a PULL build (the emitter is
// `#if MOBILEGL_PIPE_PUSH` and does not exist there); and Android, where this module links the
// shipping libMobileGL.so built -fvisibility=hidden and no internal symbol resolves. It is NOT
// backend-dependent: the emitter is the CLIENT's, and the client is this process under every
// one of inproc / spawn / tcp as well as under monolith.

#pragma once

namespace MGITest {

    // MGPipeTextureEmitterInstance()'s cumulative counters. They are process-global and the
    // library never resets them, so every caller compares a DELTA across its own window rather
    // than an absolute - the same rule the PipeStats window readers follow.
    struct TextureEmitCountersPeek {
        // resource_subdata records the drain emitted (m_subDatas).
        unsigned long long SubDataCount;
        // ...of which the applier REFUSED (m_refusedSubDatas). A refusal leaves the level dirty
        // and on the list, so a case that expected an emission and sees a refusal instead has a
        // different finding from one that sees no emission at all.
        unsigned long long RefusedSubDataCount;
        // resource_create / resource_respecify, for a message that can say whether the texture
        // was even described to the applier before the upload was due.
        unsigned long long CreateCount;
        unsigned long long RespecifyCount;
        // What is STILL owed: m_drain.size() right now. Read after the validate point, this is
        // the "did every dirty level get emitted" answer.
        unsigned long long DrainListSize;
    };

    bool PeekTextureEmitCounters(TextureEmitCountersPeek* out);

    // Why the peek could not look, for a GTEST_SKIP message. Empty when it can.
    const char* TextureEmitPeekSkipReason();

} // namespace MGITest
