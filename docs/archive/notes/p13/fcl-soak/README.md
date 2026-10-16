# FCL silent-death soak (P13, 2026-10-07)

User request: soak FCL launches on the Y700 (Magma, Espryt control) to root-cause the "silent
Magma death after MobileGL init" seen once in the P13 FCL smoke. User decision afterwards: the
cause is FCL's, leave FCL as-is, no MobileGL workaround; recorded as a debt
(`../../DEBTS.md`, "FCL game process can die silently").

## Result

Every silent death is `ApplicationExitInfo reason=2 (SIGNALED) status=34` - signal 34 is
`SIGRTMIN+2`, the wake-up signal of Android libcore's `AsynchronousCloseMonitor`. No tombstone,
no kgsl/GPU fault line, no debuggerd output, in any death.

Mechanism (ftrace `signal_generate` / `signal_deliver` filtered on `sig==34`, plus a 0.5 s
sampler of `/proc/<pid>/status` `SigCgt`):

1. FCL's game process starts with Android's handler set (`SigCgt 0x6e400084f8`, bit 33 = sig 34
   caught).
2. About 0.5 s after FCL logs `Renderer: MobileGLDev...` (JVM start for the game), the whole set
   is replaced by the JVM's (`SigCgt 0x1005cef`, later `0x2000000001005cef`): sig 34 is no longer
   caught.
3. FCL's own okhttp stack (`Okio Watchdog` thread) times out a blocked read and sends sig 34 to the
   reading `onPool-worker-N` thread (`AsynchronousCloseMonitor::signalBlockedThreads`, `code=-6`
   = `SI_TKILL`).
4. With the default disposition the kernel takes the fatal fast path: `signal_generate` with no
   `signal_deliver`, and the whole process dies. In the relaunched launcher process the same
   watchdog's signals are delivered to libcore's handler (`sa_handler=79a72f3fa0`) and nothing
   happens.

Renderer-independent: reproduced with the P13 library on both backends and with the pre-P13 (P12
end, `743f4e8e`) library; several deaths happen before MobileGL has logged anything beyond FCL's
renderer line. Rate depends on whether FCL's background network read times out in that window
(network-dependent), not on the GL library. Untested with non-MobileGL renderers.

Fix belongs in FCL: save libcore's `SIGRTMIN+2` sigaction before creating the JVM and restore it
after (or do not reset that signal). Left for the user.

## Soak sizes

| soak | library | runs | deaths (all status=34) |
|---|---|---|---|
| clean (`soak-p13-clean-summary.txt`, `soak2.sh` with the SigCgt sampler and per-run ftrace) | P13 FCL lib at `c1e985c8` | 30 (22 Magma, 8 Espryt) | 18: Magma 13/22, Espryt 5/8 |
| P12 control (`soak-p12-control-summary.txt`) | P12 end `743f4e8e` | 10 Magma | 2 |
| matched A/B (`soak-ab-p12-p13-summary.txt`, `soak3.sh`): P12 and P13 libraries alternated launch by launch, Magma, same world, settings and network window, 40 s in-world | P12 `743f4e8e` / P13 `c1e985c8` | 10 + 10 | P12 4/10, P13 2/10 |

**Does the P13 library raise the rate?** No evidence it does. The clean soak's 18/30 against the
first P12 control's 2/10 was not a comparable pair: the two ran at different times (the rate tracks
whether FCL's background request is still pending, i.e. network conditions), and the P12 control
overlapped the contaminated P13 runs below. In the matched, interleaved A/B the P12 library died
more often (4/10) than the P13 one (2/10). The timeline is library-independent too: in every death
logcat FCL's `Renderer:` line is followed by the JVM's signal reset after 0.2-0.6 s and by
MobileGL's first log line after about 6.2 s for both libraries (P12 deaths: 6.3 s and 6.2 s), and
the deaths cluster at the same fixed offsets after the `Renderer:` line on both libraries (about
1.3 s, 9.4 s and 17.4 s) - timeouts started before MobileGL is loaded, not a window MobileGL's
start-up widens.

Earlier P13 runs (first 5-run soak) are not counted: two soak scripts were found driving the
phone at once and force-stopping each other's runs; `soak2.sh` now takes a lock.

Per-death evidence kept here for one representative death (clean run 4, Magma): the ftrace
excerpt (`run004-magma-ftrace-signal34.txt` - pid 27854 is the dying process's worker, generated
with no delivery), the SigCgt transitions (`run004-magma-sigcgt-transitions.txt`) and the logcat
lines (`run004-magma-logcat-excerpt.txt`). The full per-run logcat/dmesg/ftrace/exit-info
captures stayed in the session scratchpad and were not committed.

Device restored afterwards: original FCL `libMobileGL.so` (md5 checked) and `config.json`, ftrace
signal events and `tracing_on` off, `svc power stayon false`, anland relaunched.
