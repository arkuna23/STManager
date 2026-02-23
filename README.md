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

### Configure + Build (Release)

```bash
cmake -S . -B build/Release \
  -DCMAKE_BUILD_TYPE=Release \
  -DVCPKG_OVERLAY_TRIPLETS="$PWD/cmake/triplets" \
  -DVCPKG_TARGET_TRIPLET=x64-linux-static \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

cmake --build build/Release -j
```

Outputs include:
- `build/Release/libSTManager.so`
- `build/Release/stmanager`

Release defaults for `stmanager`:
- static linking enabled (`STMANAGER_CLI_STATIC_LINK=ON`)
- size optimization enabled (`STMANAGER_CLI_SIZE_OPTIMIZED_RELEASE=ON`)
- static Linux triplet is provided in-repo at `cmake/triplets/x64-linux-static.cmake`

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
ctest --test-dir build/Release --output-on-failure
```

## CLI Usage

### Start source device

Run this on the device you want to pull data from:

```bash
./build/Release/stmanager run --root /path/to/SillyTavern
```

Defaults:
- `--root` auto-detects from current/parent directories
- `--port` defaults to `0` (auto-selected)
- `--bind` defaults to `0.0.0.0`

### Pair and pull from destination device

```bash
./build/Release/stmanager pair \
  --root /path/to/local/SillyTavern \
  --device-id <source_device_id> \
  --host <source_ip> \
  --port <source_port>
```

Optional flags:
- `--pairing-code <code>`
- `--dest-root <path>` to restore into another root
- `--git-mode` for extension backup behavior

## Library Quick Start

```cpp
#include <STManager/data.h>

STManager::DataManager manager = STManager::DataManager::locate("/path/to/SillyTavern");
if (manager.is_valid()) {
    // backup/restore/sync operations
}
```

## Notes

- Current sync runtime is Linux-oriented (sockets and signal handling).
- Device discovery/advertise is implemented inside the library via UDP LAN discovery and does not require Avahi daemon.
- Trust state is stored under `<root>/.stmanager/` by the CLI.
