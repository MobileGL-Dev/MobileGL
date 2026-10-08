# P15 wait-class audit (draft for the user's review)

Commands are recorded and replayed asynchronously by default. A record makes the client wait only
because its row in `MG_Pipe/PipeCalls.def` has a wait class other than `kWaitNone`. Since e00db8e6
the client's per-verb barrier answer is exactly the wait class of the record its emitter publishes
(`MG_Pipe/VerbRecordOps.h`). This file lists every row that still waits, says why, and flags the
candidates for async. **No row is converted until the user has seen this table.** Each conversion
needs a red-once test.

The rule for keeping a wait:
- **true reply**: the client acts on an answer it cannot derive itself; or
- **present pacing**.

Anything else (a `kWaitApplied` row whose apply still reads a BARRIER_PULLED or residual field, or a
reply the client could derive) is a candidate.

## Measured hit rates (FCL, inproc, cpuhunt, `MOBILEGL_IPC_WAIT_STATS=1`)

Waits per frame in steady state, 600-frame windows:

| wait | vanilla 1.21.5 (Magma, Espryt) | BSL (Magma) |
|---|---|---|
| present credit | 1.00 | 1.00 |
| record `ResourceCreate` (blocking create) | 0.00-0.01 | 2-3 |
| record `ResourceCopyRegion` (glCopyImageSubData) | 0 | 3 |
| record `GenerateMipmap` | 0 | 3 |
| record `CopyFramebufferToTexture` (glCopyTexSubImage2D) | 0 | 1 |
| barriered verbs quiescing the applier at validate | 0 | 8 (CopyImageSubData 3, GenerateMipmap 3, CopyTexSubImage2D 1, ClientWaitSync 1) |
| follow-on quiesce `MGPipeNoteFrontendMutation` | 0 | 14-16 |
| follow-on quiesce `MGPipeValidateForVerb/residual` | 0 | 8 |
| ring space | 0 | 0 |

- Vanilla meets the target: the present credit is the only per-frame wait.
- On BSL, every barriered verb costs about three waits, not one: the record's own applied wait, the
  quiesce before the fill, and then a re-quiesce at each frontend mutation until the next
  unbarriered verb (`g_lastFillWasBarriered`). Making the three `kWaitApplied` verbs async removes
  all of these at once.

## Rows that wait

| row | class | why it waits today | FCL hits/frame | verdict |
|---|---|---|---|---|
| `Present` | kWaitPresent | present credit, taken before the encode | 1 | keep (pacing) |
| `GetCaps` | kWaitReply | the caps snapshot | 0 (startup) | keep (true reply) |
| `ResourceCreate` | kWaitReply | accept/decline feeds the client's publish latch; a create window defers it for some objects | BSL 2-3 | **candidate**: find which creates take the blocking path every frame on BSL and why the window refuses them |
| `ResourceRespecify` | kWaitReply | texture half until the object's create is accepted; already fire-and-forget once published (`MGPipeResourceRespecifyWantsItsReply`) | 0 | keep (relaxed per call already) |
| `MapPersistent` | kWaitReply | the client needs the mapping | 0 (startup) | keep (true reply) |
| `FenceStatus`, `FenceWait` | kWaitReply | GPU state the app asked for | BSL 1 (ClientWaitSync) | keep (true reply) |
| `QueryAvailable`, `QueryResult`, `QueryTimestamp` | kWaitReply | GPU results | 0 | keep (true reply) |
| `SetTextureParams` | kWaitReply | waits only before the texture's create is accepted; fire-and-forget after | 0 | keep (relaxed per call already) |
| `ResourceSubData` | kWaitReply | buffer half and wire texture half never wait (`MGPipeSubDataWantsItsReply`) | 0 | keep (relaxed per call already) |
| `ResourceReadback`, `GetTextureImage`, `ReadPixels` | kWaitReply | the bytes | 0 | keep (true reply); **note**: the verb table names the reply op for ReadPixels / GetTexImage, so a pack-buffer read (which publishes the kWaitNone `*ToBuffer` record) is still barriered at validate. A per-call answer would make PBO reads async |
| `SharedImage` | kWaitReply | AHB/dma-buf handle exchange (anland) | 0 | keep (true reply) |
| `GenerateMipmap` | kWaitApplied | its apply read `GetActiveTextureUnit` and the unit's binding slot from the residual block (PipeCalls.def / ClientSession.cpp); P13 W4b gave the record arm the texture by handle (`VerbMipRes`) | BSL 3 | **candidate**: check whether any backend path still reads the residual unit; if not, kWaitNone |
| `ResourceCopyRegion` | kWaitApplied | its apply read `GetTextureObject`, a BARRIER_PULLED row whose retiring phase was P7 (ID-118); P13 W4b resolves both endpoints by handle | BSL 3 | **candidate**: same check |
| `CopyFramebufferToTexture` | kWaitApplied | the read framebuffer and the destination were pulled; P13 W4b resolves the destination by handle (`VerbCopyTexDst`) and P13 W4c names framebuffers by handle | BSL 1 | **candidate**: same check |
| `SetStorageBlockBinding` | kWaitApplied | probes a registry | 0 | keep for now (not on any FCL path) |
| `BeginStreamOutput`, `EndStreamOutput`, `PauseStreamOutput`, `ResumeStreamOutput`, `BindStreamOutput` | kWaitApplied | XFB span accounting and the capture-binding shadow | 0 | keep for now (not on any FCL path) |
| `ApplierReset` | kWaitApplied | orders a make-current reset against the verbs after it | 0 (make-current) | keep |

Count note: the X rows in PipeCalls.def parse as 59 kWaitNone, 15 kWaitReply, 10 kWaitApplied and
1 kWaitPresent (85 rows).

## Proposed order (after the user's go-ahead)

1. `GenerateMipmap`, `ResourceCopyRegion`, `CopyFramebufferToTexture` to kWaitNone, each with a
   red-once test (a poison or verify build that aborts on a residual read under an unbarriered
   apply). BSL then loses 7 record waits, 7 validate quiesces and the 14-16 follow-on quiesces per
   frame.
2. `ResourceCreate` on BSL: find the 2-3 blocking creates per frame and why the create window
   declines them.
3. Pack-buffer `ReadPixels` / `GetTexImage`: a per-call barrier answer instead of the verb-level
   reply op.
