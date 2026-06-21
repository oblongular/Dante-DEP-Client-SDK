# Dante-DEP-Client-SDK — Claude Instructions

## What This Is

A stripped-down C++ SDK for the Audinate DEP (Dante Embedded Platform) shared-memory audio
API. Derived from `../sw__dep_examples` and trimmed to the subset needed by a JUCE audio
backend. Ships as a single header and a single prebuilt static library.

```
include/dante/DanteAudio.hpp    — public API (single header)
src/DanteAudio.cpp              — implementation (compiled into libDanteAudio.a)
lib/libDanteAudio.a             — committed prebuilt: libdep_audio.a + DanteAudio.o
lib/libdep_audio.a              — Audinate prebuilt built from sw__dep_examples (input to dist build only)
cmake/libDanteAudio.cmake       — CMake: imports the library as the DanteAudio target
```

---

## Recreating the SDK from Scratch

### Prerequisites

- `../sw__dep_examples` checked out alongside this repo
- A C++11 toolchain with `g++` and `ar` (provided by `nix develop` in the test app repo)

---

### Step 1 — Build libdep_audio.a from the Audinate source

From `../sw__dep_examples/dep_audio_buffers/dep_audio_buffers/`:

```sh
make -f make/Makefile-lib
```

This produces `libdep_audio.a` in that directory. Copy it into the SDK:

```sh
cp ../sw__dep_examples/dep_audio_buffers/dep_audio_buffers/libdep_audio.a lib/libdep_audio.a
```

This file contains the compiled Audinate platform objects: `DanteBuffers`, `DanteSharedMemory`,
`DanteTiming`, `DanteRunner`, `DanteTelemetry`, `DdhiClient`. Never modify it.

---

### Step 2 — Consolidate into include/dante/DanteAudio.hpp

Merge these six headers from `../sw__dep_examples` into `include/dante/DanteAudio.hpp`
inside `namespace Dante {}`:

```
../sw__dep_examples/dep_audio_buffers/dep_audio_buffers/include/dante/Buffers.hpp
../sw__dep_examples/dep_audio_buffers/dep_audio_buffers/buffer_client/media/include/dante/BufferView.hpp
../sw__dep_examples/dep_audio_buffers/dep_audio_buffers/buffer_client/media/include/dante/BufferBlockAccessor.hpp
../sw__dep_examples/dep_audio_buffers/dep_audio_buffers/buffer_client/media/include/dante/BufferContext.hpp
../sw__dep_examples/dep_audio_buffers/dep_audio_buffers/buffer_client/media/include/dante/IDanteBuffers.hpp
../sw__dep_examples/dep_audio_buffers/dep_audio_buffers/buffer_client/media/include/dante/Log.hpp
```

Read `../sw__dep_examples/dep_example_apps/dep_loopback/DanteLoopback.cpp` first — it
demonstrates every pattern the SDK needs to support.

Required standard includes:
`<algorithm>`, `<atomic>`, `<chrono>`, `<cstdarg>`, `<cstdint>`, `<cstdio>`, `<ctime>`,
`<functional>`, `<memory>`, `<stdexcept>`, `<string>`, `<thread>`, `<vector>`, `<sys/types.h>`

**Keep inline in the header** (must not move to .cpp):
- Template definitions: `accessTxBlock<Fn>`, `accessRxBlock<Fn>`
- `fastForwardTx()`, `fastForwardRx()` — real-time audio path
- `sampleAt()` — inline sample accessor
- `memory_barrier_acquire()` — must be inline for correctness on all architectures
- Trivial one-line getters on `BufferView` and `BufferBlockAccessor`

**Declare in header, define in src/DanteAudio.cpp**:
- `toString(LogLevel)`, `fromString(LogLevel)`
- `PrintfLogger::log()`
- All non-trivial `BufferView` methods: `poll()`, `reset()`, timestamp helpers, channel descriptions
- All non-trivial `BufferBlockAccessor` methods: `setChannels()`, `updateAvailable()`,
  `getTxFramesToWrite()`, `getAvailRxFrames()`, head management
- All `DefaultBufferContext` methods: constructor, destructor, `connect()`, `disconnect()`,
  `wait()`, timeout handling, accessor registration

---

### Step 3 — What to Remove

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

### Step 4 — Create src/DanteAudio.cpp

```cpp
#include <dante/DanteAudio.hpp>

namespace Dante {
// all non-template implementations
}
```

Move every non-trivial, non-template function body out of the header into this file.

---

### Step 5 — Create cmake/libDanteAudio.cmake

This is the consumer-facing cmake file. It only defines the imported target — no source
compilation, no build targets:

```cmake
get_filename_component(_DANTE_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

add_library(DanteAudio STATIC IMPORTED GLOBAL)
set_target_properties(DanteAudio PROPERTIES
    IMPORTED_LOCATION "${_DANTE_SDK_ROOT}/lib/libDanteAudio.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_DANTE_SDK_ROOT}/include"
)
target_link_libraries(DanteAudio INTERFACE pthread rt)
```

Consumers include this file and link against `DanteAudio`. No SDK source is compiled on
their end.

---

### Step 6 — Build and Commit libDanteAudio.a

`libDanteAudio.a` is produced by compiling `DanteAudio.cpp` and merging it with
`libdep_audio.a`. This is done by the `dante_sdk_dist` target in the consuming project's
`CMakeLists.txt`. Add the following to the consuming project (not to this SDK's cmake):

```cmake
set(_DANTE_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../Dante-DEP-Client-SDK")
set(_DANTE_DIST_DIR "${CMAKE_BINARY_DIR}/dante-dep-sdk")
set(_DANTE_DIST_LIB "${_DANTE_DIST_DIR}/lib/libDanteAudio.a")

add_library(dante_audio_impl OBJECT EXCLUDE_FROM_ALL "${_DANTE_SDK_ROOT}/src/DanteAudio.cpp")
target_include_directories(dante_audio_impl PRIVATE "${_DANTE_SDK_ROOT}/include")

add_custom_command(
    OUTPUT "${_DANTE_DIST_LIB}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_DANTE_DIST_DIR}/lib"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${_DANTE_SDK_ROOT}/lib/libdep_audio.a"
            "${_DANTE_DIST_LIB}"
    COMMAND ${CMAKE_AR} q "${_DANTE_DIST_LIB}"
            $<TARGET_OBJECTS:dante_audio_impl>
    DEPENDS dante_audio_impl "${_DANTE_SDK_ROOT}/lib/libdep_audio.a"
    COMMENT "Building combined Dante SDK library"
)

add_custom_target(dante_sdk_dist
    DEPENDS "${_DANTE_DIST_LIB}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_DANTE_DIST_DIR}/include/dante"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${_DANTE_SDK_ROOT}/include/dante/DanteAudio.hpp"
            "${_DANTE_DIST_DIR}/include/dante/DanteAudio.hpp"
    COMMENT "Dante SDK distribution: build/dante-dep-sdk/"
)
```

`EXCLUDE_FROM_ALL` on `dante_audio_impl` ensures `DanteAudio.cpp` is not compiled during a
normal build — only when `dante_sdk_dist` is explicitly requested.

To build and commit the combined library (run from the consuming project directory):

```sh
cmake --build build --target dante_sdk_dist
cp build/dante-dep-sdk/lib/libDanteAudio.a ../Dante-DEP-Client-SDK/lib/libDanteAudio.a
```

Commit `DanteAudio.cpp` and the updated `libDanteAudio.a` together.

---

## Key Implementation Notes

- **`memory_barrier_acquire()`** emits `__sync_synchronize()` — must be at the call site.
- **`accessTxBlock` / `accessRxBlock`** take a callback and must stay in the header for
  the compiler to instantiate them at the call site.
- **`BlockAccessorConfig`** carries TX latency in microseconds (default 1000µs).
- **Late error handling**: when the accessor reports late frames, callers should use
  `fastForwardTx` / `fastForwardRx` to skip ahead rather than accumulate drift.
  See `dep_loopback/DanteLoopback.cpp` for the reference implementation of this pattern.
