# X1 — the client-thread SIGSEGV in a mapped buffer under inproc (device window #1)

Wave 2, package X1. Base `4f2d4c4e`. Branch `p7/segv`.

## 1. What the tombstone actually said

Three CI split-subset traces died on the Redmi (`2f7cbe2e`, Adreno 830, APK `p7w1-9be62cbc`)
under `MOBILEGL_TRANSPORT=inproc` and passed under monolith on the same device and APK:
`improved-transparency-minecraft-26.3`, `minecraft-1.21.1-neoforge-create-instancing-in-world`,
`minecraft-1.21.1-neoforge-create-indirect-in-world`. Every one of them is
`SIGSEGV / SEGV_ACCERR` inside `__memcpy_aarch64_nt`, on the `MobileGLTraceRe` (GL/client)
thread, in the apitrace replayer's write into a pointer MobileGL had handed it.

The deciding evidence is two lines of the tombstone that name **the same address twice, in two
different address spaces**:

```
x0  b4000073ac9a5000   x1  b4000073ab1a2000   x2  0000000001800000
signal 11 (SIGSEGV), code 2 (SEGV_ACCERR), fault addr 0x00000073ac9a5000
```

`x0` is the memcpy destination — the pointer the application is writing through. `0xb4` in its
top byte is bionic's `POINTER_TAG`: on a TBI-capable arm64 every heap pointer carries it
(`tagged_addr_ctrl: 0000000000000001 (PR_TAGGED_ADDR_ENABLE)`, same tombstone). `fault addr` is
`info->si_addr`, and the kernel reports it **untagged**. `x2` is the copy length, 24 MiB. The
fault is on the destination's *first* byte, and the destination is page-aligned — which is the
shadow allocator's signature (`PipeResource.h:77`, `::operator new(…, align_val_t{4096})`).

All four crash sites have exactly this shape:

| case | x0 (tagged dst) | si_addr | x2 (length) |
|---|---|---|---|
| 26.3, DirectVulkan (E0a) | `b4000073ac9a5000` | `0x73ac9a5000` | `0x1800000` |
| 26.3, DirectGLES (E4a) | `b4000073b1d39000` | `0x73b1d39000` | `0x1800000` |
| 26.3, DirectGLES, block=0 (E4b) | `b4000073ab788000` | `0x73ab788000` | `0x1800000` |
| create-instancing (E0a) | `b40000739de43000` | `0x739de43000` | `0x5e10` |
| create-indirect (E0a) | `b40000739f5e8000` | `0x739f5e8000` | `0x7000` |

(The logcat tombstone carries registers and backtrace but not the `memory map` section —
that lives only in `/data/tombstones/`. The registers were enough.)

## 2. The mechanism

`MG_Remote/Client/PersistentMapTracker.cpp`, two rows that never lived in the same address
space:

* **Registration** (`TrackWriteMap`, formerly `:348`) took `shadowBase =
  (uintptr_t)buffer.MappedData()` — a **tagged** heap pointer — derived `base`/`end` from it
  (`& kPageMask` clears the low 12 bits and preserves the top byte), `mprotect`ed
  `[base, end)` `PROT_READ`, and published the **tagged** `base`/`end` into the slot.
* **The handler** (`PersistentWriteFaultHandler`, formerly `:218/:231/:234`) took
  `address = (uintptr_t)info->si_addr` — **untagged** — and tested
  `if (base <= kSlotSettingUp || address < base) continue;`.

`0x73ac9a5000 < 0xb4000073ac9a5000` is true, for every fault, forever. Ownership test (2) never
matched, `matched` stayed `false`, and every fault on a page the tracker had itself protected
fell through to `ChainToPreviousSegvHandler` → bionic's debuggerd → tombstone.

`mprotect` itself was fine: `do_mprotect_pkey` calls `untagged_addr()` on its argument, so the
right pages really were protected. Only the *recognition* was broken.

The other half of the defect is the ordering that made the handler load-bearing at all:
`BufferObject::AcquireMemoryRange` sets `m_isMapped`, calls `NotePersistentMapStateChanged()`
(which lands in `TrackWriteMap` and protected the range), and only *then* returns
`m_resource.Bytes() + range.start` (`BufferObject.cpp:913-965`). So `glMapBufferRange` with
`GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT` handed the application a **read-only** pointer whose
writability depended entirely on a SIGSEGV handler reaching the writing thread.

### Why every fact fits

* **E1 (`RUN_AHEAD=0`) still crashes** — nothing here touches run-ahead.
* **E4a (DirectGLES) still crashes** — the tracker is backend-independent; it sits in the
  client's `MG_State` map path.
* **E4b (`PERSISTENT_BLOCK_KB=0`) still crashes** — the knob gates `PushBlocksFor`
  (`:849`, the `blockBytes == 0` early return), *not* `NoteMapStateChanged`/`TrackWriteMap`.
  The mprotect arm is armed with the push off.
* **No tracker warnings in the client log** — registration succeeded; nothing on the
  successful path logged anything.
* **Crashes early** (client log ends right after `CapsMirror generation 3 adopted`) — the
  first persistent write map of the trace is the first and last one.
* **WSL/lavapipe green** — x86-64 has no top-byte tag, so `base` and `si_addr` agree and the
  handler works. This is why every host lane has always been green.
* **Monolith green on the same device** — `NotePersistentMapStateChanged` returns immediately
  when `PushIsArmed()` is false (`BufferObject.cpp:489`), so nothing is ever protected.

### Ruled out

ART's `libsigchain` is not implicated: the tombstone's `F libc : Fatal signal 11` line is
bionic's `debuggerd_signal_handler` output, which proves the signal *was* delivered to a
userspace handler chain and that our handler ran and chained. A blocked SIGSEGV would have had
the kernel force `SIG_DFL` and produced no such line. The 16 KiB-page refusal (`:274-278`) is
not reached (the device is a 4 KiB kernel — the protection demonstrably took). The
`RefaultOfANonWriteAccess` decline (`:156`) is not reached either: `matched` is false long
before it is consulted. P5d's `SHADOW_ALLOCATION_ALIGNMENT` / zero-page registration are
correct and are in fact what makes the destination page-aligned in the tombstone.

## 3. The fix

`MG_Remote/Client/PersistentMapTracker.cpp`:

1. **`UntagAddress()`** — one normalisation, applied to the shadow base at registration
   (`TrackWriteMap`), to the shadow base in `PushBlocksFor` (so `trackedBase - shadowBase`
   stays in one space), and to `si_addr` in the handler. Written **without** an
   `#if defined(__aarch64__)`: masking bits 56-63 is the identity for every valid userspace
   address off arm64 (Linux/x86-64 caps `TASK_SIZE_MAX` below 2^56), and an arch guard would
   have put the only gate that can catch this on the one platform CI never runs — which is how
   it reached the device. Pointer *dereferences* keep the tagged pointer they came from
   (`PushBlocksFor`'s two edge hashes now read through `shadowBytes`), because under a future
   real-MTE tagging level the tag is load-bearing for the access itself.
2. **`SlotOwnsFault()`** — ownership test (2) extracted into one row the handler and the unit
   test both read.
3. **Registration arms nothing.** The `mprotect(base, end - base, PROT_READ)` at the end of
   `TrackWriteMap`'s slot setup is gone. Nothing is lost: every page bit is already SET at
   registration, so the first push ships the whole interior regardless, and it is that push
   (`PushBlocksFor`, `:775`) that arms the pages — which is where the dirty tracking has its
   first real question to ask. Arming at registration could only ever produce faults whose
   answer was already known, one per page: 6144 of them for the 24 MB arena that crashed.
   The invariant the file rests on — *bit set ⇔ page writable* — is now true from registration
   onward, where before it was knowingly false for the whole pre-first-push window.

### The diagnostic (independent of the fix)

* **One line per process at the first registration** (`MGLOG_I_ONCE`), naming the mapping the
  way the device run needed and did not have: allocator, the pointer the application will be
  handed, **its tag**, the extent that pointer owns, the mapped range, the published span, the
  page count and which alignment arm produced it.
* **A decline record in the handler**, written with relaxed atomic stores only (async-signal-
  safe), readable from the GL thread through `PersistentMapTracker::DeclinedFaults()`, plus one
  async-signal-safe `write(2)` line to fd 2 the first time a decline happens while any slot is
  live. A non-zero decline count whose `si_addr` lies inside `nearest-tracked` **once the top
  byte is masked off** is the signature of this defect and of nothing else.

## 4. Red-once (two, orthogonal)

`MG_Test/Wire/PersistentMapFaultTest.cpp`, six cases, registered in
`MG_Test/Wire/CMakeLists.txt`'s `wiretest` loop.

| revert | red | green |
|---|---|---|
| drop the two `UntagAddress` calls in `SlotOwnsFault` | `AKernelsUntaggedFaultAddressIsOwnedByATaggedShadowsSlot` | the other 5 |
| restore `mprotect(base, end - base, PROT_READ)` at the end of `TrackWriteMap` | `AMapHandsBackARangeWritableWithoutTheFaultHandler` (child dies by SIGSEGV), `RegistrationLeavesEveryPageOfTheRangeReadWrite` | the other 4 |

Neither revert can be hidden by the other. `AnArmedPageIsStillAnsweredByTheHandlerAndMadeWritable`
is the anti-vacuity case: it protects a page by hand the way the push does and asserts the
handler answers it, so the two writability cases cannot stay green with the whole mprotect arm
deleted.

The first case runs the **device's own two addresses** (`0xb4000073ac9a5000` against
`0x73ac9a5000`) through the real predicate, on an x86-64 host. That is the gate this defect
should have had.

## 5. What the integrator must still decide

1. **The device run.** The fix is blind with respect to the phone — see the experiment in the
   hand-off. One `--matrix` pass over the three cases under inproc is the whole test.
2. **P5d's device numbers are suspect.** If registration always protected and the handler always
   declined on arm64, then on Android the mprotect arm has never answered a single fault: every
   persistent write map either killed the process or was never written through. Any
   P5d measurement taken on the Redmi that attributes a win to the mprotect arm needs re-taking.
3. **Residual: a writer thread with SIGSEGV blocked.** After the first push the pages are armed
   again while the map is live, so a write from a thread that blocks every signal is still a
   process kill. Registration no longer creates that window, but the steady state does. If the
   integrator wants it closed outright, the arm has to become opt-in per buffer (or the push has
   to fall back to the hash arm for maps the application writes off the GL thread) — that is a
   design change, not a bug fix, and it is not in X1.
4. **Residual: `RefaultOfANonWriteAccess`** can still decline a legitimate repeat write at the
   same `(tid, pc, address)` when the page's bit is set. Deferring the arm removes the one
   window that made "bit set while page read-only" routine, but the row is still reachable by a
   sibling thread mid-answer. Not observed; recorded.
