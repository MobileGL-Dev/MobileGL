# P5 c1g v1 — ID-67 make-current control correction

Commit: `c9878d69f3e112bc4ac22487239c6f505e773c01`.

## Change

Only `MobileGL/MG_Test/Wire/RemoteClientControls.inc` changed. The former
`RepeatedMakeCurrentAdoptsRepublishedCapsWithoutAPumpOrPresent` control is split into:

- `AnIdenticalRepeatedMakeCurrentRepublishesNothing`: after the initial make-current, an
  identical `(dpy, draw, read, ctx)` repeat leaves the caps-mirror generation unchanged.
  The test double and control-plane API expose no pending-snapshot count, so this case uses
  ID-67's permitted generation assertion; the server's bind-once path publishes nothing.
- `ADifferentTupleMakeCurrentIsAdoptedWithoutAPumpOrPresent`: after changing the double's
  `MaxComputeWorkGroupCount[0]`, a make-current with a different context handle (same valid
  display/surface) immediately advances the mirror generation and changes the public
  `GetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0, ...)` answer from 113 to 227. There is
  no explicit pump and no Present in the test.

Case (b) is live, not skipped: this head republishes on a real different-tuple native bind,
and the client adopts the snapshot during that make-current.

No production code remains changed, so G1 is unaffected by construction.

## Red once

- Case (a): temporarily changed the production early return from
  `if (!ok || !outcome.boundNatively)` to `if (!ok)`, forcing an identical repeat to
  republish. The named test alone went red:
  `[  FAILED  ] RemoteClientControls.AnIdenticalRepeatedMakeCurrentRepublishesNothing`,
  child `exited 89`. The one-line perturbation was reverted.
- Case (b): temporarily suppressed the `PublishCapsSnapshot()` block on a different tuple
  by making its condition false. The named test alone went red:
  `[  FAILED  ] RemoteClientControls.ADifferentTupleMakeCurrentIsAdoptedWithoutAPumpOrPresent`,
  child `exited 95`. The one-line perturbation was reverted.

## Acceptance

Required `build-split` flags were
`MOBILEGL_PIPE_PUSH=ON`, `MOBILEGL_BUILD_DISAGGREGATED=ON`, and
`MOBILEGL_BUILD_DISAGGREGATED_INPROC=ON`; configure and final build exited 0.

`ctest -L unit --output-on-failure`: **2057/2057 passed, 0 failed** (10 documented
pre-existing skips listed by CTest; no package skip).

Final `ctest -R RemoteClientControls --output-on-failure` output:

```text
Test project /home/swung/w7/p5-c1g/build-split
 1/18 RemoteClientControls.TeardownRefusesClearOnALiveSession ......................... Passed
 2/18 RemoteClientControls.TeardownRefusesDrawArraysOnALiveSession .................... Passed
 3/18 RemoteClientControls.TeardownRefusesReadPixelsOnALiveSession .................... Passed
 4/18 RemoteClientControls.TeardownRefusesBlitFramebufferOnALiveSession ............... Passed
 5/18 RemoteClientControls.TeardownRefusesPresentOnALiveSession ....................... Passed
 6/18 RemoteClientControls.ReadPixelsPutsTheTightExtentOnTheWire ...................... Passed
 7/18 RemoteClientControls.ShortOkReadPixelsReplyRefusesByName ........................ Passed
 8/18 RemoteClientControls.ErrorReadPixelsReplyRefusesByName .......................... Passed
 9/18 RemoteClientControls.ResourceRespecifyErrorIsNotADecline ........................ Passed
10/18 RemoteClientControls.MapPersistentErrorIsNotADecline ............................ Passed
11/18 RemoteClientControls.AnIdenticalRepeatedMakeCurrentRepublishesNothing ........... Passed
12/18 RemoteClientControls.ADifferentTupleMakeCurrentIsAdoptedWithoutAPumpOrPresent ... Passed
13/18 RemoteClientControls.BounceReadPixelsShortReplyRefusesBeforeScatter ............. Passed
14/18 RemoteClientControls.DeleteTexturesAdvancesTheWireOrdinal ....................... Passed
15/18 RemoteClientControls.BoundPackBufferOffsetReadRefusesByName ..................... Passed
16/18 RemoteClientControls.ServerRoleRespecifyDoesNotEmitClientInitialBytes ........... Passed
17/18 RemoteClientControls.HeaderOnlyDynamicStateCrossesWithoutBlobMissing ............ Passed
18/18 RemoteClientControls.ServerRoleFlushDoesNotRunTheClientSubDataFollowup .......... Passed
100% tests passed, 0 tests failed out of 18
```

Final source diff is one test file, `+33/-4`; production diff is empty.
