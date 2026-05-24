# DAT Player

Standalone Windows-focused DAT player foundation for Mirasys/Spotter-style `.dat`
files containing raw H.264 frame payloads.

Phase one built the reusable indexer foundation. Phase two added a minimal native
Win32 player shell that can open `.dat` files, run the indexer, display basic
metadata, and simulate play/pause timeline movement. The current phase adds a
Media Foundation H.264 decode smoke test, a first-frame render smoke test,
simple forward playback, and scrub-on-release keyframe seeking in the Win32
shell. This is still an early diagnostic player rather than a polished media
player.

There is no FFmpeg, mpv, VLC, Qt, Electron, .NET, conversion workflow, or external
runtime dependency in this project.

## Project Layout

- `include/dat_player/DatFrameIndexer.h` - public indexer API
- `src/DatFrameIndexer.cpp` - streaming marker scanner and record parser
- `src/player/main.cpp` - compact Win32 shell using the indexer
- `src/playback/H264Decoder.*` - isolated Media Foundation decode smoke test
- `tests/DatFrameIndexerTests.cpp` - self-contained C++ unit tests
- `tools/parity_indexer_test.py` - Python verification harness for environments
  without a C++ compiler

## Developer Build And Test

```powershell
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release --output-on-failure
python tools\parity_indexer_test.py
python -m py_compile tools\parity_indexer_test.py
```

The C++ test executable is registered with CTest as `dat_indexer_tests`.
On Windows, the shell executable target is `dat_player`, and the user-facing
Release executable is emitted as `DatPlayer.exe`.
The command-line decode diagnostic target is `dat_decode_smoke_test`.
MSVC builds use the static C/C++ runtime setting for portable Release output, so
the app should not need loose Visual C++ runtime DLLs beside the executable.

If a local machine does not yet have CMake and a C++ compiler installed, the
synthetic parity verifier can still exercise the same parser edge cases:

```powershell
python tools\parity_indexer_test.py
```

The future player executable can link `dat_indexer` and publish as a single
portable Windows binary. The indexer does not modify source DAT files.

## Portable Release Package

After a successful Release build, create the end-user folder with:

```powershell
powershell -ExecutionPolicy Bypass -File tools\package_release.ps1
```

The package output is:

```text
dist\DatPlayer\
```

It contains:

- `DatPlayer.exe` - the user-facing app
- `README.md` - project notes and current limitations

The release folder intentionally does not include sample DAT files, DAT Converter
project files, FFmpeg, mpv, VLC, external tools, test executables, or smoke-test
executables. `DatPlayer.exe` relies only on Windows system components such as
Win32, COM, GDI+, and Media Foundation. If `dumpbin.exe` is available on PATH,
the package script prints the executable dependency list and flags unexpected
local DLLs.

## Phase-Two Shell

The current app shell:

- Opens a compact native Win32 window
- Uses DAT Converter companion styling: Segoe UI, clean utility spacing, and a
  DAT Player logo derived from the converter mark with rounded play-triangle
  shapes replacing the converter logo's square blocks
- Restricts file picking to `.dat` files and rejects other extensions
- Indexes selected files read-only through `dat_indexer`
- Displays frame count, first detected resolution, estimated duration/FPS,
  keyframe count, and interframe count
- Provides Play/Pause and a scrub control
- Provides a Decode Smoke Test diagnostic command for the currently loaded file
- Provides a Render First Frame diagnostic command that decodes one usable frame,
  converts NV12 to BGRA, and paints it in the video panel
- Provides simple forward-only playback from the current frame, paced against
  indexed timestamp deltas when usable, then estimated FPS, then 30 FPS
- Provides scrub-on-release seeking by snapping backward to the nearest `H264`
  keyframe and decoding forward to the requested frame

The decode smoke test initializes Media Foundation, checks indexed payloads for
Annex B H.264 start codes plus SPS/PPS NAL units, submits a short frame sequence
to a system H.264 decoder MFT, and reports whether decoded samples are produced.
The render smoke test crops padded decoder output such as `1920 x 1088 NV12` to
the indexed display size, such as `1920 x 1080`, before painting. These are only
development diagnostics. Audio, full random-access media-player behavior, and
polished controls are not implemented yet.

Forward GUI playback uses a simple high-resolution pacing loop. Frames are
scheduled against media time instead of being posted as fast as decoding allows.
The UI diagnostics report current frame/time, selected fallback FPS, frame
interval, decoded/rendered counts, late frames, and observed playback FPS. If
DAT timestamps are missing or implausible, playback falls back to the indexer's
estimated FPS, then to 30 FPS.

The GUI playback lifecycle is intentionally conservative: opening another file,
running decode/render diagnostics, moving the timeline, pausing, or closing the
window stops the active forward playback worker before continuing. Queued worker
messages are generation-tagged so stale decoded frames are discarded after pause,
reload, or shutdown.

The same diagnostic can be run without opening the UI:

```powershell
.\build\Release\dat_decode_smoke_test.exe "W:\Projects\Camera 205 Sample\Camera 205 Sample\dvrfile00000001.dat"
```

The first-frame render smoke path can also be checked from the console:

```powershell
.\build\Release\dat_decode_smoke_test.exe "W:\Projects\Camera 205 Sample\Camera 205 Sample\dvrfile00000001.dat" --render
```

A short forward playback callback smoke test can be run without the UI:

```powershell
.\build\Release\dat_decode_smoke_test.exe "W:\Projects\Camera 205 Sample\Camera 205 Sample\dvrfile00000001.dat" --playback-smoke 180
```

The scrub/seek decode path can be checked from the console by asking for a target
frame index. It snaps backward to the nearest keyframe internally and decodes
forward until the requested frame is rendered:

```powershell
.\build\Release\dat_decode_smoke_test.exe "W:\Projects\Camera 205 Sample\Camera 205 Sample\dvrfile00000001.dat" --seek-smoke 4458
```

Rendered BGRA output can be dumped to a BMP for visual inspection:

```powershell
.\build\Release\dat_decode_smoke_test.exe "W:\Projects\Camera 205 Sample\Camera 205 Sample\dvrfile00000001.dat" --dump-frame-bmp 2229 frame_2229.bmp
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

Optional `.sef` / `.sef2` calibration is represented by a stub hook in this phase.
