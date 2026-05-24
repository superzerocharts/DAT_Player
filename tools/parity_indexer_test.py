from __future__ import annotations

import io
import math
import struct


TIMEBASE = 39062.5


def append_record(data: bytearray, marker: bytes, timestamp: int, width: int, height: int, payload_size: int) -> None:
    data += struct.pack("<QII", timestamp, width, height)
    data += marker
    data += struct.pack("<I", payload_size)
    data += bytes([0x5A]) * payload_size


def append_declared_record(
    data: bytearray,
    marker: bytes,
    timestamp: int,
    width: int,
    height: int,
    declared_payload_size: int,
    actual_payload_size: int,
) -> None:
    data += struct.pack("<QII", timestamp, width, height)
    data += marker
    data += struct.pack("<I", declared_payload_size)
    data += bytes([0x5A]) * actual_payload_size


def index_bytes(payload: bytes, buffer_size: int = 1024 * 1024):
    frames = []
    candidates = 0
    rejected = 0
    stream = io.BytesIO(payload)
    carry = b""
    offset = 0
    carry_len = 23
    skip_until = 0

    while True:
        chunk = stream.read(buffer_size)
        if not chunk:
            break
        window = carry + chunk
        window_offset = offset - len(carry)
        has_more = stream.tell() < len(payload)
        scan_limit = len(window)

        pos = 16
        while pos + 4 <= scan_limit:
            if has_more and pos + 8 > len(window):
                break

            absolute_pos = window_offset + pos
            if absolute_pos < skip_until:
                if skip_until > window_offset:
                    pos = max(pos + 1, min(skip_until - window_offset, scan_limit))
                else:
                    pos += 1
                continue

            marker = window[pos:pos + 4]
            if marker not in (b"H264", b"I264"):
                pos += 1
                continue
            candidates += 1
            if pos + 8 > len(window):
                rejected += 1
                pos += 1
                continue

            timestamp = struct.unpack_from("<Q", window, pos - 16)[0]
            width = struct.unpack_from("<I", window, pos - 8)[0]
            height = struct.unpack_from("<I", window, pos - 4)[0]
            payload_size = struct.unpack_from("<I", window, pos + 4)[0]
            marker_offset = window_offset + pos
            payload_offset = marker_offset + 8

            if width == 0 or height == 0 or payload_size == 0 or payload_offset + payload_size > len(payload):
                rejected += 1
            else:
                frames.append((marker, timestamp, width, height, payload_size, marker_offset, payload_offset))
                skip_until = max(skip_until, payload_offset + payload_size)
                if payload_offset + payload_size > window_offset:
                    pos = max(pos + 1, min(payload_offset + payload_size - window_offset, scan_limit))
                    continue
            pos += 1

        carry = window[-carry_len:]
        offset += len(chunk)

    duration = 0.0
    fps = 0.0
    if len(frames) >= 2 and frames[-1][1] > frames[0][1]:
        duration = (frames[-1][1] - frames[0][1]) / TIMEBASE
        fps = (len(frames) - 1) / duration
    return frames, candidates, rejected, duration, fps


def test_valid_h264_frame_is_indexed():
    data = bytearray(b"\xaa\xbb\xcc")
    append_record(data, b"H264", 1000, 1920, 1080, 5)
    frames, _, _, _, _ = index_bytes(bytes(data))
    assert len(frames) == 1
    assert frames[0][:5] == (b"H264", 1000, 1920, 1080, 5)
    assert frames[0][5] == 19
    assert frames[0][6] == 27


def test_valid_i264_frame_is_indexed():
    data = bytearray()
    append_record(data, b"I264", 2000, 640, 360, 3)
    frames, _, _, _, _ = index_bytes(bytes(data))
    assert len(frames) == 1
    assert frames[0][:5] == (b"I264", 2000, 640, 360, 3)
    assert frames[0][5] == 16
    assert frames[0][6] == 24


def test_multiple_records_are_indexed_in_order():
    data = bytearray()
    append_record(data, b"H264", 1000, 1920, 1080, 5)
    data += b"\x00\x01"
    append_record(data, b"I264", 2000, 1920, 1080, 3)
    frames, _, _, _, _ = index_bytes(bytes(data))
    assert len(frames) == 2
    assert frames[0][0] == b"H264"
    assert frames[1][0] == b"I264"
    assert frames[0][5] < frames[1][5]


def test_invalid_sparse_data_is_rejected():
    data = bytearray([0x11] * 64)
    append_record(data, b"H264", 1000, 0, 1080, 5)
    data += struct.pack("<QII", 2000, 1920, 1080)
    data += b"I264"
    data += struct.pack("<I", 10_000)
    data += b"H264"
    frames, candidates, rejected, _, _ = index_bytes(bytes(data))
    assert frames == []
    assert candidates >= 2
    assert rejected >= 2


def test_truncated_metadata_before_marker_is_skipped():
    data = bytearray([0x22] * 15)
    data += b"H264"
    data += struct.pack("<I", 4)
    data += bytes([0x5A]) * 4
    frames, _, _, _, _ = index_bytes(bytes(data))
    assert frames == []


def test_truncated_payload_after_marker_is_skipped():
    data = bytearray()
    append_declared_record(data, b"H264", 1000, 800, 600, 8, 3)
    frames, _, rejected, _, _ = index_bytes(bytes(data))
    assert frames == []
    assert rejected == 1


def test_zero_payload_size_is_skipped():
    data = bytearray()
    append_declared_record(data, b"H264", 1000, 800, 600, 0, 0)
    frames, _, rejected, _, _ = index_bytes(bytes(data))
    assert frames == []
    assert rejected == 1


def test_absurd_payload_size_is_skipped():
    data = bytearray()
    append_declared_record(data, b"I264", 1000, 800, 600, 0xFFFFFFF0, 0)
    frames, _, rejected, _, _ = index_bytes(bytes(data))
    assert frames == []
    assert rejected == 1


def test_markers_split_across_buffers_are_detected():
    data = bytearray([0xEE] * 7)
    append_record(data, b"H264", 1000, 640, 480, 2)
    frames, _, _, _, _ = index_bytes(bytes(data), buffer_size=21)
    assert len(frames) == 1
    assert frames[0][5] == 23


def test_marker_starting_on_buffer_boundary_is_detected():
    data = bytearray([0xEE] * 16)
    append_record(data, b"H264", 1000, 640, 480, 2)
    frames, _, _, _, _ = index_bytes(bytes(data), buffer_size=32)
    assert len(frames) == 1
    assert frames[0][5] == 32


def test_complete_header_near_buffer_end_is_detected():
    data = bytearray([0xEE] * 9)
    append_record(data, b"H264", 1000, 640, 480, 12)
    frames, _, _, _, _ = index_bytes(bytes(data), buffer_size=33)
    assert len(frames) == 1
    assert frames[0][5] == 25
    assert frames[0][6] == 33


def test_duration_estimate_uses_fallback_timebase():
    data = bytearray()
    append_record(data, b"H264", 0, 1280, 720, 1)
    append_record(data, b"I264", 78125, 1280, 720, 1)
    frames, _, _, duration, fps = index_bytes(bytes(data))
    assert len(frames) == 2
    assert math.isclose(duration, 2.0, abs_tol=0.0001)


def test_fps_estimate_uses_frame_count_and_duration():
    data = bytearray()
    append_record(data, b"H264", 0, 1280, 720, 1)
    append_record(data, b"I264", 39062, 1280, 720, 1)
    append_record(data, b"I264", 78125, 1280, 720, 1)
    frames, _, _, _, fps = index_bytes(bytes(data))
    assert len(frames) == 3
    assert math.isclose(fps, 1.0, abs_tol=0.0001)


def test_final_buffer_boundary_is_scanned():
    data = bytearray()
    append_record(data, b"H264", 1000, 320, 240, 8)
    frames, _, _, _, _ = index_bytes(bytes(data), buffer_size=len(data))
    assert len(frames) == 1


if __name__ == "__main__":
    tests = [
        test_valid_h264_frame_is_indexed,
        test_valid_i264_frame_is_indexed,
        test_multiple_records_are_indexed_in_order,
        test_invalid_sparse_data_is_rejected,
        test_truncated_metadata_before_marker_is_skipped,
        test_truncated_payload_after_marker_is_skipped,
        test_zero_payload_size_is_skipped,
        test_absurd_payload_size_is_skipped,
        test_markers_split_across_buffers_are_detected,
        test_marker_starting_on_buffer_boundary_is_detected,
        test_complete_header_near_buffer_end_is_detected,
        test_duration_estimate_uses_fallback_timebase,
        test_fps_estimate_uses_frame_count_and_duration,
        test_final_buffer_boundary_is_scanned,
    ]
    for test in tests:
        test()
        print(f"[PASS] {test.__name__}")
