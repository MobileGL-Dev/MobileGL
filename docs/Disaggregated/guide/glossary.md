# Terms

> Reference. For the documentation index see [`../README.md`](../README.md).

| Term | Meaning |
|---|---|
| frontend / backend | frontend = `MG_State` + `MG_Impl`, handling the application's GL calls; backend = `MG_Backend`, calling the driver for real |
| Espryt / Magma | The two backends: Espryt goes through OpenGL ES (DirectGLES), Magma through Vulkan (DirectVulkan) |
| client / server | The frontend role / backend role after the split; they can be two threads, two processes or two machines |
| monolith / inproc / spawn | The three topologies: single thread direct calls / two threads in one process / two processes (optionally across machines) |
| pull / push | The backend reads the frontend's state itself (the old way) / the frontend pushes state to the backend (MGPipe) |
| verb | A command that makes the backend actually do work: draw, dispatch, clear, blit, readback, XFB, query, texture operations. State is pushed only before a verb |
| handle `{slot, gen}` | An object's identity: a client-allocated slot plus the reuse generation; the server never sends it back |
| CSO | Constant state object (render state, vertex format, sampler, sampler view, shader): the client deduplicates by content, the server caches by handle |
| control plane / data plane | The connection carrying the handshake and rare control messages / the channel carrying command records and bulk data; each can choose shared memory or a byte stream, local or TCP |
| lockstep / run-ahead | The client waits for the server after every command / the client sends and moves on, waiting only for commands that need an answer |
| barrier tax | The extra per-thread CPU cost of splitting into two threads (split − push) |
| device-lost | The state the client enters after the server crashes or disconnects: GL calls become no-ops and swap buffers returns "context lost" instead of crashing or hanging |
| class A / B / C | Slots in the backend function table: A answers locally, B becomes a record, C is not migrated yet and is refused by name |
| Track V / Track H | Migration of value-class read points (a whole block crosses the wire) / migration of object-class read points (object pointers become handles) |
| G1 / G2 / G5 / G14 | Verification gates: single-process build binary byte-identical / pull and push have the same test-name set / protected backend functions byte-identical / test names only grow |
| red-once (R-16) | Every gate must really have run red once, proving it can fail for the problem it is meant to catch |
| `MGGen` | The server's private "I rebuilt the driver object" epoch; it never crosses the wire and is kept strictly separate from the handle generation |
