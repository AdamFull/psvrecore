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
| `give_cpu` | the scheduler gave a thread a CPU | its parker | resumed on a free worker (a helper if none is free) |
| `on_cpu` | the calling thread goes on to guest code holding its CPU | nothing | from a helper back to a free worker |
| `begin_host_work` / `end_host_work` | host work that may block (`Kernel::Blocking`, `lend_cpu`) | nothing | the fiber moves to a helper thread meanwhile |
| `yield` | (deterministic) at a kernel call | - | the next CPU's holder runs |
| `end` | the run ended | (the OS thread ends) | the fiber's stack is given back |

Everything runs under the kernel's lock; `park` lets it go meanwhile. The backend is chosen when the kernel starts:
native, unless the build has `PSVRECORE_GUEST_FIBERS=ON`, where fibers are the default and
`PSVRECORE_GUEST_THREADS=native` picks native threads (one build, both backends, for comparing).

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

## Fibers (`PSVRECORE_GUEST_FIBERS`, default off)

Each started guest thread is a fiber: nx-foundation's contexts (x64 Windows, x64 System V, AArch64) and stacks of the
same size as a native thread's (16 times the guest stack, 4 to 256 MiB, reserved and committed as the stack grows on
Windows, from a pool per size). The host threads that run them are fixed (`libs/runtime/src/kernel/backend_fibers.cpp`):

- **Workers**, one per console CPU (3). A thread holding a CPU runs on a free worker. Which worker does not matter:
  the scheduler has already put it on a CPU its affinity mask allows. Workers are not bound to CPUs because a thread
  that loses its CPU in guest code goes on until its next kernel call and keeps its worker meanwhile. P4D's boot
  showed why. `main` lost CPU 0 to `jobqueue` (priority 67, CPU 0 only), was given CPU 2, and spun in guest code
  waiting for `jobqueue`. With a worker per CPU, `jobqueue` waited for worker 0 forever. Now the CPU's new holder
  runs on any free worker, or on a helper if none is free, so both run, as two native threads would (the runtime
  test "a thread spinning in guest code after losing its CPU sees what the thread that took it does").
- **Helpers** run host work that blocks (`Kernel::Blocking`, `lend_cpu`: file I/O, a scene's rendering, the display
  queue): the fiber moves there and back. They also run a thread that holds no CPU, the kernel's own work after a
  wake (the end of its wait, which then waits for a CPU), and a CPU's holder when no worker is free, as above. There
  are as many helpers as are needed at once: 2 made at the start, up to 64, and a new one only when the queue outgrows
  the helpers already woken. P4D's song used 5 to 7.
- **A timer thread** wakes parked threads at their deadlines (`nx::thread_parker`, well under a millisecond).

Every switch happens with the kernel's lock held, and the host thread switched to holds it on (its OS thread took
it), so a lock is always let go by the OS thread that took it. Code on guest threads keeps no `thread_local` across a
kernel call: `Kernel::current()` and `GuestLocal` are read again after each one (both not inlined). Threads the
runtime adopts (the display queue's, the vblank callbacks', a test's main thread) stay host threads and wait on
their parkers in either build. Tracy's lanes follow the fibers: each switch enters the thread's lane on the host
thread that runs it.

**Watch for.** A guest thread that loops without kernel calls holds its worker, as it holds its CPU on the
console. Debuggers show the workers and helpers, not the guest threads: `PSVRECORE_THREADS=<seconds>` (and the
stall report) lists every guest thread with where it is, `[on 'guest worker 1']`, `[fiber parked]`.

### Deterministic scheduling (`PSVRECORE_DETERMINISTIC=1`, `Kernel::Config::deterministic`)

For tests, in a fiber build: one worker runs everything. That is the CPUs' holders in turn, switching at every kernel
call, CPU by CPU, then the threads without a CPU in the order they were woken; host work runs there too. The kernel's
clock is the backend's own: a microsecond more at each kernel call and, when nothing can run, the next deadline. The
runtime test "deterministic scheduling (fibers) runs threads in the same order run after run" runs six threads of
three priorities through a mutex, a semaphore and delays five times and gets the same 25 events in the same order
each time. Ordinary fiber runs of the same program gave four different orders in four runs. Threads the runtime
adopts (host threads) do not follow it. A thread that spins without kernel calls until another goes on never ends
there. Native threads have no such mode: the OS schedules them.

### Measured: native against fibers

The same sources, one build of each (`PSVRECORE_GUEST_FIBERS` off and on), P4D's `run/script_prof.txt`, runs taken
in turn, Tracy off (2026-11-18 04:25-04:45; the machine as above). Frame times over the song (frames after the first
25 s):

| | Frames | Mean | p50 | p95 | p99 | Max | Over 50 ms | CPU (process, each run) |
|---|---|---|---|---|---|---|---|---|
| Native, 1x (3 runs) | 6441 | 33.37 ms | 33.36 | 33.75 | 33.88 | 50.3 | 1 | 27.2, 26.8, 26.4 s |
| Fibers, 1x (3 runs) | 6398 | 33.37 ms | 33.37 | 33.74 | 33.87 | 34.5 | 0 | 27.8, 28.1, 27.3 s |
| Native, 2x | 2162 | 33.38 ms | 33.36 | 33.77 | 34.01 | 64.7 | 2 | 27.0 s |
| Fibers, 2x | 2162 | 33.37 ms | 33.37 | 33.77 | 34.00 | 45.6 | 0 | 30.4 s |

The frames are the same. Fibers took about 3% more process CPU at 1x (27.7 s against 26.8 s on average), and one
2x run each is too few to say more than "no better". (Runs after 05:16 are left out: another program started then and
kept the GPU at 100%, and both backends' 2x runs then missed frames: native 13-45 over 50 ms, fibers 98-125. The late
frames waited the same way in both, the display queue for the renderer's lock while a scene rendered.)

Kernel calls (`PSVRECORE_HLE_COUNTS=10`, two runs each):

| | Calls that do not wait | Their time a call | `sceKernelSignalSema` | `sceKernelSetEventFlag` | `sceKernelLockLwMutex` | `sceKernelDelayThreadCB` |
|---|---|---|---|---|---|---|
| Native | 4.9 million | 0.29, 0.28 us | 1.7, 1.6 us | 5.7, 5.3 us | 0.7, 0.6 us | 1.19, 1.20 ms (74 000 calls) |
| Fibers | 4.5 million | 0.31, 0.29 us | 1.4, 1.3 us | 3.4, 3.5 us | 0.9, 0.9 us | 1.36, 1.38 ms (64 000 calls) |

Waking a thread costs less with fibers (a fiber resumed against an OS thread woken). A timed wake costs more. The
timer thread wakes, the fiber ends its wait on a helper, and it moves to a worker once it has a CPU: three host
wake-ups where a native thread has one. So the game's 1 ms delays last about 0.17 ms longer, and it makes fewer of
them in a song. The runtime test "delays end soon after their deadline" measures the same: 1 ms delays end on
average 310-320 us late natively and 430-480 us late with fibers (three runs each).

Thread starts: P4D's 27, median 305-475 us natively and 12-25 us with fibers. The start-and-end loop runs 5 000-10 000
a second natively and 53 000-86 000 with fibers (pooled native threads, before: 65 000-70 000). Host threads: native
27 (one per start) plus the runtime's own; fibers 9-11 (3 workers, the timer, 5-7 helpers).

Tracy (`PSVRECORE_TRACY=ON` with fibers): builds, the runtime tests pass, and a capture of them (711 zones, the
guest threads' waits on their lanes) completes; not used for the numbers above.

### Kept

- **Native, no reuse: kept** (the default). P4D's frame times are the same as with the pool, and starts cost 0.3-0.5 ms,
  which P4D's 0.28 starts a second never notice.
- **Fibers: kept behind `PSVRECORE_GUEST_FIBERS`, off by default.** The kernel and runtime tests (117 cases, 5 runs),
  the SDK test programs (4 of 4, 3 runs each) and P4D's song at 1x and 2x pass, and it builds for Android arm64 and
  with Tracy. On this machine it is no faster: the same frames, a little more CPU, slower timed wakes. It is the only
  backend with deterministic scheduling. Turning it on by default is the user's call.
- Not tried with fibers: WipEout 2048 and DIVA F 2nd, a phone (Android arm64 is built, not run).
- Tried and not kept: sending a thread woken without a CPU to a free worker rather than a helper (one wake-up fewer
  once it gets a CPU). P4D's delays were no shorter with it (1.32-1.39 ms), and in its three 1x runs one had a 267 ms
  frame and 36.7 s of CPU, cause not found. Ideas not done: idle workers could keep the timer themselves (one wake
  for a timed wait), and workers pinned to a phone's big cores.
