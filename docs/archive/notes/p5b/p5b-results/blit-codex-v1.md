# P5b named blit migration

Commit `31b26a440de4d52c1afb746ed934247da8b8b145`, branch `p5b/blit-codex`, base `9606466a`; independent tree `/home/swung/w7/p5b-blit-codex`.

The client emit table now lowers BlitNamedFramebuffer through a scoped client-shadow read/draw binding override. It revalidates the existing classified fields, emits the original two handles/rectangles/mask/filter in the existing MGPBlit row, waits for server apply, then restores both bindings. BindingSlot versions make the following ordinary verb republish restored bindings. The server keeps using its existing bound backend entry point; no driver call runs on the client, and no pointer or extra side channel was added. Name zero uses the existing default-framebuffer handle. CONTRACT-P5B §6 item 13 records the integrator-approved ruling and its retirement when the lockstep barrier/process-shared access disappears.

Only split production sources changed. Pull G1 was not remeasured locally; the integrator owns the merged-head four-flavor gate. Slot partition is now A=2/B=49/C=20. Named clears are unchanged.

## Directed validation

- Split configure and full default ALL build succeeded at -j8; CTestTestfile.cmake exists. Logs `/home/swung/w7/p5b-blit-codex-{config-split,build-split,build-driver}.log`.
- Four new split cases all executed and passed: two cases × DirectGLES/DirectVulkan. They check separately unbound source/destination, preserved public read/draw names, an immediate ordinary clear before another bind, following ordinary blit, and default FBO as source/destination.
- RemoteEmitTable: 15/15 pass. Combined targeted CTest: 23 selected = 19 executed pass + four deliberately skipped monolith registrations of the split-only scenarios. Those skips are not counted as split execution. Log `p5b-blit-codex-targeted.log`.
- Dynamic retrace: exactly four selected, stock `/home/swung/w7/retrace_gate.py`, `MOBILEGL_TRANSPORT=inproc`, **default SEG_STAGE=32 MiB**. No adb, LFS fetch, repeated full gate or adversarial review.

| Trace | Backend | Result / next first blocker |
|---|---|---|
| improved-transparency-minecraft-26.3 | DirectVulkan | pass, SSIM 0.999914064 (threshold 0.995), zero private Fatal |
| minecraft-1.21.4-fabric-iris-bsl-in-world | DirectVulkan | pass, SSIM 0.997324199 (threshold 0.99), zero private Fatal |
| improved-transparency-minecraft-26.3 | DirectGLES | named blit crossed; Fatal{RingOverrun, "SEG_STAGE"}: 134217728-byte blob exceeds 33554432-byte segment |
| minecraft-1.21.4-fabric-iris-bsl-in-world | DirectGLES | named blit crossed; Fatal{UnmigratedEmulation, "generate-mipmap-storage"} |

Trace evidence: `/home/swung/w7/p5b-blit-codex-traces.log`, `/home/swung/w7/retrace-out/p5b-blit-codex/summary.json`, and per-case/backend `output/mobilegl.log`, `result.json`, images and outer logs below that output root. The trace fixtures were copied from hydrated pipe files for the run and restored to their original tracked pointers before commit.

The integrator owns a separate larger-stage improved-transparency run; it is not claimed here. The follow-up mip-storage fix is a separate commit/report. `git diff --check` clean, committer Swung0x48, no trailers, submodule links off at commit.
