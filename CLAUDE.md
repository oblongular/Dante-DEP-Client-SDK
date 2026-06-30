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

### Build the daemon

```sh
cmake -S daemon -B daemon/build -DCMAKE_BUILD_TYPE=Release
cmake --build daemon/build
```

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
uint32_t seen = static_cast<uint32_t>(period_count);
struct timespec ts { timeout_ms / 1000, (timeout_ms % 1000) * 1000000L };
long ret = syscall(SYS_futex,
    reinterpret_cast<uint32_t *>(&period_count),
    FUTEX_WAIT, seen, &ts, nullptr, 0);
// EAGAIN: period_count already changed — process immediately
// ETIMEDOUT / EINTR: retry or abort
```

Any number of clients can call `FUTEX_WAIT` simultaneously; a single
`FUTEX_WAKE(INT_MAX)` from the daemon wakes them all.

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
