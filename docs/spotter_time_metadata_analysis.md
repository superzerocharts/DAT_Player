# Spotter Time Metadata Analysis

Read-only investigation performed against:

`C:\Users\schmijon\Projects\Camera 205 Sample\Camera 205 Sample`

No original sample media files were modified.

## Summary

The recording wall-clock time is extractable.

For this sample, the absolute recording start/end times are stored in three places:

1. `Camera 205 Sample.sef2` as plain XML:
   - `<start>2026-05-20T04:25:16.5410000</start>`
   - `<end>2026-05-20T04:30:14.7380000</end>`
2. `MaterialFolderIndex.dat` as little-endian .NET `DateTime` ticks:
   - offset `36`: `639148479165410000` -> `2026-05-20T04:25:16.541000`
   - offset `44`: `639148482147380000` -> `2026-05-20T04:30:14.738000`
3. `dvrfile00000001.dat` as little-endian .NET `DateTime` ticks:
   - archive/index copies at offsets `4096` and `4512`
   - per-frame timestamps immediately before each `H264`/`I264` marker

Important DAT layout correction for this sample:

- The true frame timestamp is at `marker - 17`, not `marker - 16`.
- `marker - 16` reads a one-byte-shifted .NET tick value. The old `39062.5` timebase works because `10,000,000 / 256 = 39062.5`.

Spotter displays the same instant converted through the archive timezone/daylight information:

- Raw `.sef2`/`.dat` start: `2026-05-20T04:25:16.541`
- Spotter displayed start/current overlay near clip start: `9:25:17 PM  5/19/2026`
- This is a UTC-07:00 conversion, matching Pacific daylight time from the serialized timezone blob.

Confidence: high for this sample. Medium-high for broader support until more `.sef2`/`.dat` exports are checked.

## File Inventory

| File | Size | LastWriteTime | SHA-256 |
| --- | ---: | --- | --- |
| `Camera 205 Sample.sef2` | 3,786 | 2026-05-20 11:39:02 | `3D6F9F19E7D96914CA9CE650B9AF328E1B39337EC96555D429F503C12AE6F5DF` |
| `dvrfile00000001.dat` | 194,760,704 | 2026-05-20 11:39:02 | `FF8A8DA4FC31990A2AEAAAE068309569E5DF740CAE2A21F11C7BFFCB9F62F8BC` |
| `MaterialFolderIndex.dat` | 81 | 2026-05-20 11:39:02 | `58EA69BC4727FA35EE17EC688BD7D7D5A10ABA1EFDDFE7E6A3F6A2CE22B50412` |
| `SpotterPlayer.exe` | 490,310,168 | 2025-09-10 07:39:54 | `4A6E62A4F9D6BEB0FCC9B69EE1F80EDE703DF44BDDF9674D8BCFEA433CF48638` |

Likely pairing:

- `Camera 205 Sample.sef2` references `dvrfile00000001.dat` in its XML `<files>` section.
- `MaterialFolderIndex.dat` repeats the same start/end tick values and appears to be a small export index file.

## Spotter Displayed Values

Spotter GUI observation succeeded after launching the child `Spotter.exe` process:

- Window title: `Spotter Player - 9.9.5.1`
- Camera/device tree: `205 Choke Point`
- Video overlay text near clip start: `9:25:17 PM  5/19/2026`
- Date/time picker value near clip start: `5/19/2026 9:25:17 PM`
- Screenshot captured: `docs/spotter_player_start_display.png`

The player was running during observation, so the screenshot itself shows a later playback instant (`9:26:16 PM  5/19/2026`). The first accessible UI values above are the closest observed values near the beginning of playback.

Values expected to match Spotter's displayed archive metadata:

- Raw start in files: `2026-05-20 04:25:16.541`
- Raw end in files: `2026-05-20 04:30:14.738`
- Spotter-displayed start, rounded: `5/19/2026 9:25:17 PM`
- Spotter-displayed end, if converted with the same UTC-07:00 offset: `9:30:14.738 PM  5/19/2026` or rounded to `9:30:15 PM`
- Duration: `298.197` seconds, about `4:58.197`
- Camera: `205 Choke Point`
- Channel ID: `16`
- Manufacturer/model: `AXIS` / `AXIS Q1645 Network Camera`
- Resolution from DAT frames: `1920x1080`
- Inferred frame rate over the clip: `29.896344` fps

Manual confirmation still useful: whether Spotter exposes the true archive end time in another panel. Moving the accessible timeline slider to its maximum displayed `9:29:13 PM`, which appears to be the end of the current visible timeline range rather than the actual archive end.

## SEF2 Findings

`Camera 205 Sample.sef2` is plain XML, not ZIP/GZIP/ZLIB/SQLite/encrypted.

Relevant XML offsets:

- Start string byte offset: `120`
- End string byte offset: `164`

Channel metadata:

- `channelId="16"`
- `dataType="Video"`
- `channelType="Material"`
- `dataSize="193307677"`
- `name="MjA1IENob2tlIFBvaW50"` decodes as UTF-8/base64: `205 Choke Point`
- `manufacturer="AXIS"`
- `model="AXIS Q1645 Network Camera"`
- `timezone` is a base64 .NET `System.CurrentSystemTimeZone` serialized blob.

Timezone blob observations:

- Contains strings such as `System.CurrentSystemTimeZone`, `m_ticksOffset`, `m_standardName`, `m_daylightName`, and `System.Globalization.DaylightTime`.
- Signed little-endian tick offset candidate at decoded blob offset `162`: `-288000000000` ticks, equivalent to UTC-08:00 standard offset.
- Daylight delta candidate at decoded blob offset `492`: `36000000000` ticks, equivalent to +1 hour.
- The XML start/end values themselves have no explicit `Z` or numeric UTC offset. Spotter appears to treat them as UTC-like ticks and converts them through the serialized timezone data for display. In this sample, `2026-05-20T04:25:16.541` displays as `5/19/2026 9:25:17 PM`, a UTC-07:00 conversion.

## Timestamp Encoding Tests

For the observed start/end datetimes, exact binary matches were found only as little-endian .NET `DateTime` ticks in binary files.

Checked encodings included:

- Unix epoch seconds
- Unix epoch milliseconds
- Unix epoch microseconds
- Windows FILETIME
- .NET `DateTime` ticks
- OLE Automation `DATE` double
- Little-endian and big-endian variants
- ASCII ISO strings in `.sef2`
- UTF-16-style string extraction

Matches:

- `.sef2`: ASCII ISO-8601-like strings only.
- `MaterialFolderIndex.dat`: little-endian .NET ticks at offsets `36` and `44`.
- `dvrfile00000001.dat`: little-endian .NET ticks at offsets listed below and per frame.

No meaningful XML/JSON/INI/SQLite/archive signature was found inside the video `.dat`; printable strings in sampled DAT ranges were dominated by `H264`/`I264` markers and encoded video bytes.

## DAT Frame Timestamp Analysis

Frame scan results from `dvrfile00000001.dat`:

- Valid frames with plausible .NET timestamps: `8916`
- Marker counts:
  - `H264`: `298`
  - `I264`: `8618`
- Dimensions: `1920x1080` for all detected frames
- Unknown byte at `marker - 9`: always `1`
- First frame:
  - marker offset `1069083`
  - marker `H264`
  - timestamp offset `1069066`
  - ticks `639148479165410000`
  - datetime `2026-05-20T04:25:16.541000`
- Last frame:
  - marker offset `194740788`
  - marker `I264`
  - timestamp offset `194740771`
  - ticks `639148482147380000`
  - datetime `2026-05-20T04:30:14.738000`
- Span from true .NET ticks: `298.197000` seconds
- FPS by frame count/span: `29.896344`
- Median positive frame delta: `0.033000` seconds
- Mode positive frame delta: `0.033000` seconds
- There were two negative deltas in file order, around `-1.867` and `-1.967` seconds. The absolute timestamps still remain valid per frame; playback/index ordering should be validated with more samples before assuming strict monotonic file order.

Observed DAT frame record layout for this sample:

| Relative to marker | Size | Meaning |
| ---: | ---: | --- |
| `-17` | 8 | little-endian .NET `DateTime` ticks |
| `-9` | 1 | unknown, value `1` in this sample |
| `-8` | 4 | little-endian width |
| `-4` | 4 | little-endian height |
| `0` | 4 | ASCII `H264` or `I264` |
| `+4` | 4 | little-endian payload size |
| `+8` | variable | H.264 payload |

The existing `marker - 16` interpretation yields shifted values. Its span divided by `39062.5` is `298.197018` seconds, nearly identical because it is derived from the true .NET tick stream shifted right by one byte.

## DAT Metadata Offsets

Exact little-endian start tick `639148479165410000` occurs in `dvrfile00000001.dat` at:

- `4096`
- `1052691`
- `1069066` (first frame timestamp)

Exact little-endian end tick `639148482147380000` occurs in `dvrfile00000001.dat` at:

- `4512`
- `187460531`
- `194740771` (last frame timestamp)

The `4096`/`4512` occurrences appear to be index/header entries. The per-frame occurrences are enough to recover wall-clock time without `.sef2` for this sample.

## Conclusions

1. The absolute recording date/time is stored in `.sef2`, `MaterialFolderIndex.dat`, and the video `.dat`.
2. The `.dat` frame timestamp is absolute .NET `DateTime` ticks, not merely relative, when read at `marker - 17`.
3. DAT Player/DAT Converter can map frames directly to raw recording instants:
   - `frame_time = dotnet_ticks_to_datetime(u64_le_at_marker_minus_17)`
   - `elapsed = (frame_ticks - first_frame_ticks) / 10_000_000`
4. To match Spotter's displayed wall-clock time, convert raw ticks using the `.sef2` timezone blob. For this sample, the effective display offset is UTC-07:00.
5. `.sef2` contains recording start, end, camera/channel metadata, model/manufacturer, and serialized timezone data. It does not appear to contain per-frame index data in this sample.
6. Extraction is reliable for this sample without running Spotter Player.
7. More samples are needed to confirm:
   - whether `marker - 17` is universal
   - whether timestamps are always local/unspecified .NET ticks
   - how timezone should be displayed or converted
   - how to handle non-monotonic frame timestamps in file order

## Scripts Created

- `tools/analyze_sef2_metadata.py`
  - Parses `.sef2` XML.
  - Decodes camera name.
  - Reports timezone blob strings and offset candidates.
  - Reports sibling `MaterialFolderIndex.dat` .NET tick hits.
  - Searches sibling video `.dat` files for exact start/end tick matches.

- `tools/analyze_dat_timestamps.py`
  - Scans `H264`/`I264` records.
  - Reads timestamps at `marker - 17`.
  - Reports first/last frame datetimes, duration, FPS, delta distribution, dimensions, payload sizes, and shifted `marker - 16` compatibility span.

Example commands:

```powershell
python tools\analyze_sef2_metadata.py "C:\Users\schmijon\Projects\Camera 205 Sample\Camera 205 Sample\Camera 205 Sample.sef2"
python tools\analyze_dat_timestamps.py "C:\Users\schmijon\Projects\Camera 205 Sample\Camera 205 Sample\dvrfile00000001.dat"
```

## Recommended Later Integration

Do not implement yet in this phase.

DAT Player:

- Prefer true DAT frame ticks at `marker - 17` when they decode as plausible .NET datetimes.
- Show recording start/end from the first/last frame or `.sef2`.
- Use `.sef2` timezone data when present to match Spotter display.
- Show current wall-clock frame time while scrubbing/playback.
- Keep elapsed time as `frame_ticks - first_frame_ticks`.

DAT Converter:

- Add optional `.sef2` metadata parser.
- Optionally set MP4/MKV metadata such as `creation_time`, with careful timezone handling.
- Optionally suggest output names based on recording start time and camera name.

Shared parser:

- Implement a small read-only metadata parser that can accept `.sef2`, `MaterialFolderIndex.dat`, and `.dat`.
- Use confidence levels:
  - high: `.sef2` start/end match first/last DAT frame ticks
  - medium: only `.sef2` exists
  - medium: only DAT frame ticks exist
  - low: only shifted `marker - 16` timing exists

Suggested next implementation task:

```text
Implement metadata extraction in DAT Player/DAT Converter using the research in docs/spotter_time_metadata_analysis.md. Add a shared read-only parser that reads .sef2 XML start/end/camera metadata and DAT frame .NET ticks at marker-17, preserves existing playback behavior, and adds tests using copies or synthetic fixtures only.
```
