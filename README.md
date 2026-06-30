# Dante DEP Client SDK

A C++ client SDK for the Audinate DEP (Dante Embedded Platform) shared-memory audio API.

## Contents

```
include/dante/DanteAudio.hpp     — public API (single header)
lib/libDanteAudio.a              — prebuilt static library
lib/libdep_audio.a               — Audinate DEP prebuilt (input to library rebuild only)
src/DanteAudio.cpp               — source used to build libDanteAudio.a
cmake/libDanteAudio.cmake        — CMake integration for consumers
daemon/dep_sync_fanoutd.cpp      — period-sync relay daemon (see below)
daemon/CMakeLists.txt            — CMake build for the daemon
daemon/dep_sync_fanoutd.service  — systemd unit
flake.nix                        — Nix flake: dev shell + daemon build targets
```

## Development environment

```sh
nix develop
```

Drops into a shell with `cmake`, `gcc`, and the aarch64 cross-compiler on `PATH`.

## Using the SDK (consumers)

Include `cmake/libDanteAudio.cmake` and link against the `DanteAudio` target:

```cmake
include(path/to/Dante-DEP-Client-SDK/cmake/libDanteAudio.cmake)

target_link_libraries(MyApp PRIVATE DanteAudio)
```

This gives `MyApp` the include path for `<dante/DanteAudio.hpp>` and links
`libDanteAudio.a` with its `pthread` and `rt` dependencies.

## Rebuilding libDanteAudio.a

The committed `lib/libDanteAudio.a` bundles `DanteAudio.cpp` with the Audinate
`libdep_audio.a`. Rebuild it after changing `src/DanteAudio.cpp`:

```sh
# Inside nix develop (or with an equivalent g++ available)
g++ -O2 -std=c++17 -c src/DanteAudio.cpp -Iinclude -o /tmp/DanteAudio.o
mkdir -p /tmp/dep_objs && (cd /tmp/dep_objs && ar x $(pwd)/../../lib/libdep_audio.a)
ar rcs lib/libDanteAudio.a /tmp/DanteAudio.o /tmp/dep_objs/*.o
```

Commit `src/DanteAudio.cpp` and the updated `lib/libDanteAudio.a` together.
For cross-compilation, see CLAUDE.md.

## Building the daemon

`dep_sync_fanoutd` is a small Linux daemon that bridges the DEP POSIX semaphore into a
futex broadcast, allowing any number of clients to block on `period_count` simultaneously.
It must be running before any process calls `DefaultBufferContext::wait()`.

```sh
# Cross-compiled for the aarch64 target (deploy this one)
nix build .#dep-sync-fanoutd-aarch64
result/bin/dep_sync_fanoutd   # copy to target

# Native x86-64 build (smoke test only)
nix build .#dep-sync-fanoutd
```

## Deploying the daemon

Copy `result/bin/dep_sync_fanoutd` to `/usr/local/bin/` on the target, then install
the systemd unit:

```sh
cp daemon/dep_sync_fanoutd.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now dep-sync-fanoutd
```

The unit is `BindsTo=dep.service` — it starts and stops with DEP.
