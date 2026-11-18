# Guest thread backends

How the kernel runs guest threads on the host, and what each way costs. The kernel's own objects (semaphores, event
flags, mutexes, condition variables, callbacks, signals) and its scheduling of the console's three CPUs are the same
whatever the backend: they wait and wake through one boundary, `ThreadBackend` (`libs/runtime/src/kernel/backend.hpp`).
Host primitives are not used for the guest's objects: they differ between platforms (wake order, spurious wakes,
timeout resolution) and have none of the console's semantics.

## The boundary

| Call | What it does | Native | Fibers |
|---|---|---|---|
| `start` | a started thread gets a host context that runs `run_thread` | an OS thread of its own, named after the guest thread | a fiber |
| `park` / `unpark` | the calling thread waits until woken or a deadline; a thread is woken | its parker | the fiber suspends; resumed on a host thread when woken or at its deadline |
| `give_cpu` | the scheduler gave a thread a CPU | its parker | resumed on that CPU's worker |
| `on_cpu` | the calling thread goes on to guest code holding its CPU | nothing | moves to the CPU's worker |
| `begin_host_work` / `end_host_work` | host work that may block (`Kernel::Blocking`, `lend_cpu`) | nothing | the fiber moves to a helper thread meanwhile |
| `end` | the run ended | (the OS thread ends) | the fiber's stack is given back |

Everything runs under the kernel's lock; `park` lets it go meanwhile.

## Native: one OS thread per started guest thread

Each `sceKernelStartThread` makes an OS thread with the host stack the thread needs (16 times its guest stack, 4 to
256 MiB) and the guest thread's name, and the thread ends with the run. Until 2026-11-18 ended threads' OS threads
were kept for the next starts (4 kept spare), because starting an OS thread took 30-100 ms on the development machine
in October 2026; it no longer does, and a kept thread carried its `thread_local` state into the next guest thread's
run. The runtime's own state of a guest thread is now `GuestLocal<T>` (`runtime/kernel.hpp`), made for each run:
SceFiber's per-thread state, the fios2 overlay switch, the VFP exception mask, the HLE call trace, the GXM layer's
given-up CPU flag, frame scenes and finish times.

### Measured (2026-11-18, Windows 11, Core Ultra 7 155H, RTX 4060 Laptop; release builds, clang-cl)

Thread starts. P4D (`run/script_prof.txt`: boot, menus, a song, 97 s) starts 27 guest threads, 0.28 a second,
spread over 61 s. From the start call to the thread running on its host thread
(`PSVRECORE_THREAD_STATS=1`, 4 runs each):

| | Median | Mean | Worst |
|---|---|---|---|
| Pooled (before) | 113-159 us | 163-240 us | 0.8-1.3 ms |
| Native (no reuse) | 304-664 us | 366-933 us | 0.9-4.1 ms |

A start and its end, waited for, in a loop (the runtime test "a thread started again and again", 300 starts, 3 runs):
pooled 65 000-70 000 a second (15 us each), native 5 000-6 300 a second (170 us each). P4D needs 0.28.

P4D's frame times over the song (frames after the first 25 s, three runs each at 1x, Tracy off,
`PSVRECORE_FRAME_TIMES`):

| | Frames | Mean | p50 | p95 | p99 | Max | Over 50 ms | CPU (process, each run) |
|---|---|---|---|---|---|---|---|---|
| Pooled, 1x | 6411 | 33.37 ms | 33.37 | 33.77 | 33.95 | 50.2 | 1 | 42.1, 37.5, 43.8 s |
| Native, 1x | 6420 | 33.37 ms | 33.36 | 33.76 | 33.91 | 49.6 | 0 | 28.8, 42.8, 30.7 s |
| Pooled, 2x | 2162 | 33.41 ms | 33.35 | 33.91 | 34.54 | 90.5 | 2 | 50.0 s |
| Native, 2x | 2163 | 33.43 ms | 33.37 | 33.84 | 34.06 | 124.8 | 2 | 50.7 s |

The game is locked at 30 frames a second; the frames are the same. The process's CPU time varies by a third from run
to run with the machine's other load (an antivirus, other programs), more than between the two.

Kernel calls (`PSVRECORE_HLE_COUNTS=10`, two runs each; counting adds two clock reads a call): a song makes about
4.9 million kernel calls that do not wait (`sceKernelUnlockLwMutex` 1.7 million, `sceKernelGetThreadId` 1.2 million,
`sceKernelLockLwMutexCB` 1.2 million, `sceKernelLockLwMutex` 0.5 million, `sceKernelSignalSema` 0.14 million); they
took 0.28 and 0.44 us a call pooled, 0.53 and 0.25 us native: the same within the runs' spread. The waits are the
game's (`sceKernelWaitSema` about 2.7-3.3 ms a call, 0.13-0.16 million calls).

Kept: the native backend is no worse within noise.

## Where a guest thread blocks in host code

Every place a guest thread can wait goes through the kernel, so that it gives up its CPU meanwhile and, with fibers,
leaves its CPU's worker free (audit of 2026-11-18, `runtime/src`, `gxm/src`, `platform/src`):

| Where | What it waits for | How |
|---|---|---|
| Kernel objects, `sceKernelDelayThread`, thread ends, signals, callbacks | other guest threads, time | the kernel's own waits (`Impl::block`) |
| `sceAudioOutOutput` (its block's start), sceDisplayWaitVblankStart* (`wait_vblanks`), `sceCtrlRead*` / `sceTouchRead` (the next blank's sample), `sceNgsSystemLock` (another holder's unlock, looked for every 200 us) | the kernel's clock | `Kernel::wait_until`: a kernel wait with a deadline (were host sleeps in a blocking scope; the audio one spun its last half millisecond) |
| File I/O (`sceIo*`, through libfios2 too), save data and AppUtil, module loading, SceAvPlayer's reads | the host's files | `Kernel::Blocking` |
| The display queue (`sceGxmDisplayQueueAddEntry` for room, `sceGxmDisplayQueueFinish`), the vblank thread's end | the display queue's and vblank threads (adopted host threads) | `Kernel::Blocking` |
| A scene's end (`sceGxmEndScene`: rendering, its fragment sync object, the renderer's thread), `sceGxmFinish`, notifications, the renderer's lock | the GPU, the renderer's threads | `Kernel::Lending` (the scene's rendering) and `Kernel::Blocking` (the waits inside it, `CpuGivenUp`, `TargetsHeld`) |
| H.264 decoding (`sceVideodecDecode`) | host work | `Kernel::Lending` |
| Host mutexes of the HLE layer (audio ports, NGS, the GXM queue) | another thread's short critical section | taken as is: held for microseconds, never across a kernel wait |

Not bound to an OS thread: the audio device's SDL streams (thread-safe, put from any thread), the Vulkan command pools
(one per frame slot under the renderer's lock), SDL's window and events (the frontend's main thread only).

## Fibers (`PSVRECORE_GUEST_FIBERS`)

The fiber backend (guest threads as fibers on one host worker per console CPU) is the next step; its column above
is what it is built to do.
