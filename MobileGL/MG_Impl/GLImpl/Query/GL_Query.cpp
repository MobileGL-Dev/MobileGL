// MobileGL - MobileGL/MG_Impl/GLImpl/Query/GL_Query.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Query.h"
#include "../Getter/GL_Getter.h"
#include <Config.h>
#include <MG_Backend/BackendObjects.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/ErrorState/ErrorInfo.h>

namespace MobileGL::MG_Impl::GLImpl {
    namespace {
        // Frontend query object (GL_ARB_timer_query): wraps an optional backend
        // timer-query handle. A null backend handle (backend has no timer-query
        // support, timer queries are disabled by config, or the backend could
        // not create a query at call time) keeps a graceful fallback: the query
        // result is immediately available and reads as zero.
        struct QueryObject {
            GLuint id = 0;
            GLenum target = 0; // 0 = gen'd but never used with BeginQuery/QueryCounter
            // glCreateQueries makes the object outright; glGenQueries only reserves the name,
            // and the object appears when the name is first used (GL 4.6 core 4.2.1).
            Bool created = false;
            MG_Backend::BackendQueryHandle backendHandle = nullptr;
            Bool active = false;
            Bool ended = false;
            Bool resultCached = false;
            Uint64 cachedResult = 0;
            // The transform feedback primitive counter matching this query's target, at
            // BeginQuery time.
            Uint64 counterSnapshot = 0;
            // Capture-draw counters at BeginQuery time: how many capture draws the CPU
            // accounting had reproduced exactly, and how many of those it could not (a
            // geometry stage amplifies). Their deltas decide whether the CPU result may
            // stand in for the backend's.
            Uint64 accountedCaptureDrawSnapshot = 0;
            Uint64 geometryCaptureDrawSnapshot = 0;
        };

        // Query calls may arrive from any thread (launchers migrate the context
        // across JVM threads), so the live-object registry is mutex-guarded,
        // like the sync-object registry in GL_Sync.cpp. Entries left at process
        // shutdown are simply dropped; their backend handles die with the
        // backend.
        std::mutex g_queryObjectsMutex;
        UnorderedMap<GLuint, QueryObject*> g_liveQueryObjects;
        // Monotonically increasing id allocator; ids are valid query objects
        // immediately after GenQueries.
        GLuint g_nextQueryId = 1;
        // Id of the query currently active on GL_TIME_ELAPSED (0 = none).
        GLuint g_activeTimeElapsedQueryId = 0;
        // Ids of the queries active on the transform feedback targets (0 = none).
        GLuint g_activePrimitivesWrittenQueryId = 0;
        GLuint g_activePrimitivesGeneratedQueryId = 0;
        // Id of the query active on GL_SAMPLES_PASSED (0 = none).
        GLuint g_activeSamplesPassedQueryId = 0;
        // Ids of the queries active on the GL_ARB_pipeline_statistics_query targets, one slot per
        // target (0 = none). A map rather than a field per target: the eleven behave identically
        // and none of them has any state beyond "which object is counting".
        UnorderedMap<GLenum, GLuint> g_activePipelineStatisticsQueryIds;

        // The eleven pipeline-statistics counters (GL 4.6 core table 4.3 / ARB_pipeline_statistics_query).
        // A 4.6 core context has to ACCEPT all of them at glBeginQuery - the extension is core
        // since 4.6 and there is no query by which an application could learn otherwise before
        // calling. MobileGL instruments none of them, and says so the way GL 4.6 core 4.2.1
        // provides for: GL_QUERY_COUNTER_BITS answers zero for these targets, which is the
        // spec's own signal that the counter is unsupported and its results indeterminate. That
        // is an honest zero, not an advertised capability - the alternative, GL_INVALID_ENUM on a
        // core entry point, is both non-conformant AND less informative.
        Bool IsPipelineStatisticsQueryTarget(GLenum target) {
            switch (target) {
            case GL_VERTICES_SUBMITTED:
            case GL_PRIMITIVES_SUBMITTED:
            case GL_VERTEX_SHADER_INVOCATIONS:
            case GL_TESS_CONTROL_SHADER_PATCHES:
            case GL_TESS_EVALUATION_SHADER_INVOCATIONS:
            case GL_GEOMETRY_SHADER_INVOCATIONS:
            case GL_GEOMETRY_SHADER_PRIMITIVES_EMITTED:
            case GL_FRAGMENT_SHADER_INVOCATIONS:
            case GL_COMPUTE_SHADER_INVOCATIONS:
            case GL_CLIPPING_INPUT_PRIMITIVES:
            case GL_CLIPPING_OUTPUT_PRIMITIVES:
                return true;
            default:
                return false;
            }
        }

        Bool TimerQueryDisabled() {
            return MG_Config::Features.DisableTimerQuery;
        }

        void RecordQueryError(ErrorCode code, const char* function, const char* message) {
            MG_State::pGLContext->RecordError(code,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", function, message));
        }

        // The by-buffer query getters write the result into a buffer object instead of client
        // memory. Everything about the query itself - the name, whether it is still active, the
        // parameter - is checked by GetQueryObjectValue; what is left is the destination, so this
        // resolves the buffer and confirms the write lands inside it (GL 4.6 core 4.2.1).
        Bool ResolveQueryResultDestination(GLuint buffer, GLintptr offset, SizeT writeSize, const char* function,
                                           SharedPtr<MG_State::GLState::BufferObject>& outBuffer) {
            if (offset < 0) {
                RecordQueryError(ErrorCode::InvalidValue, function, "Offset cannot be negative.");
                return false;
            }
            if (!MG_State::pGLContext->ValidateBufferObject(buffer)) {
                RecordQueryError(ErrorCode::InvalidOperation, function, "Buffer object does not exist.");
                return false;
            }
            auto bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
            if (!bufferObject) {
                RecordQueryError(ErrorCode::InvalidOperation, function, "Buffer object does not exist.");
                return false;
            }
            if (static_cast<SizeT>(offset) + writeSize > bufferObject->GetSize()) {
                RecordQueryError(ErrorCode::InvalidOperation, function,
                                 "The query result does not fit in the buffer object at this offset.");
                return false;
            }
            outBuffer = bufferObject;
            return true;
        }

        // Callers must hold g_queryObjectsMutex.
        QueryObject* FindQueryObjectLocked(GLuint id) {
            const auto it = g_liveQueryObjects.find(id);
            return it != g_liveQueryObjects.end() ? it->second : nullptr;
        }

        // Callers must hold g_queryObjectsMutex. Releases the backend handle
        // (if any) and clears any cached result, so the object can be reused.
        void ResetQueryObjectLocked(QueryObject* queryObject) {
            if (queryObject->backendHandle) {
                if (const auto deleteBackendQuery = MG_Backend::gBackendFunctionsTable.GL.DeleteBackendQuery) {
                    deleteBackendQuery(queryObject->backendHandle);
                }
                queryObject->backendHandle = nullptr;
            }
            queryObject->active = false;
            queryObject->ended = false;
            queryObject->resultCached = false;
            queryObject->cachedResult = 0;
        }

        // Callers must hold g_queryObjectsMutex.
        void EndTimeElapsedQueryLocked(QueryObject* queryObject) {
            const auto endTimeElapsedQuery = MG_Backend::gBackendFunctionsTable.GL.EndTimeElapsedQuery;
            if (endTimeElapsedQuery && queryObject->backendHandle) {
                endTimeElapsedQuery(queryObject->backendHandle);
            }
            queryObject->active = false;
            queryObject->ended = true;
            g_activeTimeElapsedQueryId = 0;
        }

        // The CPU accounting counter a transform feedback query target reads: what the capture
        // buffers took for GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, and everything the capture
        // stage assembled - a paused span included - for GL_PRIMITIVES_GENERATED. One counter
        // for both targets would report the clamped written count as the generated one.
        Uint64 TransformFeedbackCounterForTarget(GLenum target) {
            return target == GL_PRIMITIVES_GENERATED
                       ? MG_State::pGLContext->GetTransformFeedbackGeneratedCounter()
                       : MG_State::pGLContext->GetTransformFeedbackPrimitiveCounter();
        }

        // The span's CPU accounting delta. Saturating: a snapshot left above its counter (a
        // context switch between Begin and End, a counter that never moved) would otherwise
        // wrap to 2^64-1, which GetQueryObjectuiv hands the app as 4294967295.
        Uint64 TransformFeedbackCpuResult(const QueryObject* queryObject) {
            const Uint64 counter = TransformFeedbackCounterForTarget(queryObject->target);
            return counter > queryObject->counterSnapshot ? counter - queryObject->counterSnapshot : 0;
        }

        // Whether this ended span's result should come from the CPU accounting rather than from
        // the backend query it also ran. Three conditions, all necessary:
        //   * the backend asked for it (DirectGLES, whose ES driver counter is the unreliable
        //     one; DirectVulkan never sets the bit and so is untouched by any of this);
        //   * the target is PRIMITIVES_WRITTEN. GL_PRIMITIVES_GENERATED counts primitives
        //     whether or not a capture is active, and the accounting only ever sees capture
        //     draws, so the backend's counter is the more complete answer there;
        //   * the span was fully accounted: at least one capture draw reached the accounting
        //     (the instanced, indirect and multi-draw entry points do not call it at all, so a
        //     span made of those is invisible to it) and none of them amplified through a
        //     geometry stage, which the CPU cannot model.
        Bool PrefersCpuTransformFeedbackResult(const QueryObject* queryObject) {
            if (!MG_Backend::gBackendFunctionsTable.GL.PrefersCpuXfbPrimitiveAccounting) return false;
            if (queryObject->target != GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN) return false;
            if (MG_State::pGLContext->GetTransformFeedbackGeometryCaptureDraws() !=
                queryObject->geometryCaptureDrawSnapshot) {
                return false;
            }
            return MG_State::pGLContext->GetTransformFeedbackAccountedCaptureDraws() !=
                   queryObject->accountedCaptureDrawSnapshot;
        }

        // Shared GetQueryObject* implementation. Returns false when an error
        // was recorded and no value should be written back. `outValueProduced`, when given,
        // additionally distinguishes "succeeded with a value" from "succeeded but the result is not
        // ready" - the GL_QUERY_RESULT_NO_WAIT case, where GL_ARB_query_buffer_object says the
        // destination is left alone rather than written with a placeholder.
        Bool GetQueryObjectValue(GLuint id, GLenum pname, const char* function, Uint64& outValue,
                                 Bool* outValueProduced = nullptr) {
            if (outValueProduced) *outValueProduced = true;
            const std::lock_guard<std::mutex> lock(g_queryObjectsMutex);
            auto* queryObject = FindQueryObjectLocked(id);
            if (!queryObject) {
                RecordQueryError(ErrorCode::InvalidOperation, function, "Query object does not exist.");
                return false;
            }
            if (queryObject->active) {
                RecordQueryError(ErrorCode::InvalidOperation, function, "Query object is still active.");
                return false;
            }

            switch (pname) {
            case GL_QUERY_TARGET:
                // The target a query was begun with (or created with, for glCreateQueries) - state
                // the object has carried all along, GL 4.6 core table 23.35.
                outValue = queryObject->target;
                return true;
            case GL_QUERY_RESULT_NO_WAIT: {
                if (queryObject->resultCached) {
                    outValue = queryObject->cachedResult;
                    return true;
                }
                Uint64 result = 0;
                const auto getQueryResult64 = MG_Backend::gBackendFunctionsTable.GL.GetQueryResult64;
                if (queryObject->backendHandle && getQueryResult64 &&
                    !getQueryResult64(queryObject->backendHandle, /*wait=*/false, &result)) {
                    // Not ready. The whole point of the no-wait form is that the caller's
                    // destination keeps whatever it already held.
                    if (outValueProduced) *outValueProduced = false;
                    outValue = 0;
                    return true;
                }
                if (queryObject->target == GL_ANY_SAMPLES_PASSED ||
                    queryObject->target == GL_ANY_SAMPLES_PASSED_CONSERVATIVE) {
                    result = result != 0 ? 1 : 0;
                }
                if (queryObject->backendHandle) {
                    if (const auto deleteBackendQuery = MG_Backend::gBackendFunctionsTable.GL.DeleteBackendQuery) {
                        deleteBackendQuery(queryObject->backendHandle);
                    }
                    queryObject->backendHandle = nullptr;
                }
                queryObject->cachedResult = result;
                queryObject->resultCached = true;
                outValue = result;
                return true;
            }
            case GL_QUERY_RESULT_AVAILABLE: {
                if (queryObject->resultCached || !queryObject->backendHandle) {
                    outValue = 1;
                    return true;
                }
                const auto isQueryResultAvailable = MG_Backend::gBackendFunctionsTable.GL.IsQueryResultAvailable;
                outValue = (!isQueryResultAvailable || isQueryResultAvailable(queryObject->backendHandle)) ? 1 : 0;
                return true;
            }
            case GL_QUERY_RESULT: {
                if (queryObject->resultCached) {
                    outValue = queryObject->cachedResult;
                    return true;
                }
                Uint64 result = 0;
                if (queryObject->backendHandle) {
                    const auto getQueryResult64 = MG_Backend::gBackendFunctionsTable.GL.GetQueryResult64;
                    if (getQueryResult64 &&
                        !getQueryResult64(queryObject->backendHandle, /*wait=*/true, &result)) {
                        // The backend could not produce the result YET (e.g. a
                        // Vulkan wait refusing to block on a not-yet-submitted
                        // frame serial). Per the documented no-stall tradeoff
                        // this call reads 0, but the value is NOT cached and
                        // the backend handle is kept, so a later AVAILABLE
                        // poll / RESULT read still produces the real value.
                        outValue = 0;
                        return true;
                    }
                    // ANY_SAMPLES_PASSED* report a boolean.
                    if (queryObject->target == GL_ANY_SAMPLES_PASSED ||
                        queryObject->target == GL_ANY_SAMPLES_PASSED_CONSERVATIVE) {
                        result = result != 0 ? 1 : 0;
                    }
                    // Final value produced (or no GetQueryResult64 hook: the
                    // query degrades to a zero result); the backend handle is
                    // consumed and the value cached for later reads.
                    if (const auto deleteBackendQuery = MG_Backend::gBackendFunctionsTable.GL.DeleteBackendQuery) {
                        deleteBackendQuery(queryObject->backendHandle);
                    }
                    queryObject->backendHandle = nullptr;
                }
                queryObject->cachedResult = result;
                queryObject->resultCached = true;
                outValue = result;
                return true;
            }
            default:
                RecordQueryError(ErrorCode::InvalidEnum, function, "Unsupported query object parameter.");
                return false;
            }
        }

        template <typename T>
        void GetQueryBufferObject(GLuint id, GLuint buffer, GLenum pname, GLintptr offset, const char* function) {
            SharedPtr<MG_State::GLState::BufferObject> bufferObject;
            if (!ResolveQueryResultDestination(buffer, offset, sizeof(T), function, bufferObject)) return;

            Uint64 value = 0;
            Bool valueProduced = false;
            if (!GetQueryObjectValue(id, pname, function, value, &valueProduced)) return;
            // GL_QUERY_RESULT_NO_WAIT on a result that has not landed writes nothing at all.
            if (!valueProduced) return;

            const T narrowed = static_cast<T>(value);
            bufferObject->UploadSubData({const_cast<T*>(&narrowed), sizeof(T)}, static_cast<SizeT>(offset));
        }
    } // namespace

    void GenQueries(GLsizei n, GLuint* ids) {
        if (n < 0) {
            RecordQueryError(ErrorCode::InvalidValue, __FUNCTION__, "n cannot be negative.");
            return;
        }
        if (!ids) {
            return;
        }
        const std::lock_guard<std::mutex> lock(g_queryObjectsMutex);
        for (GLsizei i = 0; i < n; ++i) {
            const GLuint id = g_nextQueryId++;
            auto* queryObject = new QueryObject;
            queryObject->id = id;
            g_liveQueryObjects[id] = queryObject;
            ids[i] = id;
        }
    }

    // glCreateQueries differs from glGenQueries in creating the objects outright, with their
    // target already fixed and the rest of their state at the defaults (GL 4.6 core 4.2.1).
    void CreateQueries(GLenum target, GLsizei n, GLuint* ids) {
        switch (target) {
        case GL_SAMPLES_PASSED:
        case GL_ANY_SAMPLES_PASSED:
        case GL_ANY_SAMPLES_PASSED_CONSERVATIVE:
        case GL_TIME_ELAPSED:
        case GL_TIMESTAMP:
        case GL_PRIMITIVES_GENERATED:
        case GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN:
            break;
        default:
            RecordQueryError(ErrorCode::InvalidEnum, __FUNCTION__, "Query target is not accepted.");
            return;
        }
        if (n < 0) {
            RecordQueryError(ErrorCode::InvalidValue, __FUNCTION__, "n cannot be negative.");
            return;
        }
        if (!ids) {
            return;
        }
        const std::lock_guard<std::mutex> lock(g_queryObjectsMutex);
        for (GLsizei i = 0; i < n; ++i) {
            const GLuint id = g_nextQueryId++;
            auto* queryObject = new QueryObject;
            queryObject->id = id;
            queryObject->target = target;
            queryObject->created = true;
            g_liveQueryObjects[id] = queryObject;
            ids[i] = id;
        }
    }

    void DeleteQueries(GLsizei n, const GLuint* ids) {
        if (n < 0) {
            RecordQueryError(ErrorCode::InvalidValue, __FUNCTION__, "n cannot be negative.");
            return;
        }
        if (!ids) {
            return;
        }
        const std::lock_guard<std::mutex> lock(g_queryObjectsMutex);
        for (GLsizei i = 0; i < n; ++i) {
            const auto it = g_liveQueryObjects.find(ids[i]);
            if (it == g_liveQueryObjects.end()) {
                continue; // unknown ids are silently ignored
            }
            QueryObject* queryObject = it->second;
            if (queryObject->active) {
                // Implicitly end before deletion, releasing the matching active slot.
                if (queryObject->target == GL_SAMPLES_PASSED || queryObject->target == GL_ANY_SAMPLES_PASSED ||
                    queryObject->target == GL_ANY_SAMPLES_PASSED_CONSERVATIVE) {
                    if (const auto endOcclusionQuery = MG_Backend::gBackendFunctionsTable.GL.EndOcclusionQuery;
                        endOcclusionQuery && queryObject->backendHandle) {
                        endOcclusionQuery(queryObject->backendHandle);
                    }
                    queryObject->active = false;
                    g_activeSamplesPassedQueryId = 0;
                } else if (IsPipelineStatisticsQueryTarget(queryObject->target)) {
                    queryObject->active = false;
                    g_activePipelineStatisticsQueryIds[queryObject->target] = 0;
                } else if (queryObject->target == GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN ||
                           queryObject->target == GL_PRIMITIVES_GENERATED) {
                    queryObject->active = false;
                    (queryObject->target == GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN
                         ? g_activePrimitivesWrittenQueryId
                         : g_activePrimitivesGeneratedQueryId) = 0;
                } else {
                    EndTimeElapsedQueryLocked(queryObject);
                }
            }
            if (queryObject->backendHandle) {
                if (const auto deleteBackendQuery = MG_Backend::gBackendFunctionsTable.GL.DeleteBackendQuery) {
                    deleteBackendQuery(queryObject->backendHandle);
                }
                queryObject->backendHandle = nullptr;
            }
            g_liveQueryObjects.erase(it);
            delete queryObject;
        }
    }

    GLboolean IsQuery(GLuint id) {
        if (id == 0) {
            return GL_FALSE;
        }
        const std::lock_guard<std::mutex> lock(g_queryObjectsMutex);
        // A name from glGenQueries is not yet a query object: it becomes one when it is first
        // used with BeginQuery/QueryCounter (which is what a non-zero target records), or
        // immediately if it came from glCreateQueries.
        const auto* queryObject = FindQueryObjectLocked(id);
        return (queryObject != nullptr && (queryObject->created || queryObject->target != 0)) ? GL_TRUE : GL_FALSE;
    }

    void BeginQuery(GLenum target, GLuint id) {
        const Bool isTransformFeedbackQuery =
            target == GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN || target == GL_PRIMITIVES_GENERATED;
        const Bool isOcclusionQuery =
            (target == GL_SAMPLES_PASSED || target == GL_ANY_SAMPLES_PASSED ||
             target == GL_ANY_SAMPLES_PASSED_CONSERVATIVE) &&
            MG_Backend::gBackendFunctionsTable.GL.BeginOcclusionQuery != nullptr;
        const Bool isPipelineStatisticsQuery = IsPipelineStatisticsQueryTarget(target);
        if (target != GL_TIME_ELAPSED && !isTransformFeedbackQuery && !isOcclusionQuery &&
            !isPipelineStatisticsQuery) {
            // GL_TIMESTAMP is not a valid BeginQuery target; the occlusion targets
            // need backend support.
            RecordQueryError(ErrorCode::InvalidEnum, __FUNCTION__, "Query target is not supported.");
            return;
        }
        if (id == 0) {
            RecordQueryError(ErrorCode::InvalidOperation, __FUNCTION__, "Query id 0 cannot be used.");
            return;
        }
        const std::lock_guard<std::mutex> lock(g_queryObjectsMutex);
        auto* queryObject = FindQueryObjectLocked(id);
        if (!queryObject) {
            RecordQueryError(ErrorCode::InvalidOperation, __FUNCTION__, "Query object does not exist.");
            return;
        }
        GLuint& activeQueryId = isPipelineStatisticsQuery
            ? g_activePipelineStatisticsQueryIds[target]
            : (isTransformFeedbackQuery
                   ? (target == GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN ? g_activePrimitivesWrittenQueryId
                                                                         : g_activePrimitivesGeneratedQueryId)
                   : (isOcclusionQuery ? g_activeSamplesPassedQueryId : g_activeTimeElapsedQueryId));
        if (activeQueryId != 0) {
            RecordQueryError(ErrorCode::InvalidOperation, __FUNCTION__,
                             "A query is already active on this target.");
            return;
        }
        if (queryObject->active) {
            RecordQueryError(ErrorCode::InvalidOperation, __FUNCTION__, "Query object is already active.");
            return;
        }
        if (queryObject->target != 0 && queryObject->target != target) {
            RecordQueryError(ErrorCode::InvalidOperation, __FUNCTION__,
                             "Query object was already used with a different target.");
            return;
        }

        ResetQueryObjectLocked(queryObject); // discard any previous result
        queryObject->target = target;
        queryObject->active = true;
        if (isPipelineStatisticsQuery) {
            // Nothing to start: the counter is uninstrumented and GL_QUERY_COUNTER_BITS says so.
            // The object still becomes a real, target-latched query so every other rule about it
            // (re-use with another target, double-begin, EndQuery pairing) keeps holding.
        } else if (isTransformFeedbackQuery) {
            // Prefer real GPU transform-feedback queries (exact with geometry shaders);
            // the CPU accounting delta stays as the fallback when the backend lacks them.
            const auto beginXfbPrimitivesQuery = MG_Backend::gBackendFunctionsTable.GL.BeginXfbPrimitivesQuery;
            queryObject->backendHandle =
                beginXfbPrimitivesQuery ? beginXfbPrimitivesQuery(target == GL_PRIMITIVES_GENERATED) : nullptr;
            queryObject->counterSnapshot = TransformFeedbackCounterForTarget(target);
            queryObject->accountedCaptureDrawSnapshot =
                MG_State::pGLContext->GetTransformFeedbackAccountedCaptureDraws();
            queryObject->geometryCaptureDrawSnapshot =
                MG_State::pGLContext->GetTransformFeedbackGeometryCaptureDraws();
        } else if (isOcclusionQuery) {
            queryObject->backendHandle = MG_Backend::gBackendFunctionsTable.GL.BeginOcclusionQuery();
        } else {
            const auto beginTimeElapsedQuery = MG_Backend::gBackendFunctionsTable.GL.BeginTimeElapsedQuery;
            queryObject->backendHandle =
                (!TimerQueryDisabled() && beginTimeElapsedQuery) ? beginTimeElapsedQuery() : nullptr;
        }
        activeQueryId = id;
    }

    void EndQuery(GLenum target) {
        const Bool isTransformFeedbackQuery =
            target == GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN || target == GL_PRIMITIVES_GENERATED;
        const Bool isOcclusionQuery =
            (target == GL_SAMPLES_PASSED || target == GL_ANY_SAMPLES_PASSED ||
             target == GL_ANY_SAMPLES_PASSED_CONSERVATIVE) &&
            MG_Backend::gBackendFunctionsTable.GL.BeginOcclusionQuery != nullptr;
        const Bool isPipelineStatisticsQuery = IsPipelineStatisticsQueryTarget(target);
        if (target != GL_TIME_ELAPSED && !isTransformFeedbackQuery && !isOcclusionQuery &&
            !isPipelineStatisticsQuery) {
            RecordQueryError(ErrorCode::InvalidEnum, __FUNCTION__, "Query target is not supported.");
            return;
        }
        const std::lock_guard<std::mutex> lock(g_queryObjectsMutex);
        GLuint& activeQueryId = isPipelineStatisticsQuery
            ? g_activePipelineStatisticsQueryIds[target]
            : (isTransformFeedbackQuery
                   ? (target == GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN ? g_activePrimitivesWrittenQueryId
                                                                         : g_activePrimitivesGeneratedQueryId)
                   : (isOcclusionQuery ? g_activeSamplesPassedQueryId : g_activeTimeElapsedQueryId));
        if (activeQueryId == 0) {
            RecordQueryError(ErrorCode::InvalidOperation, __FUNCTION__, "No query is active on this target.");
            return;
        }
        auto* queryObject = FindQueryObjectLocked(activeQueryId);
        if (!queryObject) {
            activeQueryId = 0; // should not happen; keep state consistent
            return;
        }
        if (isPipelineStatisticsQuery) {
            // The result is a definite zero rather than an unread backend handle, so a later
            // GetQueryObject* answers immediately and never waits on something that was never
            // started. GL_QUERY_COUNTER_BITS = 0 is what marks that zero indeterminate.
            queryObject->cachedResult = 0;
            queryObject->resultCached = true;
            queryObject->active = false;
            queryObject->ended = true;
            activeQueryId = 0;
            return;
        }
        if (isTransformFeedbackQuery) {
            if (queryObject->backendHandle) {
                if (const auto endXfbPrimitivesQuery = MG_Backend::gBackendFunctionsTable.GL.EndXfbPrimitivesQuery) {
                    endXfbPrimitivesQuery(queryObject->backendHandle);
                }
            }
            // A backend query that is not going to be read is released here, not left to be
            // collected later: the span is over, the driver object has nothing left to say.
            // Ending it first is what makes that legal.
            if (!queryObject->backendHandle || PrefersCpuTransformFeedbackResult(queryObject)) {
                if (queryObject->backendHandle) {
                    if (const auto deleteBackendQuery = MG_Backend::gBackendFunctionsTable.GL.DeleteBackendQuery) {
                        deleteBackendQuery(queryObject->backendHandle);
                    }
                    queryObject->backendHandle = nullptr;
                }
                queryObject->cachedResult = TransformFeedbackCpuResult(queryObject);
                queryObject->resultCached = true;
            }
            // Otherwise the result comes from the GPU query at read time.
            queryObject->active = false;
            queryObject->ended = true;
            activeQueryId = 0;
            return;
        }
        if (isOcclusionQuery) {
            if (const auto endOcclusionQuery = MG_Backend::gBackendFunctionsTable.GL.EndOcclusionQuery;
                endOcclusionQuery && queryObject->backendHandle) {
                endOcclusionQuery(queryObject->backendHandle);
            }
            queryObject->active = false;
            queryObject->ended = true;
            activeQueryId = 0;
            return;
        }
        EndTimeElapsedQueryLocked(queryObject);
    }

    void QueryCounter(GLuint id, GLenum target) {
        if (target != GL_TIMESTAMP) {
            RecordQueryError(ErrorCode::InvalidEnum, __FUNCTION__, "QueryCounter target must be GL_TIMESTAMP.");
            return;
        }
        if (id == 0) {
            RecordQueryError(ErrorCode::InvalidOperation, __FUNCTION__, "Query id 0 cannot be used.");
            return;
        }
        const std::lock_guard<std::mutex> lock(g_queryObjectsMutex);
        auto* queryObject = FindQueryObjectLocked(id);
        if (!queryObject) {
            RecordQueryError(ErrorCode::InvalidOperation, __FUNCTION__, "Query object does not exist.");
            return;
        }
        if (queryObject->active) {
            RecordQueryError(ErrorCode::InvalidOperation, __FUNCTION__, "Query object is currently active.");
            return;
        }
        if (queryObject->target != 0 && queryObject->target != target) {
            RecordQueryError(ErrorCode::InvalidOperation, __FUNCTION__,
                             "Query object was already used with a different target.");
            return;
        }

        ResetQueryObjectLocked(queryObject); // discard any previous result
        queryObject->target = target;
        const auto queryCounterTimestamp = MG_Backend::gBackendFunctionsTable.GL.QueryCounterTimestamp;
        queryObject->backendHandle =
            (!TimerQueryDisabled() && queryCounterTimestamp) ? queryCounterTimestamp() : nullptr;
        queryObject->ended = true;
    }

    void BeginConditionalRender(GLuint id, GLenum mode) {
        // GL 4.6 core 10.9's eight modes. The _INVERTED half flips the sense of the predicate;
        // the BY_REGION half only narrows WHERE an implementation is permitted to discard, so
        // treating it as its whole-framebuffer sibling is what an implementation without region
        // granularity does. The _NO_WAIT half is a permission to render rather than stall, not an
        // obligation - see the resolve below.
        Bool inverted = false;
        switch (mode) {
        case GL_QUERY_WAIT:
        case GL_QUERY_NO_WAIT:
        case GL_QUERY_BY_REGION_WAIT:
        case GL_QUERY_BY_REGION_NO_WAIT:
            inverted = false;
            break;
        case GL_QUERY_WAIT_INVERTED:
        case GL_QUERY_NO_WAIT_INVERTED:
        case GL_QUERY_BY_REGION_WAIT_INVERTED:
        case GL_QUERY_BY_REGION_NO_WAIT_INVERTED:
            inverted = true;
            break;
        default:
            RecordQueryError(ErrorCode::InvalidEnum, __FUNCTION__, "mode is not a conditional render mode.");
            return;
        }

        if (MG_State::pGLContext->IsConditionalRenderActive()) {
            RecordQueryError(ErrorCode::InvalidOperation, __FUNCTION__, "Conditional rendering is already active.");
            return;
        }

        {
            const std::lock_guard<std::mutex> lock(g_queryObjectsMutex);
            const auto* queryObject = FindQueryObjectLocked(id);
            // A generated NAME is not yet a query object; it becomes one at its first use with a
            // target (the same rule glIsQuery answers by).
            if (!queryObject || (!queryObject->created && queryObject->target == 0)) {
                RecordQueryError(ErrorCode::InvalidValue, __FUNCTION__, "id is not the name of a query object.");
                return;
            }
            if (queryObject->active) {
                RecordQueryError(ErrorCode::InvalidOperation, __FUNCTION__, "The query object is still active.");
                return;
            }
            if (queryObject->target != GL_SAMPLES_PASSED && queryObject->target != GL_ANY_SAMPLES_PASSED &&
                queryObject->target != GL_ANY_SAMPLES_PASSED_CONSERVATIVE) {
                RecordQueryError(ErrorCode::InvalidOperation, __FUNCTION__,
                                 "Conditional rendering requires an occlusion query object.");
                return;
            }
        }

        // Resolved ONCE, here, and by WAITING even for the _NO_WAIT modes: the spec lets those
        // render instead of stalling, so always waiting is conforming and is the only choice that
        // gives the whole block one deterministic verdict. Reading it per command instead would
        // let a result that lands mid-block change the answer half way through.
        Uint64 samplesPassed = 0;
        if (!GetQueryObjectValue(id, GL_QUERY_RESULT, __FUNCTION__, samplesPassed)) return;
        const Bool passed = samplesPassed != 0;
        MG_State::pGLContext->BeginConditionalRender(id, mode, inverted ? passed : !passed);
    }

    void EndConditionalRender() {
        if (!MG_State::pGLContext->IsConditionalRenderActive()) {
            RecordQueryError(ErrorCode::InvalidOperation, __FUNCTION__, "Conditional rendering is not active.");
            return;
        }
        MG_State::pGLContext->EndConditionalRender();
    }

    void GetQueryiv(GLenum target, GLenum pname, GLint* params) {
        if (!params) {
            return;
        }
        switch (pname) {
        case GL_CURRENT_QUERY: {
            const std::lock_guard<std::mutex> lock(g_queryObjectsMutex);
            switch (target) {
            case GL_TIME_ELAPSED:
                *params = static_cast<GLint>(g_activeTimeElapsedQueryId);
                break;
            case GL_SAMPLES_PASSED:
            case GL_ANY_SAMPLES_PASSED:
            case GL_ANY_SAMPLES_PASSED_CONSERVATIVE:
                *params = static_cast<GLint>(g_activeSamplesPassedQueryId);
                break;
            case GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN:
                *params = static_cast<GLint>(g_activePrimitivesWrittenQueryId);
                break;
            case GL_PRIMITIVES_GENERATED:
                *params = static_cast<GLint>(g_activePrimitivesGeneratedQueryId);
                break;
            default:
                if (IsPipelineStatisticsQueryTarget(target)) {
                    const auto it = g_activePipelineStatisticsQueryIds.find(target);
                    *params = it != g_activePipelineStatisticsQueryIds.end() ? static_cast<GLint>(it->second) : 0;
                } else {
                    *params = 0;
                }
                break;
            }
            return;
        }
        case GL_QUERY_COUNTER_BITS: {
            // 64 bits are advertised only while the live backend can actually
            // time: IsTimerQuerySupported is the dynamic truth (extension /
            // entry points / timestamp valid bits at call time, not at table
            // init), and the MOBILEGL_DISABLE_TIMERQUERY kill switch always
            // wins.
            if (IsPipelineStatisticsQueryTarget(target)) {
                // Zero: GL 4.6 core 4.2.1's way of saying the counter is not implemented and its
                // results are indeterminate. The conformance suite reads exactly this and skips
                // the functional half of each such target, which is the outcome an uninstrumented
                // counter should produce.
                *params = 0;
                return;
            }
            if (target == GL_SAMPLES_PASSED || target == GL_ANY_SAMPLES_PASSED ||
                target == GL_ANY_SAMPLES_PASSED_CONSERVATIVE) {
                const Bool occlusionSupported = MG_Backend::gBackendFunctionsTable.GL.BeginOcclusionQuery != nullptr;
                *params = occlusionSupported ? (target == GL_SAMPLES_PASSED ? 32 : 1) : 0;
                return;
            }
            const Bool timerTarget = target == GL_TIME_ELAPSED || target == GL_TIMESTAMP;
            const auto isTimerQuerySupported = MG_Backend::gBackendFunctionsTable.GL.IsTimerQuerySupported;
            const Bool supported =
                timerTarget && !TimerQueryDisabled() && isTimerQuerySupported && isTimerQuerySupported();
            *params = supported ? 64 : 0;
            return;
        }
        default:
            RecordQueryError(ErrorCode::InvalidEnum, __FUNCTION__, "Unsupported query parameter.");
            return;
        }
    }

    void GetQueryBufferObjectiv(GLuint id, GLuint buffer, GLenum pname, GLintptr offset) {
        GetQueryBufferObject<GLint>(id, buffer, pname, offset, __FUNCTION__);
    }

    void GetQueryBufferObjectuiv(GLuint id, GLuint buffer, GLenum pname, GLintptr offset) {
        GetQueryBufferObject<GLuint>(id, buffer, pname, offset, __FUNCTION__);
    }

    void GetQueryBufferObjecti64v(GLuint id, GLuint buffer, GLenum pname, GLintptr offset) {
        GetQueryBufferObject<GLint64>(id, buffer, pname, offset, __FUNCTION__);
    }

    void GetQueryBufferObjectui64v(GLuint id, GLuint buffer, GLenum pname, GLintptr offset) {
        GetQueryBufferObject<GLuint64>(id, buffer, pname, offset, __FUNCTION__);
    }

    void GetQueryObjectiv(GLuint id, GLenum pname, GLint* params) {
        Uint64 value = 0;
        Bool valueProduced = false;
        if (!GetQueryObjectValue(id, pname, __FUNCTION__, value, &valueProduced) || !valueProduced || !params) {
            return;
        }
        constexpr Uint64 kMaxInt = static_cast<Uint64>(INT_MAX);
        *params = value > kMaxInt ? INT_MAX : static_cast<GLint>(value);
    }

    void GetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params) {
        Uint64 value = 0;
        Bool valueProduced = false;
        if (!GetQueryObjectValue(id, pname, __FUNCTION__, value, &valueProduced) || !valueProduced || !params) {
            return;
        }
        *params = static_cast<GLuint>(value & 0xFFFFFFFFull);
    }

    void GetQueryObjecti64v(GLuint id, GLenum pname, GLint64* params) {
        Uint64 value = 0;
        Bool valueProduced = false;
        if (!GetQueryObjectValue(id, pname, __FUNCTION__, value, &valueProduced) || !valueProduced || !params) {
            return;
        }
        *params = static_cast<GLint64>(value);
    }

    void GetQueryObjectui64v(GLuint id, GLenum pname, GLuint64* params) {
        Uint64 value = 0;
        Bool valueProduced = false;
        if (!GetQueryObjectValue(id, pname, __FUNCTION__, value, &valueProduced) || !valueProduced || !params) {
            return;
        }
        *params = static_cast<GLuint64>(value);
    }

    namespace {
        Bool IsPerVertexStreamQueryTarget(GLenum target) {
            return target == GL_PRIMITIVES_GENERATED || target == GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN;
        }

        // The indexed query entry points differ from the plain ones only in the vertex
        // stream they address (GL 4.6 core 4.2.1): index must be below GL_MAX_VERTEX_STREAMS
        // for the two transform feedback targets and zero for every other target. MobileGL
        // implements ONE vertex stream, so both bounds are 1 and a valid call is always index 0 -
        // which is what makes the three forwards below equivalent to the unindexed entry points.
        //
        // THAT EQUIVALENCE IS THE WHOLE JUSTIFICATION, and it is read out of the getter rather
        // than assumed: the moment GL_MAX_VERTEX_STREAMS answers more than one, index 1..3 starts
        // reaching EndQueryIndexed and GetQueryIndexediv, which resolve the active query from
        // per-TARGET globals and would end - or report - a query begun on a different stream.
        // Raising that limit therefore means giving each active query a stream index and
        // comparing it here, not just changing the number.
        Bool ValidateQueryStreamIndex(const char* function, GLenum target, GLuint index) {
            const Bool perStreamTarget = IsPerVertexStreamQueryTarget(target);
            GLint maxVertexStreams = 1;
            if (perStreamTarget) {
                GetIntegerv(GL_MAX_VERTEX_STREAMS, &maxVertexStreams);
            }
            if (index < static_cast<GLuint>(std::max(maxVertexStreams, 1))) {
                return true;
            }
            RecordQueryError(ErrorCode::InvalidValue, function,
                             perStreamTarget ? "index is not less than GL_MAX_VERTEX_STREAMS."
                                             : "index must be zero for this query target.");
            return false;
        }

    } // namespace

    void BeginQueryIndexed(GLenum target, GLuint index, GLuint id) {
        if (!ValidateQueryStreamIndex(__FUNCTION__, target, index)) return;
        BeginQuery(target, id);
    }

    void EndQueryIndexed(GLenum target, GLuint index) {
        if (!ValidateQueryStreamIndex(__FUNCTION__, target, index)) return;
        EndQuery(target);
    }

    void GetQueryIndexediv(GLenum target, GLuint index, GLenum pname, GLint* params) {
        if (!ValidateQueryStreamIndex(__FUNCTION__, target, index)) return;
        GetQueryiv(target, pname, params);
    }

    void DestroyAllQueryObjects() {
        // Detach the registry under the lock, release outside it - same discipline
        // (and the same accepted teardown race) as DestroyAllSyncObjects. Without
        // this drain, every query the app left undeleted survived full library
        // teardown in the process-global registry: the objects and their backend
        // wrappers leaked across Destroy/Initialize cycles, stale ids kept
        // answering IsQuery == GL_TRUE in the re-initialized library, and a later
        // glDeleteQueries could hand the OLD backend's handle to a DIFFERENT
        // backend's DeleteBackendQuery, which casts it to the wrong wrapper type.
        UnorderedMap<GLuint, QueryObject*> orphans;
        {
            const std::lock_guard<std::mutex> lock(g_queryObjectsMutex);
            orphans.swap(g_liveQueryObjects);
            g_activeTimeElapsedQueryId = 0;
            g_activePrimitivesWrittenQueryId = 0;
            g_activePrimitivesGeneratedQueryId = 0;
            g_activeSamplesPassedQueryId = 0;
        }
        if (orphans.empty()) {
            return;
        }
        // Backend handles must be released by the backend that created them, so
        // this runs while the function table is still populated. Both backends'
        // DeleteBackendQuery are generation-guarded, so a handle whose renderer
        // or ES context is already gone frees only the wrapper.
        const auto deleteBackendQuery = MG_Backend::gBackendFunctionsTable.GL.DeleteBackendQuery;
        for (const auto& [_, queryObject] : orphans) {
            if (deleteBackendQuery && queryObject->backendHandle) {
                deleteBackendQuery(queryObject->backendHandle);
            }
            delete queryObject;
        }
        MGLOG_D("DestroyAllQueryObjects: reclaimed %zu query object(s) the app left undeleted", orphans.size());
    }
} // namespace MobileGL::MG_Impl::GLImpl
