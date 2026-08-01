// MobileGL - MobileGL/MG_Impl/GLImpl/Query/GL_Query.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Query.h"
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
            MG_Backend::BackendQueryHandle backendHandle = nullptr;
            Bool active = false;
            Bool ended = false;
            Bool resultCached = false;
            Uint64 cachedResult = 0;
            // Transform feedback primitive counter at BeginQuery time.
            Uint64 counterSnapshot = 0;
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

        Bool TimerQueryDisabled() {
            return MG_Config::Features.DisableTimerQuery;
        }

        void RecordQueryError(ErrorCode code, const char* function, const char* message) {
            MG_State::pGLContext->RecordError(code,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", function, message));
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

        // Shared GetQueryObject* implementation. Returns false when an error
        // was recorded and no value should be written back.
        Bool GetQueryObjectValue(GLuint id, GLenum pname, const char* function, Uint64& outValue) {
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
        // Gen'd ids count as query objects here: the registry creates live
        // objects at GenQueries time.
        return FindQueryObjectLocked(id) != nullptr ? GL_TRUE : GL_FALSE;
    }

    void BeginQuery(GLenum target, GLuint id) {
        const Bool isTransformFeedbackQuery =
            target == GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN || target == GL_PRIMITIVES_GENERATED;
        const Bool isOcclusionQuery =
            (target == GL_SAMPLES_PASSED || target == GL_ANY_SAMPLES_PASSED ||
             target == GL_ANY_SAMPLES_PASSED_CONSERVATIVE) &&
            MG_Backend::gBackendFunctionsTable.GL.BeginOcclusionQuery != nullptr;
        if (target != GL_TIME_ELAPSED && !isTransformFeedbackQuery && !isOcclusionQuery) {
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
        GLuint& activeQueryId = isTransformFeedbackQuery
            ? (target == GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN ? g_activePrimitivesWrittenQueryId
                                                                  : g_activePrimitivesGeneratedQueryId)
            : (isOcclusionQuery ? g_activeSamplesPassedQueryId : g_activeTimeElapsedQueryId);
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
        if (isTransformFeedbackQuery) {
            // Prefer real GPU transform-feedback queries (exact with geometry shaders);
            // the CPU accounting delta stays as the fallback when the backend lacks them.
            const auto beginXfbPrimitivesQuery = MG_Backend::gBackendFunctionsTable.GL.BeginXfbPrimitivesQuery;
            queryObject->backendHandle =
                beginXfbPrimitivesQuery ? beginXfbPrimitivesQuery(target == GL_PRIMITIVES_GENERATED) : nullptr;
            queryObject->counterSnapshot = MG_State::pGLContext->GetTransformFeedbackPrimitiveCounter();
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
        if (target != GL_TIME_ELAPSED && !isTransformFeedbackQuery && !isOcclusionQuery) {
            RecordQueryError(ErrorCode::InvalidEnum, __FUNCTION__, "Query target is not supported.");
            return;
        }
        const std::lock_guard<std::mutex> lock(g_queryObjectsMutex);
        GLuint& activeQueryId = isTransformFeedbackQuery
            ? (target == GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN ? g_activePrimitivesWrittenQueryId
                                                                  : g_activePrimitivesGeneratedQueryId)
            : (isOcclusionQuery ? g_activeSamplesPassedQueryId : g_activeTimeElapsedQueryId);
        if (activeQueryId == 0) {
            RecordQueryError(ErrorCode::InvalidOperation, __FUNCTION__, "No query is active on this target.");
            return;
        }
        auto* queryObject = FindQueryObjectLocked(activeQueryId);
        if (!queryObject) {
            activeQueryId = 0; // should not happen; keep state consistent
            return;
        }
        if (isTransformFeedbackQuery) {
            if (queryObject->backendHandle) {
                if (const auto endXfbPrimitivesQuery = MG_Backend::gBackendFunctionsTable.GL.EndXfbPrimitivesQuery) {
                    endXfbPrimitivesQuery(queryObject->backendHandle);
                }
                // Result comes from the GPU query at read time.
            } else {
                queryObject->cachedResult =
                    MG_State::pGLContext->GetTransformFeedbackPrimitiveCounter() - queryObject->counterSnapshot;
                queryObject->resultCached = true;
            }
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
                *params = 0;
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

    void GetQueryObjectiv(GLuint id, GLenum pname, GLint* params) {
        Uint64 value = 0;
        if (!GetQueryObjectValue(id, pname, __FUNCTION__, value) || !params) {
            return;
        }
        constexpr Uint64 kMaxInt = static_cast<Uint64>(INT_MAX);
        *params = value > kMaxInt ? INT_MAX : static_cast<GLint>(value);
    }

    void GetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params) {
        Uint64 value = 0;
        if (!GetQueryObjectValue(id, pname, __FUNCTION__, value) || !params) {
            return;
        }
        *params = static_cast<GLuint>(value & 0xFFFFFFFFull);
    }

    void GetQueryObjecti64v(GLuint id, GLenum pname, GLint64* params) {
        Uint64 value = 0;
        if (!GetQueryObjectValue(id, pname, __FUNCTION__, value) || !params) {
            return;
        }
        *params = static_cast<GLint64>(value);
    }

    void GetQueryObjectui64v(GLuint id, GLenum pname, GLuint64* params) {
        Uint64 value = 0;
        if (!GetQueryObjectValue(id, pname, __FUNCTION__, value) || !params) {
            return;
        }
        *params = static_cast<GLuint64>(value);
    }
} // namespace MobileGL::MG_Impl::GLImpl
