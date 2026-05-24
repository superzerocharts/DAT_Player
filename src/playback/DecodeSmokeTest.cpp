#include "dat_player/DatFrameIndexer.h"
#include "playback/H264Decoder.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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

void write_u16(std::ofstream& output, std::uint16_t value) {
    output.put(static_cast<char>(value & 0xff));
    output.put(static_cast<char>((value >> 8) & 0xff));
}

void write_u32(std::ofstream& output, std::uint32_t value) {
    output.put(static_cast<char>(value & 0xff));
    output.put(static_cast<char>((value >> 8) & 0xff));
    output.put(static_cast<char>((value >> 16) & 0xff));
    output.put(static_cast<char>((value >> 24) & 0xff));
}

void write_bgra_bmp(const std::filesystem::path& output_path, const dat_player::playback::BgraVideoFrame& frame) {
    if (frame.pixels.empty() || frame.display_width == 0 || frame.display_height == 0 ||
        frame.stride_bytes != frame.display_width * 4) {
        throw std::runtime_error("No tightly packed BGRA frame is available to write.");
    }

    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Unable to create BMP output file.");
    }

    const std::uint32_t pixel_bytes = frame.stride_bytes * frame.display_height;
    const std::uint32_t file_header_bytes = 14;
    const std::uint32_t dib_header_bytes = 40;
    const std::uint32_t pixel_offset = file_header_bytes + dib_header_bytes;
    const std::uint32_t file_size = pixel_offset + pixel_bytes;

    write_u16(output, 0x4d42);
    write_u32(output, file_size);
    write_u16(output, 0);
    write_u16(output, 0);
    write_u32(output, pixel_offset);

    write_u32(output, dib_header_bytes);
    write_u32(output, frame.display_width);
    write_u32(output, static_cast<std::uint32_t>(0 - frame.display_height));
    write_u16(output, 1);
    write_u16(output, 32);
    write_u32(output, 0);
    write_u32(output, pixel_bytes);
    write_u32(output, 2835);
    write_u32(output, 2835);
    write_u32(output, 0);
    write_u32(output, 0);
    output.write(reinterpret_cast<const char*>(frame.pixels.data()), static_cast<std::streamsize>(frame.pixels.size()));
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        std::wcerr << L"Usage: dat_decode_smoke_test.exe <path-to-dat> [--render|--playback-smoke [max-frames]|--seek-smoke <frame-index>|--dump-frame-bmp <frame-index> <output.bmp>]\n";
        return 2;
    }

    const std::filesystem::path dat_path(argv[1]);
    const bool render_first_frame = argc >= 3 && std::wstring(argv[2]) == L"--render";
    const bool playback_smoke = argc >= 3 && std::wstring(argv[2]) == L"--playback-smoke";
    const bool seek_smoke = argc >= 3 && std::wstring(argv[2]) == L"--seek-smoke";
    const bool dump_frame_bmp = argc >= 3 && std::wstring(argv[2]) == L"--dump-frame-bmp";
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
    std::uint64_t dump_target = 0;
    std::filesystem::path dump_output_path;
    if (dump_frame_bmp) {
        if (argc < 5) {
            std::wcerr << L"--dump-frame-bmp requires <frame-index> <output.bmp>.\n";
            return 2;
        }
        dump_target = std::wcstoull(argv[3], nullptr, 10);
        dump_output_path = argv[4];
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
        if (dump_frame_bmp) {
            if (index.frames.empty()) {
                std::wcerr << L"No indexed frames to dump.\n";
                return 1;
            }
            dump_target = std::min<std::uint64_t>(dump_target, static_cast<std::uint64_t>(index.frames.size() - 1));
            dat_player::playback::ForwardPlaybackOptions options;
            options.start_frame = dump_target;
            std::uint64_t rendered = 0;
            dat_player::playback::ForwardPlaybackFrame rendered_frame;
            const auto result = tester.play_forward(dat_path, index, options, [&](dat_player::playback::ForwardPlaybackFrame&& frame) {
                ++rendered;
                rendered_frame = std::move(frame);
                return false;
            });
            print_result(result);
            if (rendered == 0 || rendered_frame.frame_index != dump_target) {
                std::wcerr << L"Unable to decode requested frame for BMP dump.\n";
                return 1;
            }
            write_bgra_bmp(dump_output_path, rendered_frame.frame);
            std::wcout << L"Dumped frame " << rendered_frame.frame_index << L" to " << dump_output_path.wstring() << L"\n";
            std::wcout << L"Dumped display: " << rendered_frame.frame.display_width << L" x " << rendered_frame.frame.display_height << L"\n";
            return 0;
        }

        const auto result = tester.run(dat_path, index);
        print_result(result);
        return result.decoded_any_frame ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cerr << "Smoke test failed: " << ex.what() << "\n";
        return 1;
    }
}
