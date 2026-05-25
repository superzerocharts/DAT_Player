#include "dat_player/DatFrameIndexer.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace dat_player {
namespace {

constexpr std::size_t kLegacyPreMarkerBytes = 16;
constexpr std::size_t kRecordingTickPreMarkerBytes = 17;
constexpr std::size_t kMarkerBytes = 4;
constexpr std::size_t kPayloadSizeBytes = 4;
constexpr std::size_t kRecordHeaderBytes = kRecordingTickPreMarkerBytes + kMarkerBytes + kPayloadSizeBytes;
constexpr std::size_t kCarryBytes = kRecordHeaderBytes - 1;
constexpr double kDotNetTicksPerSecond = 10'000'000.0;

std::uint32_t read_u32_le(const unsigned char* data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint64_t read_u64_le(const unsigned char* data) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(data[i]) << (i * 8);
    }
    return value;
}

bool is_marker_at(const std::vector<unsigned char>& data, std::size_t pos, DatFrameType& type) {
    if (pos + kMarkerBytes > data.size()) {
        return false;
    }

    if (data[pos] == 'H' && data[pos + 1] == '2' && data[pos + 2] == '6' && data[pos + 3] == '4') {
        type = DatFrameType::H264;
        return true;
    }

    if (data[pos] == 'I' && data[pos + 1] == '2' && data[pos + 2] == '6' && data[pos + 3] == '4') {
        type = DatFrameType::I264;
        return true;
    }

    return false;
}

std::uint64_t checked_add(std::uint64_t left, std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

void finalize_estimates(DatFrameIndex& index, double timebase_units_per_second) {
    if (index.frames.size() < 2 || timebase_units_per_second <= 0.0) {
        return;
    }

    const auto first = index.frames.front().timestamp;
    const auto last = index.frames.back().timestamp;
    if (last <= first) {
        return;
    }

    index.summary.duration_seconds = static_cast<double>(last - first) / timebase_units_per_second;
    if (index.summary.duration_seconds > 0.0) {
        index.summary.estimated_fps =
            static_cast<double>(index.frames.size() - 1) / index.summary.duration_seconds;
    }
}

bool recording_tick_span_is_plausible(const DatFrameIndex& index) {
    if (index.frames.size() < 2) {
        return false;
    }
    const auto& first = index.frames.front();
    const auto& last = index.frames.back();
    if (!first.has_recording_ticks || !last.has_recording_ticks || last.recording_ticks <= first.recording_ticks) {
        return false;
    }
    const double duration_seconds = static_cast<double>(last.recording_ticks - first.recording_ticks) / kDotNetTicksPerSecond;
    return duration_seconds > 0.0 && duration_seconds < 24.0 * 60.0 * 60.0;
}

void apply_recording_tick_timing(DatFrameIndex& index) {
    if (!recording_tick_span_is_plausible(index)) {
        index.summary.timestamp_units_per_second = 39062.5;
        return;
    }

    const auto first_ticks = index.frames.front().recording_ticks;
    const auto last_ticks = index.frames.back().recording_ticks;
    index.summary.duration_seconds = static_cast<double>(last_ticks - first_ticks) / kDotNetTicksPerSecond;
    index.summary.estimated_fps = static_cast<double>(index.frames.size() - 1) / index.summary.duration_seconds;
    index.summary.timestamp_units_per_second = kDotNetTicksPerSecond;
    index.summary.using_recording_ticks_for_timing = true;
    index.summary.recording_metadata.has_dat_frame_ticks = true;
    index.summary.recording_metadata.first_frame_ticks = first_ticks;
    index.summary.recording_metadata.last_frame_ticks = last_ticks;
    index.summary.recording_metadata.dat_duration_seconds = index.summary.duration_seconds;
    index.summary.recording_metadata.confidence = RecordingMetadataConfidence::Medium;
    index.summary.recording_metadata.source = "DAT frame ticks";

    for (auto& frame : index.frames) {
        if (frame.has_recording_ticks && frame.recording_ticks >= first_ticks) {
            frame.timestamp = frame.recording_ticks;
            frame.elapsed_seconds = static_cast<double>(frame.recording_ticks - first_ticks) / kDotNetTicksPerSecond;
            frame.has_elapsed_seconds = true;
        }
    }
}

void apply_sidecar_metadata(DatFrameIndex& index, RecordingSidecarMetadata sidecar) {
    if (!sidecar.available) {
        return;
    }

    index.summary.recording_metadata.sidecar = std::move(sidecar);
    auto& metadata = index.summary.recording_metadata;
    const bool sidecar_has_range = metadata.sidecar.has_start_ticks && metadata.sidecar.has_end_ticks;
    if (!metadata.has_dat_frame_ticks) {
        metadata.confidence = sidecar_has_range ? RecordingMetadataConfidence::Medium : RecordingMetadataConfidence::None;
        metadata.source = sidecar_has_range ? ".sef2 data" : metadata.source;
        if (sidecar_has_range && metadata.sidecar.end_ticks > metadata.sidecar.start_ticks) {
            metadata.dat_duration_seconds =
                static_cast<double>(metadata.sidecar.end_ticks - metadata.sidecar.start_ticks) / kDotNetTicksPerSecond;
        }
        return;
    }

    if (sidecar_has_range) {
        constexpr std::uint64_t tolerance_ticks = 2ULL * 10'000'000ULL;
        const auto start_delta = metadata.first_frame_ticks > metadata.sidecar.start_ticks
            ? metadata.first_frame_ticks - metadata.sidecar.start_ticks
            : metadata.sidecar.start_ticks - metadata.first_frame_ticks;
        const auto end_delta = metadata.last_frame_ticks > metadata.sidecar.end_ticks
            ? metadata.last_frame_ticks - metadata.sidecar.end_ticks
            : metadata.sidecar.end_ticks - metadata.last_frame_ticks;
        if (start_delta <= tolerance_ticks && end_delta <= tolerance_ticks) {
            metadata.confidence = RecordingMetadataConfidence::High;
            metadata.source = "DAT frame ticks + .sef2 data";
        }
    }
}

} // namespace

DatFrameIndexer::DatFrameIndexer(DatIndexOptions options)
    : options_(options) {
    if (options_.buffer_size < kRecordHeaderBytes) {
        options_.buffer_size = kRecordHeaderBytes;
    }
}

DatFrameIndex DatFrameIndexer::index_file(const std::filesystem::path& dat_path) const {
    std::ifstream input(dat_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Unable to open DAT file for reading: " + dat_path.string());
    }

    const auto file_size = std::filesystem::file_size(dat_path);
    auto index = index_stream(input, static_cast<std::uint64_t>(file_size));
    apply_sidecar_metadata(index, try_load_sef2_sidecar(dat_path));

    return index;
}

DatFrameIndex DatFrameIndexer::index_stream(std::istream& input, std::uint64_t source_size) const {
    DatFrameIndex index;
    index.summary.source_size = source_size;

    std::vector<unsigned char> carry;
    carry.reserve(kCarryBytes);

    std::vector<unsigned char> read_buffer(options_.buffer_size);
    std::uint64_t chunk_offset = 0;
    std::uint64_t skip_until_offset = 0;

    while (input) {
        input.read(reinterpret_cast<char*>(read_buffer.data()), static_cast<std::streamsize>(read_buffer.size()));
        const auto bytes_read = static_cast<std::size_t>(input.gcount());
        if (bytes_read == 0) {
            break;
        }

        std::vector<unsigned char> window;
        window.reserve(carry.size() + bytes_read);
        window.insert(window.end(), carry.begin(), carry.end());
        window.insert(window.end(), read_buffer.begin(), read_buffer.begin() + bytes_read);

        const std::uint64_t window_offset = chunk_offset - static_cast<std::uint64_t>(carry.size());
        const bool has_more_bytes = checked_add(chunk_offset, static_cast<std::uint64_t>(bytes_read)) < source_size;
        const std::size_t scan_limit = window.size();

        std::size_t pos = kLegacyPreMarkerBytes;
        while (pos + kMarkerBytes <= scan_limit) {
            if (has_more_bytes && pos + kMarkerBytes + kPayloadSizeBytes > window.size()) {
                break;
            }

            const auto absolute_pos = window_offset + static_cast<std::uint64_t>(pos);
            if (absolute_pos < skip_until_offset) {
                if (skip_until_offset > window_offset) {
                    const auto jump_to = static_cast<std::size_t>(
                        std::min<std::uint64_t>(skip_until_offset - window_offset, scan_limit));
                    pos = std::max(pos + 1, jump_to);
                } else {
                    ++pos;
                }
                continue;
            }

            DatFrameType type{};
            if (!is_marker_at(window, pos, type)) {
                ++pos;
                continue;
            }

            ++index.summary.candidate_markers;
            const auto marker_offset = window_offset + static_cast<std::uint64_t>(pos);
            if (pos < kLegacyPreMarkerBytes || pos + kMarkerBytes + kPayloadSizeBytes > window.size()) {
                ++index.summary.rejected_records;
                ++pos;
                continue;
            }

            const auto* base = window.data() + pos;
            const auto legacy_timestamp = read_u64_le(base - kLegacyPreMarkerBytes);
            const bool has_recording_ticks = pos >= kRecordingTickPreMarkerBytes &&
                is_plausible_dotnet_ticks(read_u64_le(base - kRecordingTickPreMarkerBytes));
            const auto recording_ticks = has_recording_ticks ? read_u64_le(base - kRecordingTickPreMarkerBytes) : 0;
            const auto timestamp = has_recording_ticks ? recording_ticks : legacy_timestamp;
            const auto width = read_u32_le(base - 8);
            const auto height = read_u32_le(base - 4);
            const auto payload_size = read_u32_le(base + 4);
            const auto payload_offset = marker_offset + kMarkerBytes + kPayloadSizeBytes;
            const auto payload_end = checked_add(payload_offset, payload_size);

            const bool dimensions_ok = options_.allow_zero_dimensions || (width > 0 && height > 0);
            const bool payload_ok = options_.allow_zero_payload || payload_size > 0;
            const bool bounds_ok = !options_.validate_payload_bounds || payload_end <= source_size;

            if (!dimensions_ok || !payload_ok || !bounds_ok) {
                ++index.summary.rejected_records;
                ++pos;
                continue;
            }

            index.frames.push_back(DatFrameRecord{
                type,
                timestamp,
                legacy_timestamp,
                recording_ticks,
                0.0,
                width,
                height,
                payload_size,
                marker_offset - (has_recording_ticks ? kRecordingTickPreMarkerBytes : kLegacyPreMarkerBytes),
                marker_offset,
                payload_offset,
                type == DatFrameType::H264,
                has_recording_ticks,
                false
            });

            skip_until_offset = std::max(skip_until_offset, payload_end);
            if (payload_end > window_offset) {
                const auto next_pos = static_cast<std::size_t>(
                    std::min<std::uint64_t>(payload_end - window_offset, scan_limit));
                pos = std::max(pos + 1, next_pos);
            } else {
                ++pos;
            }
        }

        const auto carry_size = std::min(kCarryBytes, window.size());
        carry.assign(window.end() - static_cast<std::ptrdiff_t>(carry_size), window.end());
        chunk_offset += static_cast<std::uint64_t>(bytes_read);
    }

    std::sort(index.frames.begin(), index.frames.end(), [](const auto& left, const auto& right) {
        return left.marker_offset < right.marker_offset;
    });

    apply_recording_tick_timing(index);
    if (!index.summary.using_recording_ticks_for_timing) {
        finalize_estimates(index, options_.fallback_timebase_units_per_second);
        if (index.summary.duration_seconds > 0.0) {
            index.summary.recording_metadata.confidence = RecordingMetadataConfidence::Low;
            index.summary.recording_metadata.source = "legacy DAT timing";
        }
    }
    return index;
}

} // namespace dat_player
