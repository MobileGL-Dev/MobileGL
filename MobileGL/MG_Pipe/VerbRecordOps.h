// MobileGL - MobileGL/MG_Pipe/VerbRecordOps.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include <array>

#include <MG_Pipe/MGPipe.h>

// THE VERB -> RECORD TABLE (P15). Every MGPipeVerb names the record op its emitter publishes, and
// the client's barrier decision at the validate point is that op's wait class (PipeCalls.def) and
// nothing else. Commands are recorded and replayed asynchronously by default; a verb syncs only
// because the record it emits has a wait class that says so.
//
// IT IS THE FORWARD DIRECTION. MGP_VERB_OP_LIST (FieldOwnership.def) is op -> verb, the
// direction the SERVER needs to stamp a verb boundary, and it is many-to-one (one draw_vbo row
// for twenty draw verbs, one blit row for two blit verbs). Inverting it verb-first answered
// "no row" for every verb but the representative, and the old fallback turned "no row" into
// "barriered" - a forced applier quiesce per verb (FCL: one per frame from the blit, 1.65 ms of
// GL-thread wait). Here there is no fallback in either direction: a verb with no row, or with two,
// is a compile error (the static_assert below).
//
// A verb whose emitter can publish more than one op names the one with the STRONGEST wait class
// (the fill decision is made before the record exists, so it must cover the worst record the verb
// may emit). kMGPipeNoRecord names a verb that publishes nothing: the client answers it from the
// caps mirror, so no apply can read a fill for it.
//
// X(Verb, Op)
// clang-format off
#define MGP_VERB_RECORD_OP_LIST(X)                                                     \
    /* The draw family: every draw verb is one draw_vbo record. */                     \
    X(DrawArrays,                                  DrawVbo)                            \
    X(DrawElements,                                DrawVbo)                            \
    X(DrawElementsBaseVertex,                      DrawVbo)                            \
    X(MultiDrawArrays,                             DrawVbo)                            \
    X(MultiDrawElements,                           DrawVbo)                            \
    X(MultiDrawElementsBaseVertex,                 DrawVbo)                            \
    X(MultiDrawElementsIndirect,                   DrawVbo)                            \
    X(MultiDrawArraysIndirect,                     DrawVbo)                            \
    X(MultiDrawElementsIndirectCount,              DrawVbo)                            \
    X(MultiDrawArraysIndirectCount,                DrawVbo)                            \
    X(DrawRangeElementsBaseVertex,                 DrawVbo)                            \
    X(DrawRangeElements,                           DrawVbo)                            \
    X(DrawElementsInstancedBaseVertexBaseInstance, DrawVbo)                            \
    X(DrawElementsInstancedBaseVertex,             DrawVbo)                            \
    X(DrawElementsInstancedBaseInstance,           DrawVbo)                            \
    X(DrawElementsInstanced,                       DrawVbo)                            \
    X(DrawArraysInstancedBaseInstance,             DrawVbo)                            \
    X(DrawArraysInstanced,                         DrawVbo)                            \
    X(DrawElementsIndirect,                        DrawVbo)                            \
    X(DrawArraysIndirect,                          DrawVbo)                            \
    /* Every clear entry point is one clear record (EmitClear / EmitF1Clear). */       \
    X(Clear,                                       Clear)                              \
    X(ClearBufferfi,                               Clear)                              \
    X(ClearBufferfv,                               Clear)                              \
    X(ClearBufferuiv,                              Clear)                              \
    X(ClearBufferiv,                               Clear)                              \
    X(ClearNamedFramebufferfv,                     Clear)                              \
    X(ClearNamedFramebufferfi,                     Clear)                              \
    X(ClearNamedFramebufferiv,                     Clear)                              \
    X(ClearNamedFramebufferuiv,                    Clear)                              \
    X(BlitFramebuffer,                             Blit)                               \
    X(BlitNamedFramebuffer,                        Blit)                               \
    X(CopyTexImage2D,                              CopyFramebufferToTexture)           \
    X(CopyTexSubImage2D,                           CopyFramebufferToTexture)           \
    X(CopyImageSubData,                            ResourceCopyRegion)                 \
    X(GenerateMipmap,                              GenerateMipmap)                     \
    /* read_pixels (reply) or read_pixels_to_buffer (pack buffer): the reply wins. */  \
    X(ReadPixels,                                  ReadPixels)                         \
    /* get_texture_image (reply) or its pack-buffer half: the reply wins. */           \
    X(GetTexImage,                                 GetTextureImage)                    \
    X(GetTextureImage,                             GetTextureImage)                    \
    X(DispatchCompute,                             LaunchGrid)                         \
    X(DispatchComputeIndirect,                     LaunchGrid)                         \
    X(MemoryBarrier,                               MemoryBarrier)                      \
    X(MemoryBarrierByRegion,                       MemoryBarrier)                      \
    X(BindImageTexture,                            BindShaderImage)                    \
    X(GetIntegeri_v,                               kMGPipeNoRecord)                    \
    X(ShaderStorageBlockBinding,                   SetStorageBlockBinding)             \
    X(FenceSync,                                   FenceCreate)                        \
    X(ClientWaitSync,                              FenceWait)                          \
    X(WaitSync,                                    FenceWaitServer)                    \
    X(DeleteSync,                                  FenceDestroy)                       \
    X(GetSyncStatus,                               FenceStatus)                        \
    X(IsTimerQuerySupported,                       kMGPipeNoRecord)                    \
    /* Query begins publish query_create then query_begin / query_counter. */          \
    X(BeginTimeElapsedQuery,                       QueryBegin)                         \
    X(EndTimeElapsedQuery,                         QueryEnd)                           \
    X(QueryCounterTimestamp,                       QueryCounter)                       \
    X(IsQueryResultAvailable,                      QueryAvailable)                     \
    X(GetQueryResult64,                            QueryResult)                        \
    X(DeleteBackendQuery,                          QueryDestroy)                       \
    X(BeginOcclusionQuery,                         QueryBegin)                         \
    X(EndOcclusionQuery,                           QueryEnd)                           \
    X(BeginXfbPrimitivesQuery,                     QueryBegin)                         \
    X(EndXfbPrimitivesQuery,                       QueryEnd)                           \
    X(PatchParameteri,                             PatchParameter)                     \
    X(BeginTransformFeedback,                      BeginStreamOutput)                  \
    X(EndTransformFeedback,                        EndStreamOutput)                    \
    X(PauseTransformFeedback,                      PauseStreamOutput)                  \
    X(ResumeTransformFeedback,                     ResumeStreamOutput)                 \
    X(BindTransformFeedback,                       BindStreamOutput)                   \
    /* delete_stream_output, preceded by a bind_stream_output when it was bound. */    \
    X(DeleteTransformFeedback,                     BindStreamOutput)                   \
    X(GetGpuTimestampNs,                           QueryTimestamp)
// clang-format on

namespace MobileGL::MG_Pipe {
    // The op a verb names when it publishes no record at all.
    inline constexpr MGPWireOp kMGPipeNoRecord = MGPWireOp::kOpCount;

    struct MGPipeVerbRecordOpRow {
        MGPipeVerb Verb;
        MGPWireOp Op;
    };

    namespace VerbRecordOpsDetail {
        constexpr auto MakeRows() {
            using enum MGPWireOp;
            return std::array{
#define MGP_VERB_RECORD_OP_ROW(Verb, Op) MGPipeVerbRecordOpRow{MGPipeVerb::Verb, Op},
                MGP_VERB_RECORD_OP_LIST(MGP_VERB_RECORD_OP_ROW)
#undef MGP_VERB_RECORD_OP_ROW
            };
        }
        inline constexpr auto kRows = MakeRows();

        // Every verb exactly once: a verb with no row (or two) is a compile error, never a
        // runtime default in either direction.
        constexpr Bool EveryVerbHasExactlyOneRow() {
            for (SizeT verb = 0; verb < static_cast<SizeT>(MGPipeVerb::kVerbCount); ++verb) {
                SizeT rows = 0;
                for (const auto& row : kRows) {
                    if (static_cast<SizeT>(row.Verb) == verb) ++rows;
                }
                if (rows != 1) return false;
            }
            return true;
        }
        static_assert(kRows.size() == static_cast<SizeT>(MGPipeVerb::kVerbCount),
                      "MGP_VERB_RECORD_OP_LIST must have exactly one row per MGPipeVerb");
        static_assert(EveryVerbHasExactlyOneRow(),
                      "an MGPipeVerb has no row (or two) in MGP_VERB_RECORD_OP_LIST: name the record op "
                      "its emitter publishes");

        constexpr auto MakeTable() {
            Array<MGPWireOp, static_cast<SizeT>(MGPipeVerb::kVerbCount)> table{};
            for (const auto& row : kRows) table[static_cast<SizeT>(row.Verb)] = row.Op;
            return table;
        }
        inline constexpr auto kTable = MakeTable();
    } // namespace VerbRecordOpsDetail

    // The record op `verb` publishes (kMGPipeNoRecord: none).
    constexpr MGPWireOp MGPipeRecordOpForVerb(MGPipeVerb verb) {
        return VerbRecordOpsDetail::kTable[static_cast<SizeT>(verb)];
    }
} // namespace MobileGL::MG_Pipe
