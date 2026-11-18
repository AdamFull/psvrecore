# Profiling and debugging a game

The console's profiling and debugging hooks that games call (and ship with) do something useful on the host: the
game's own CPU and GPU markers appear in Tracy and in GPU debuggers, beside the runtime's own zones, and a game's
GPU capture trigger (or a key) makes a RenderDoc capture of one of its frames. In a build without Tracy and without
a GPU debugger attached, they cost nothing.

## Tracy

[Tracy](https://github.com/wolfpld/tracy) (BSD) is built in with the CMake option `PSVRECORE_TRACY=ON` (default
off). Its client is nx-foundation's copy (`libs/nx-foundation/third_party/tracy`, Tracy 0.13.1): the profiler that
connects to the game must be the same version. Off, every hook (`psvrecore/profile.hpp`) compiles to nothing.

    scripts\build.ps1 -Preset windows-x64-release -BuildDir build/tracy -Define PSVRECORE_TRACY=ON -NoTest

For a game, configure its build with the same option (psvrecore is its submodule), in a folder of its own:

    psvrecore\scripts\build.ps1 -Root . -Preset windows-x64-release -BuildDir build/tracy -Define PSVRECORE_TRACY=ON -NoTest

The client records only while a profiler is connected (Tracy's on-demand mode), so a Tracy build runs as fast as the
others until then (P4D's song: 30 frames a second, 99% of frame times under 34 ms, connected or not); it listens on
localhost only, port 8086.

### On an Android phone

The Android preset builds Tracy in the same way. The option stays in the build folder's cache, so a game's
`scripts/android_deploy.py` (which builds `build/android-arm64-release`) builds with it once the folder is configured
so, until it is set off again:

    psvrecore\scripts\build.ps1 -Root . -Preset android-arm64-release -Define PSVRECORE_TRACY=ON -NoTest
    python scripts/android_deploy.py --launch
    ...
    psvrecore\scripts\build.ps1 -Root . -Preset android-arm64-release -Define PSVRECORE_TRACY=OFF -NoTest

`scripts/android_apk.py` adds the INTERNET permission to the package when the library has Tracy's client in it
(Android refuses any socket, even one on localhost, to an application without it; `--internet` adds it anyway). Then
forward Tracy's port from the computer to the phone, start the game, and connect the profiler to `127.0.0.1` (port
8086) as for a local program:

    adb forward tcp:8086 tcp:8086
    tracy-capture -o phone.tracy -f -s 60      (or tracy-profiler: Connect to 127.0.0.1)

`adb forward --remove tcp:8086` undoes the forwarding. With several devices, give adb the phone's serial (`adb -s
<serial> forward ...`).

Start the game, then connect to it from Tracy's profiler (`tracy-profiler`, "Connect" to `127.0.0.1`), or record a
stretch from the command line and look at it afterwards:

    tracy-capture -o song.tracy -f -s 60
    tracy-csvexport song.tracy

What a game shows in Tracy:

- **A lane for each guest thread, named as the game named it** (`sceKernelCreateThread`), and one for each of its
  fibers (`sceFiberInitialize`'s name). A guest thread may run on more than one host thread (with the
  fiber backend, and a thread started again gets another), so each is a Tracy "fiber": what a guest thread did is in
  its lane whichever host thread ran it. Two threads alive at once with the
  same name get "name 2", "name 3". Host threads (the renderer's, the audio's, the display queue's) are threads of
  their own, named after their host names.
- **The game's CPU markers**: `sceRazorCpuPushMarker`, `sceRazorCpuPushMarkerWithHud` (with its colour),
  `sceFiberPushUserMarkerWithHud` and their pops are zones of the game's label in the calling thread's (or fiber's)
  lane, nested as pushed.
- **The runtime's zones**: a guest thread's waits, by what it waits for (`wait: semaphore`, `wait: event flag`,
  `wait: lightweight mutex`, `wait: delay`, ..., with the object's name as the zone's text); `waiting for a CPU`
  (ready, behind threads of higher or equal priority on the three CPUs); `blocked on the host` (a GPU or display wait,
  blocking file reads: the CPU given up meanwhile); `CPU lent (host work)` and `taking the CPU back` (a scene rendered
  on the game's thread while its CPU is lent to the others); `gxm: render scene`, `gxm: waiting for scenes on the GPU`,
  `gxm: frame queued`, `display: frame shown`.
- **Plots**: `kernel: CPUs running a thread` (0 to 3) and `kernel: threads ready without a CPU`.
- **Frame marks** at each frame displayed (handed to the window), so Tracy's frame graph is the game's frame times.
- **GPU zones** ("GXM scenes" context): each scene's commands as a `scene` zone, and the game's GPU markers
  (`sceGxmPushUserMarker` / `sceGxmPopUserMarker`) inside it, on the CPU's timeline (calibrated with
  `VK_KHR_calibrated_timestamps` where the device has it).

`sceRazorCpuIsCapturing` answers whether a profiler is connected; `sceRazorCpuStartCapture` and
`sceRazorCpuStopCapture` do nothing else (what is recorded is what Tracy records while connected).

## Frame times, and sampling the CPU on an Android phone

`PSVRECORE_FRAME_TIMES=<file>` writes each frame's time (microseconds, from one frame shown to the next) a line at a
time, so a run stopped from outside keeps them all; on Android give it through the launcher's arguments
(`--esa args "...,--env,PSVRECORE_FRAME_TIMES=/data/user/0/<package>/files/ft.txt"`) and read it back with `adb
exec-out run-as <package> cat files/ft.txt`.

Where a thread's time goes, without a Tracy build: Android's `simpleperf` samples the game's process. The shell user
may not profile another application, but a debuggable package (`android_apk.py --debuggable`, as `android_deploy.py`
makes it) may profile itself, so run it through `run-as`, user space only, and pull the data the same way:

    adb shell "run-as <package> /system/bin/simpleperf record -p $(adb shell pidof <package>) -e task-clock:u -f 2000 --duration 15 -o files/perf.data"
    adb exec-out run-as <package> cat files/perf.data > perf.data
    simpleperf report -i perf.data --sort comm                                    (the NDK's simpleperf/bin/<host>)
    simpleperf report -i perf.data --symdir <a folder with the unstripped libmain.so> --comms <thread> --sort symbol

The build folder's `libmain.so` has the symbols (the package's is stripped); the game's own threads are named as the
game named them (cut to 15 characters), the recompiled code's functions `f_<address>`.

## GPU debug labels

The renderer names its commands with `VK_EXT_debug_utils` labels, which RenderDoc, Nsight Graphics and Radeon GPU
Profiler show as the list of commands:

    scene 1234: 960x544 colour 0x60440000 at 2x    (its number, surface size, kind, address and internal resolution)
      draws                                        (the render pass)
        <the game's markers, nested as pushed; sceGxmSetUserMarker's as single points>
      write-back                                   (the copies of what guest memory gets)

The labels are on with the validation layer (`--validation`), when RenderDoc is in the process, in a Tracy build, and
with `PSVRECORE_GPU_LABELS=1` (for another tool, Nsight or RGP); otherwise nothing is recorded, as the console's
library records no user markers without Razor's capture module. The game's markers are kept in the scene's draw list
at their place among its draws (and in command lists: deferred contexts); a pop without its push is left out, and a
push the scene leaves open ends with its draws.

## RenderDoc captures

Start the game from [RenderDoc](https://renderdoc.org) (its UI's "Launch Application", or
`renderdoccmd capture -w <game>.exe ...`). The runtime finds RenderDoc's in-application API
(`third_party/renderdoc/renderdoc_app.h`, MIT; RenderDoc is never loaded by psvrecore itself) and then:

- **F12** captures the game's next frame: its scenes from the first after the next frame boundary (a frame queued for
  the display, or set) to the last before the one after. RenderDoc's own capture keys are turned off, since they
  capture from one host present to the next, which does not bound a frame of the game's scenes.
- `[display] capture_frame = N` (or `--set display.capture_frame=N`) captures the N-th frame the game queues or sets.
- **The game's Razor triggers**: `sceRazorGpuCaptureSetTriggerNextFrame` (and the deprecated
  `sceRazorCaptureSetTriggerNextFrame`) capture its next frame; `sceRazorGpuCaptureSetTrigger(frame, file)` the frame
  after that many `sceGxmPadHeartbeat` calls. The capture's title says which; the game's file name is not used
  (RenderDoc keeps its captures where its UI says). `sceRazorGpuCaptureIsInProgress` is true from the request until
  the capture is written.

Without RenderDoc these succeed and do nothing (the first request is logged).

## The console's functions, on the host

| Library | What it does here |
|---|---|
| ScePerf: `sceRazorCpuPushMarker`, `...WithHud`, `sceRazorCpuPopMarker` | Tracy zones (above); nothing without Tracy |
| ScePerf: `sceRazorCpuStartCapture`, `StopCapture`, `IsCapturing`, `Sync` | Accepted; capturing means a profiler is connected |
| ScePerf: `scePerfGetTimebaseValue`, `scePerfGetTimebaseFrequency` | The kernel's clock (the guest's time) in nanoseconds; 1000 ticks a microsecond, as the SDK's samples divide by it |
| ScePerf: `scePerfArmPmon*` (the ARM performance monitor) | Accepted, every counter reads 0: the host has no equivalent of the Cortex-A9's events worth faking. A counter that does not exist is refused (`SCE_PERF_ERROR_INVALID_ARGUMENT`) |
| ScePerf: the user marker trace and the activity monitor | The game's buffer taken and read back with no packets in it |
| ScePerf: `_sceRazorCpuWriteFiberUltPkt`, `_pLibPerfCaptureFlagPtr` | libult's packets: its capture flag stays 0, so it writes none |
| SceRazorHud | Accepted (no HUD, no GPU metrics); `sceRazorGpuPerfGetMode` answers "none" |
| SceRazorCapture | RenderDoc captures (above) |
| SceGxm: `sceGxmPushUserMarker`, `PopUserMarker`, `SetUserMarker` | Debug labels and Tracy GPU zones (above); refused outside a scene (immediate context) or a command list (deferred), as on the console |
| SceGxm: `sceGxmAddRazorGpuCaptureBuffer`, `RemoveRazorGpuCaptureBuffer`, `sceGxmIsDebugVersion` | Accepted; the release library (false) |
| SceDbg: `sceDbgLoggingHandler`, `sceDbgAssertionHandler` | The game's `SCE_DBG_LOG_*` and failed `SCE_DBG_ASSERT*` messages, formatted with their arguments, to psvrecore's log (`hle`) at their level (trace and debug: debug; info; warning; error; assertions: error) with the component, file and line |
| SceDbg: `sceDbgSetMinimumLogLevel`, `SetBreakOnErrorState`, `SetBreakOnWarningState` | The level kept (messages below it are dropped); no debugger to break into |
| SceUlobjDbg | libult's debugger hooks: accepted |
| SceDebugLed: `sceKernelSetGPO`, `sceKernelGetGPI` | The development kit's LEDs: a change logged at debug level; the switches read 0 |
