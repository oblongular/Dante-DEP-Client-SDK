# Dante-DEP-Client-SDK — Claude Instructions

## What This Is

A stripped-down C++ SDK for the Audinate DEP (Dante Embedded Platform) shared-memory audio
API. Derived from `../sw__dep_examples` and trimmed to the subset needed by a JUCE audio
backend. Ships as a single header and a single prebuilt static library.

```
include/dante/DanteAudio.hpp    — public API (single header)
src/DanteAudio.cpp              — implementation (compiled into libDanteAudio.a)
lib/libDanteAudio.a             — committed prebuilt: libdep_audio.a + DanteAudio.o
lib/libdep_audio.a              — original Audinate prebuilt (input to dist build only)
cmake/libDanteAudio.cmake       — CMake: imports the library as the DanteAudio target
```

---

## Recreating the SDK from Scratch

### Source Material

**Reference example** — read this first. It demonstrates every pattern the SDK supports:
```
../sw__dep_examples/dep_example_apps/dep_loopback/DanteLoopback.cpp
```

**Headers to consolidate** into `DanteAudio.hpp`:
```
../sw__dep_examples/dep_audio_buffers/dep_audio_buffers/include/dante/Buffers.hpp
../sw__dep_examples/dep_audio_buffers/dep_audio_buffers/buffer_client/media/include/dante/BufferView.hpp
../sw__dep_examples/dep_audio_buffers/dep_audio_buffers/buffer_client/media/include/dante/BufferBlockAccessor.hpp
../sw__dep_examples/dep_audio_buffers/dep_audio_buffers/buffer_client/media/include/dante/BufferContext.hpp
../sw__dep_examples/dep_audio_buffers/dep_audio_buffers/buffer_client/media/include/dante/IDanteBuffers.hpp
../sw__dep_examples/dep_audio_buffers/dep_audio_buffers/buffer_client/media/include/dante/Log.hpp
```

**Prebuilt library** — commit as `lib/libdep_audio.a`, never modify:
```
../sw__dep_examples/dep_audio_buffers/dep_audio_buffers/lib/<platform>/libdep_audio.a
```
Contains the compiled Audinate platform objects (`DanteBuffers`, `DanteSharedMemory`,
`DanteTiming`, `DanteRunner`, `DanteTelemetry`, `DdhiClient`).

---

### Step 1 — Consolidate into DanteAudio.hpp

Merge all six headers above into `include/dante/DanteAudio.hpp` inside `namespace Dante {}`.

Required standard includes:
`<algorithm>`, `<atomic>`, `<chrono>`, `<cstdarg>`, `<cstdint>`, `<cstdio>`, `<ctime>`,
`<functional>`, `<memory>`, `<stdexcept>`, `<string>`, `<thread>`, `<vector>`, `<sys/types.h>`

**Keep inline in the header** (must not move to .cpp):
- Template definitions: `accessTxBlock<Fn>`, `accessRxBlock<Fn>`
- `fastForwardTx()`, `fastForwardRx()` — real-time audio path
- `sampleAt()` — inline sample accessor
- `memory_barrier_acquire()` — must be inline for correctness on all architectures
- Trivial one-line getters on `BufferView` and `BufferBlockAccessor`

**Declare in header, define in DanteAudio.cpp**:
- `toString(LogLevel)`, `fromString(LogLevel)`
- `PrintfLogger::log()`
- All non-trivial `BufferView` methods: `poll()`, `reset()`, timestamp helpers, channel descriptions
- All non-trivial `BufferBlockAccessor` methods: `setChannels()`, `updateAvailable()`,
  `getTxFramesToWrite()`, `getAvailRxFrames()`, head management
- All `DefaultBufferContext` methods: constructor, destructor, `connect()`, `disconnect()`,
  `wait()`, timeout handling, accessor registration

---

### Step 2 — What to Remove

The following exist in the source headers but must NOT be included in the SDK:

| Remove | Reason |
|---|---|
| `Runner` class and `RunnerActiveChangedFn` | Consumers implement their own event loop |
| `Telemetry` class | Requires DDHI/JSON chain, not needed by audio-only consumers |
| `DdhiClient.hpp` and all DDHI code | Pulls in `nlohmann/json.hpp` (24k-line header) |
| `Priority.hpp` / `setDantePriority()` / `cleanupDantePriority()` | Consumers manage thread priority themselves |
| `IDanteTiming.hpp` | Included transitively but never used |
| `writeTxFrames()` / `readRxFrames()` and sample format helpers | Block accessor pattern (`accessTxBlock`/`accessRxBlock`) is sufficient |
| `connect()` overloads taking a mapped-file buffer path | Embedded/file-backed buffers only |
| `nlohmann/` directory | Only needed by the removed telemetry chain |

The `ddhiTelemetry` parameter in `DefaultBufferContext`'s constructor may be left as a
commented-out no-op (`/*ddhiTelemetry*/`) to avoid breaking existing call sites.

---

### Step 3 — Create src/DanteAudio.cpp

```cpp
#include <dante/DanteAudio.hpp>

namespace Dante {
// all non-template implementations
}
```

Move every non-trivial, non-template function body listed above out of the header into this file.

---

### Step 4 — Build and Commit the Combined Library

`libDanteAudio.a` is produced by compiling `DanteAudio.cpp` and merging the resulting object
with `libdep_audio.a`. The `dante_sdk_dist` CMake target in the consuming project reproduces
this step exactly — it is the canonical record of how the committed `.a` was built.

To rebuild after changing `DanteAudio.cpp` (run from the consuming project directory):
```sh
cmake --build build --target dante_sdk_dist
cp build/dante-dep-sdk/lib/libDanteAudio.a /path/to/Dante-DEP-Client-SDK/lib/libDanteAudio.a
```

Commit `DanteAudio.cpp` and the updated `libDanteAudio.a` together.

---

### Step 5 — cmake/libDanteAudio.cmake

This file only defines the consumer-facing `DanteAudio` imported target. It does not
compile any source or define any build targets. Consumers include it and link against
`DanteAudio` — no SDK source compilation is required on their end.

The dist build logic (`dante_audio_impl` OBJECT library, `dante_sdk_dist` custom target)
lives in the consuming project's `CMakeLists.txt`, not here.

---

## Key Implementation Notes

- **`memory_barrier_acquire()`** emits `__sync_synchronize()` — must be at the call site.
- **`accessTxBlock` / `accessRxBlock`** take a callback and must stay in the header for
  the compiler to instantiate them at the call site.
- **`BlockAccessorConfig`** carries TX latency in microseconds (default 1000µs).
- **Late error handling**: when the accessor reports late frames, callers should use
  `fastForwardTx` / `fastForwardRx` to skip ahead rather than accumulate drift.
  See `dep_loopback/DanteLoopback.cpp` for the reference implementation of this pattern.
