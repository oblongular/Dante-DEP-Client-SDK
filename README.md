# Dante DEP Client SDK

A C++ client SDK for the Audinate DEP (Dante Embedded Platform) shared-memory audio API.

## Contents

```
include/dante/DanteAudio.hpp   — public API (single header)
lib/libDanteAudio.a            — prebuilt static library
lib/libdep_audio.a             — original Audinate DEP prebuilt (input to rebuild only)
src/DanteAudio.cpp             — source used to build libDanteAudio.a
cmake/libDanteAudio.cmake      — CMake integration
```

## Using the SDK

Include `cmake/libDanteAudio.cmake` and link against the `DanteAudio` target:

```cmake
include(path/to/Dante-DEP-Client-SDK/cmake/libDanteAudio.cmake)

target_link_libraries(MyApp PRIVATE DanteAudio)
```

This gives `MyApp` the include path for `<dante/DanteAudio.hpp>` and links `libDanteAudio.a`
with its `pthread` and `rt` dependencies.

## CMake Targets

### `DanteAudio` (defined in `cmake/libDanteAudio.cmake`)

The consumer-facing imported target. Links against the committed prebuilt `libDanteAudio.a`.
No SDK source is compiled as part of a consumer's build.

### `dante_sdk_dist` (defined in the consuming project's `CMakeLists.txt`)

An explicit-only target (not built by default) that documents and reproduces how
`libDanteAudio.a` was built. It compiles `DanteAudio.cpp`, merges the result with the
original Audinate `libdep_audio.a`, and writes the combined library and header to
`build/dante-dep-sdk/`:

```sh
cmake --build build --target dante_sdk_dist
```

Output:
```
build/dante-dep-sdk/
  include/dante/DanteAudio.hpp
  lib/libDanteAudio.a
```

To update the committed `libDanteAudio.a` after changing `DanteAudio.cpp`:

```sh
cmake --build build --target dante_sdk_dist
cp build/dante-dep-sdk/lib/libDanteAudio.a lib/libDanteAudio.a
```

Commit `DanteAudio.cpp` and the updated `libDanteAudio.a` together.
