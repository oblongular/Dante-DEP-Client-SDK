# Dante DEP Client SDK

A C++ client SDK for the Audinate DEP (Dante Embedded Platform) shared-memory audio API.

## Contents

```
include/dante/DanteAudio.hpp     — public API (single header)
lib/libDanteAudio.a              — prebuilt static library
cmake/libDanteAudio.cmake        — CMake integration for consumers
```

This repo ships a prebuilt library only. Source, the relay daemon, and diagnostic
tools are developed elsewhere; this is the published artifact consumers build against.

## Using the SDK (consumers)

Include `cmake/libDanteAudio.cmake` and link against the `DanteAudio` target:

```cmake
include(path/to/Dante-DEP-Client-SDK/cmake/libDanteAudio.cmake)

target_link_libraries(MyApp PRIVATE DanteAudio)
```

This gives `MyApp` the include path for `<dante/DanteAudio.hpp>` and links
`libDanteAudio.a` with its `pthread` and `rt` dependencies.

**Important**: a process using `DefaultBufferContext::wait()` requires `dep_sync_fanoutd`
to be running on the target — it bridges the DEP POSIX semaphore into a futex broadcast,
allowing any number of clients to block on `period_count` simultaneously.
