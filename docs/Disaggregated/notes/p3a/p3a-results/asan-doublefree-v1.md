# P3a — the CI `double free or corruption (!prev)` on `DirectVulkan.Verify.CrossFrameBufferScenario.{Vertex,Index}CopyBufferSubData`

Investigator: ASan round, 2026-09-08. Tree: private worktree `~/w7/p3a-asan` on branch `p3a/asan`, pinned at
`3e298c9a` (the commit CI ran). Nothing in `~/w7/pipe` or `~/w7/p3a-final` was modified.

## VERDICT

**Reproduced, root-caused, and the fix is validated in both a sanitizer build and a plain Release build.**

It is **not a `CopyBufferSubData` bug and not a Magma bug.** It is a **static-destruction-order use-after-free
that every one of the 26 `CrossFrameBufferScenario` case × backend combinations executes**, on both backends,
at `exit()`. The two `CopyBufferSubData` cases are simply the two whose heap layout makes the corrupted write
that follows the UAF *fatal* under CI's allocator arrangement; the other 24 execute the same use-after-free and
survive it. The corruption is P3a's: `~BufferObject` is the first frontend-object destructor in the project's
history to call into `MG_Pipe`'s process-lifetime singletons, and the objects that hold the last reference to a
`BufferObject` are destroyed by `__run_exit_handlers` **after** those singletons. It is **not verify-only**
either: the same crash reproduces with the comparator switched off, so a shipped `MOBILEGL_PIPE_PUSH` build is
exposed identically.

The defect is still present on today's `feat/disaggregated` head (`c20e2f2b`, i.e. after the `p3a/final`
rework); the rework's C-1 fix adds a **second** reaching path (`~VertexArrayObject` →
`MGPipeEmitVertexElementsDestroyAndFree` → `MGPipeSlots()`), so the exposure is now larger, not smaller.

---

## 1. The mechanism, in one chain

```
exit()
  __run_exit_handlers
    MobileGL::MG_Pipe::MGPipeSlotAllocator::~MGPipeSlotAllocator()      <-- runs FIRST
        MG_Impl/Pipe/SlotAllocator.h:39  ->  KindState::~KindState()  SlotAllocator.h:84
        (frees Slots, FreeList and the ska::flat_hash_map ByLifetimeId entry block)
    ...
    MobileGL::MG_Pipe::PipeInputs::~PipeInputs()                        <-- runs LATER
        MG_Backend/MGPipe/PipeInputs.h:678  SharedPtr<VertexArrayObject> m_boundVertexArray
      ~VertexArrayObject                    MG_State/.../VertexArrayObject.cpp:54
        ~BindingSlot<BufferObject>          MG_Util/Types.h:181
          ~BufferObject                     MG_State/GLState/BufferState/BufferObject.cpp:60
            MGPipeEmitResourceDestroyAndFree  MG_Impl/Pipe/PipeFill.cpp:743
              tracker.Find(buffer)            MG_Impl/Pipe/ResourceTracker.h:304
                MGPipeSlots().FindByLifetimeId  MG_Impl/Pipe/SlotAllocator.cpp:100
                  ska::flat_hash_map::find      ***USE AFTER FREE***
```

`MGPipeSlots()` (`MG_Impl/Pipe/SlotAllocator.cpp:170-173`) is a Meyers singleton:

```cpp
MGPipeSlotAllocator& MGPipeSlots() {
    static MGPipeSlotAllocator allocator;
    return allocator;
}
```

Its `static` is constructed lazily, at the **first `BufferObject`** (`MGPipeMintResourceHandle`, PipeFill.cpp:610),
i.e. deep inside the test. `gPipeInputs` (`MG_Backend/MGPipe/PipeInputs.h:691`, `inline PipeInputs gPipeInputs{}`)
is a namespace-scope global, dynamically initialised **before `main`**. Reverse-order destruction therefore
destroys the allocator **first** and `gPipeInputs` — with its live `SharedPtr<VertexArrayObject>` and, through
the VAO's binding slots, live `SharedPtr<BufferObject>`s — **last**. Calling `MGPipeSlots()` after its static's
destructor has run does **not** reconstruct it; it hands back the destroyed object.

The same holds for `MGPipeResourceTrackerInstance()` (`ResourceTracker.h:554-557`, another Meyers singleton
that `~BufferObject` reads *and writes* via `WasPublished` / `NoteDestroy` / `Retire`) and for
`g_applier` (`MG_Pipe/PipeApply.cpp:380`, a namespace-scope value global that `MGPipeApplyResourceDestroy`
touches whenever the create was published — i.e. on every Espryt/DirectGLES buffer).

**What actually corrupts the heap.** The ASan report catches the first *read*. In a shipped (non-sanitizer)
build the read usually returns garbage, and then one of two things happens:

* the garbage does not resolve to a live slot → `MGPipeHandleIsNull(handle)` → `~BufferObject` returns early and
  nothing is written. **This is the 24 surviving cases.**
* the garbage still looks like a live `{slot, gen}` (the freed 288-byte hash-table block is usually still
  intact) → the destructor proceeds into `MGPipeSlotAllocator::Free` (`SlotAllocator.cpp:113-130`), which
  * `state.Slots[handle.Slot]` — writes into a **freed** `std::vector` buffer (`:116`, `:126-128`),
  * `state.ByLifetimeId.erase(it)` — mutates a **freed** hash table (`:123`),
  * `state.FreeList.push_back(...)` — **grows a freed `std::vector`**, i.e. `operator new` + `memcpy` +
    `operator delete` **on a pointer that was already freed** (`:129`).

  That last line is a literal double free of the old `FreeList` buffer, and the writes at `:116`/`:126-128`
  scribble over whatever glibc has since placed in the freed chunks. Hence the two different glibc verdicts CI
  printed for the two cases — `double free or corruption (!prev)` and `corrupted size vs. prev_size in
  fastbins` — from the same defect.

Before P3a, `~BufferObject` touched only `g_bufferBackendOps`, a plain namespace-scope pointer that the backend
nulls at shutdown, so the destructor was exit-safe. P3a's `MGPipeEmitResourceDestroyAndFree` is what made a
frontend destructor depend on function-local statics.

## 2. Why only those two cases (and why the local box did not reproduce it)

The selection is pure allocator layout. `CopySubData` is the only mutation that creates a **third**
`BufferObject` with a real store (`m_staging`, `glBufferData(GL_COPY_READ_BUFFER, …)` at
`CrossFrameBufferScenario.cpp:327-330`), which shifts every subsequent allocation and, critically, the size and
reuse history of the `ByLifetimeId` table that is read after free. Nothing about `glCopyBufferSubData`,
`BufferObject::CopyDataFrom` or Magma's buffer teardown is implicated — `m_staging` never even acquires a
`VkBufferResource` (`VkBufferManager::OnRespecify` returns early on `!resource`).

Two independent confirmations of "layout, not logic":

* under **ASan**, all 13 cases × 2 backends = 26 runs report the identical `heap-use-after-free` (§4);
* on this box the native build is clean with the default allocator (40 × 2 runs, `MALLOC_CHECK_=3
  MALLOC_PERTURB_=165`), but **`GLIBC_TUNABLES=glibc.malloc.tcache_count=0` flips exactly the two CI cases,
  and only those two, to a hard SIGSEGV** — the same selection CI makes, with a one-variable change.

Local lavapipe vs CI:

| | renderer string |
|---|---|
| CI | `Magma (MobileGL Core) (llvmpipe (LLVM 20.1.2, 256 bits), Vulkan 1.4.318, Driver 25.2.8)` — Mesa 25.2.8 |
| this box | `Magma (MobileGL Core) (llvmpipe (LLVM 22.1.6, 256 bits), Vulkan 1.4.354, Driver 26.1.4)` — Mesa 26.1.4 (`vulkan-swrast 1:26.1.4-1`) |

The driver difference is not the cause; it only changes the allocation history that decides fatality.

## 3. Exact commands that reproduce it

**A — native, no sanitizer, on any existing verify build (fastest; this is the one to hand to CI):**

```bash
cd <build-verify>/MobileGL/MG_IntegrationTest
MOBILEGL_BACKEND_TYPE=DirectVulkan MOBILEGL_PIPE_VERIFY=1 MOBILEGL_ITEST_REQUIRE_GPU=1 \
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json \
GLIBC_TUNABLES=glibc.malloc.tcache_count=0 \
  ./MobileGLIntegrationTest --gtest_filter=CrossFrameBufferScenario.VertexCopyBufferSubData
# -> "[  PASSED  ] 1 test." then Segmentation fault (rc 139)
```

**B — ASan, which names the defect instead of the symptom (and fires on ANY case of the family):**

```bash
bash ~/w7/notes/tools/wsl_tree.sh p3a asan 3e298c9a          # or just: git worktree add ~/w7/p3a-asan 3e298c9a
cd ~/w7/p3a-asan
cmake -S . -B build-asan <wsl_p3a_gate.sh COMMON, minus -DCMAKE_BUILD_TYPE=Release> \
  -DMOBILEGL_PIPE_VERIFY=ON -DMOBILEGL_ITEST_REQUIRE_GPU=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address
cmake --build build-asan -j 8 --target MobileGLIntegrationTest      # NOT `all`: see the build note below
cd build-asan/MobileGL/MG_IntegrationTest
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 LSAN_OPTIONS= \
MOBILEGL_BACKEND_TYPE=DirectVulkan MOBILEGL_PIPE_VERIFY=1 MOBILEGL_ITEST_REQUIRE_GPU=1 \
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json \
  ./MobileGLIntegrationTest --gtest_filter=CrossFrameBufferScenario.VertexCopyBufferSubData
```

Build note for whoever repeats this: `cmake --build build-asan` (the `all` target) **fails to link**
`libSPIRV-Tools-shared.so` under `-fsanitize=address` — clang does not link the ASan runtime into a shared
object and that target carries `-Wl,--no-undefined`. Build the `MobileGLIntegrationTest` target only.
`valgrind` on this box is unusable (`valgrind-3.25.1` refuses to start: glibc's stripped `ld-linux` has no
`memcmp` to redirect, and the debuginfo package is not installed), so ASan was the only sanitizer used.

## 4. The sanitizer report (trimmed; symbol soup collapsed, file:line verbatim)

```
==1608571==ERROR: AddressSanitizer: heap-use-after-free on address 0x6db2b22b18d8 ...
READ of size 1 at 0x6db2b22b18d8 thread T0
    #0/#1 ska::detailv3::sherwood_v3_table<...>::find(unsigned long const&)   include/ska/flat_hash_map.hpp:543:39
    #2 MobileGL::MG_Pipe::MGPipeSlotAllocator::FindByLifetimeId(MGPipeKind, unsigned long) const
                                                                MobileGL/MG_Impl/Pipe/SlotAllocator.cpp:100:44
    #3 MobileGL::MG_Pipe::MGPipeResourceTracker::Find(BufferObject const&) const
                                                                MobileGL/MG_Impl/Pipe/ResourceTracker.h:304:34
    #4 MobileGL::MG_Pipe::MGPipeEmitResourceDestroyAndFree(BufferObject&)
                                                                MobileGL/MG_Impl/Pipe/PipeFill.cpp:743:45
    #5 MobileGL::MG_State::GLState::BufferObject::~BufferObject()
                                                MobileGL/MG_State/GLState/BufferState/BufferObject.cpp:60:13
    #6..#8  std::_Sp_counted_base::_M_release / ~shared_ptr<BufferObject>
    #9 MobileGL::BindingSlot<BufferObject>::~BindingSlot()      MobileGL/MG_Util/Types.h:181:11
    #10 MobileGL::MG_State::GLState::VertexArrayObject::~VertexArrayObject()
                                        MobileGL/MG_State/GLState/VertexArrayState/VertexArrayObject.cpp:54:5
    #11..#13 ~shared_ptr<VertexArrayObject>
    #14 MobileGL::MG_Pipe::PipeInputs::~PipeInputs()            MobileGL/MG_Backend/MGPipe/PipeInputs.h:158:12
    #15/#16 __run_exit_handlers / exit                          (libc)
    #17..#19 __libc_start_main / _start

0x6db2b22b18d8 is located 24 bytes inside of 288-byte region [0x6db2b22b18c0,0x6db2b22b19e0)
freed by thread T0 here:
    #0 operator delete(void*, unsigned long)
    #1 MobileGL::MG_Pipe::MGPipeSlotAllocator::KindState::~KindState()
                                                                MobileGL/MG_Impl/Pipe/SlotAllocator.h:84:16
    #2 std::array<MGPipeSlotAllocator::KindState, 14ul>::~array()
    #3 MobileGL::MG_Pipe::MGPipeSlotAllocator::~MGPipeSlotAllocator()
                                                                MobileGL/MG_Impl/Pipe/SlotAllocator.h:39:11
    #4/#5 __run_exit_handlers / exit                            (libc)

previously allocated by thread T0 here:
    #0 operator new(unsigned long)
    #4 ska::...::rehash(unsigned long)                          include/ska/flat_hash_map.hpp:636:34
    #5 ska::...::grow()                                         include/ska/flat_hash_map.hpp:871:9
    #8 ska::flat_hash_map<...>::operator[](unsigned long const&) include/ska/flat_hash_map.hpp:1345:16
    #9 MobileGL::MG_Pipe::MGPipeSlotAllocator::AllocateFor(MGPipeKind, unsigned long)
                                                                MobileGL/MG_Impl/Pipe/SlotAllocator.cpp:92:13
    #10 MobileGL::MG_Pipe::MGPipeResourceTracker::Acquire(BufferObject&)
                                                                MobileGL/MG_Impl/Pipe/ResourceTracker.h:293:55
    #11 MobileGL::MG_Pipe::MGPipeMintResourceHandle(BufferObject&)
                                                                MobileGL/MG_Impl/Pipe/PipeFill.cpp:610:41
    #12 MobileGL::MG_State::GLState::BufferObject::BufferObject(unsigned int)
                                                MobileGL/MG_State/GLState/BufferState/BufferObject.cpp:44:9
    ... BufferState::CreateBufferObject -> GLImpl::BindBuffer_State (GL_Buffer.cpp:1457)
        -> CrossFrameBufferScenario::ApplyMutation (CrossFrameBufferScenario.cpp:327)

SUMMARY: AddressSanitizer: heap-use-after-free include/ska/flat_hash_map.hpp:543:39 in ...::find(...)
```

The same defect in a **plain Release verify build**, without any sanitizer, under gdb
(`GLIBC_TUNABLES=glibc.malloc.tcache_count=0`, binary built from today's `feat/disaggregated` head `c20e2f2b`):

```
Thread 1 "MobileGLIntegra" received signal SIGSEGV, Segmentation fault.
#0  MGPipeSlotAllocator::FindByLifetimeId(MGPipeKind, unsigned long) const
#1  MGPipeEmitResourceDestroyAndFree(BufferObject&)
#2  MG_State::GLState::BufferObject::~BufferObject()
#3  MG_State::GLState::VertexArrayObject::~VertexArrayObject()
#4  MG_Pipe::PipeInputs::~PipeInputs()
#5/#6  __run_exit_handlers / exit
```

### ASan matrix at `3e298c9a`, unpatched (one process per case, `abort_on_error=1`)

| backend | cases run | `heap-use-after-free` at `SlotAllocator.cpp:100` |
|---|---|---|
| DirectVulkan | 13/13 | **13/13** |
| DirectGLES   | 13/13 | **13/13** |
| controls (`StreamedArenaScenario`, `ResidentIndexScenario`), both backends | 4/4 | **4/4** |

Every one of them printed `[  PASSED  ]` first, exactly as CI did.

## 5. Owning code

* **Introduced by:** package B (client). `MG_State/GLState/BufferState/BufferObject.cpp:49-60` (the
  `MGPipeMintResourceHandle` / `MGPipeEmitResourceDestroyAndFree` pair) and
  `MG_Impl/Pipe/PipeFill.cpp:741-758`.
* **The unsafe singletons:** `MG_Impl/Pipe/SlotAllocator.cpp:170-173` (`MGPipeSlots`, P2 contract package),
  `MG_Impl/Pipe/ResourceTracker.h:554-557` (`MGPipeResourceTrackerInstance`, package B),
  `MG_Pipe/PipeApply.cpp:380` (`g_applier`, package A/wire).
* **The holder that outlives them:** `MG_Backend/MGPipe/PipeInputs.h:678` + `:691` — `gPipeInputs`'
  `SharedPtr<VertexArrayObject> m_boundVertexArray`, pre-existing (P1/P2), never released. A verify build has
  two *more* of these (`MG_Impl/Pipe/PipeFill.cpp:366-367`, `g_snapshot` / `g_readScratch`), but they are
  **not** what makes it fire: the same two cases SIGSEGV identically with `MOBILEGL_PIPE_VERIFY=0`, i.e. with
  the comparator never filling those two blocks, so `gPipeInputs` alone is enough. **A shipped push build is
  exposed exactly as much as a verify build**; the verify lane is only where CI happened to run these cases.
* **Not implicated:** `glCopyBufferSubData`'s frontend path (`GL_Buffer.cpp:784-843`,
  `BufferObject::CopyDataFrom`), `VkBufferManager`'s buffer teardown, `MGPipeApplierReset` /
  `MGPipeApplierReleaseObjectRecords`, and the `RecordAt` / blob bounds gates — all of those were read and are
  correct for this shape.

## 6. Recommended minimal fix (validated here, NOT committed to any shared branch)

Make every process-scope singleton a frontend destructor can reach outlive `__run_exit_handlers`. Three lines
of substance, no behaviour change, no new allocation on any hot path, and G1-neutral (all three sites are
inside `#if MOBILEGL_PIPE_PUSH` translation units, so the pull build's symbols do not move):

```cpp
// MG_Impl/Pipe/SlotAllocator.cpp  MGPipeSlots()
-   static MGPipeSlotAllocator allocator;
-   return allocator;
+   static MGPipeSlotAllocator* allocator = new MGPipeSlotAllocator();   // never destroyed
+   return *allocator;

// MG_Impl/Pipe/ResourceTracker.h  MGPipeResourceTrackerInstance()
-   static MGPipeResourceTracker tracker;
-   return tracker;
+   static MGPipeResourceTracker* tracker = new MGPipeResourceTracker();
+   return *tracker;

// MG_Pipe/PipeApply.cpp
-   MGPipeApplierState g_applier{};
+   MGPipeApplierState& g_applier = *new MGPipeApplierState{};
```

(The `g_applier` line is a reference to a never-destroyed block precisely so that the ~60 `g_applier.X` uses in
that file need no edit.) Each carries a comment naming the exit-order hazard.

**Validation performed, all in `~/w7/p3a-asan` at `3e298c9a`:**

| arm | result |
|---|---|
| ASan, unpatched, 13 cases × 2 backends + 4 controls | 26 + 4 aborts, all `heap-use-after-free` at `SlotAllocator.cpp:100` |
| ASan, patched, same 30 runs | **30/30 clean, rc 0, zero ASan reports** |
| plain Release verify build, unpatched, `tcache_count=0`, 13 × 2 | exactly `Vertex/IndexCopyBufferSubData` on DirectVulkan SIGSEGV — the CI selection |
| plain Release verify build, patched, `tcache_count=0`, 13 × 2 | **26/26 rc 0** |
| plain Release verify build, patched, full `ctest -L integration-verify` | **842/842 passed, 0 failed** |
| plain Release verify build, unpatched, `MOBILEGL_PIPE_VERIFY=0`, `tcache_count=0` | both cases still SIGSEGV — the hazard is **not** verify-only |

**Alternatives considered and rejected as the *minimal* fix:**

* *Release `gPipeInputs`' object references at context teardown.* Correct in principle (it is also a real
  object leak: a VAO and its buffers stay alive for the whole process after the context dies) but it is a
  `MG_Backend/MGPipe` edit, it is contract-frozen territory, and it does not protect the next destructor that
  reaches a singleton. Worth a follow-up row, not this fix.
* *A `g_slotsAlive` latch set in `~MGPipeSlotAllocator`, checked in `MGPipeEmitResourceDestroyAndFree`.* Only
  covers the one caller and leaves the same trap armed for `~VertexArrayObject`'s new path.
* *Ordering the statics.* Not expressible portably; the construction order is decided by the first
  `glGenBuffers`, not by us.

**Where the patch lives.** It is **not merged anywhere**. It sits on the private branch **`p3a/asan`**
(`82d9f314`, worktree `~/w7/p3a-asan`, parent `3e298c9a`) and, as a plain diff, at
`~/w7/notes/p3a/p3a-results/asan-doublefree-v1.patch`. It is *not* landed on `feat/disaggregated` because it
spans `SlotAllocator.cpp` (the contract package's file, behind the `p3a/contract` tag) and `PipeApply.cpp`
(wire's), and the `p3a/final` rework is in flight on the same tree — the ownership call is the integrator's.
It will need a rebase onto the current head (`c20e2f2b`); the three hunks are trivial and should apply as-is.

## 7. Regression check on the patched tree

`ctest -L integration-verify --no-tests=error -j 4` on the patched plain Release verify build of `3e298c9a`:
**`100% tests passed, 0 tests failed out of 842`** (log: `~/w7/p3a-asan-iverify.log`). That is the same lane
and the same 842 entries CI ran, with the two aborts gone and nothing else moved.

## 8. Follow-ups for the integrator

1. **The exposure grew with the `p3a/final` rework.** `~VertexArrayObject` now calls
   `MGPipeEmitVertexElementsDestroyAndFree`, which reaches `MGPipeSlots()` *and* `g_applier`
   (`delete_vertex_elements`) on the same exit path. Whatever fix lands must cover it; the three-singleton fix
   above does.
2. **`gPipeInputs` never releases its `SharedPtr` GL objects.** Independently of this crash, a VAO, its
   buffers and their backend twins outlive the context that owned them for the life of the process. Worth its
   own row.
3. **CI cannot see this class today.** Nothing in the gate runs a sanitizer. One cheap arm that would have
   caught it on day one: run the integration-verify lane once with
   `GLIBC_TUNABLES=glibc.malloc.tcache_count=0` — it costs nothing and turns this exact defect into a hard,
   deterministic red.
4. `espryt-review-v1` asked for `StorageBufferRegrow` and an adopted-source `DoublePrecisionScenario` under
   ASan (final-review M-3). The ASan build recipe in §3 is now known-good; those two runs are cheap to add.
