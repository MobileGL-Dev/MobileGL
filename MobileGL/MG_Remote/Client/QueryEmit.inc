// Opaque local proxies carry only {slot, generation, target} across the wire.
        struct RemoteQueryProxy { MG_Pipe::MGPQueryDesc Desc{}; };
        std::mutex g_queryProxyMutex;
        UnorderedMap<MG_Backend::BackendQueryHandle, UniquePtr<RemoteQueryProxy>> g_queryProxies;

        const MG_Pipe::MGPQueryDesc& QueryDesc(MG_Backend::BackendQueryHandle handle) {
            const auto found = g_queryProxies.find(handle);
            if (found == g_queryProxies.end())
                Wire::WireProtocolFatal("Query.proxy", "unknown local query proxy");
            return found->second->Desc;
        }

        template <typename Result>
        Result ReadQueryReply(ClientSession& session, MG_Pipe::MGPWireOp op, const void* request, Uint64 size) {
            Result result{};
            Int32 status = Wire::ReplySink::kStatusError;
            Uint64 returned = 0;
            session.EmitAndWait(op, request, size, nullptr, 0, &result, sizeof(result), &status, &returned);
            // A DECLINED QUERY IS AN ANSWER TOO (P12), for ReadFenceReply's reason and on the
            // same three-value space (PipeWireCodec.h:457-460). The server declines these three
            // rows by design whenever it cannot answer them - QueryAvailable :1841-1847,
            // QueryResult :1848-1854 and QueryTimestamp :1855-1859 all post kStatusDeclined with
            // a zero-length payload - and the client produces the same status once the session
            // stops carrying records (ClientSession.cpp:1837 is the device-lost latch, :2025 a
            // doorbell that died during teardown). `returned != sizeof(result)` is guaranteed for
            // such an answer, so the guard below used to abort on every one of them.
            //
            // THE ZEROED RESULT IS THE NO-OP EACH ROW ALREADY HAS, and it is what a query has
            // when there is no answer YET rather than no answer ever:
            //   - QueryAvailable -> 0 -> false, "the result is not available" (the same answer an
            //     unfinished query gives - GL_QUERY_RESULT_AVAILABLE exists to be told that);
            //   - QueryResult -> Produced = 0 -> the caller returns false with *value untouched
            //     (:70 spells the healthy analogue of the same answer);
            //   - QueryTimestamp -> 0, no timestamp to report.
            // ERROR stays fatal below, and so does an OK reply that arrived short.
            if (status == Wire::ReplySink::kStatusDeclined) {
                MGLOG_E_ONCE("MG_Remote client: the peer DECLINED a %s reply - the verb did not "
                             "happen (a lost device, or a teardown); answering the row's "
                             "'no answer yet' value",
                             Wire::WireOpName(op));
                return result;
            }
            if (status != Wire::ReplySink::kStatusOk || returned != sizeof(result))
                Wire::WireProtocolFatal("Query.reply", "missing or malformed backend query reply");
            return result;
        }

        MG_Backend::BackendQueryHandle EmitCreateQuery(GLenum kind, Bool counter) {
            auto& session = RequireSession("QueryBegin");
            const std::lock_guard<std::mutex> lock(g_queryProxyMutex);
            BeforeReadOnlyVerb();
            auto proxy = MakeUnique<RemoteQueryProxy>();
            proxy->Desc.Query = MG_Pipe::MGPipeSlots().Allocate(MG_Pipe::MGPipeKind::Query);
            proxy->Desc.Kind = kind;
            session.EmitAndWait(MG_Pipe::MGPWireOp::QueryCreate, &proxy->Desc, sizeof(proxy->Desc),
                                nullptr, 0, nullptr, 0, nullptr);
            session.EmitAndWait(counter ? MG_Pipe::MGPWireOp::QueryCounter : MG_Pipe::MGPWireOp::QueryBegin,
                                &proxy->Desc, sizeof(proxy->Desc), nullptr, 0, nullptr, 0, nullptr);
            auto* local = proxy.get();
            g_queryProxies.emplace(local, Move(proxy));
            return local;
        }
        MG_Backend::BackendQueryHandle EmitBeginTimeElapsedQuery() { return EmitCreateQuery(GL_TIME_ELAPSED, false); }
        MG_Backend::BackendQueryHandle EmitQueryCounterTimestamp() { return EmitCreateQuery(GL_TIMESTAMP, true); }
        MG_Backend::BackendQueryHandle EmitBeginOcclusionQuery() { return EmitCreateQuery(GL_SAMPLES_PASSED, false); }
        MG_Backend::BackendQueryHandle EmitBeginXfbPrimitivesQuery(Bool generated) {
            return EmitCreateQuery(generated ? GL_PRIMITIVES_GENERATED : GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, false);
        }
        void EmitEndQuery(MG_Backend::BackendQueryHandle handle) {
            auto& session = RequireSession("QueryEnd");
            const std::lock_guard<std::mutex> lock(g_queryProxyMutex);
            const auto& desc = QueryDesc(handle);
            BeforeReadOnlyVerb();
            session.EmitAndWait(MG_Pipe::MGPWireOp::QueryEnd, &desc, sizeof(desc), nullptr, 0, nullptr, 0, nullptr);
        }
        Bool EmitIsQueryResultAvailable(MG_Backend::BackendQueryHandle handle) {
            auto& session = RequireSession("IsQueryResultAvailable");
            const std::lock_guard<std::mutex> lock(g_queryProxyMutex);
            const MG_Pipe::MGPHandleOnly request{QueryDesc(handle).Query, static_cast<Uint32>(MG_Pipe::MGPipeKind::Query), 0};
            const auto result = ReadQueryReply<Uint32>(session, MG_Pipe::MGPWireOp::QueryAvailable, &request, sizeof(request));
            if (result > 1) Wire::WireProtocolFatal("QueryAvailable.reply", "availability is not boolean");
            return result != 0;
        }
        Bool EmitGetQueryResult64(MG_Backend::BackendQueryHandle handle, Bool wait, Uint64* value) {
            auto& session = RequireSession("GetQueryResult64");
            const std::lock_guard<std::mutex> lock(g_queryProxyMutex);
            MG_Pipe::MGPQueryResultRequest request{};
            request.Query = QueryDesc(handle).Query;
            request.Wait = wait ? 1 : 0;
            const auto reply = ReadQueryReply<Wire::QueryResultReply>(session, MG_Pipe::MGPWireOp::QueryResult,
                                                                     &request, sizeof(request));
            if (reply.Produced > 1 || reply.Reserved)
                Wire::WireProtocolFatal("QueryResult.reply", "invalid result metadata");
            if (!reply.Produced) return false;
            if (value) *value = reply.Value;
            return true;
        }
        void EmitDeleteBackendQuery(MG_Backend::BackendQueryHandle handle) {
            const std::lock_guard<std::mutex> lock(g_queryProxyMutex);
            const auto query = QueryDesc(handle).Query;
            if (auto* session = ClientSession::Active(); session && session->Started()) {
                const MG_Pipe::MGPHandleOnly request{query, static_cast<Uint32>(MG_Pipe::MGPipeKind::Query), 0};
                session->EmitAndWait(MG_Pipe::MGPWireOp::QueryDestroy, &request, sizeof(request),
                                     nullptr, 0, nullptr, 0, nullptr);
            }
            MG_Pipe::MGPipeSlots().Free(MG_Pipe::MGPipeKind::Query, query);
            g_queryProxies.erase(handle);
        }
        Int64 EmitGetGpuTimestampNs() {
            auto& session = RequireSession("GetGpuTimestampNs");
            BeforeReadOnlyVerb();
            const MG_Pipe::MGPTimestampRequest request{};
            return ReadQueryReply<Int64>(session, MG_Pipe::MGPWireOp::QueryTimestamp, &request, sizeof(request));
        }

        void EmitDeleteTransformFeedback(GLuint name) {
            auto& session = RequireSession("DeleteTransformFeedback");
            BeforeReadOnlyVerb();
            const auto& context = *MG_State::pGLContext;
            const MG_Pipe::MGPStreamOutputBind object{name, 0, context.GetTransformFeedbackLifetimeId(name)};
            session.EmitAndWait(MG_Pipe::MGPWireOp::DeleteStreamOutput, &object, sizeof(object),
                                nullptr, 0, nullptr, 0, nullptr);
            if (context.GetBoundTransformFeedbackName() == name) {
                const MG_Pipe::MGPStreamOutputBind fallback{0, 0, context.GetTransformFeedbackLifetimeId(0)};
                session.EmitAndWait(MG_Pipe::MGPWireOp::BindStreamOutput, &fallback, sizeof(fallback),
                                    nullptr, 0, nullptr, 0, nullptr);
            }
        }
