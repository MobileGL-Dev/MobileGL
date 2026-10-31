# Measured results (index)

> This page holds only conclusions and pointers; the full tables, commands, protocols, and how to read them are in each phase's "measured results" section (originals in git history). `MEASUREMENTS.md §N` in code and old notes maps to the phase in the "§" column below (subsection numbering is preserved there as-is).

**Basis**: performance is recorded only, never gated; look at **per-thread CPU time**, not whole-machine frame rate. Performance comparisons use only one phone (Redmi `2f7cbe2e`, Adreno 830), with pinned clocks, the same thermal window, and the two arms run alternately ([`guide/pin-verification-2026-09-07.md`](guide/pin-verification-2026-09-07.md)).

## Key conclusions

- **Cost of the interface itself**: in the monolith, changing to "frontend pushes, backend receives" costs about 6–12% more CPU (P2), which was accepted; scenes with dense vertex-array switching are higher (P3a, +27–30%).
- **Split into two threads**: at first only 7–13 fps in the game; after optimization 103–106 fps (P5d); after the client runs ahead, under heavy load (render distance 32) it matches single-threaded (P5e).
- **Split into two processes**: on device the cost matches two-thread; the GL thread CPU of both is about 30% lower than single-threaded (backend work moved to the other thread / process) (P6).
- **Cross-machine**: a PC connects to the phone over TCP, all 102 integration cases pass; after the server is killed, 133 ms; after Wi-Fi disconnects, about 5 s to cleanly report "device lost" (P6.5).
- **Texture upload wait granularity (P12, handoff §4)**: on the wire arm, the texture half of `resource_subdata` no longer buys a reply per record; the wait point moves from "every record" to "SEG_STAGE window exhausted".
  A single pure upload occupies a reply wait of **1 → 0** (`wait_replies`, exactly the counter that was "55,428 per frame" in §3.2).
  Measured by shape (`RemoteClientControls.ReplyWaitsByOpForAnAtlasShapedLoad`, host inproc, counted per op): **atlas assembly** (64 `glTexSubImage2D` calls on one level) **64 → 0** reply waits;
  **one texture per sprite** (64 textures + 256 `glTexParameteri` calls) **384 → 320**, and the rest is entirely the three per-resource / per-call rows (create 76 / respecify 130 / params 130) —
  **these two were later replaced by "first-use confirmation"** (see the item below): once an object's existence is confirmed as fact by its own create,
  params and texture respecify no longer buy an answer per row; instead a blocking create is paid once at **first use**.
  Cumulative measured result for the same frame: **43,232 waits / 221.5 s → 4,543 waits / 31.3 s (7.1×)**, with staged bytes and the number of records sent (103,094) both unchanged.
  On device (TrebleDroid GSI, MC 26.2 entering the world over TCP), that frame consisted of `ResourceRespecify` 20,633 + `SetTextureParams` 18,056 + `ResourceCreate` 4,536 records **each waiting once**,
  while `ResourceSubData` 12,671 records **waited zero times** (B's on-device evidence); these records are all real state changes, and the client had nothing to deduplicate (a `glTexParameteri` with the same value produces no record in the first place).
- **The ~5 ms per round trip is the device-side deep idle exit, not the link and not the client** (P12, this round's measured result): same frame, same set of records,
  the client spinning 40× was only ~10% faster; switching to `adb forward` (USB) was ~25–30% faster; and after pinning the device's 8 cores to `performance` and turning off `cpuoff_l`/`clusteroff_l`/`mcusysoff`/`s2idle`,
  a single wait went **5,040 → 3,715 µs** and that frame's wall time **221.5 s → 166.5 s (−25%)** — with record count and byte count unchanged.
  This explains handoff §3.6 "switching to a link 3.4× faster leaves wall time unchanged": the money is spent on each wake-up of the device; the 249 s in handoff §3.2 was taken on a machine **without pinned clocks** and includes this idle tax.
  **The same diagnosis yields a portable fix**: raise the server's spin budget (the Activity's `env` extra → `MOBILEGL_IPC_SPIN_US=2000`, no root and no code change needed),
  and that frame goes **221.5 s → 182.8 s (−17%)**; pinning device clocks + turning off deep idle gives **166.5 s (−25%)**. The server log prints `spin 2000 us` as proof.
  See git history §2.6–2.10.
- **Frames are correct**: the Vulkan backend's on-device frame check passes 36/36; the five CTS blocks show no regression beyond 0.5 percentage points against baseline and no new crashes (P7); on-screen presentation in the server's own window gives SSIM 1.0 on both backends (P12).
- **Five-arm transport comparison (2026-09-28, unpinned-clock interleaved A/B)**: under heavy load (rd12) inproc ≈ spawn+shm is **about 1/3 faster** than monolith (run-ahead pipeline parallelism + fewer applier-path probes); under light load (openra) split pays only fixed overhead; **tcp localhost is ~8× slower, the bottleneck being present credit=1 serial waiting (~74%) + futex wake-up tax**, `MOBILEGL_IPC_SPIN_US=2000` alone measured 21.7→63.6 fps; the cross-host TCP "`eglMakeCurrent` hang" **was caused by the test environment** (the host's v2rayN `xray_tun` proxy terminated TCP and the relay hung, not a source bug); after bypassing it P12 exit gate (b) passes: the WSL client replays an in-world rd12 trace over Wi-Fi, ssim 0.999883, steady state 17–22 fps (`PRESENT_CREDIT` 1/2/3 = 16.9 / 21.3 / 22.1), openra steady state 105–133 fps; the 2.7 GB loading frame is bandwidth-bound (server measured 52 MB/s), steady state is round-trip-latency-bound (20–24 MB/s). Full text in git history.

## By phase

| § | Phase | One-line conclusion | Full text |
|---|---|---|---|
| 1 | P0 | A second process can be launched from the application process; cross-process GPU memory sharing has only one path available on all devices | git history |
| 2 | P1 | Verify pushed state per draw; 79 traces with zero divergence | git history |
| 3 | P2 | Desktop benchmark: the new tracker is cheaper than the old fill it replaced | git history |
| 4 | P3a | Two devices, on-device Release: interface cost +6–12%, scenes with dense VAO switching +27–30% | git history |
| 5 | P4a | Switched to Redmi; P4a itself adds only 3–6 percentage points | git history |
| 6 | P5 | The first frame draws correctly in two threads; the on-device two-thread arm still cannot run to completion | git history |
| 7 | P5b | Four game traces run to completion in two threads on device; two-thread extra cost +6–18% | git history |
| 8 | P5c | With zero shared memory between the two threads, all tests still green | git history |
| 9 | P5d | In-game 7–13 → 103–106 fps, client CPU 15.0 → 9.2 ms/frame | git history |
| 10–11 | P5e | Running ahead is 69% faster than waiting on every command; matches single-threaded under heavy load | git history |
| 12 | P6 | Two-process matches two-thread; first measured data volume and record count per frame | git history |
| 13 | P3b/P4b | Texture upload shapes are identical under all four topologies | git history |
| 14 | P6.5 | Cross-machine TCP 102/102; disconnect detected in about 5 s | git history |
| 15 | P7 | On-device frames 36/36; unbounded server memory growth reined in (host 825 → 546 MiB) | git history |
| 16 | P12 | On-device retest after review: Espryt SSIM 1.0; Magma 1.0 / 0.999999511; P12 targeted CTest 46/46; texture upload reply waits 1→0 (host mechanism gate, on-device wall time pending); **P12 closed (09-29)**: cross-machine TCP gate (b) passes (rd12 ssim 0.999883, steady state 17–22 fps); FCL gate (a) passes (Minecraft presented on screen in the render server window; after killing the server, device-lost latches in 33–54 ms) | git history |
| 17 | Five-arm transport measurement (2026-09-28, not a phase) | inproc/shm under heavy load is ~1/3 faster than monolith; tcp localhost bottleneck is credit=1 + wake-up tax (SPIN_US=2000 → 2.9×); the cross-host hang was actually caused by the host's xray TUN proxy; direct Wi-Fi steady state rd12 17–22 fps, openra 105–133 fps | git history |
| 18 | P11 | Same-machine zero-copy (T0) MC 26.3 peak memory Espryt −41–42%, Magma −18–26%, ssim / frame time / CPU unchanged; external client over shared memory rd12 Espryt about 119 fps (TCP 40–53); split T2 relative to monolith adoption p99 Espryt +37–42%, Magma about 2.5×. Not measured: after M2, on the split server, when a texture has no GPU writes, Espryt chooses the precise write box by its own storage threshold; the non-ring path may send a few extra small boxes | git history |
| 19 | P8 | On real content only Magma wire indirect hit (MC 26.3 × Magma inproc 13 million draws expanded on CPU) → after going native, the per-create-indirect full-GPU wait 321 → 0; Redmi create-instancing × Espryt × monolith 0.870 → 0.99998 (Adreno drops queued ring copies, the dev + feat two ladders); largest program archive 355 KB = 4.2% of arena/4 (no chunking); exit device matrix 77 cells / arms, none worse | git history |
