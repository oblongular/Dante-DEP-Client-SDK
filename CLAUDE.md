# Dante-DEP-Client-SDK — Claude Instructions

## What This Is

A stripped-down SDK for the Audinate DEP (Dante Embedded Platform) shared-memory audio API.
Derived from `../sw__dep_examples` and trimmed to the subset needed by a JUCE audio backend.

The implementation is C++17. The library (`libDanteAudio.a`) is built by compiling
`src/DanteAudio.cpp` against the closed-source `lib/libdep_audio.a` supplied by Audinate.

```
src/DanteAudio.cpp               — non-template method implementations
include/dante/DanteAudio.hpp     — consolidated SDK header (BufferView, BufferBlockAccessor,
                                   DefaultBufferContext, SharedMemory, Buffers, Timing, …)
lib/libdep_audio.a               — Audinate DEP shared-memory library (prebuilt, target-specific)
lib/libDanteAudio.a              — DanteAudio.cpp + libdep_audio.a bundled (committed prebuilt)
cmake/libDanteAudio.cmake        — CMake: imports libDanteAudio.a as the DanteAudio target
daemon/dep_sync_fanoutd.cpp      — Relay daemon (see Period Synchronisation section)
daemon/CMakeLists.txt            — CMake build for the daemon
daemon/dep_sync_fanoutd.service  — systemd unit
tools/dep_wakeup_counter.cpp     — Diagnostic: counts real FUTEX_WAKE deliveries/sec
tools/CMakeLists.txt             — CMake build for the tools
```

---

## Building the SDK

### Prerequisites

- A C++17 toolchain (g++ ≥ 9 or clang++ ≥ 10)
- `lib/libdep_audio.a` for the target architecture (from Audinate DEP SDK)

### Build and update the committed library

```sh
# Compile DanteAudio.cpp
g++ -O2 -std=c++17 -c src/DanteAudio.cpp -Iinclude -o /tmp/DanteAudio.o

# Extract libdep_audio.a objects into a temp dir
mkdir -p /tmp/dep_objs
(cd /tmp/dep_objs && ar x $(pwd)/../../lib/libdep_audio.a)

# Bundle into a single self-contained archive
ar rcs lib/libDanteAudio.a /tmp/DanteAudio.o /tmp/dep_objs/*.o
```

Commit `src/DanteAudio.cpp`, `include/dante/DanteAudio.hpp`, and the updated
`lib/libDanteAudio.a` together.

### Cross-compile for aarch64 Linux

Replace `g++` above with an aarch64 cross-compiler, e.g.:
```sh
aarch64-linux-gnu-g++ -O2 -std=c++17 -c src/DanteAudio.cpp -Iinclude -o /tmp/DanteAudio.o
```
Use the aarch64 `libdep_audio.a` from the Audinate DEP SDK for that target.

### Build the daemon and diagnostic tools

```sh
cmake -S daemon -B daemon/build -DCMAKE_BUILD_TYPE=Release
cmake --build daemon/build

cmake -S tools -B tools/build -DCMAKE_BUILD_TYPE=Release
cmake --build tools/build
```

The flake (`nix build .#dep-client-sdk`) runs both as separate configure/build/install
cycles — see `flake.nix`'s `configurePhase`/`buildPhase`.

---

## Architecture

```
JUCE backend (C++)
  → include/dante/DanteAudio.hpp   (SDK header — templates + class declarations)
    ← src/DanteAudio.cpp           (non-template implementations)
    ← lib/libdep_audio.a           (Audinate: SharedMemory, Buffers, Timing implementations)
    → lib/libDanteAudio.a          (the two above bundled as a prebuilt)
```

`DanteAudio.hpp` is the single include for consumers. The template methods
`accessTxBlock<Fn>` and `accessRxBlock<Fn>` are defined in the header so the compiler
instantiates them at the call site. All other methods are defined in `DanteAudio.cpp`.

### Shared memory layout (`DanteAudio.hpp` — `buffer_header_t`)

The library opens `/dev/shm/<name>` (via `shm_open`) and optional separate
`/dev/shm/<name>Tx` + `/dev/shm/<name>Rx` regions when
`DANTE_BUFFERS_FLAG__SEPARATE_CHANNEL_MEMORY` is set.

The critical `period_count` field is at byte offset **80** from the start of
`buffer_header_t`:
```
metadata (32 bytes) + audio (32 bytes)
  + time.{ epoch_seconds(4) + epoch_samples(4) + samples_per_period(4) + _pad(4) } (16 bytes)
  = 80
```
(The 4-byte pad is inserted by the compiler to align the subsequent `uint64_t`.)

---

## Key Implementation Notes

- **`accessTxBlock` / `accessRxBlock`** take a callback and must stay in the header for
  the compiler to instantiate them at the call site.
- **`BlockAccessorConfig`** carries TX latency in microseconds (default 10 000 µs).
- **Late error handling**: when the accessor reports late frames, callers should use
  `fastForwardTx` / `fastForwardRx` to skip ahead rather than accumulate drift.
  See `dep_loopback/DanteLoopback.cpp` for the reference implementation of this pattern.
- **`frames` must be capped to `periodSize`** before passing to the audio callback and
  to `readRxToFloat`/`writeFloatToTx`. After a late recovery `getTxFramesToWrite()`
  returns `mTxLatencyFrames` (48 at 48 kHz / 1 ms), which exceeds `periodSize` (16),
  causing out-of-bounds writes into the `periodSize`-allocated staging buffers.
  Cap: `frames = std::min((unsigned)txFrames, periodSize)`.

---

## Real DEP Buffer Geometry (verified on target hardware)

From `dinfo` on the target:

| Parameter | Value |
|---|---|
| Sample rate | 48000 Hz |
| Samples per channel (buffer depth) | 48000 (= 1 second) |
| Samples per period | **16** |
| TX/RX channels | 32 each |
| Encoding | PCM32 |

Period duration at 48 kHz: **333 µs**. The TX latency default of 1000 µs = **48 frames**,
which is 3 periods. This is correct — the latency spans multiple periods by design.

---

## Period Synchronisation — Timing Object and Futex

### What the timing object is

The `timing_object_subheader_t` names a **POSIX named semaphore** (Linux) or an
**auto-reset Win32 Event** (Windows). The DEP daemon calls `sem_post()` once per period.

**Critical**: this is NOT a broadcast primitive. `sem_post()` unblocks exactly one
`sem_wait()` caller. If multiple clients open and wait on the same semaphore, they compete
— only one wakes per period. The others starve or receive periods at a fraction of the
real rate. **Only one process may call `Timing::wait()`.**

### dep_sync_fanoutd — the relay daemon

`dep_sync_fanoutd` is the sole consumer of the DEP semaphore. It broadcasts a futex wake
after each period so any number of clients can block on `period_count` simultaneously:

```
dep_sync_fanoutd:
    sem_timedwait(semaphore)            // sole holder of the DEP semaphore
    FUTEX_WAKE(INT_MAX, &period_count)  // broadcast to all waiting clients
    loop
```

Source: `daemon/dep_sync_fanoutd.cpp`. Built with `daemon/CMakeLists.txt`.
Systemd unit: `daemon/dep_sync_fanoutd.service` — `BindsTo=dep.service`.

**Important**: once `dep_sync_fanoutd` is running, no other process should call
`Timing::wait()` (which calls `sem_timedwait` on the same semaphore). Doing so causes
semaphore contention and missed periods. Clients must switch to `FUTEX_WAIT` on
`period_count` instead of using the `Timing` class.

### Client-side futex wait pattern

Clients that use the daemon wait on the low 32 bits of `header->time.period_count`
using `FUTEX_WAIT` (no `FUTEX_PRIVATE_FLAG` — the kernel keys on the physical page,
enabling cross-process wake):

```cpp
uint32_t seen = static_cast<uint32_t>(header->time.period_count);
struct timespec ts { timeout_ms / 1000, (timeout_ms % 1000) * 1000000L };
long ret = syscall(SYS_futex,
    getPeriodCountFutexWord(header),
    FUTEX_WAIT, seen, &ts, nullptr, 0);
// EAGAIN: period_count already changed — process immediately
// ETIMEDOUT / EINTR: retry or abort
```

`getPeriodCountFutexWord(header)` (in `DanteAudio.hpp`) is the **single canonical
place** that knows `period_count`'s offset and does the volatile/const-cast dance
to hand back a `uint32_t*` usable in `FUTEX_WAIT`/`FUTEX_WAKE`. Both
`DanteAudio.cpp`'s `futex_wait_period()` (the wait side) and
`dep_sync_fanoutd.cpp`'s main loop (the wake side) call it — don't hand-roll the
cast again at a new call site. If `buffer_header_t`'s layout ever changes, this is
the one function to update; it does **not**, however, protect an already-built
binary from a server-side layout change (see "Known limitation" below).

Any number of clients can call `FUTEX_WAIT` simultaneously; a single
`FUTEX_WAKE(INT_MAX)` from the daemon wakes them all.

**Known limitation — no ABI/version check**: the field offset is resolved at
compile time via the C++ struct definition. If DEP's shared-memory layout changes
after a client is built, that client keeps computing the *old* offset and silently
waits on/reads the wrong word — there is no runtime version negotiation. The only
check in place is `dep_sync_fanoutd`'s `magic_marker == DANTE_BUFFERS_HDR_MAGIC`,
which confirms "this is a DEP buffer" but not "this is the layout I was built
against." Not solved here — worth addressing before this SDK survives a DEP
header revision.

### depasound needed the same migration

`depasound.so` (Audinate's ALSA bridge, built from `sw__dep_examples`) is **not**
part of this repo, but it independently solved the exact same "only one process
may hold the semaphore" problem with its own bespoke mechanism: `depThread()`
called `SyncClient()`, which self-forked a socket-based fan-out server
(`dep_notification_server` — `SyncServer`/`SyncClient`/`SyncWait`) that itself held
the DEP semaphore via `Dep_Timing::Sleep()` → `timing.wait()`. Enabling
`dep_sync_fanoutd` on any host that also runs `depasound` recreated the multi-consumer
semaphore contention this whole design exists to avoid, just one layer down —
manifesting as intermittent audio ("comes and goes") on both sides.

Fixed via `fix-depasound-futex-sync.patch` in the `rnd__aidan` nix repo
(`dep_examples/package.nix`), which:
- Deletes `dep_notification_server.cpp`/`.hpp` and `SyncClient`/`SyncServer`/`SyncWait`
  entirely (removed from `dep_example_apps/CMakeLists.txt`'s `depasound_card` target
  and from `depasound/Makefile`).
- Replaces `depThread()`'s `SyncWait(notify_fd)` with a direct `FUTEX_WAIT` on
  `DEP.Hdr->time.period_count`, via a **local** `getPeriodCountFutexWord()` mirroring
  this SDK's version — `depasound` uses Audinate's own `dep_audio_buffers` copy of
  `buffer_header_t`, a structurally identical but type-distinct struct from this SDK's,
  so the function can't literally be shared across the two codebases.

This means `depasound` now hard-depends on `dep_sync_fanoutd` running — it used to be
self-sufficient. In `rnd__aidan/dep/module.nix`, `services.dep_examples.depasound.enable`
has a NixOS `assertions` entry requiring `services.dep.sync_fanoutd.enable`, and the
latter defaults to `true` whenever `services.dep.enable` is true.

**Note**: `DefaultBufferContext` no longer holds or opens a `Timing` object.
`mTiming` has been removed from the class; `connect()` skips the semaphore open,
and `disconnect()` skips the semaphore close. The daemon is now the sole semaphore consumer.

### Migration path

When the DEP server is updated to call `FUTEX_WAKE(INT_MAX, &period_count)` natively
after each period increment, `dep_sync_fanoutd` is simply removed from the service
configuration. Clients require no changes — they already use `FUTEX_WAIT`.

| Stage | Server | dep_sync_fanoutd | Clients |
|---|---|---|---|
| Now | semaphore only | running | `FUTEX_WAIT` |
| After server update | semaphore + `FUTEX_WAKE` | removed | `FUTEX_WAIT` — unchanged |

The `Dante::Timing` class and semaphore path can be removed from the SDK entirely once
server-native futex wake is deployed.

---

## JUCE Native Dante Backend (recreate-from-scratch notes)

This backend is **not part of this repo** — it's a JUCE fork patch, consumed by JUCE
apps that link against this SDK. Documented here because it's the primary consumer of
`DanteAudio.hpp` and its correct implementation depends on several non-obvious details
discovered during testing, none of which are written down anywhere else.

### Where it lives

- Fork: `github:oblongular/JUCE`, branch `futex-wait`, file
  `modules/juce_audio_devices/native/juce_Dante.cpp` (classes `DanteAudioIODeviceType`
  and `DanteAudioIODevice` in `namespace juce::DanteClasses`). Hooked into JUCE proper via
  `AudioIODeviceType::createAudioIODeviceType_Dante()` (declared in
  `juce_AudioIODeviceType.h`, guarded by the `JUCE_DANTE` config macro,
  `juce_audio_devices.h`) and registered in `AudioDeviceManager::createAudioDeviceTypes()`.
- Distributed as `JUCE-add-dante-backend.patch` in the `JUCE-Dante-TestApp` repo, applied
  by its `flake.nix` (`pkgs.applyPatches`) on top of a vanilla JUCE 8.0.14 tarball fetched
  fresh from GitHub — the patch is the *only* copy of this backend that downstream builds
  actually consume; the `~/JUCE` fork is just where it's developed and diffed from.
- **Regenerating the patch**: `cd` into the `~/JUCE` fork and run
  `git diff <vanilla-import-commit> --no-color > JUCE-add-dante-backend.patch` (copy the
  result into `JUCE-Dante-TestApp/`). The vanilla-import commit is the one whose message
  is literally `"JUCE version 8.0.14"` — find it with
  `git log --oneline -- CMakeLists.txt | tail -1` or similar; verify by confirming the
  regenerated patch is byte-identical to the currently-committed one *before* making new
  changes, since a wrong base commit produces a patch that silently includes unrelated
  upstream JUCE diffs. `git diff <base>` (no second ref) diffs against the working tree,
  so uncommitted changes in `~/JUCE` are picked up without needing a commit first.

### Device/type naming

- The backend registers as `AudioIODeviceType` named `"Dante"` — analogous to `"ALSA"`,
  a peer backend, not a device within it.
- It exposes exactly one device, named after the DEP shared-memory buffer it connects to
  (`kEndpointName = "DanteEP"`, a `static constexpr const char*` in `juce_Dante.cpp` —
  currently hardcoded, not derived from a command-line/config value). `-l DanteEP` in
  `JUCE-Dante-TestApp` matches this name.
- `scanForDevices()` does a throwaway `DefaultBufferContext::connect()`/`wait()`/
  `disconnect()` cycle purely to populate `cachedNumInputs`/`cachedNumOutputs`/
  `cachedSampleRate`/`cachedPeriodSize` for the real `open()` later.

### Buffer size: deliberately fixed, not accumulated

`getAvailableBufferSizes()` returns a single-element array containing the current DEP
`periodSize` (16 at 48 kHz) — **not** a range. This is a direct pass-through with no
buffering logic: one DEP period in, one JUCE callback out. Contrast with the ALSA/
`depasound` path, which legitimately supports a range of buffer sizes (16/32/48/512...)
because `depasound` accumulates multiple DEP periods into a staging buffer before ALSA's
`ioplug` invokes the application (`snd_pcm_ioplug_set_param_minmax` on
`HW_PERIOD_BYTES`/`HW_BUFFER_BYTES`). Adding equivalent accumulation to the native Dante backend is possible but not
implemented — it would add real complexity to the late-recovery/`fastForward*` logic
(which currently reasons in single-period increments) and reintroduce the latency this
backend exists to avoid. Only worth doing for a concrete consumer that needs native
(non-ALSA) Dante access at a coarser buffer size; `depasound` already covers the
"I want a bigger, more forgiving buffer" case.

### The audio thread: three non-obvious real-time scheduling gotchas

`DanteAudioIODevice` privately inherits `Thread` and runs its DEP-period-wait +
audio-callback loop (`run()`/`runAudioLoop()`) on that thread, started from `start()`.
Getting this thread real-time-scheduled correctly on Linux took three separate fixes:

1. **`startThread(Thread::Priority::highest)` is a no-op on Linux.** JUCE's own source
   comment on `Thread::startThreadInternal()` says non-realtime priority "is essentially
   useless on Linux as only realtime has any options." Confirmed empirically: the thread
   showed up as `CLS=TS` (plain `SCHED_OTHER`) in `ps -eLo`, not `FF`/`RR`. Must use
   `startRealtimeThread(Thread::RealtimeOptions)` instead.

2. **`startRealtimeThread()` maps to `SCHED_RR` on Linux, not `SCHED_FIFO`.** Traced
   through `juce_Threads_linux.cpp` → `PosixSchedulerPriority::getNativeSchedulerAndPriority()`
   in `juce_SharedCode_posix.h`: `isRealtime ? SCHED_RR : ...`. JUCE's portable API has no
   way to request `SCHED_FIFO` specifically (unlike `dep_sync_fanoutd`/DepApe, which use
   raw `chrt -f`/`SCHED_FIFO`). Decided this is fine: RR vs. FIFO only differ when multiple
   threads share the same priority and need to time-slice, which doesn't apply as long as
   this thread runs on `perf_tuning.dsp_cores` (isolated from `dep_cores`/`net_cores`).
   Priority chosen: `RealtimeOptions().withPriority(8)` (JUCE's 0-10 scale) — JUCE maps
   this linearly onto `sched_get_priority_min/max(SCHED_RR)` (typically 1-99 on Linux),
   landing at raw priority ~79-80, matching this project's `dsp_rtprio` NixOS convention.
   Verified on target hardware via `rt-ps.sh`: `RTPRIO=79`, `CLS=RR`.

3. **A failed `startRealtimeThread()` means NO thread runs at all — not a degraded
   fallback.** Traced through `Thread::createNativeThread()` → `pthread_create()` with the
   realtime scheduling attributes already attached via `pthread_attr_setschedpolicy()`
   *before* the call. If that `pthread_create()` fails (most likely `EPERM`: no
   `CAP_SYS_NICE` / `RLIMIT_RTPRIO` is 0 for the running user), `makeThreadHandle()`
   returns `nullptr` and `startRealtimeThread()` returns `false` with **no thread created
   at all** — silent, total audio failure, not jitter. `start()` must check the return
   value and log loudly (`Logger::writeToLog` + set `lastError`) rather than silently
   continuing as if playback might still work.

### Thread naming: 15-character limit, silently enforced

The audio thread is named via `Thread("JUCE/DanteAudio")` (constructor parameter).
Linux's `TASK_COMM_LEN` is 16 bytes **including the null terminator** — 15 usable
characters. JUCE's `Thread::setCurrentThreadName()` (`juce_SharedCode_posix.h`) calls
`pthread_setname_np()` and **discards its return value** with no length check
beforehand. A too-long name (`"JUCE/Dante-Audio"`, 16 chars, was the first attempt)
makes `pthread_setname_np()` return `ERANGE` and leave the kernel-visible name
**unchanged** — the thread then shows up under the inherited process name instead
(confirmed via `ps -eLo comm` showing the parent process's name for both its threads).
Keep this name at ≤15 characters, and re-verify with `ps -T`/`rt-ps.sh` after any rename,
since there's no compile-time or runtime warning when it silently fails.

### Consumer gotcha: don't touch `AudioDeviceManager` for a named Dante connection

Discovered via `JUCE-Dante-TestApp`, not the backend itself, but critical for anyone
building a new consumer: `juce::AudioDeviceManager::initialise()` and
`getAvailableDeviceTypes()` unconditionally scan **every** registered backend, ALSA
included. On a host running `depasound`, that touches ALSA PCMs backed by `depasound`,
which can crash (observed: SEGV inside `depasound.so`'s `snd_pcm_ioplug_poll_revents`
during `AudioDeviceManager`'s default-device open/teardown race) and drags in exactly the
semaphore-contention problem described above, even when the app only wants the native
Dante device. Fix: when a specific Dante endpoint is wanted, bypass `AudioDeviceManager`
entirely — use `juce::AudioIODeviceType::createAudioIODeviceType_Dante()` directly, call
`scanForDevices()`/`getDeviceNames()`/`createDevice()`/`open()`/`start()` on the raw
`AudioIODeviceType`/`AudioIODevice` interfaces. Reserve `AudioDeviceManager` for genuine
"list every backend" use cases (e.g. no specific device requested).
