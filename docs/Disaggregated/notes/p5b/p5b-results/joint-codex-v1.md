# P5b joint Codex evidence — 2026-09-16

<!-- BEGIN CODEX CENSUS -->
## Inproc census — merged head, lane default 32 MiB / traces explicit 256 MiB

Head: `348d22a4b9c15cc571af840dedc4b4edc8c53dbb`. Evidence directory: `/home/swung/w7/p5b-final-census-348d22a4`.

Frozen libMobileGL SHA256: `50fecb248368a4de02d7f51d4229c4ebac555b3bea18d3a22398c1247d2a28c4`. `identity.json` records the head, all frozen executables/scripts, hashes and explicit profiles.

The single complete integration census is the final gate’s inproc debt arm. No second full
inproc ctest run was used for these numbers. Each case has its own fresh log; static fixture
intersections are not execution evidence. Missing fixtures/results, zero cases and timeouts
are not passes.

### Integration lane — stage 32 MiB

Final **1267** exact manifest entries: **62 aborted / 191 failed / 811 passed / 203 skipped**.

Baseline c0b is 432 passed / 185 skipped / 505 aborted / 27 failed (1149 entries).
Exact-name join: 1149 retained, 118 added, 0 removed. Previously passing entries now failed/aborted: 0.

| c0b status | Final status | Matched entries |
|---|---|---:|
| aborted | aborted | 58 |
| aborted | failed | 164 |
| aborted | passed | 265 |
| aborted | skipped | 18 |
| failed | failed | 26 |
| failed | passed | 1 |
| passed | passed | 432 |
| skipped | skipped | 185 |

All exact names, including additions/removals, are in `lane-transitions.md` and
`lane-transitions.json` beside `identity.json`. Final first-blocker distribution:

| Outcome / first blocker | Entries |
|---|---:|
| passed | 811 |
| skipped | 203 |
| failed | 191 |
| UnmigratedEmulation, "texture-remint-pull" | 46 |
| UnmigratedVerb, "CopyImageSubData+RENDERBUFFER" | 5 |
| UnmigratedVerb, "ClearNamedFramebufferfv+UNBOUND" | 3 |
| UnmigratedVerb, "DeleteTransformFeedback" | 2 |
| UnmigratedVerb, "MultiDrawElementsBaseVertex+CLIENT_INDICES" | 2 |
| UnimplementedWritebackWait | 1 |
| UnmigratedVerb, "ClearNamedFramebufferfi+UNBOUND" | 1 |
| UnmigratedVerb, "ClearNamedFramebufferiv+UNBOUND" | 1 |
| UnmigratedVerb, "ClearNamedFramebufferuiv+UNBOUND" | 1 |

### Complete trace census — stage 256 MiB

**79** backend entries: **6 aborted / 1 failed / 72 passed**. This inventory includes OpenRA and honors each fixture’s declared ci_backends.

Production staging remains 32 MiB. The 256 MiB profile is authorized for real workloads
with a 128 MiB upload; a profile-assisted success is not evidence that default-capacity debt
was fixed. Standalone d1’s stage32 baseline had 28 passes / 49 failures over 77 Minecraft
entries, including SEG_STAGE failures on GLES iterationt and iterationt-nodsa. r1’s retirement
wait fix and a larger capacity address different conditions and are not merged in attribution.

| Fixture | Backend | Final outcome / first blocker | SSIM |
|---|---|---|---|
| OpenRA | DirectGLES | passed | 1.0 |
| OpenRA | DirectVulkan | passed | 1.0 |
| improved-transparency-minecraft-26.3 | DirectGLES | passed | 1.0 |
| improved-transparency-minecraft-26.3 | DirectVulkan | passed | 0.999914064 |
| minecraft-1.17-main-menu-854 | DirectGLES | passed | 0.9999616 |
| minecraft-1.17-main-menu-854 | DirectVulkan | passed | 0.99996113 |
| minecraft-1.21.1-neoforge-create-indirect-in-world | DirectGLES | UnmigratedEmulation, "texture-remint-pull" | — |
| minecraft-1.21.1-neoforge-create-indirect-in-world | DirectVulkan | failed | — |
| minecraft-1.21.1-neoforge-create-instancing-in-world | DirectGLES | passed | 0.999957572 |
| minecraft-1.21.1-neoforge-create-instancing-in-world | DirectVulkan | passed | 0.99995247 |
| minecraft-1.21.11-main-menu | DirectGLES | passed | 0.993474922 |
| minecraft-1.21.11-main-menu | DirectVulkan | passed | 0.99347492 |
| minecraft-1.21.4-fabric-common-mods-in-world | DirectGLES | passed | 0.999947917 |
| minecraft-1.21.4-fabric-common-mods-in-world | DirectVulkan | passed | 0.999947917 |
| minecraft-1.21.4-fabric-common-mods-inventory | DirectGLES | passed | 0.999996158 |
| minecraft-1.21.4-fabric-common-mods-inventory | DirectVulkan | passed | 0.999996158 |
| minecraft-1.21.4-fabric-iris-bliss-in-world | DirectGLES | passed | 0.998057535 |
| minecraft-1.21.4-fabric-iris-bliss-in-world | DirectVulkan | passed | 0.997986272 |
| minecraft-1.21.4-fabric-iris-bsl-esc-menu-854 | DirectGLES | InitialBytesNotCarried, "resource_respecify" | — |
| minecraft-1.21.4-fabric-iris-bsl-esc-menu-854 | DirectVulkan | passed | 0.998402027 |
| minecraft-1.21.4-fabric-iris-bsl-in-world | DirectGLES | passed | 0.997496023 |
| minecraft-1.21.4-fabric-iris-bsl-in-world | DirectVulkan | passed | 0.997324199 |
| minecraft-1.21.4-fabric-iris-chocapic-v6-lite-in-world | DirectGLES | passed | 0.999992296 |
| minecraft-1.21.4-fabric-iris-chocapic-v6-lite-in-world | DirectVulkan | passed | 0.99575325 |
| minecraft-1.21.4-fabric-iris-complementary-reimagined-in-world | DirectGLES | passed | 0.999503181 |
| minecraft-1.21.4-fabric-iris-complementary-reimagined-in-world | DirectVulkan | passed | 0.99929651 |
| minecraft-1.21.4-fabric-iris-complementary-unbound-in-world | DirectGLES | passed | 0.999478525 |
| minecraft-1.21.4-fabric-iris-complementary-unbound-in-world | DirectVulkan | passed | 0.999244084 |
| minecraft-1.21.4-fabric-iris-derivative-main-d24.4.14-in-world | DirectGLES | UnmigratedEmulation, "texture-remint-pull" | — |
| minecraft-1.21.4-fabric-iris-derivative-main-d24.4.14-in-world | DirectVulkan | passed | 0.996658574 |
| minecraft-1.21.4-fabric-iris-iterationrp-in-world | DirectVulkan | passed | 0.995833421 |
| minecraft-1.21.4-fabric-iris-iterationt-in-world | DirectGLES | passed | 0.997752215 |
| minecraft-1.21.4-fabric-iris-iterationt-in-world | DirectVulkan | passed | 0.999198504 |
| minecraft-1.21.4-fabric-iris-iterationt-nodsa-in-world | DirectGLES | passed | 0.995919679 |
| minecraft-1.21.4-fabric-iris-iterationt-nodsa-in-world | DirectVulkan | passed | 0.99864077 |
| minecraft-1.21.4-fabric-iris-makeup-ultrafast-in-world | DirectGLES | passed | 0.99542552 |
| minecraft-1.21.4-fabric-iris-makeup-ultrafast-in-world | DirectVulkan | passed | 0.996629722 |
| minecraft-1.21.4-fabric-iris-mellow-in-world | DirectGLES | passed | 0.999822891 |
| minecraft-1.21.4-fabric-iris-mellow-in-world | DirectVulkan | passed | 0.999312266 |
| minecraft-1.21.4-fabric-iris-nostalgia-in-world | DirectGLES | passed | 0.999988313 |
| minecraft-1.21.4-fabric-iris-nostalgia-in-world | DirectVulkan | passed | 0.999983572 |
| minecraft-1.21.4-fabric-iris-photon-v1.1-in-world | DirectGLES | passed | 0.999135212 |
| minecraft-1.21.4-fabric-iris-photon-v1.1-in-world | DirectVulkan | passed | 0.998781357 |
| minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world | DirectGLES | UnmigratedEmulation, "texture-remint-pull" | — |
| minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world | DirectVulkan | passed | 0.998529629 |
| minecraft-1.21.4-fabric-iris-sundial-lite-in-world | DirectGLES | passed | 0.996300657 |
| minecraft-1.21.4-fabric-iris-sundial-lite-in-world | DirectVulkan | passed | 0.996284254 |
| minecraft-1.21.4-fabric-iris-super-duper-vanilla-in-world | DirectGLES | passed | 0.999643966 |
| minecraft-1.21.4-fabric-iris-super-duper-vanilla-in-world | DirectVulkan | passed | 0.999439052 |
| minecraft-1.21.4-fabric-journeymap-in-world-normal-world | DirectGLES | passed | 0.99997899 |
| minecraft-1.21.4-fabric-journeymap-in-world-normal-world | DirectVulkan | passed | 0.99997899 |
| minecraft-1.21.4-fabric-journeymap-in-world | DirectGLES | passed | 0.999984122 |
| minecraft-1.21.4-fabric-journeymap-in-world | DirectVulkan | passed | 0.999984122 |
| minecraft-1.21.4-fabric-modernui-inventory-normal-world | DirectGLES | passed | 0.999997315 |
| minecraft-1.21.4-fabric-modernui-inventory-normal-world | DirectVulkan | passed | 0.999997292 |
| minecraft-1.21.4-fabric-modernui-inventory | DirectGLES | passed | 0.999996645 |
| minecraft-1.21.4-fabric-modernui-inventory | DirectVulkan | passed | 0.999996645 |
| minecraft-1.21.4-fabric-rei-inventory-normal-world | DirectGLES | passed | 0.99998801 |
| minecraft-1.21.4-fabric-rei-inventory-normal-world | DirectVulkan | passed | 0.99998801 |
| minecraft-1.21.4-fabric-rei-inventory | DirectGLES | passed | 0.999989912 |
| minecraft-1.21.4-fabric-rei-inventory | DirectVulkan | passed | 0.999989912 |
| minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | passed | 0.999985623 |
| minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | passed | 0.999986092 |
| minecraft-1.21.4-fabric-xaero-minimap-in-world-normal-world | DirectGLES | passed | 0.999991832 |
| minecraft-1.21.4-fabric-xaero-minimap-in-world-normal-world | DirectVulkan | passed | 0.999991832 |
| minecraft-1.21.4-fabric-xaero-minimap-in-world | DirectGLES | passed | 0.999999456 |
| minecraft-1.21.4-fabric-xaero-minimap-in-world | DirectVulkan | passed | 0.999999456 |
| minecraft-1.21.4-fabric-xaero-world-map-in-world-normal-world | DirectGLES | passed | 0.999995376 |
| minecraft-1.21.4-fabric-xaero-world-map-in-world-normal-world | DirectVulkan | passed | 0.999995376 |
| minecraft-1.21.4-fabric-xaero-world-map-in-world | DirectGLES | passed | 0.999999823 |
| minecraft-1.21.4-fabric-xaero-world-map-in-world | DirectVulkan | passed | 0.999999823 |
| minecraft-1.21.4-in-world | DirectGLES | passed | 0.999976222 |
| minecraft-1.21.4-in-world | DirectVulkan | passed | 0.999976105 |
| minecraft-1.21.4-main-menu | DirectGLES | passed | 0.997008676 |
| minecraft-1.21.4-main-menu | DirectVulkan | passed | 0.99700863 |
| minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | InitialBytesNotCarried, "resource_respecify" | — |
| minecraft-1.21.4-rd12-odinlite-in-world | DirectVulkan | BarrierTimeout, "Present" | — |
| minecraft-1.21.4-startup | DirectGLES | passed | 0.999999662 |
| minecraft-1.21.4-startup | DirectVulkan | passed | 0.999999662 |

`trace-transitions.json` maps all names to standalone d1’s first blocker and carries both
profile sizes. Every result links its actual private log and result.json in the isolated
trace output directory. A new first blocker counts as progress, never as rendered success.

<!-- END CODEX CENSUS -->

<!-- BEGIN CODEX HOST DOCS -->
## Host gate and documentation — pending final replay/device evidence

The one host gate is pinned to `348d22a4b9c15cc571af840dedc4b4edc8c53dbb`.
Authoritative directory: `/home/swung/w7/p5b-final-host-348d22a4/`;
`head.txt`, `libraries.sha256`, `status.tsv`, complete per-lane JUnit,
`summary-counts.json` and `summary-counts.md` distinguish selection from actual passes/skips.
The completed split lane is 107 selected = 105 passed + 2 skipped, zero failures.
The host replay runner is still in flight; no complete verdict is asserted here.

A runner execution anomaly is retained separately: appending an archive command while Bash
was reading the live runner produced one `-test-dir: command not found` line (original session
87762; outer shell lacked set-e). It is not classified as a required-test failure. The verify
and audit lanes have complete independent JUnit and recorded step rc=0; E2 then ran normally.
The E1/E3 private logs were already copied successfully to `negative-private-logs` manually.
Final required-step inventory, JUnit counts and replay counts must show no omitted step;
a final shell exit alone is not the proof. No repeated full gate is requested.

Three tree documents are prepared in independent `/home/swung/w7/p5b-docs-final-codex`, branch
`p5b/docs-final-codex`, based on `348d22a4`. They separate package evidence (§32), the one host
gate and combined census (§33), the one phase review plus targeted repairs (§34), and the final
Redmi exit/four-arm measurements (§35). Default stage remains 32 MiB; real-workload trace and
all four device arms explicitly use a 256 MiB profile. No final device/source identity or
successful P5b exit is filled in before the integrator sends DEVICE_DONE and sourcehead.

Documentation commit: pending final host/census/targeted-closure/device evidence.
<!-- END CODEX HOST DOCS -->
