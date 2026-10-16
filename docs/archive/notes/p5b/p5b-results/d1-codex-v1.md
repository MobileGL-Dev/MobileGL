# d1 Codex handoff completion — 2026-09-16

Source d1 commits: `e5649396..fbe17d06` (base `37fc4fdb`); preserved from the clean Windows
worktree through a local bundle. Integrated with f1/i1/t2/r2 at `9606466a`.
No d1 implementation rewrite was needed. Existing nineteen slots emit draw_vbo; Linux
validation completes the missing evidence. Windows STATIC trace intersections are explicitly
not execution evidence, and their SetSwapInterval/FenceSync predictions do not describe the
actual retracer path.

## Validation

- Linux split build succeeded (clang, Release, ccache, -j8).
- Split unit: 2098/2098, set -e script progressed to integration; no Windows timeout debt.
- integration-split: 54/54, including 32 d1 additions; two pre-existing map-arm skips.
- No repeated full gate or review. Integrator runs four-lane quick checks on the combined head.
- Dynamic Minecraft census: 77 backend cases, 39 fixtures (iterationRP DirectVulkan only).
  All entries reached rendering or a non-draw first blocker. Results below are real inproc
  execution, not static predictions. Source tree was merged while the census ran, but the
  d1 binary was held unchanged until every entry completed.

Binary SHA256: `aeee247d77f56622fcb622fbdb7ad4a636d2f21268a858edc287572a6091df6a`.

Outcome counts: {'UnmigratedVerb, "BlitNamedFramebuffer"': 31, 'PASS': 28, 'UnmigratedVerb, "DispatchCompute"': 2, 'UnmigratedVerb, "GenerateMipmap"': 7, 'UnmigratedVerb, "BindImageTexture"': 1, 'RingOverrun, "SEG_STAGE"': 2, 'UnmigratedVerb, "MemoryBarrier"': 2, 'UnmigratedVerb, "ClearBufferfv"': 2, 'InitialBytesNotCarried, "resource_respecify"': 1, 'BarrierTimeout, "Present"': 1}

Evidence: `/home/swung/w7/retrace-out/p5b-d1-codex/summary.json`; each `actual_private_log`
points to its isolated retrace output directory. Console summaries are in
`/home/swung/w7/p5b-d1-codex-trace-logs/`.

## First blocker by fixture and backend

| Fixture | Backend | Result / first blocker | SSIM |
|---|---|---|---|
| improved-transparency-minecraft-26.3 | DirectGLES | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| improved-transparency-minecraft-26.3 | DirectVulkan | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.17-main-menu-854 | DirectGLES | PASS | 0.999962 |
| minecraft-1.17-main-menu-854 | DirectVulkan | PASS | 0.999961 |
| minecraft-1.21.1-neoforge-create-indirect-in-world | DirectGLES | UnmigratedVerb, "DispatchCompute" | — |
| minecraft-1.21.1-neoforge-create-indirect-in-world | DirectVulkan | UnmigratedVerb, "DispatchCompute" | — |
| minecraft-1.21.1-neoforge-create-instancing-in-world | DirectGLES | PASS | 0.999958 |
| minecraft-1.21.1-neoforge-create-instancing-in-world | DirectVulkan | PASS | 0.999952 |
| minecraft-1.21.11-main-menu | DirectGLES | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.11-main-menu | DirectVulkan | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-common-mods-in-world | DirectGLES | UnmigratedVerb, "GenerateMipmap" | — |
| minecraft-1.21.4-fabric-common-mods-in-world | DirectVulkan | UnmigratedVerb, "GenerateMipmap" | — |
| minecraft-1.21.4-fabric-common-mods-inventory | DirectGLES | UnmigratedVerb, "GenerateMipmap" | — |
| minecraft-1.21.4-fabric-common-mods-inventory | DirectVulkan | UnmigratedVerb, "GenerateMipmap" | — |
| minecraft-1.21.4-fabric-iris-bliss-in-world | DirectGLES | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-bliss-in-world | DirectVulkan | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-bsl-esc-menu-854 | DirectGLES | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-bsl-esc-menu-854 | DirectVulkan | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-bsl-in-world | DirectGLES | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-bsl-in-world | DirectVulkan | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-chocapic-v6-lite-in-world | DirectGLES | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-chocapic-v6-lite-in-world | DirectVulkan | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-complementary-reimagined-in-world | DirectGLES | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-complementary-reimagined-in-world | DirectVulkan | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-complementary-unbound-in-world | DirectGLES | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-complementary-unbound-in-world | DirectVulkan | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-derivative-main-d24.4.14-in-world | DirectGLES | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-derivative-main-d24.4.14-in-world | DirectVulkan | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-iterationrp-in-world | DirectVulkan | UnmigratedVerb, "BindImageTexture" | — |
| minecraft-1.21.4-fabric-iris-iterationt-in-world | DirectGLES | RingOverrun, "SEG_STAGE" | — |
| minecraft-1.21.4-fabric-iris-iterationt-in-world | DirectVulkan | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-iterationt-nodsa-in-world | DirectGLES | RingOverrun, "SEG_STAGE" | — |
| minecraft-1.21.4-fabric-iris-iterationt-nodsa-in-world | DirectVulkan | UnmigratedVerb, "GenerateMipmap" | — |
| minecraft-1.21.4-fabric-iris-makeup-ultrafast-in-world | DirectGLES | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-makeup-ultrafast-in-world | DirectVulkan | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-mellow-in-world | DirectGLES | UnmigratedVerb, "MemoryBarrier" | — |
| minecraft-1.21.4-fabric-iris-mellow-in-world | DirectVulkan | UnmigratedVerb, "MemoryBarrier" | — |
| minecraft-1.21.4-fabric-iris-nostalgia-in-world | DirectGLES | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-nostalgia-in-world | DirectVulkan | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-photon-v1.1-in-world | DirectGLES | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-photon-v1.1-in-world | DirectVulkan | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world | DirectGLES | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world | DirectVulkan | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-sundial-lite-in-world | DirectGLES | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-sundial-lite-in-world | DirectVulkan | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-super-duper-vanilla-in-world | DirectGLES | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-iris-super-duper-vanilla-in-world | DirectVulkan | UnmigratedVerb, "BlitNamedFramebuffer" | — |
| minecraft-1.21.4-fabric-journeymap-in-world-normal-world | DirectGLES | PASS | 0.999979 |
| minecraft-1.21.4-fabric-journeymap-in-world-normal-world | DirectVulkan | PASS | 0.999979 |
| minecraft-1.21.4-fabric-journeymap-in-world | DirectGLES | PASS | 0.999984 |
| minecraft-1.21.4-fabric-journeymap-in-world | DirectVulkan | PASS | 0.999984 |
| minecraft-1.21.4-fabric-modernui-inventory-normal-world | DirectGLES | UnmigratedVerb, "ClearBufferfv" | — |
| minecraft-1.21.4-fabric-modernui-inventory-normal-world | DirectVulkan | UnmigratedVerb, "ClearBufferfv" | — |
| minecraft-1.21.4-fabric-modernui-inventory | DirectGLES | UnmigratedVerb, "GenerateMipmap" | — |
| minecraft-1.21.4-fabric-modernui-inventory | DirectVulkan | UnmigratedVerb, "GenerateMipmap" | — |
| minecraft-1.21.4-fabric-rei-inventory-normal-world | DirectGLES | PASS | 0.999988 |
| minecraft-1.21.4-fabric-rei-inventory-normal-world | DirectVulkan | PASS | 0.999988 |
| minecraft-1.21.4-fabric-rei-inventory | DirectGLES | PASS | 0.99999 |
| minecraft-1.21.4-fabric-rei-inventory | DirectVulkan | PASS | 0.99999 |
| minecraft-1.21.4-fabric-sodium-in-world | DirectGLES | PASS | 0.999986 |
| minecraft-1.21.4-fabric-sodium-in-world | DirectVulkan | PASS | 0.999986 |
| minecraft-1.21.4-fabric-xaero-minimap-in-world-normal-world | DirectGLES | PASS | 0.999992 |
| minecraft-1.21.4-fabric-xaero-minimap-in-world-normal-world | DirectVulkan | PASS | 0.999992 |
| minecraft-1.21.4-fabric-xaero-minimap-in-world | DirectGLES | PASS | 0.999999 |
| minecraft-1.21.4-fabric-xaero-minimap-in-world | DirectVulkan | PASS | 0.999999 |
| minecraft-1.21.4-fabric-xaero-world-map-in-world-normal-world | DirectGLES | PASS | 0.999995 |
| minecraft-1.21.4-fabric-xaero-world-map-in-world-normal-world | DirectVulkan | PASS | 0.999995 |
| minecraft-1.21.4-fabric-xaero-world-map-in-world | DirectGLES | PASS | 1.0 |
| minecraft-1.21.4-fabric-xaero-world-map-in-world | DirectVulkan | PASS | 1.0 |
| minecraft-1.21.4-in-world | DirectGLES | PASS | 0.999976 |
| minecraft-1.21.4-in-world | DirectVulkan | PASS | 0.999976 |
| minecraft-1.21.4-main-menu | DirectGLES | PASS | 0.997009 |
| minecraft-1.21.4-main-menu | DirectVulkan | PASS | 0.997009 |
| minecraft-1.21.4-rd12-odinlite-in-world | DirectGLES | InitialBytesNotCarried, "resource_respecify" | — |
| minecraft-1.21.4-rd12-odinlite-in-world | DirectVulkan | BarrierTimeout, "Present" | — |
| minecraft-1.21.4-startup | DirectGLES | PASS | 1.0 |
| minecraft-1.21.4-startup | DirectVulkan | PASS | 1.0 |

## Rulings and follow-up

Plain base-vertex-zero indexed draws dispatch to DrawElements, rather than the optional
DrawElementsBaseVertex backend entry, preserving the monolith path on ES 3.1/ANGLE. This is
the d1 implementation's documented correction to CONTRACT-P5B's original sink pseudocode.
Client-index multi-draw remains the named `+CLIENT_INDICES` refusal pending P8; it is not a
Minecraft trace blocker. GuiBatch was not in the standalone d1 split registration because
its next MemoryBarrier belongs to i1.

Vanilla and sodium target traces already render (both backends); iris-bsl and
improved-transparency now stop at BlitNamedFramebuffer. The integrator owns that migration.
Sync migration is a separate next commit and contract appendix. rd12 DirectGLES hits
InitialBytesNotCarried/resource_respecify; DirectVulkan times out at Present. Neither result
is the historical dev scudo crash.
