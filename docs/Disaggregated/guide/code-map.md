# Code map

> Reference. For the documentation index see [`../README.md`](../README.md); for the directory layout and build options see [`../design/09-build-switches-counters.md`](../design/09-build-switches-counters.md) §16.

| Path | Role |
|---|---|
| `MobileGL/MG_Pipe/` | The interface itself: the call catalog `PipeCalls.def` and the other `.def` tables, the payload / handle / callback headers, `PipeApply`, `PipeRoute`, `generated/*.inc` (generated, committed to the tree) |
| `MobileGL/MG_Impl/Pipe/` | Frontend: Tracker, `PipeFill`, `SlotAllocator`, `CsoCache`, `CompositeResolver`, `ResourceTracker`, the per-family `*Emit.h` |
| `MobileGL/MG_Backend/MGPipe/` | `PipeInputs`: the state block the backend reads and the frontend pushes |
| `MobileGL/MG_Remote/` | Split-only (`MOBILEGL_BUILD_DISAGGREGATED`): `Protocol/` (control-plane schema), `Transport/` (control plane `ITransport` × data plane `ILink`), `Wire/` (record encoding and decoding), `Client/`, `Server/`; the per-phase wire contracts `CONTRACT-*.md` |
| `MobileGL/MG_Util/Metrics/PipeStats.{h,cpp}` | Boundary counters (`MOBILEGL_PIPE_STATS=1`) |
| `MobileGL/MG_Test/Pipe/`, `MG_Test/Wire/`, `MG_IntegrationTest/Harness/*Peek` | Catalog / tracker / emitter unit tests, wire and remote unit tests, white-box assertions |
| `scripts/` | Generators (`gen_pipe.py`, `gen_pipe_field_ownership.py`, `gen_pipe_dirty_surface.py`, `gen_protocol.py`), gate scripts (`symbol_report.py`, `check_include_closure.py`, `link_ratchet.py`, the G5 protected-region scripts, `check_doc_citations.py`), `ci/` |
| `tools/trace_replay/`, `tools/device_bench/` | Trace replay (including the TCP matrix runner), device clock pinning and paired A/B tooling |
