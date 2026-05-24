#include "dat_player/DatFrameIndexer.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace dat_player {
namespace {

constexpr std::size_t kPreMarkerBytes = 16;
constexpr std::size_t kMarkerBytes = 4;
constexpr std::size_t kPayloadSizeBytes = 4;
constexpr std::size_t kRecordHeaderBytes = kPreMarkerBytes + kMarkerBytes + kPayloadSizeBytes;
constexpr std::size_t kCarryBytes = kRecordHeaderBytes - 1;

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
    index.summary.sidecar_calibration = try_load_sidecar_calibration(dat_path);

    if (index.summary.sidecar_calibration.available) {
        index.summary.duration_seconds = index.summary.sidecar_calibration.duration_seconds;
        index.summary.estimated_fps = index.summary.sidecar_calibration.fps;
    }

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

        std::size_t pos = kPreMarkerBytes;
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
            if (pos < kPreMarkerBytes || pos + kMarkerBytes + kPayloadSizeBytes > window.size()) {
                ++index.summary.rejected_records;
                ++pos;
                continue;
            }

            const auto* base = window.data() + pos;
            const auto timestamp = read_u64_le(base - 16);
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
                width,
                height,
                payload_size,
                marker_offset - kPreMarkerBytes,
                marker_offset,
                payload_offset,
                type == DatFrameType::H264
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

    finalize_estimates(index, options_.fallback_timebase_units_per_second);
    return index;
}

DatSidecarCalibration try_load_sidecar_calibration(const std::filesystem::path&) {
    // Future phase: inspect .sef/.sef2 sidecars to refine FPS and duration.
    return {};
}

std::string to_string(DatFrameType type) {
    return type == DatFrameType::H264 ? "H264" : "I264";
}

} // namespace dat_player
