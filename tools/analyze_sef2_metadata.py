#!/usr/bin/env python3
"""Read-only .sef2 data inspector for compatible recording exports."""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import mmap
import re
import struct
import xml.etree.ElementTree as ET
from pathlib import Path


DOTNET_EPOCH = dt.datetime(1, 1, 1)


def parse_dotnet_iso(value: str) -> tuple[dt.datetime, int]:
    date_part, _, frac = value.partition(".")
    parsed = dt.datetime.fromisoformat(date_part)
    frac_digits = "".join(ch for ch in frac if ch.isdigit())
    ticks_fraction = int((frac_digits + "0000000")[:7]) if frac_digits else 0
    delta = parsed - DOTNET_EPOCH
    whole_seconds = delta.days * 86400 + delta.seconds
    ticks = whole_seconds * 10_000_000 + ticks_fraction
    return parsed + dt.timedelta(microseconds=ticks_fraction // 10), ticks


def dotnet_ticks_to_datetime(ticks: int) -> dt.datetime | None:
    try:
        seconds, remainder = divmod(ticks, 10_000_000)
        return DOTNET_EPOCH + dt.timedelta(seconds=seconds, microseconds=remainder // 10)
    except (OverflowError, ValueError):
        return None


def find_all(data: bytes, needle: bytes) -> list[int]:
    offsets: list[int] = []
    pos = data.find(needle)
    while pos != -1:
        offsets.append(pos)
        pos = data.find(needle, pos + 1)
    return offsets


def plausible_dotnet_ticks(data: bytes, start_year: int = 2020, end_year: int = 2035) -> list[tuple[int, int, dt.datetime]]:
    start = dt.datetime(start_year, 1, 1)
    end = dt.datetime(end_year, 1, 1)
    hits: list[tuple[int, int, dt.datetime]] = []
    for offset in range(0, max(0, len(data) - 7)):
        ticks = struct.unpack_from("<Q", data, offset)[0]
        parsed = dotnet_ticks_to_datetime(ticks)
        if parsed and start <= parsed <= end:
            hits.append((offset, ticks, parsed))
    return hits


def print_related_file_matches(path: Path, label: str, ticks: list[tuple[str, int]]) -> None:
    if not path.exists() or not path.is_file():
        return
    with path.open("rb") as handle:
        mm = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)
        print(f"\nExact tick matches in {label}: {path.name}")
        for name, value in ticks:
            offsets = find_all(mm, struct.pack("<Q", value))
            print(f"  {name}: {offsets or 'not found'}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sef2", type=Path, help="Path to a .sef2 file")
    args = parser.parse_args()

    sef2 = args.sef2
    data = sef2.read_bytes()
    root = ET.fromstring(data)

    print(f"File: {sef2}")
    print(f"Size: {len(data)} bytes")
    print(f"Root: {root.tag}")

    tick_values: list[tuple[str, int]] = []
    for tag in ("start", "end"):
        value = root.findtext(tag)
        if not value:
            continue
        parsed, ticks = parse_dotnet_iso(value)
        tick_values.append((tag, ticks))
        print(f"\n{tag}: {value}")
        print(f"  parsed local/unspecified: {parsed.isoformat(timespec='microseconds')}")
        print(f"  .NET ticks: {ticks}")
        print(f"  XML byte offsets: {find_all(data, value.encode('ascii'))}")

    print("\nChannels:")
    for channel in root.findall("./channels/channel"):
        attrs = channel.attrib
        print(f"  channelId={attrs.get('channelId')} dataType={attrs.get('dataType')} channelType={attrs.get('channelType')}")
        for key in ("manufacturer", "model", "dataSize", "is360"):
            if key in attrs:
                print(f"    {key}: {attrs[key]}")
        if "name" in attrs:
            try:
                decoded = base64.b64decode(attrs["name"]).decode("utf-8")
            except Exception as exc:  # pragma: no cover - research helper
                decoded = f"<decode failed: {exc}>"
            print(f"    name: {decoded} (base64 {attrs['name']})")
        if "timezone" in attrs:
            blob = base64.b64decode(attrs["timezone"])
            print(f"    timezone blob: {len(blob)} bytes")
            strings = re.findall(rb"[\x20-\x7e]{4,}", blob)
            if strings:
                print("    timezone strings:")
                for item in strings:
                    print(f"      {item.decode('latin1')}")
            offsets = []
            for offset in range(0, len(blob) - 7):
                signed = struct.unpack_from("<q", blob, offset)[0]
                if -14 * 3600 * 10_000_000 <= signed <= 14 * 3600 * 10_000_000 and signed % 10_000_000 == 0:
                    offsets.append((offset, signed))
            print(f"    timezone tick-offset candidates: {offsets}")

    sibling_index = sef2.with_name("MaterialFolderIndex.dat")
    if sibling_index.exists():
        index_data = sibling_index.read_bytes()
        print(f"\nSibling index: {sibling_index.name} ({len(index_data)} bytes)")
        for offset, ticks, parsed in plausible_dotnet_ticks(index_data):
            print(f"  .NET ticks at offset {offset}: {ticks} -> {parsed.isoformat(timespec='microseconds')}")

    for dat_path in sorted(sef2.parent.glob("*.dat")):
        if dat_path.name.lower() == "materialfolderindex.dat":
            continue
        print_related_file_matches(dat_path, "DAT video file", tick_values)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
