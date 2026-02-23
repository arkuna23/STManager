# STManager

STManager is a C++11 library and CLI for managing SillyTavern data.

It provides:
- SillyTavern root detection (`data` and `public/scripts/extensions`)
- Backup/restore using streaming `tar + zstd`
- Device sync primitives (pairing, trust store, pull sync)
- A CLI binary (`stmanager`) for simple cross-device sync

## Project Layout

- `include/STManager/`: public library headers
- `src/`: library implementation
- `cli/`: CLI-only source files
- `tests/`: test executables

## Dependencies

Managed by `vcpkg.json`:
- `libarchive`
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

### Windows Build (MinGW)

On Windows host (MSYS2/MinGW available in `PATH`):

```bash
cmake --preset windows-native-mingw-release-static
cmake --build --preset windows-native-mingw-release-static -j
ctest --preset windows-native-mingw-release-static
```

### Linux -> Windows x64 Cross Build

Requires cross toolchain (`x86_64-w64-mingw32-gcc/g++`) on host:

```bash
cmake --preset linux-cross-windows-x64-release
cmake --build --preset linux-cross-windows-x64-release -j
```

### Linux -> Android arm64 Cross Build (Library Only)

Set `ANDROID_NDK_HOME` before configure:

```bash
cmake --preset linux-cross-android-arm64-release
cmake --build --preset linux-cross-android-arm64-release -j
```

Android preset intentionally disables CLI/tests:
- `STMANAGER_BUILD_CLI=OFF`
- `STMANAGER_BUILD_TESTS=OFF`
- `STMANAGER_BUILD_SHARED=OFF` (build static library target for Android)

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

Release defaults for `stmanager`:
- static linking enabled (`STMANAGER_CLI_STATIC_LINK=ON`)
- size optimization enabled (`STMANAGER_CLI_SIZE_OPTIMIZED_RELEASE=ON`)
- in-repo triplets:
  - `cmake/triplets/x64-linux-static.cmake`
  - `cmake/triplets/x64-mingw-static.cmake`

Android preset uses vcpkg built-in triplet `arm64-android` with NDK chainload toolchain.

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

### Start source device

Run this on the device you want to pull data from:

```bash
./build/linux-release-static/stmanager run --root /path/to/SillyTavern
```

Defaults:
- `--root` auto-detects from current/parent directories
- `--port` defaults to `38591`
- `--bind` defaults to `0.0.0.0`

### Pair and pull from destination device

```bash
./build/linux-release-static/stmanager pair \
  --root /path/to/local/SillyTavern
```

Optional flags:
- `--device-id <id>`: skip selection prompt and target one device directly
- `--host <ip>` and `--port <port>`: direct connection bypassing discovery
- `--pairing-code <code>`
- `--dest-root <path>` to restore into another root
- `--git-mode` for extension backup behavior

If `--device-id` is not provided, `pair` auto-discovers devices in the local network and shows a list for interactive selection.
The discovered endpoint host is always a connectable LAN address (not `0.0.0.0`).

## Library Quick Start

```cpp
#include <STManager/data.h>

STManager::DataManager manager = STManager::DataManager::locate("/path/to/SillyTavern");
if (manager.is_valid()) {
    // backup/restore/sync operations
}
```

## Notes

- CMake presets are provided in `CMakePresets.json`.
- Device discovery/advertise is implemented inside the library via UDP LAN discovery and does not require Avahi daemon.
- `run --bind` controls listening interface only; discovery returns peer-reachable source IP for connection.
- Trust state is stored under `<root>/.stmanager/` by the CLI.
