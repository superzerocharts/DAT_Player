#pragma once

#include "dat_player/RecordingMetadata.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <string>
#include <vector>

namespace dat_player {

enum class DatFrameType {
    H264,
    I264
};

struct DatFrameRecord {
    DatFrameType type{};
    std::uint64_t timestamp = 0;
    std::uint64_t legacy_timestamp = 0;
    std::uint64_t recording_ticks = 0;
    double elapsed_seconds = 0.0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t payload_size = 0;
    std::uint64_t record_offset = 0;
    std::uint64_t marker_offset = 0;
    std::uint64_t payload_offset = 0;
    bool keyframe = false;
    bool has_recording_ticks = false;
    bool has_elapsed_seconds = false;
};

struct DatIndexOptions {
    std::size_t buffer_size = 1024 * 1024;
    double fallback_timebase_units_per_second = 39062.5;
    bool validate_payload_bounds = true;
    bool allow_zero_dimensions = false;
    bool allow_zero_payload = false;
};

struct DatSidecarCalibration {
    bool available = false;
    double fps = 0.0;
    double duration_seconds = 0.0;
};

struct DatIndexSummary {
    std::uint64_t source_size = 0;
    std::uint64_t candidate_markers = 0;
    std::uint64_t rejected_records = 0;
    double duration_seconds = 0.0;
    double estimated_fps = 0.0;
    double timestamp_units_per_second = 39062.5;
    bool using_recording_ticks_for_timing = false;
    DatSidecarCalibration sidecar_calibration{};
    RecordingMetadata recording_metadata{};
};

struct DatFrameIndex {
    std::vector<DatFrameRecord> frames;
    DatIndexSummary summary{};
};

class DatFrameIndexer {
public:
    explicit DatFrameIndexer(DatIndexOptions options = {});

    DatFrameIndex index_file(const std::filesystem::path& dat_path) const;
    DatFrameIndex index_stream(std::istream& input, std::uint64_t source_size) const;

private:
    DatIndexOptions options_;
};

DatSidecarCalibration try_load_sidecar_calibration(const std::filesystem::path& dat_path);

std::string to_string(DatFrameType type);

} // namespace dat_player
