#!/usr/bin/env python3
"""Read-only DAT frame timestamp analyzer for compatible H264/I264 records."""

from __future__ import annotations

import argparse
import datetime as dt
import mmap
import statistics
import struct
from collections import Counter
from pathlib import Path


DOTNET_EPOCH = dt.datetime(1, 1, 1)
TIMEBASE_FROM_SHIFTED_DOTNET_TICKS = 10_000_000 / 256


def dotnet_ticks_to_datetime(ticks: int) -> dt.datetime | None:
    try:
        seconds, remainder = divmod(ticks, 10_000_000)
        return DOTNET_EPOCH + dt.timedelta(seconds=seconds, microseconds=remainder // 10)
    except (OverflowError, ValueError):
        return None


def is_plausible_dotnet_time(ticks: int) -> bool:
    parsed = dotnet_ticks_to_datetime(ticks)
    return bool(parsed and dt.datetime(2000, 1, 1) <= parsed <= dt.datetime(2100, 1, 1))


def scan_frames(path: Path) -> list[dict[str, object]]:
    frames: list[dict[str, object]] = []
    with path.open("rb") as handle:
        mm = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)
        positions: list[tuple[int, bytes]] = []
        for marker in (b"H264", b"I264"):
            pos = mm.find(marker)
            while pos != -1:
                positions.append((pos, marker))
                pos = mm.find(marker, pos + 1)
        for pos, marker in sorted(positions):
            if pos < 17 or pos + 8 > len(mm):
                continue
            timestamp = struct.unpack_from("<Q", mm, pos - 17)[0]
            width = struct.unpack_from("<I", mm, pos - 8)[0]
            height = struct.unpack_from("<I", mm, pos - 4)[0]
            payload_size = struct.unpack_from("<I", mm, pos + 4)[0]
            if not is_plausible_dotnet_time(timestamp):
                continue
            if not (0 < width <= 8192 and 0 < height <= 8192):
                continue
            if not (0 < payload_size < 50_000_000 and pos + 8 + payload_size <= len(mm)):
                continue
            frames.append(
                {
                    "offset": pos,
                    "marker": marker.decode("ascii"),
                    "timestamp": timestamp,
                    "datetime": dotnet_ticks_to_datetime(timestamp),
                    "unknown_byte": mm[pos - 9],
                    "width": width,
                    "height": height,
                    "payload_size": payload_size,
                    "legacy_shifted_timestamp": struct.unpack_from("<Q", mm, pos - 16)[0],
                }
            )
    return frames


def fmt_datetime(value: dt.datetime | None) -> str:
    if value is None:
        return "<invalid>"
    return value.isoformat(timespec="microseconds")


def print_frame(prefix: str, frame: dict[str, object]) -> None:
    print(
        f"{prefix} offset={frame['offset']} marker={frame['marker']} "
        f"time={fmt_datetime(frame['datetime'])} ticks={frame['timestamp']} "
        f"unknown_byte={frame['unknown_byte']} {frame['width']}x{frame['height']} payload={frame['payload_size']}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dat", type=Path, help="Path to DAT video payload file")
    parser.add_argument("--sample", type=int, default=5, help="Number of first/last frames to print")
    args = parser.parse_args()

    frames = scan_frames(args.dat)
    print(f"File: {args.dat}")
    print(f"Frames with plausible .NET timestamps at marker-17: {len(frames)}")
    if not frames:
        return 1

    counts = Counter(frame["marker"] for frame in frames)
    print(f"Marker counts: {dict(counts)}")
    print(f"Dimensions: {Counter((frame['width'], frame['height']) for frame in frames)}")
    print(f"Unknown byte values: {dict(Counter(frame['unknown_byte'] for frame in frames))}")

    for frame in frames[: args.sample]:
        print_frame("first", frame)
    for frame in frames[-args.sample :]:
        print_frame("last ", frame)

    ticks = [int(frame["timestamp"]) for frame in frames]
    shifted = [int(frame["legacy_shifted_timestamp"]) for frame in frames]
    deltas = [b - a for a, b in zip(ticks, ticks[1:])]
    positive_deltas = [delta for delta in deltas if delta > 0]
    span_seconds = (ticks[-1] - ticks[0]) / 10_000_000
    shifted_span_seconds = (shifted[-1] - shifted[0]) / TIMEBASE_FROM_SHIFTED_DOTNET_TICKS

    print("\nTiming:")
    print(f"  first: {fmt_datetime(frames[0]['datetime'])} ({ticks[0]})")
    print(f"  last:  {fmt_datetime(frames[-1]['datetime'])} ({ticks[-1]})")
    print(f"  span_seconds_from_dotnet_ticks: {span_seconds:.6f}")
    print(f"  fps_by_count_span: {(len(frames) - 1) / span_seconds:.6f}")
    print(f"  shifted_marker_minus_16_span_at_39062.5: {shifted_span_seconds:.6f}")
    print(f"  positive_delta_median_seconds: {statistics.median(positive_deltas) / 10_000_000:.6f}")
    print(f"  positive_delta_mode_seconds: {Counter(positive_deltas).most_common(1)[0][0] / 10_000_000:.6f}")
    print(f"  top_delta_seconds: {[(count, delta / 10_000_000) for delta, count in Counter(deltas).most_common(10)]}")

    negative = [(idx, delta) for idx, delta in enumerate(deltas) if delta < 0]
    print(f"  negative_delta_count: {len(negative)}")
    for idx, delta in negative[:10]:
        print(
            "    "
            f"index={idx} delta_seconds={delta / 10_000_000:.6f} "
            f"{frames[idx]['offset']}->{frames[idx + 1]['offset']} "
            f"{fmt_datetime(frames[idx]['datetime'])}->{fmt_datetime(frames[idx + 1]['datetime'])}"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
