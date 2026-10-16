# X2 — the `InitialCapsStartup` unit-test flake under load

Wave 2, package X2. Base `9ae1e6c0`. Branch `p7/x2-caps-flake`.

## 1. What actually failed

`InitialCapsStartup.ANullInitialSnapshotFailsInsteadOfStartingWithAPlaceholder` failed in 3 of
the last 4 integration gates (`ctest -L '^unit$' -j 8` on a host where other agents build) and
passed 3/3 alone. Reproduced on the same host with 20 busy loops plus a repeating `-j 20` clean
rebuild in a second build directory (load average 40-60), 8 test processes at a time:

```
[ RUN      ] InitialCapsStartup.ANullInitialSnapshotFailsInsteadOfStartingWithAPlaceholder
MGPipe: Fatal{ProtocolCorruption, "StreamLink"} invalid data frame header
### rc=134 (SIGABRT) wall=262ms
```

It is not an EXPECT and it is not the caps path: the abort is `StreamLink.cpp:229`, the DATA
link's reader thread failing `FrameReader::Feed` on the first 8 bytes it ever reads. The same
line kills `DelayedTcpSnapshotArmsRunAheadAndLaterSnapshotsCanOnlyDemote` (the case project
memory recorded flaking before), and `AHealthyPeerWithoutInitialCapsFailsStartupWithoutDeviceLoss`
never fails only because its peer sends nothing.

## 2. The mechanism

The fixture rendezvoused over loopback TCP with `SocketTransport::ConnectTo` (two back-to-back
connects: control, then aux) against `AcceptPair` on a thread (two accepts: "first connection is
control, second is aux. The order IS the protocol"). Under load the kernel does not deliver two
loopback connects to the listener's accept queue in connect order: the final ACK of each
handshake is processed by the loopback softirq, and once the connecting thread migrates or
ksoftirqd is starved the second connection's ACK can land first. Measured with a raw-socket
probe that does exactly ConnectTcp's dance (`~/w7/x2bin/swapprobe.c`): **0 of 2000** on an idle
host, **5 of 3000 (0.17 %)** with 20 CPU hogs.

When the pair is swapped the peer's *control* transport is the client's *data* socket. The
test's first control frame (`CapsSnapshot`, or the `LogLine` of the delayed case) then arrives
on the client's `StreamLink` reader, whose framing magic is `MGLD`, and the reader aborts by
name. No timing assumption in the tests, no 250 ms window, no fixed sleep: an ordering the
fixture assumed and TCP never promised.

**Verdict: TEST.** The product does not pair by arrival order on TCP. PH-7 (4) / ID-P7-3 took
that out (`SocketTransport.h`: "never paired by arrival order"; `ClientSession.cpp:780`:
`ConnectControl` then `ConnectDataConnection` with `Welcome.dataNonce`; `ServerMain.cpp`:
`AcceptOne` + `ReceiveOneFrame` + `DecodeDataBind`). `ConnectTo`/`AcceptPair` survive for AF_UNIX
only, where `unix_stream_connect` enqueues on the listener synchronously and the order is real.
Window 1b's device notes show the same defect in the pre-F product over the LAN ("only 1 of 2
connections within 250 ms", connections reordered under adb forward) - the fixture was simply
left behind on the old rendezvous. `CapsMirror::ServerConsumes`, `AwaitFirstCapsSnapshot` and
`FinishStartup` step 7 were read end to end and are not implicated; no product bytes change.

## 3. The fix

`MG_Test/Wire/InitialCapsStartupTest.cpp` (`9f7e8013`): the fixture does what
`ServerMain::ListenerSource` does. `ConnectControl` is accepted with `AcceptOne` and wrapped as
`SocketTransport(fd, -1, Server)` BEFORE the data connection is opened; the data connection is
`ConnectDataConnection` carrying `EncodeDataBind(MintNonce())`, accepted with `AcceptOne`, read
with `ReceiveOneFrame`, decoded with `DecodeDataBind` and matched with `ConstantTimeNonceMatch`.
No accept thread: a loopback connect completes against the backlog without a concurrent accept.
The three cases are untouched.

`MG_Test/Wire/SocketTransportTest.cpp` (`d0284455`):
`TcpCarriesControlAndIndependentDataWithKeepalive` had the same `ConnectTo`/`AcceptPair` shape
and, swapped, hung in `recv(serverData, MSG_WAITALL)` until the 60 s kill (rc=124). Same
rendezvous now; it keeps the NODELAY / keepalive / `ShareFd` checks on both control transports,
the independent-data check on the bound data sockets, and asserts a control-only TCP transport
owns no data descriptor (`TakeDataFd() == -1`).

## 4. Red-once, under the same load (20 hogs + `-j 20` rebuild loop, 8 processes at a time)

| case | before | after |
|---|---|---|
| `InitialCapsStartup.ANullInitialSnapshotFailsInsteadOfStartingWithAPlaceholder` | 2 / 200, then **2 / 500** (SIGABRT, text above) | **0 / 500** |
| `InitialCapsStartup.DelayedTcpSnapshotArmsRunAheadAndLaterSnapshotsCanOnlyDemote` | **5 / 500** (same abort) | **0 / 500** |
| `SocketTransportTest.TcpCarriesControlAndIndependentDataWithKeepalive` | **3 / 500** (60 s hang) | **0 / 500** |

Pass wall time did not move (median ~250 ms in both columns).

## 5. Gates

- `ctest -L '^unit$' -j 8`: **2435 / 2435**, rc 0.
- `scripts/ci/fatal_census.py`: rc 0, **79** abort sites / 20 files, 44 family words, 3 refusal
  words, 0 unmarked (unchanged).
- `scripts/link_ratchet.py --build-dir build-split --baseline scripts/data/link_ratchet_baseline.txt --assert-monotone`: **173**, unchanged.
- G1 pull build (`build-linux`, the p5_g1base flags): `.text` **0xa52203**, `nm --defined-only`
  against `~/w7/p7-before/pull-syms.txt` **+0 / −0** (30570 lines).
- `integration-split` not re-run: no product file changed.

## 6. Open

1. `AcceptPair` over a `tcp://` listener is still callable and still pairs by order. The product
   never calls it that way, but nothing refuses it by name; a `MOBILEGL_ERR_UNSUPPORTED` for TCP
   listeners in `AcceptPair` (and `ConnectTo` for `tcp://`) would close the door the fixture
   walked through. Not in X2 - it is a product change and needs its own red-once.
2. The `::alarm(10)` in the fixture is the only remaining time bound;
   `AHealthyPeer...` spends 5 s of it in `kHandshakeTimeoutMs` by design. Not observed to fire.
