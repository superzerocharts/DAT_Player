#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dat_player {

enum class RecordingMetadataConfidence {
    None,
    Low,
    Medium,
    High
};

struct RecordingDateTimeParts {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int millisecond = 0;
    int microsecond = 0;
};

struct RecordingSidecarMetadata {
    bool available = false;
    std::filesystem::path path;
    std::uint64_t start_ticks = 0;
    std::uint64_t end_ticks = 0;
    bool has_start_ticks = false;
    bool has_end_ticks = false;
    std::string camera_name;
    std::string manufacturer;
    std::string model;
    std::vector<int> timezone_offset_minutes_candidates;
};

struct RecordingMetadata {
    bool has_dat_frame_ticks = false;
    std::uint64_t first_frame_ticks = 0;
    std::uint64_t last_frame_ticks = 0;
    double dat_duration_seconds = 0.0;
    RecordingSidecarMetadata sidecar{};
    RecordingMetadataConfidence confidence = RecordingMetadataConfidence::None;
    std::string source;
};

bool is_plausible_dotnet_ticks(std::uint64_t ticks);
bool parse_dotnet_iso_ticks(const std::string& value, std::uint64_t& ticks);
bool dotnet_ticks_to_parts(std::uint64_t ticks, RecordingDateTimeParts& parts);
std::string format_dotnet_ticks(std::uint64_t ticks);
std::string to_string(RecordingMetadataConfidence confidence);

RecordingSidecarMetadata parse_sef2_metadata_xml(const std::string& xml);
RecordingSidecarMetadata parse_sef2_metadata_file(const std::filesystem::path& path);
RecordingSidecarMetadata try_load_sef2_sidecar(const std::filesystem::path& dat_path);

} // namespace dat_player
