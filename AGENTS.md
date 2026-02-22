# Repository Guidelines

## Project Structure & Module Organization
This repository builds a shared C++ library named `STManager`.

- `include/STManager/`: Public headers (API surface), e.g. `data.h`.
- `src/`: Library implementation files, e.g. `data.cpp`.
- `CMakeLists.txt`: Single build entrypoint and target definition.
- `build/`: Local build artifacts (generated; do not commit).
- `compile_commands.json`: Generated from CMake for editor tooling.

Keep public interfaces in `include/STManager/` and implementation details in `src/`.

## Build, Test, and Development Commands
Use out-of-source CMake builds:

```bash
cmake -S . -B build/Debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Debug -j
```

- First command configures the project and generates build files.
- Second command compiles the shared library target `STManager`.

Optional release build:

```bash
cmake -S . -B build/Release -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release -j
```

## Coding Style & Naming Conventions
- Language standard: C++11 (`cxx_std_11` in `CMakeLists.txt`).
- Formatting: `.clang-format` (Google-based, 4 spaces, 100 columns, no tabs).
- Header names use lowercase (`data.h`); include path prefix is `STManager/...`.
- Types/classes use `PascalCase` (e.g., `DataManager`).
- Functions and methods use `camelCase` (e.g., `locate`).

Run formatting before opening a PR:

```bash
clang-format -i src/*.cpp include/STManager/*.h
```

## Testing Guidelines
No test framework is configured yet. When adding tests:
- Prefer CTest integration via CMake (`enable_testing()` + `add_test(...)`).
- Place tests under `tests/` with names like `*_test.cpp`.
- Keep tests deterministic and focused on public API behavior.

## Commit & Pull Request Guidelines
Current history favors short, imperative commit messages, sometimes with emojis (e.g., `🎉 Init project`, `Rename project`).

- Commit format: concise imperative summary; optional emoji prefix.
- PRs should include: purpose, key changes, build steps used, and follow-up work.
- Link related issues when available and mention any API or ABI-impacting changes.
