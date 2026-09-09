// MobileGL - MobileGL/MG_IntegrationTest/Harness/P4aFinalFixPeek.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "P4aFinalFixPeek.h"

#if !defined(__ANDROID__)
#include <MG_Pipe/MGPipe.h>
#if MOBILEGL_PIPE_PUSH
#include <MG_Pipe/MGPipeTypes.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Util/Metrics/PipeStats.h>
#define MGITEST_P4A_FINALFIX_PEEK_LIVE 1
#endif
#endif

namespace MGITest {

#if defined(MGITEST_P4A_FINALFIX_PEEK_LIVE)
    namespace {
        namespace MGP = MobileGL::MG_Pipe;
    } // namespace

    bool PeekPipeTextureResourceRecord(unsigned glTextureName, PipeTextureResourceRecordPeek* out) {
        if (out == nullptr) return false;
        const MGP::MGPipeApplierState& applier = MGP::MGPipeApplier();
        // Slot 0 is the reserved null slot; the walk is the same shape PipeApplyPeek.cpp's
        // params reading takes. A GL name is never an identity on the wire, which is exactly
        // why it is the right key for a harness that starts from the application's view.
        for (MobileGL::SizeT slot = 1; slot < applier.TextureResources.size(); ++slot) {
            const MGP::MGPipeResourceRecord& record = applier.TextureResources[slot];
            if (!record.Live) continue;
            if (record.Desc.GlNameForDiag != static_cast<MobileGL::Uint32>(glTextureName)) continue;
            out->Slot = static_cast<unsigned>(slot);
            out->Gen = static_cast<unsigned>(record.Gen);
            out->Serial = static_cast<unsigned long long>(record.Serial);
            out->BindMask = static_cast<unsigned>(record.Desc.BindMask);
            out->ImageBindableHint = static_cast<unsigned>(record.Desc.ImageBindableHint);
            out->Levels = static_cast<unsigned>(record.Desc.Levels);
            out->PendingUploads = static_cast<unsigned>(record.PendingUploads.size());
            return true;
        }
        return false;
    }

    bool PeekPipeStatsTextureRemintPulls(unsigned long long* out) {
        if (out == nullptr) return false;
        namespace Stats = MobileGL::MG_Util::PipeStats;
        if (!Stats::Enabled()) Stats::SetEnabledForTesting(true);
        *out = static_cast<unsigned long long>(Stats::TotalCalls(Stats::CallClass::TextureRemintPulls));
        return true;
    }

    bool PeekPipeStatsTextureUploadEmissions(unsigned long long* out) {
        if (out == nullptr) return false;
        namespace Stats = MobileGL::MG_Util::PipeStats;
        if (!Stats::Enabled()) Stats::SetEnabledForTesting(true);
        *out = static_cast<unsigned long long>(Stats::TotalCalls(Stats::CallClass::TextureUploadEmissions));
        return true;
    }
#else
    bool PeekPipeTextureResourceRecord(unsigned, PipeTextureResourceRecordPeek*) { return false; }
    bool PeekPipeStatsTextureRemintPulls(unsigned long long*) { return false; }
    bool PeekPipeStatsTextureUploadEmissions(unsigned long long*) { return false; }
#endif

} // namespace MGITest
