# DAT Player

Standalone native Windows DAT player for compatible Mirasys/Spotter-style `.dat`
files containing raw H.264 frame payloads.

DAT Player is intentionally small and portable. It has no FFmpeg, mpv, VLC, Qt,
Electron, .NET, conversion workflow, bundled decoder, or external media-player
runtime dependency. Playback uses Windows system components: Win32, COM, GDI+,
and Media Foundation.

## Current Features

- Read-only DAT indexing for `H264` and `I264` frame records
- Media Foundation H.264 decode path
- Cropped NV12-to-BGRA rendering for padded decoder output such as `1920 x 1088`
  displayed as the indexed/source size such as `1920 x 1080`
- Forward playback with stable estimated-FPS pacing
- Timeline scrub-on-release keyframe seek
- Throttled live scrub preview while dragging
- Autoplay when a valid `.dat` file is opened or dropped
- Strict single-file `.dat` drag-and-drop loading
- `Actual Size` window sizing based on the indexed/source resolution
- Portable Release package folder containing only `DatPlayer.exe` and `README.md`

DAT Player still supports only compatible `.dat` files in this phase. It does
not support `.mp4`, `.mkv`, `.avi`, `.sef`, `.sef2`, arbitrary video files,
audio, or conversion.

## Project Layout

- `include/dat_player/DatFrameIndexer.h` - public indexer API
- `src/DatFrameIndexer.cpp` - streaming marker scanner and record parser
- `src/player/main.cpp` - native Win32 app shell
- `src/playback/H264Decoder.*` - isolated Media Foundation decode/playback layer
- `src/playback/DecodeSmokeTest.cpp` - command-line decode/render/playback smoke tool
- `tests/DatFrameIndexerTests.cpp` - self-contained C++ unit tests
- `tools/parity_indexer_test.py` - Python indexer parity verifier
- `tools/package_release.ps1` - portable Release package helper
- `docs/DEVELOPMENT_SETUP.md` - clean Windows laptop setup guide

## Quick Start From A Fresh Clone

```powershell
git clone https://github.com/superzerocharts/DAT_Player.git
cd DAT_Player
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
python tools\parity_indexer_test.py
python -m py_compile tools\parity_indexer_test.py
```

If `cmake` is not on PATH, use the Visual Studio Build Tools bundled CMake:

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release --output-on-failure
```

Required tools:

- Windows 10/11
- Visual Studio 2022 or Visual Studio 2022 Build Tools with the C++ desktop workload
- CMake 3.20 or newer
- Python 3 only for `tools\parity_indexer_test.py`

No sample DAT file is required to build or run the automated tests.

## Run

```powershell
.\build\Release\DatPlayer.exe
```

Then open or drag one compatible `.dat` file. DAT Player rejects folders,
multiple files, non-DAT extensions, empty files, and incompatible DATs.

## Package

After a successful Release build:

```powershell
powershell -ExecutionPolicy Bypass -File tools\package_release.ps1 -Zip
```

Package output:

```text
dist\DatPlayer\DatPlayer.exe
dist\DatPlayer\README.md
dist\DatPlayer-portable.zip
```

The package helper does not include sample DAT files, DAT Converter files,
developer test executables, FFmpeg, mpv, VLC, or other tools. If `dumpbin.exe`
is available on PATH, the script prints the executable dependency list and flags
unexpected local DLLs.

## Optional Local Sample Smoke Tests

These checks require your own compatible DAT sample. The path below is only a
local example from the original development machine; it is not required by the
repo.

```powershell
$sample = "W:\Projects\Camera 205 Sample\Camera 205 Sample\dvrfile00000001.dat"
.\build\Release\dat_decode_smoke_test.exe $sample
.\build\Release\dat_decode_smoke_test.exe $sample --render
.\build\Release\dat_decode_smoke_test.exe $sample --playback-smoke 180
.\build\Release\dat_decode_smoke_test.exe $sample --seek-smoke 2229
```

Rendered BGRA output can be dumped to a BMP for visual inspection:

```powershell
.\build\Release\dat_decode_smoke_test.exe $sample --dump-frame-bmp 2229 frame_2229.bmp
```

## DAT Record Assumptions

- Markers are ASCII `H264` and `I264`
- `H264` records are keyframes
- `I264` records are interframes
- Timestamp is 16 bytes before marker, little-endian `uint64`
- Width is 8 bytes before marker, little-endian `uint32`
- Height is 4 bytes before marker, little-endian `uint32`
- Payload size is 4 bytes after marker, little-endian `uint32`
- Payload starts immediately after the marker and payload-size field
- Fallback timestamp timebase is `39062.5` units per second

Optional `.sef` / `.sef2` calibration is reserved for a future phase. The player
does not modify source DAT files.
