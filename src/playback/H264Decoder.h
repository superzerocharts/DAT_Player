#pragma once

#include "dat_player/DatFrameIndexer.h"
#include "playback/VideoFrame.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace dat_player::playback {

struct H264PayloadInspection {
    bool saw_start_code = false;
    bool saw_sps = false;
    bool saw_pps = false;
    bool saw_idr = false;
    std::uint64_t payloads_inspected = 0;
};

struct DecodeSmokeTestResult {
    bool media_foundation_initialized = false;
    bool decoder_found = false;
    bool payload_format_supported = false;
    bool decoded_any_frame = false;
    std::uint64_t frames_submitted = 0;
    std::uint64_t frames_decoded = 0;
    std::uint32_t decoded_width = 0;
    std::uint32_t decoded_height = 0;
    std::wstring decoded_subtype;
    std::wstring message;
    H264PayloadInspection inspection{};
};

struct FirstFrameRenderResult {
    DecodeSmokeTestResult decode{};
    BgraVideoFrame frame{};
    bool frame_available = false;
    std::wstring message;
};

struct ForwardPlaybackOptions {
    std::uint64_t start_frame = 0;
    double fallback_fps = 30.0;
};

struct ForwardPlaybackFrame {
    BgraVideoFrame frame{};
    std::uint64_t frame_index = 0;
    std::uint64_t timestamp = 0;
    std::uint64_t frames_submitted = 0;
    std::uint64_t frames_decoded = 0;
};

using ForwardPlaybackCallback = std::function<bool(ForwardPlaybackFrame&& frame)>;

class H264DecodeSmokeTester {
public:
    DecodeSmokeTestResult run(
        const std::filesystem::path& dat_path,
        const DatFrameIndex& index,
        std::size_t max_frames_to_submit = 120) const;

    FirstFrameRenderResult render_first_frame(
        const std::filesystem::path& dat_path,
        const DatFrameIndex& index,
        std::size_t max_frames_to_submit = 120) const;

    DecodeSmokeTestResult play_forward(
        const std::filesystem::path& dat_path,
        const DatFrameIndex& index,
        const ForwardPlaybackOptions& options,
        const ForwardPlaybackCallback& on_frame) const;
};

} // namespace dat_player::playback
