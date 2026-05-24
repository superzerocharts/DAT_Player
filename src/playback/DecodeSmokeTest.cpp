#include "dat_player/DatFrameIndexer.h"
#include "playback/H264Decoder.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

void print_result(const dat_player::playback::DecodeSmokeTestResult& result) {
    std::wcout << L"Media Foundation initialized: " << (result.media_foundation_initialized ? L"yes" : L"no") << L"\n";
    std::wcout << L"H.264 decoder found: " << (result.decoder_found ? L"yes" : L"no") << L"\n";
    std::wcout << L"Annex B start codes found: " << (result.inspection.saw_start_code ? L"yes" : L"no") << L"\n";
    std::wcout << L"SPS found: " << (result.inspection.saw_sps ? L"yes" : L"no") << L"\n";
    std::wcout << L"PPS found: " << (result.inspection.saw_pps ? L"yes" : L"no") << L"\n";
    std::wcout << L"IDR found: " << (result.inspection.saw_idr ? L"yes" : L"no") << L"\n";
    std::wcout << L"Payload format accepted for test: " << (result.payload_format_supported ? L"yes" : L"no") << L"\n";
    std::wcout << L"Payloads inspected: " << result.inspection.payloads_inspected << L"\n";
    std::wcout << L"Frames submitted: " << result.frames_submitted << L"\n";
    std::wcout << L"Frames decoded: " << result.frames_decoded << L"\n";
    if (result.decoded_width > 0 && result.decoded_height > 0) {
        std::wcout << L"First decoded format: " << result.decoded_width << L" x " << result.decoded_height;
        if (!result.decoded_subtype.empty()) {
            std::wcout << L" " << result.decoded_subtype;
        }
        std::wcout << L"\n";
    } else {
        std::wcout << L"First decoded format: n/a\n";
    }
    std::wcout << L"Result: " << result.message << L"\n";
}

void print_render_result(const dat_player::playback::FirstFrameRenderResult& result) {
    print_result(result.decode);
    std::wcout << L"Rendered frame available: " << (result.frame_available ? L"yes" : L"no") << L"\n";
    if (result.frame_available) {
        std::wcout << L"Rendered display size: " << result.frame.display_width << L" x " << result.frame.display_height << L"\n";
        std::wcout << L"Rendered BGRA stride: " << result.frame.stride_bytes << L"\n";
    }
    std::wcout << L"Render result: " << result.message << L"\n";
}

void print_playback_result(const dat_player::playback::DecodeSmokeTestResult& result, std::uint64_t frames_rendered) {
    print_result(result);
    std::wcout << L"Forward playback frames rendered callback count: " << frames_rendered << L"\n";
}

std::uint64_t nearest_previous_keyframe(const dat_player::DatFrameIndex& index, std::uint64_t frame) {
    if (index.frames.empty()) {
        return 0;
    }
    const auto clamped = std::min<std::uint64_t>(frame, static_cast<std::uint64_t>(index.frames.size() - 1));
    for (std::uint64_t i = clamped + 1; i > 0; --i) {
        const auto candidate = i - 1;
        if (index.frames[static_cast<std::size_t>(candidate)].keyframe) {
            return candidate;
        }
    }
    return 0;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        std::wcerr << L"Usage: dat_decode_smoke_test.exe <path-to-dat> [--render|--playback-smoke [max-frames]|--seek-smoke <frame-index>]\n";
        return 2;
    }

    const std::filesystem::path dat_path(argv[1]);
    const bool render_first_frame = argc >= 3 && std::wstring(argv[2]) == L"--render";
    const bool playback_smoke = argc >= 3 && std::wstring(argv[2]) == L"--playback-smoke";
    const bool seek_smoke = argc >= 3 && std::wstring(argv[2]) == L"--seek-smoke";
    std::uint64_t max_playback_frames = 180;
    if (playback_smoke && argc >= 4) {
        max_playback_frames = std::wcstoull(argv[3], nullptr, 10);
        if (max_playback_frames == 0) {
            max_playback_frames = 180;
        }
    }
    std::uint64_t seek_target = 0;
    if (seek_smoke && argc >= 4) {
        seek_target = std::wcstoull(argv[3], nullptr, 10);
    }
    try {
        dat_player::DatFrameIndexer indexer;
        const auto index = indexer.index_file(dat_path);
        std::wcout << L"Indexed frames: " << index.frames.size() << L"\n";
        if (!index.frames.empty()) {
            std::wcout << L"First resolution: " << index.frames.front().width << L" x " << index.frames.front().height << L"\n";
        }

        dat_player::playback::H264DecodeSmokeTester tester;
        if (render_first_frame) {
            const auto result = tester.render_first_frame(dat_path, index);
            print_render_result(result);
            return result.frame_available ? 0 : 1;
        }
        if (playback_smoke) {
            dat_player::playback::ForwardPlaybackOptions options;
            std::uint64_t rendered = 0;
            const auto result = tester.play_forward(dat_path, index, options, [&](dat_player::playback::ForwardPlaybackFrame&& frame) {
                ++rendered;
                if (rendered == 1 || rendered == max_playback_frames) {
                    std::wcout << L"Playback frame " << rendered
                               << L": index=" << frame.frame_index
                               << L" display=" << frame.frame.display_width << L" x " << frame.frame.display_height
                               << L" decoded=" << frame.frame.decoded_width << L" x " << frame.frame.decoded_height
                               << L" timestamp=" << frame.timestamp << L"\n";
                }
                return rendered < max_playback_frames;
            });
            print_playback_result(result, rendered);
            return rendered > 1 && result.decoded_any_frame ? 0 : 1;
        }
        if (seek_smoke) {
            if (index.frames.empty()) {
                std::wcerr << L"No indexed frames to seek.\n";
                return 1;
            }
            seek_target = std::min<std::uint64_t>(seek_target, static_cast<std::uint64_t>(index.frames.size() - 1));
            const auto keyframe = nearest_previous_keyframe(index, seek_target);
            dat_player::playback::ForwardPlaybackOptions options;
            options.start_frame = seek_target;
            std::uint64_t rendered = 0;
            dat_player::playback::ForwardPlaybackFrame rendered_frame;
            const auto result = tester.play_forward(dat_path, index, options, [&](dat_player::playback::ForwardPlaybackFrame&& frame) {
                ++rendered;
                rendered_frame = std::move(frame);
                return false;
            });
            print_result(result);
            std::wcout << L"Seek requested frame: " << seek_target << L"\n";
            std::wcout << L"Seek keyframe used: " << keyframe << L"\n";
            std::wcout << L"Seek rendered callbacks: " << rendered << L"\n";
            if (rendered > 0) {
                std::wcout << L"Seek rendered frame index: " << rendered_frame.frame_index << L"\n";
                std::wcout << L"Seek rendered display: " << rendered_frame.frame.display_width << L" x " << rendered_frame.frame.display_height << L"\n";
            }
            return rendered > 0 && rendered_frame.frame_index == seek_target ? 0 : 1;
        }

        const auto result = tester.run(dat_path, index);
        print_result(result);
        return result.decoded_any_frame ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cerr << "Smoke test failed: " << ex.what() << "\n";
        return 1;
    }
}
