#include "dat_player/DatFrameIndexer.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using dat_player::DatFrameIndexer;
using dat_player::DatFrameType;
using dat_player::DatIndexOptions;
using dat_player::RecordingMetadataConfidence;

struct TestFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw TestFailure(message);
    }
}

void append_u32_le(std::vector<unsigned char>& data, std::uint32_t value) {
    data.push_back(static_cast<unsigned char>(value & 0xff));
    data.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
    data.push_back(static_cast<unsigned char>((value >> 16) & 0xff));
    data.push_back(static_cast<unsigned char>((value >> 24) & 0xff));
}

void append_u64_le(std::vector<unsigned char>& data, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        data.push_back(static_cast<unsigned char>((value >> (i * 8)) & 0xff));
    }
}

void append_record(
    std::vector<unsigned char>& data,
    const char marker[4],
    std::uint64_t timestamp,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t payload_size) {
    append_u64_le(data, timestamp);
    append_u32_le(data, width);
    append_u32_le(data, height);
    data.insert(data.end(), marker, marker + 4);
    append_u32_le(data, payload_size);
    data.insert(data.end(), payload_size, 0x5a);
}

void append_record_with_dotnet_ticks(
    std::vector<unsigned char>& data,
    const char marker[4],
    std::uint64_t recording_ticks,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t payload_size) {
    append_u64_le(data, recording_ticks);
    data.push_back(0x01);
    append_u32_le(data, width);
    append_u32_le(data, height);
    data.insert(data.end(), marker, marker + 4);
    append_u32_le(data, payload_size);
    data.insert(data.end(), payload_size, 0x5a);
}

void append_declared_record(
    std::vector<unsigned char>& data,
    const char marker[4],
    std::uint64_t timestamp,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t declared_payload_size,
    std::uint32_t actual_payload_size) {
    append_u64_le(data, timestamp);
    append_u32_le(data, width);
    append_u32_le(data, height);
    data.insert(data.end(), marker, marker + 4);
    append_u32_le(data, declared_payload_size);
    data.insert(data.end(), actual_payload_size, 0x5a);
}

std::istringstream stream_from(const std::vector<unsigned char>& data) {
    return std::istringstream(
        std::string(reinterpret_cast<const char*>(data.data()), data.size()),
        std::ios::in | std::ios::binary);
}

void valid_h264_frame_is_indexed() {
    std::vector<unsigned char> data = {0xaa, 0xbb, 0xcc};
    append_record(data, "H264", 1000, 1920, 1080, 5);

    auto input = stream_from(data);
    const auto index = DatFrameIndexer().index_stream(input, data.size());

    require(index.frames.size() == 1, "expected one valid H264 frame");
    require(index.frames[0].type == DatFrameType::H264, "first frame should be H264");
    require(index.frames[0].keyframe, "H264 should be marked keyframe");
    require(index.frames[0].timestamp == 1000, "first timestamp mismatch");
    require(index.frames[0].width == 1920, "first width mismatch");
    require(index.frames[0].height == 1080, "first height mismatch");
    require(index.frames[0].payload_size == 5, "first payload size mismatch");
    require(index.frames[0].marker_offset == 19, "marker offset mismatch");
    require(index.frames[0].payload_offset == 27, "payload offset mismatch");
    require(index.frames[0].record_offset == 3, "record offset mismatch");
}

void valid_i264_frame_is_indexed() {
    std::vector<unsigned char> data;
    append_record(data, "I264", 2000, 640, 360, 3);

    auto input = stream_from(data);
    const auto index = DatFrameIndexer().index_stream(input, data.size());

    require(index.frames.size() == 1, "expected one valid I264 frame");
    require(index.frames[0].type == DatFrameType::I264, "frame should be I264");
    require(!index.frames[0].keyframe, "I264 should not be marked keyframe");
    require(index.frames[0].timestamp == 2000, "timestamp mismatch");
    require(index.frames[0].width == 640, "width mismatch");
    require(index.frames[0].height == 360, "height mismatch");
    require(index.frames[0].payload_size == 3, "payload size mismatch");
    require(index.frames[0].marker_offset == 16, "marker offset mismatch");
    require(index.frames[0].payload_offset == 24, "payload offset mismatch");
}

void multiple_records_are_indexed_in_order() {
    std::vector<unsigned char> data;
    append_record(data, "H264", 1000, 1920, 1080, 5);
    data.insert(data.end(), {0x00, 0x01});
    append_record(data, "I264", 2000, 1920, 1080, 3);

    auto input = stream_from(data);
    const auto index = DatFrameIndexer().index_stream(input, data.size());

    require(index.frames.size() == 2, "expected two valid frames");
    require(index.frames[0].type == DatFrameType::H264, "first frame should be H264");
    require(index.frames[1].type == DatFrameType::I264, "second frame should be I264");
    require(index.frames[0].marker_offset < index.frames[1].marker_offset, "frames should be ordered by marker offset");
}

void invalid_sparse_data_is_rejected() {
    std::vector<unsigned char> data(64, 0x11);
    append_record(data, "H264", 1000, 0, 1080, 5);
    append_u64_le(data, 2000);
    append_u32_le(data, 1920);
    append_u32_le(data, 1080);
    data.insert(data.end(), {'I', '2', '6', '4'});
    append_u32_le(data, 10'000);
    data.insert(data.end(), {'H', '2', '6', '4'});

    auto input = stream_from(data);
    const auto index = DatFrameIndexer().index_stream(input, data.size());

    require(index.frames.empty(), "invalid records should be rejected");
    require(index.summary.candidate_markers >= 2, "expected candidate markers");
    require(index.summary.rejected_records >= 2, "expected rejected records");
}

void truncated_metadata_before_marker_is_skipped() {
    std::vector<unsigned char> data(15, 0x22);
    data.insert(data.end(), {'H', '2', '6', '4'});
    append_u32_le(data, 4);
    data.insert(data.end(), 4, 0x5a);

    auto input = stream_from(data);
    const auto index = DatFrameIndexer().index_stream(input, data.size());

    require(index.frames.empty(), "marker without full pre-marker metadata should be skipped");
}

void truncated_payload_after_marker_is_skipped() {
    std::vector<unsigned char> data;
    append_declared_record(data, "H264", 1000, 800, 600, 8, 3);

    auto input = stream_from(data);
    const auto index = DatFrameIndexer().index_stream(input, data.size());

    require(index.frames.empty(), "truncated payload should be skipped");
    require(index.summary.rejected_records == 1, "truncated payload should be counted as rejected");
}

void zero_payload_size_is_skipped() {
    std::vector<unsigned char> data;
    append_declared_record(data, "H264", 1000, 800, 600, 0, 0);

    auto input = stream_from(data);
    const auto index = DatFrameIndexer().index_stream(input, data.size());

    require(index.frames.empty(), "zero payload should be skipped by default");
    require(index.summary.rejected_records == 1, "zero payload should be counted as rejected");
}

void absurd_payload_size_is_skipped() {
    std::vector<unsigned char> data;
    append_declared_record(data, "I264", 1000, 800, 600, 0xfffffff0u, 0);

    auto input = stream_from(data);
    const auto index = DatFrameIndexer().index_stream(input, data.size());

    require(index.frames.empty(), "absurd payload should be skipped");
    require(index.summary.rejected_records == 1, "absurd payload should be counted as rejected");
}

void markers_split_across_buffers_are_detected() {
    std::vector<unsigned char> data(7, 0xee);
    append_record(data, "H264", 1000, 640, 480, 2);

    DatIndexOptions options;
    options.buffer_size = 21;
    auto input = stream_from(data);
    const auto index = DatFrameIndexer(options).index_stream(input, data.size());

    require(index.frames.size() == 1, "cross-buffer marker should be indexed");
    require(index.frames[0].marker_offset == 23, "unexpected marker offset");
}

void marker_starting_on_buffer_boundary_is_detected() {
    std::vector<unsigned char> data(16, 0xee);
    append_record(data, "H264", 1000, 640, 480, 2);

    DatIndexOptions options;
    options.buffer_size = 32;
    auto input = stream_from(data);
    const auto index = DatFrameIndexer(options).index_stream(input, data.size());

    require(index.frames.size() == 1, "marker starting on buffer boundary should be indexed");
    require(index.frames[0].marker_offset == 32, "unexpected boundary marker offset");
}

void complete_header_near_buffer_end_is_detected() {
    std::vector<unsigned char> data(9, 0xee);
    append_record(data, "H264", 1000, 640, 480, 12);

    DatIndexOptions options;
    options.buffer_size = 33;
    auto input = stream_from(data);
    const auto index = DatFrameIndexer(options).index_stream(input, data.size());

    require(index.frames.size() == 1, "complete header near buffer end should be indexed");
    require(index.frames[0].marker_offset == 25, "unexpected near-end marker offset");
    require(index.frames[0].payload_offset == 33, "unexpected near-end payload offset");
}

void duration_estimate_uses_fallback_timebase() {
    std::vector<unsigned char> data;
    append_record(data, "H264", 0, 1280, 720, 1);
    append_record(data, "I264", 78125, 1280, 720, 1);

    auto input = stream_from(data);
    const auto index = DatFrameIndexer().index_stream(input, data.size());

    require(index.frames.size() == 2, "expected two frames");
    require(std::fabs(index.summary.duration_seconds - 2.0) < 0.0001, "duration estimate mismatch");
}

void fps_estimate_uses_frame_count_and_duration() {
    std::vector<unsigned char> data;
    append_record(data, "H264", 0, 1280, 720, 1);
    append_record(data, "I264", 39062, 1280, 720, 1);
    append_record(data, "I264", 78125, 1280, 720, 1);

    auto input = stream_from(data);
    const auto index = DatFrameIndexer().index_stream(input, data.size());

    require(index.frames.size() == 3, "expected three frames");
    require(std::fabs(index.summary.estimated_fps - 1.0) < 0.0001, "fps estimate mismatch");
}

void marker_minus_17_dotnet_ticks_drive_timing() {
    constexpr std::uint64_t start_ticks = 639148479165410000ULL;
    std::vector<unsigned char> data;
    append_record_with_dotnet_ticks(data, "H264", start_ticks, 1280, 720, 1);
    append_record_with_dotnet_ticks(data, "I264", start_ticks + 10'000'000ULL, 1280, 720, 1);
    append_record_with_dotnet_ticks(data, "I264", start_ticks + 20'000'000ULL, 1280, 720, 1);

    auto input = stream_from(data);
    const auto index = DatFrameIndexer().index_stream(input, data.size());

    require(index.frames.size() == 3, "expected three frames with recording ticks");
    require(index.summary.using_recording_ticks_for_timing, "expected true recording ticks to drive timing");
    require(index.frames[0].has_recording_ticks, "first frame should store recording ticks");
    require(index.frames[0].recording_ticks == start_ticks, "recording tick mismatch");
    require(index.frames[0].timestamp == start_ticks, "timestamp should use true recording tick");
    require(index.frames[0].record_offset == 0, "true-tick record should start at marker -17");
    require(std::fabs(index.summary.duration_seconds - 2.0) < 0.0001, "dotnet duration estimate mismatch");
    require(std::fabs(index.summary.estimated_fps - 1.0) < 0.0001, "dotnet fps estimate mismatch");
    require(index.summary.recording_metadata.confidence == RecordingMetadataConfidence::Medium, "DAT-only confidence should be medium");
    require(!index.summary.recording_metadata.sidecar.has_display_offset_minutes, "DAT-only metadata should not claim archive display offset");
}

void marker_minus_16_legacy_timing_remains_fallback() {
    std::vector<unsigned char> data;
    append_record(data, "H264", 0, 1280, 720, 1);
    append_record(data, "I264", 78125, 1280, 720, 1);

    auto input = stream_from(data);
    const auto index = DatFrameIndexer().index_stream(input, data.size());

    require(index.frames.size() == 2, "expected two legacy frames");
    require(!index.summary.using_recording_ticks_for_timing, "legacy timing should remain fallback");
    require(!index.frames[0].has_recording_ticks, "legacy frame should not claim recording ticks");
    require(index.frames[0].timestamp == 0, "legacy timestamp should be preserved");
    require(std::fabs(index.summary.duration_seconds - 2.0) < 0.0001, "legacy fallback duration mismatch");
    require(index.summary.recording_metadata.confidence == RecordingMetadataConfidence::Low, "legacy-only confidence should be low");
}

void sef2_parser_extracts_recording_metadata() {
    const std::string xml =
        "<root>"
        "<start>2026-05-20T04:25:16.5410000</start>"
        "<end>2026-05-20T04:30:14.7380000</end>"
        "<channels><channel name=\"MjA1IENob2tlIFBvaW50\" manufacturer=\"AXIS\" model=\"AXIS Q1645 Network Camera\" "
        "timezone=\"AMDc8bz///8AaMRhCAAAAA==\" /></channels>"
        "</root>";

    const auto metadata = dat_player::parse_sef2_metadata_xml(xml);
    require(metadata.available, "expected sidecar metadata");
    require(metadata.has_start_ticks && metadata.start_ticks == 639148479165410000ULL, "start tick mismatch");
    require(metadata.has_end_ticks && metadata.end_ticks == 639148482147380000ULL, "end tick mismatch");
    require(metadata.camera_name == "205 Choke Point", "camera name should decode from base64");
    require(metadata.manufacturer == "AXIS", "manufacturer mismatch");
    require(metadata.model == "AXIS Q1645 Network Camera", "model mismatch");
    require(metadata.has_display_offset_minutes, "display offset should be derived from timezone metadata");
    require(metadata.display_offset_minutes == -420, "display offset should combine standard and daylight offsets");

    std::uint64_t display_ticks = 0;
    require(dat_player::offset_dotnet_ticks(metadata.start_ticks, metadata.display_offset_minutes, display_ticks), "display tick offset should apply");
    dat_player::RecordingDateTimeParts parts;
    require(dat_player::dotnet_ticks_to_parts(display_ticks + 5'000'000ULL, parts), "display ticks should decode");
    require(parts.year == 2026 && parts.month == 5 && parts.day == 19, "display date should match archive-local date");
    require(parts.hour == 21 && parts.minute == 25 && parts.second == 17, "display time should match archive-local time");
}

std::filesystem::path unique_temp_dir() {
    auto dir = std::filesystem::temp_directory_path() /
        ("dat_player_metadata_test_" + std::to_string(static_cast<unsigned long long>(std::rand())));
    std::filesystem::create_directories(dir);
    return dir;
}

void write_binary_file(const std::filesystem::path& path, const std::vector<unsigned char>& data) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

void write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    output << text;
}

void dat_ticks_matching_sef2_are_high_confidence() {
    constexpr std::uint64_t start_ticks = 639148479165410000ULL;
    constexpr std::uint64_t end_ticks = 639148482147380000ULL;
    const auto dir = unique_temp_dir();
    const auto dat_path = dir / "clip.dat";
    const auto sef2_path = dir / "clip.sef2";
    std::vector<unsigned char> data;
    append_record_with_dotnet_ticks(data, "H264", start_ticks, 1280, 720, 1);
    append_record_with_dotnet_ticks(data, "I264", end_ticks, 1280, 720, 1);
    write_binary_file(dat_path, data);
    write_text_file(sef2_path,
        "<root><start>2026-05-20T04:25:16.5410000</start>"
        "<end>2026-05-20T04:30:14.7380000</end></root>");

    const auto index = DatFrameIndexer().index_file(dat_path);
    require(index.summary.recording_metadata.confidence == RecordingMetadataConfidence::High, "matching sidecar should be high confidence");
    require(index.summary.recording_metadata.sidecar.available, "sidecar should be available");
    require(index.summary.recording_metadata.source == "DAT frame ticks + .sef2 sidecar", "metadata source mismatch");
    std::filesystem::remove_all(dir);
}

void mismatched_sef2_keeps_dat_metadata_source() {
    constexpr std::uint64_t start_ticks = 639148479165410000ULL;
    constexpr std::uint64_t end_ticks = 639148482147380000ULL;
    const auto dir = unique_temp_dir();
    const auto dat_path = dir / "clip.dat";
    const auto sef2_path = dir / "clip.sef2";
    std::vector<unsigned char> data;
    append_record_with_dotnet_ticks(data, "H264", start_ticks, 1280, 720, 1);
    append_record_with_dotnet_ticks(data, "I264", end_ticks, 1280, 720, 1);
    write_binary_file(dat_path, data);
    write_text_file(sef2_path,
        "<root><start>2026-05-21T04:25:16.5410000</start>"
        "<end>2026-05-21T04:30:14.7380000</end></root>");

    const auto index = DatFrameIndexer().index_file(dat_path);
    require(index.summary.recording_metadata.confidence == RecordingMetadataConfidence::Medium, "mismatched sidecar should leave DAT confidence");
    require(index.summary.recording_metadata.sidecar.available, "mismatched sidecar should still be parsed");
    require(index.summary.recording_metadata.source == "DAT frame ticks", "mismatched sidecar should not become combined metadata source");
    std::filesystem::remove_all(dir);
}

void sef2_only_metadata_is_medium_confidence() {
    const auto dir = unique_temp_dir();
    const auto dat_path = dir / "clip.dat";
    const auto sef2_path = dir / "clip.sef2";
    std::vector<unsigned char> data;
    append_record(data, "H264", 0, 1280, 720, 1);
    append_record(data, "I264", 78125, 1280, 720, 1);
    write_binary_file(dat_path, data);
    write_text_file(sef2_path,
        "<root><start>2026-05-20T04:25:16.5410000</start>"
        "<end>2026-05-20T04:30:14.7380000</end></root>");

    const auto index = DatFrameIndexer().index_file(dat_path);
    require(index.summary.recording_metadata.confidence == RecordingMetadataConfidence::Medium, "sidecar-only confidence should be medium");
    require(index.summary.recording_metadata.sidecar.available, "sidecar should be available");
    require(!index.summary.using_recording_ticks_for_timing, "sidecar should not alter legacy playback timing");
    std::filesystem::remove_all(dir);
}

void final_buffer_boundary_is_scanned() {
    std::vector<unsigned char> data;
    append_record(data, "H264", 1000, 320, 240, 8);

    DatIndexOptions options;
    options.buffer_size = data.size();
    auto input = stream_from(data);
    const auto index = DatFrameIndexer(options).index_stream(input, data.size());

    require(index.frames.size() == 1, "record ending on buffer boundary should be indexed");
}

int run_test(const char* name, void (*test)()) {
    try {
        test();
        std::cout << "[PASS] " << name << '\n';
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << name << ": " << ex.what() << '\n';
        return 1;
    }
}

} // namespace

int main() {
    int failures = 0;
    failures += run_test("valid_h264_frame_is_indexed", valid_h264_frame_is_indexed);
    failures += run_test("valid_i264_frame_is_indexed", valid_i264_frame_is_indexed);
    failures += run_test("multiple_records_are_indexed_in_order", multiple_records_are_indexed_in_order);
    failures += run_test("invalid_sparse_data_is_rejected", invalid_sparse_data_is_rejected);
    failures += run_test("truncated_metadata_before_marker_is_skipped", truncated_metadata_before_marker_is_skipped);
    failures += run_test("truncated_payload_after_marker_is_skipped", truncated_payload_after_marker_is_skipped);
    failures += run_test("zero_payload_size_is_skipped", zero_payload_size_is_skipped);
    failures += run_test("absurd_payload_size_is_skipped", absurd_payload_size_is_skipped);
    failures += run_test("markers_split_across_buffers_are_detected", markers_split_across_buffers_are_detected);
    failures += run_test("marker_starting_on_buffer_boundary_is_detected", marker_starting_on_buffer_boundary_is_detected);
    failures += run_test("complete_header_near_buffer_end_is_detected", complete_header_near_buffer_end_is_detected);
    failures += run_test("duration_estimate_uses_fallback_timebase", duration_estimate_uses_fallback_timebase);
    failures += run_test("fps_estimate_uses_frame_count_and_duration", fps_estimate_uses_frame_count_and_duration);
    failures += run_test("marker_minus_17_dotnet_ticks_drive_timing", marker_minus_17_dotnet_ticks_drive_timing);
    failures += run_test("marker_minus_16_legacy_timing_remains_fallback", marker_minus_16_legacy_timing_remains_fallback);
    failures += run_test("sef2_parser_extracts_recording_metadata", sef2_parser_extracts_recording_metadata);
    failures += run_test("dat_ticks_matching_sef2_are_high_confidence", dat_ticks_matching_sef2_are_high_confidence);
    failures += run_test("mismatched_sef2_keeps_dat_metadata_source", mismatched_sef2_keeps_dat_metadata_source);
    failures += run_test("sef2_only_metadata_is_medium_confidence", sef2_only_metadata_is_medium_confidence);
    failures += run_test("final_buffer_boundary_is_scanned", final_buffer_boundary_is_scanned);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
