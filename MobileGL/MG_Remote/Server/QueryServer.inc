// Server-owned query identities and actual backend query/fence results.
    ServerVerbSink::QueryEntry& ServerVerbSink::FindQuery(MG_Pipe::MGPipeHandle handle) {
        const auto found = m_queries.find(handle.Slot);
        if (!handle.Slot || found == m_queries.end() || !found->second.Live || found->second.Gen != handle.Gen)
            Wire::WireProtocolFatal("Query.handle", "missing, destroyed or stale query handle");
        return found->second;
    }

    Bool ServerVerbSink::OnQueryCreate(const MG_Pipe::MGPQueryDesc& desc) {
        if (!Table("QueryCreate")) return false;
        if (!desc.Query.Slot || desc.Stream != 0)
            Wire::WireProtocolFatal("QueryCreate.identity", "reserved handle or unsupported stream");
        switch (desc.Kind) {
        case GL_TIME_ELAPSED: case GL_TIMESTAMP: case GL_SAMPLES_PASSED:
        case GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN: case GL_PRIMITIVES_GENERATED: break;
        default: Wire::WireProtocolFatal("QueryCreate.Kind", "unknown query target");
        }
        const Bool seen = m_queries.find(desc.Query.Slot) != m_queries.end();
        auto& entry = m_queries[desc.Query.Slot];
        if (entry.Live || (seen && desc.Query.Gen <= entry.Gen))
            Wire::WireProtocolFatal("QueryCreate.handle", "duplicate or stale query generation");
        entry = QueryEntry{};
        entry.Gen = desc.Query.Gen;
        entry.Kind = desc.Kind;
        entry.Live = true;
        return true;
    }

    Bool ServerVerbSink::OnQueryBegin(const MG_Pipe::MGPQueryDesc& desc) {
        auto& entry = FindQuery(desc.Query);
        const auto* table = Table("QueryBegin");
        if (!table) return false;
        if (desc.Stream || desc.Kind != entry.Kind || entry.Active || entry.Native)
            Wire::WireProtocolFatal("QueryBegin.state", "query target or lifecycle mismatch");
        const auto& gl = table->GL;
        switch (entry.Kind) {
        case GL_TIME_ELAPSED:
            if (!gl.BeginTimeElapsedQuery) return false;
            entry.Native = gl.BeginTimeElapsedQuery();
            break;
        case GL_SAMPLES_PASSED:
            if (!gl.BeginOcclusionQuery) return false;
            entry.Native = gl.BeginOcclusionQuery();
            break;
        case GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN:
        case GL_PRIMITIVES_GENERATED:
            if (!gl.BeginXfbPrimitivesQuery) return false;
            entry.Native = gl.BeginXfbPrimitivesQuery(entry.Kind == GL_PRIMITIVES_GENERATED);
            break;
        default: Wire::WireProtocolFatal("QueryBegin.Kind", "target is not a begin/end query");
        }
        // A failed native allocation remains a real, unreadable query. Returning a
        // null proxy to the frontend would silently select CPU primitive accounting.
        entry.Active = true;
        return true;
    }

    void ServerVerbSink::EndNativeQuery(QueryEntry& entry) {
        if (!entry.Active || !m_backend) return;
        const auto& gl = m_backend->GetBackendFunctions().GL;
        if (entry.Native) {
            switch (entry.Kind) {
            case GL_TIME_ELAPSED: if (gl.EndTimeElapsedQuery) gl.EndTimeElapsedQuery(entry.Native); break;
            case GL_SAMPLES_PASSED: if (gl.EndOcclusionQuery) gl.EndOcclusionQuery(entry.Native); break;
            case GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN:
            case GL_PRIMITIVES_GENERATED:
                if (gl.EndXfbPrimitivesQuery) gl.EndXfbPrimitivesQuery(entry.Native);
                break;
            default: break;
            }
        }
        entry.Active = false;
    }

    Bool ServerVerbSink::OnQueryEnd(const MG_Pipe::MGPQueryDesc& desc) {
        auto& entry = FindQuery(desc.Query);
        const auto* table = Table("QueryEnd");
        if (!table) return false;
        if (desc.Stream || desc.Kind != entry.Kind || !entry.Active)
            Wire::WireProtocolFatal("QueryEnd.state", "query target or lifecycle mismatch");
        EndNativeQuery(entry);
        if (entry.Native && table->GL.FenceSync) entry.Completion = table->GL.FenceSync();
        return true;
    }

    Bool ServerVerbSink::OnQueryCounter(const MG_Pipe::MGPQueryDesc& desc) {
        auto& entry = FindQuery(desc.Query);
        const auto* table = Table("QueryCounter");
        if (!table || !table->GL.QueryCounterTimestamp) return false;
        if (desc.Stream || desc.Kind != GL_TIMESTAMP || entry.Kind != GL_TIMESTAMP || entry.Native || entry.Active)
            Wire::WireProtocolFatal("QueryCounter.state", "query target or lifecycle mismatch");
        entry.Native = table->GL.QueryCounterTimestamp();
        if (entry.Native && table->GL.FenceSync) entry.Completion = table->GL.FenceSync();
        return true;
    }

    Bool ServerVerbSink::QueryGpuComplete(QueryEntry& entry, Bool wait) {
        if (!m_backend || !entry.Native || entry.Active) return false;
        const auto& gl = m_backend->GetBackendFunctions().GL;
        if (!entry.Completion || !gl.ClientWaitSync || !gl.IsQueryResultAvailable) return false;
        // Polling may submit the pending batch, but never waits for GPU completion.
        // A blocking result explicitly submits/waits, including present-less workloads.
        const GLenum completion = gl.ClientWaitSync(entry.Completion, GL_SYNC_FLUSH_COMMANDS_BIT,
                                                     wait ? GL_TIMEOUT_IGNORED : 0);
        if (completion != GL_ALREADY_SIGNALED && completion != GL_CONDITION_SATISFIED) return false;
        return gl.IsQueryResultAvailable(entry.Native);
    }

    Bool ServerVerbSink::OnQueryAvailable(const MG_Pipe::MGPHandleOnly& request, Uint32& result) {
        if (request.Kind != static_cast<Uint32>(MG_Pipe::MGPipeKind::Query))
            Wire::WireProtocolFatal("Query.Kind", "expected query namespace");
        auto& entry = FindQuery(request.Handle);
        result = QueryGpuComplete(entry, false) ? 1 : 0;
        return true;
    }

    Bool ServerVerbSink::OnQueryResult(const MG_Pipe::MGPQueryResultRequest& request, Wire::QueryResultReply& reply) {
        if (request.Wait > 1) Wire::WireProtocolFatal("QueryResult.Wait", "wait must be boolean");
        auto& entry = FindQuery(request.Query);
        const auto* table = Table("QueryResult");
        if (!table || !table->GL.GetQueryResult64) return false;
        reply = {};
        if (QueryGpuComplete(entry, request.Wait != 0))
            reply.Produced = table->GL.GetQueryResult64(entry.Native, request.Wait != 0, &reply.Value) ? 1 : 0;
        return true;
    }

    Bool ServerVerbSink::OnQueryDestroy(const MG_Pipe::MGPHandleOnly& request) {
        if (request.Kind != static_cast<Uint32>(MG_Pipe::MGPipeKind::Query))
            Wire::WireProtocolFatal("Query.Kind", "expected query namespace");
        auto& entry = FindQuery(request.Handle);
        const auto* table = Table("QueryDestroy");
        if (!table) return false;
        EndNativeQuery(entry);
        if (entry.Native && table->GL.DeleteBackendQuery) table->GL.DeleteBackendQuery(entry.Native);
        if (entry.Completion && table->GL.DeleteSync) table->GL.DeleteSync(entry.Completion);
        entry.Native = nullptr;
        entry.Completion = nullptr;
        entry.Live = false;
        return true;
    }

    Bool ServerVerbSink::OnQueryTimestamp(const MG_Pipe::MGPTimestampRequest& request, Int64& result) {
        if (request.Reserved) Wire::WireProtocolFatal("QueryTimestamp.Reserved", "reserved field is not zero");
        const auto* table = Table("QueryTimestamp");
        if (!table || !table->GL.QueryCounterTimestamp || !table->GL.GetQueryResult64 ||
            !table->GL.DeleteBackendQuery || !table->GL.FenceSync || !table->GL.DeleteSync) return false;
        QueryEntry entry{};
        entry.Native = table->GL.QueryCounterTimestamp();
        if (entry.Native) entry.Completion = table->GL.FenceSync();
        Uint64 timestamp = 0;
        const Bool produced = QueryGpuComplete(entry, true) &&
            table->GL.GetQueryResult64(entry.Native, true, &timestamp);
        if (entry.Native) table->GL.DeleteBackendQuery(entry.Native);
        if (entry.Completion) table->GL.DeleteSync(entry.Completion);
        if (produced) result = static_cast<Int64>(timestamp);
        return produced;
    }

    void ServerVerbSink::ReleaseQueries() {
        if (m_backend) {
            const auto& gl = m_backend->GetBackendFunctions().GL;
            for (auto& [slot, entry] : m_queries) {
                if (!entry.Live) continue;
                EndNativeQuery(entry);
                if (entry.Native && gl.DeleteBackendQuery) gl.DeleteBackendQuery(entry.Native);
                if (entry.Completion && gl.DeleteSync) gl.DeleteSync(entry.Completion);
            }
        }
        m_queries.clear();
    }
