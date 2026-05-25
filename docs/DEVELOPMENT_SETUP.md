# Windows Development Setup

This guide is for continuing DAT Player development from a clean Windows laptop.

## Clone

```powershell
git clone https://github.com/superzerocharts/DAT_Player.git
cd DAT_Player
```

The repository does not contain sample DAT/video files, build outputs, or
portable release artifacts. A compatible sample DAT is useful for manual smoke
testing, but it is not needed for normal build and automated tests.

## Install Prerequisites

Install one of:

- Visual Studio 2022 with `Desktop development with C++`
- Visual Studio 2022 Build Tools with the C++ desktop workload

Recommended individual components:

- MSVC v143 C++ x64/x86 build tools
- Windows 10 or Windows 11 SDK
- CMake tools for Windows

Python 3 is optional, but recommended for the parity verifier:

```powershell
python --version
```

No FFmpeg, mpv, VLC, Qt, Electron, .NET runtime, or external decoder package is
needed.

## Build With CMake On PATH

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The user-facing app is:

```text
build\Release\DatPlayer.exe
```

Developer smoke tools are also built under `build\Release`, but they are not
included in the end-user package.

## Build With Visual Studio Bundled CMake

If `cmake` is not on PATH:

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

CTest is next to that bundled CMake:

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release --output-on-failure
```

## Test

```powershell
ctest --test-dir build -C Release --output-on-failure
python tools\parity_indexer_test.py
python -m py_compile tools\parity_indexer_test.py
```

If Python is unavailable, the native CTest suite is still the primary C++ test
path. If a C++ compiler is unavailable, the Python parity verifier can still run
the synthetic parser edge cases.

## Optional Sample Smoke Tests

Use any compatible local DAT sample from a supported recording export. This example path is
local to one development machine and is not required by the repository:

```powershell
$sample = "W:\Projects\Camera 205 Sample\Camera 205 Sample\dvrfile00000001.dat"
.\build\Release\dat_decode_smoke_test.exe $sample
.\build\Release\dat_decode_smoke_test.exe $sample --render
.\build\Release\dat_decode_smoke_test.exe $sample --playback-smoke 180
.\build\Release\dat_decode_smoke_test.exe $sample --seek-smoke 2229
```

DAT Player and the smoke tools open DAT files read-only.

## Package

```powershell
powershell -ExecutionPolicy Bypass -File tools\package_release.ps1 -Zip
```

Expected output:

```text
dist\DatPlayer\DatPlayer.exe
dist\DatPlayer\README.txt
dist\DatPlayer-portable.zip
```

The release folder is intended for end users. It should not contain sample media,
build artifacts, DAT Converter project files, test executables, or local tool
folders.

## Git Hygiene

Before committing:

```powershell
git status
git diff --check
```

The `.gitignore` excludes Visual Studio state, CMake outputs, package outputs,
native binaries, logs, temp files, and media files such as `.dat`, `.sef`,
`.sef2`, `.mp4`, `.mkv`, and `.avi`.
