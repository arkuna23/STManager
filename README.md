# STManager

STManager is a C++11 library and CLI for managing SillyTavern data.

It provides:
- SillyTavern root detection (`data` and `public/scripts/extensions`)
- Backup/restore using streaming `tar + zstd`
- Local backup archive export/import (`.tar.zst`)
- Device sync primitives (pairing, trust store, pull sync)
- SillyTavernRaze release install/update from `version.json`
- A CLI binary (`stmanager`) for simple cross-device sync

## Project Layout

- `include/STManager/`: public library headers
- `src/`: library implementation
- `cli/`: CLI-only source files
- `tests/`: test executables

## Dependencies

Managed by `vcpkg.json`:
- `libarchive`
- `curl`
- `nlohmann-json`

## Build

`VCPKG_ROOT` must point to your vcpkg checkout.

```bash
cmake --preset linux-release-static
cmake --build --preset linux-release-static -j
```

### Native Linux Debug

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug -j
ctest --preset linux-debug
```

### Windows Build (MinGW64)

On Windows host (MSYS2 MinGW64 toolchain available in `PATH`):

```bash
cmake --preset windows-native-mingw64-release-static
cmake --build --preset windows-native-mingw64-release-static -j
ctest --preset windows-native-mingw64-release-static
```

Optional native debug build:

```bash
cmake --preset windows-native-mingw64-debug
cmake --build --preset windows-native-mingw64-debug -j
ctest --preset windows-native-mingw64-debug
```

### Linux -> Windows x64 Cross Build

Requires cross toolchain (`x86_64-w64-mingw32-gcc/g++`) on host:

```bash
cmake --preset linux-cross-windows-x64-release
cmake --build --preset linux-cross-windows-x64-release -j
```

### Linux -> Android Cross Build (Library Only)

Set `ANDROID_NDK_HOME` before configure:

```bash
cmake --preset linux-cross-android-arm64-release
cmake --build --preset linux-cross-android-arm64-release -j

cmake --preset linux-cross-android-armeabi-v7a-release
cmake --build --preset linux-cross-android-armeabi-v7a-release -j

cmake --preset linux-cross-android-x86_64-release
cmake --build --preset linux-cross-android-x86_64-release -j
```

Android presets intentionally disable CLI/tests:
- `STMANAGER_BUILD_CLI=OFF`
- `STMANAGER_BUILD_TESTS=OFF`
- `STMANAGER_BUILD_SHARED=ON` (build shared library target for Android)

### Manual Configure (without presets)

```bash
cmake -S . -B build/Release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_OVERLAY_TRIPLETS="$PWD/cmake/triplets" \
  -DVCPKG_TARGET_TRIPLET=x64-linux-static
```

Outputs include:
- `build/<preset>/libSTManager.*`
- `build/<preset>/stmanager` (when CLI is enabled)
- `build/<preset>/artifact/stmanager/` distributable bundle:
  - `include/STManager/*.h` (public headers)
  - `include/STManager/stmanager_export.h`
  - `lib/libSTManager.so` (when shared enabled)
  - `lib/libSTManagerStatic.a`

Release defaults for `stmanager`:
- static linking enabled (`STMANAGER_CLI_STATIC_LINK=ON`)
- size optimization enabled (`STMANAGER_CLI_SIZE_OPTIMIZED_RELEASE=ON`)
- in-repo triplets:
  - `cmake/triplets/x64-linux-static.cmake`
  - `cmake/triplets/x64-mingw-static.cmake`

Android presets use vcpkg Android triplets with the NDK chainload toolchain:
`arm64-android`, `arm-neon-android`, and `x64-android`.

Optional overrides:

```bash
cmake -S . -B build/Release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DSTMANAGER_CLI_STATIC_LINK=OFF \
  -DSTMANAGER_CLI_SIZE_OPTIMIZED_RELEASE=OFF
```

## Run Tests

```bash
ctest --preset linux-release-static
```

## CLI Usage

Run with no arguments to open an interactive action menu:

```bash
./build/linux-release-static/stmanager
```

### Start source device

Run this on the device you want to pull data from:

```bash
./build/linux-release-static/stmanager serve backup --root /path/to/SillyTavern
```

Defaults:
- `--root` auto-detects from current/parent directories
- `--port` defaults to `38591`
- `--bind` defaults to `0.0.0.0`

### Pair and pull from destination device

```bash
./build/linux-release-static/stmanager pair restore \
  --root /path/to/local/SillyTavern
```

Optional flags:
- `--device-id <id>`: skip selection prompt and target one device directly
- `--host <ip>` and `--port <port>`: direct connection bypassing discovery
- `--pairing-code <code>`
- `--dest-root <path>` to restore into another root

If `--device-id` is not provided, `pair restore` auto-discovers devices in the local network and shows a list for interactive selection.
The discovered endpoint host is always a connectable LAN address (not `0.0.0.0`).

### Export local backup archive

```bash
./build/linux-release-static/stmanager export backup \
  --root /path/to/SillyTavern \
  --file /path/to/st-backup.tar.zst
```

Optional flags:
- `--file <path>` defaults to `st-backup.tar.zst`
- `--git-mode` enables git extension manifest behavior

### Restore from local backup archive

```bash
./build/linux-release-static/stmanager restore backup \
  --root /path/to/SillyTavern \
  --file /path/to/st-backup.tar.zst
```

Optional flags:
- `--file <path>` defaults to `st-backup.tar.zst`

### Install or update SillyTavernRaze

```bash
./build/linux-release-static/stmanager update \
  --root /path/to/SillyTavern
```

Optional flags:
- `--root <path>` defaults to auto-detect, then `./SillyTavern` for initialization
- `--repo <owner/repo>` defaults to `arkuna23/SillyTavernRaze`
- `--cache-dir <path>` defaults to `<root>/.stmanager/raze-cache`

The updater reads the latest non-draft GitHub release, downloads `version.json`,
compares package hashes, downloads only changed packages, verifies SHA-256, extracts
them into the root, and then writes the new local `version.json`.

## Library Quick Start

```cpp
#include <STManager/sync.h>
#include <STManager/manager.h>

STManager::Manager manager;
STManager::Status create_status =
    STManager::Manager::create_from_root("/path/to/SillyTavern", &manager);
if (create_status.ok()) {
    STManager::ServeSyncOptions options;
    options.server_options.port = 38591;
    std::unique_ptr<STManager::SyncTaskHandle> handle = manager.serve_sync(options);
    STManager::SyncTaskInfo info = handle->info();  // device_id/local endpoint
    // stop from another thread/signal if needed
    STManager::Status run_status = handle->wait();
}
```

## Notes

- CMake presets are provided in `CMakePresets.json`.
- Device discovery/advertise is implemented inside the library via UDP LAN discovery and does not require Avahi daemon.
- `serve backup --bind` controls listening interface only; discovery returns peer-reachable source IP for connection.
- Trust/device state is managed by the library under `<root>/.stmanager/`.
