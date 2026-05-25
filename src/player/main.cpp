#include "dat_player/DatFrameIndexer.h"
#include "playback/H264Decoder.h"
#include "player/resource.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

constexpr int kOpenButtonId = 1001;
constexpr int kPlayButtonId = 1002;
constexpr int kDecodeButtonId = 1003;
constexpr int kRenderFirstFrameButtonId = 1004;
constexpr int kActualSizeButtonId = 1005;
constexpr int kTimelineId = 1006;
constexpr int kInfoLabelId = 1007;
constexpr int kStatusLabelId = 1008;
constexpr int kFilePathId = 1010;
constexpr int kFileLabelId = 1011;
constexpr int kDetailsGroupId = 1012;
constexpr int kCurrentTimeLabelId = 1013;
constexpr int kTotalTimeLabelId = 1014;
constexpr int kDetailsToggleButtonId = 1015;
constexpr int kSpeedButtonId = 1016;
constexpr int kThumbTimeLabelId = 1017;
constexpr int kIntegrityDotId = 1018;
constexpr UINT kPlaybackFrameMessage = WM_APP + 1;
constexpr UINT kPlaybackFinishedMessage = WM_APP + 2;
constexpr UINT kSeekFinishedMessage = WM_APP + 3;
constexpr UINT kPreviewFrameMessage = WM_APP + 4;
constexpr UINT kPreviewFinishedMessage = WM_APP + 5;
constexpr UINT kIntegrityFinishedMessage = WM_APP + 6;
constexpr UINT_PTR kTimelinePreviewTimerId = 42;
constexpr UINT_PTR kResizeRefreshTimerId = 43;
constexpr int kTrackbarMax = 10000;
constexpr int kDefaultWindowWidth = 670;
constexpr int kDefaultWindowHeight = 584;
constexpr auto kPreviewThrottle = std::chrono::milliseconds(200);
constexpr auto kDiagnosticsUpdateThrottle = std::chrono::milliseconds(500);
constexpr auto kResizeRefreshDelay = std::chrono::milliseconds(90);

struct UiPlaybackFrame {
    dat_player::playback::BgraVideoFrame frame;
    std::uint64_t generation = 0;
    std::uint64_t frame_index = 0;
    std::uint64_t timestamp = 0;
    std::uint64_t frames_submitted = 0;
    std::uint64_t frames_decoded = 0;
    std::uint64_t late_frames = 0;
    double effective_fps = 0.0;
    double target_interval_ms = 0.0;
    double frame_interval_ms = 0.0;
    double scheduled_sleep_ms = 0.0;
    double convert_ms = 0.0;
    std::uint64_t clock_reanchors = 0;
    std::chrono::steady_clock::time_point posted_at{};
    bool seek_frame = false;
};

struct PlaybackFinishedMessage {
    std::uint64_t generation = 0;
    bool decoded_any_frame = false;
    std::wstring text;
};

struct SeekFinishedMessage {
    std::uint64_t generation = 0;
    bool decoded_any_frame = false;
    bool resume_after_seek = false;
    std::uint64_t requested_frame = 0;
    std::uint64_t keyframe_frame = 0;
    std::uint64_t frames_decoded = 0;
    std::wstring text;
};

struct PreviewFinishedMessage {
    std::uint64_t generation = 0;
    std::uint64_t target_frame = 0;
    std::uint64_t keyframe_frame = 0;
    std::uint64_t rendered_frame = 0;
    std::uint64_t frames_decoded = 0;
    std::wstring text;
};

struct IntegrityFinishedMessage {
    std::uint64_t generation = 0;
    std::wstring cache_key;
    dat_player::RecordingSidecarMetadata sidecar{};
};

struct PlayerState {
    HWND hwnd = nullptr;
    HWND open_button = nullptr;
    HWND play_button = nullptr;
    HWND actual_size_button = nullptr;
    HWND details_toggle_button = nullptr;
    HWND speed_button = nullptr;
    HWND decode_test_button = nullptr;
    HWND render_first_frame_button = nullptr;
    HWND timeline = nullptr;
    HWND file_label = nullptr;
    HWND file_path_edit = nullptr;
    HWND integrity_dot = nullptr;
    HWND details_group = nullptr;
    HWND current_time_label = nullptr;
    HWND total_time_label = nullptr;
    HWND thumb_time_label = nullptr;
    HWND video_panel = nullptr;
    HWND info_label = nullptr;
    HWND status_label = nullptr;
    HFONT ui_font = nullptr;
    HBRUSH window_background_brush = nullptr;
    dat_player::DatFrameIndex index;
    std::filesystem::path loaded_path;
    std::wstring decode_test_text;
    std::wstring displayed_info_text;
    dat_player::playback::BgraVideoFrame rendered_frame;
    std::thread playback_thread;
    std::thread preview_thread;
    std::atomic_bool stop_playback_requested = false;
    std::atomic_bool stop_preview_requested = false;
    std::atomic<int> playback_speed_index = 0;
    std::atomic<std::uint64_t> playback_timing_generation = 0;
    std::atomic<std::uint64_t> playback_generation = 0;
    std::atomic<std::uint64_t> preview_generation = 0;
    std::atomic<std::uint64_t> integrity_generation = 0;
    std::map<std::wstring, dat_player::RecordingSidecarMetadata> integrity_cache;
    std::uint64_t frames_rendered = 0;
    std::uint64_t frames_decoded = 0;
    std::uint64_t late_frames = 0;
    std::uint64_t clock_reanchors = 0;
    double effective_playback_fps = 0.0;
    double target_frame_interval_ms = 0.0;
    double frame_interval_ms = 0.0;
    double average_scheduled_sleep_ms = 0.0;
    double max_scheduled_sleep_ms = 0.0;
    double recent_frame_interval_ms = 0.0;
    double max_recent_frame_interval_ms = 0.0;
    double average_ui_delay_ms = 0.0;
    double max_ui_delay_ms = 0.0;
    double average_convert_ms = 0.0;
    double max_convert_ms = 0.0;
    double last_paint_ms = 0.0;
    double average_paint_ms = 0.0;
    double max_paint_ms = 0.0;
    std::chrono::steady_clock::time_point last_ui_frame_time{};
    std::chrono::steady_clock::time_point last_info_update_time{};
    RECT thumb_time_label_rect = {};
    bool has_thumb_time_label_rect = false;
    std::wstring displayed_thumb_time_text;
    int integrity_dot_state = -1;
    std::uint64_t current_frame = 0;
    bool timeline_dragging = false;
    bool resume_after_timeline_drag = false;
    bool pending_seek_resume_after_completion = false;
    std::uint64_t pending_seek_resume_generation = 0;
    bool has_timeline_preview = false;
    std::uint64_t timeline_preview_frame = 0;
    bool has_pending_seek = false;
    std::uint64_t pending_seek_frame = 0;
    bool preview_in_flight = false;
    bool preview_timer_armed = false;
    std::uint64_t latest_preview_frame = 0;
    std::uint64_t active_preview_frame = 0;
    std::uint64_t preview_keyframe_frame = 0;
    std::uint64_t preview_rendered_frame = 0;
    std::uint64_t preview_frames_decoded = 0;
    std::uint64_t preview_superseded = 0;
    std::chrono::steady_clock::time_point last_preview_started{};
    bool seeking = false;
    bool playing = false;
    bool details_visible = false;
    bool actual_size_applied = false;
    bool force_native_video_size = false;
    int requested_video_width = 0;
    int requested_video_height = 0;
    bool live_resizing = false;
};

PlayerState g_state;

void start_forward_playback();
void handle_drop_files(HWND hwnd, HDROP drop);
void layout_controls(HWND hwnd);
void update_info(bool force = false);

std::wstring widen(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return L"";
    }

    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::wstring format_double(double value, int precision) {
    if (value <= 0.0) {
        return L"n/a";
    }

    std::wostringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

std::wstring format_nonnegative_double(double value, int precision) {
    if (value < 0.0) {
        return L"n/a";
    }

    std::wostringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

bool has_dat_extension(const std::filesystem::path& path) {
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return extension == L".dat";
}

bool is_existing_readable_dat_file(const std::filesystem::path& path, std::wstring& error) {
    std::error_code ec;
    if (!has_dat_extension(path)) {
        error = L"Unsupported drop: please drop one .dat file.";
        return false;
    }
    if (!std::filesystem::exists(path, ec) || ec) {
        error = L"Dropped .dat file does not exist.";
        return false;
    }
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        error = L"Unsupported drop: please drop one .dat file, not a folder.";
        return false;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0) {
        error = L"Dropped .dat file is empty or unreadable.";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = L"Dropped .dat file is not readable.";
        return false;
    }
    return true;
}

std::uint64_t frame_count() {
    return static_cast<std::uint64_t>(g_state.index.frames.size());
}

std::uint64_t keyframe_count() {
    return static_cast<std::uint64_t>(std::count_if(
        g_state.index.frames.begin(),
        g_state.index.frames.end(),
        [](const dat_player::DatFrameRecord& frame) { return frame.keyframe; }));
}

std::uint64_t nearest_previous_keyframe(std::uint64_t frame) {
    if (g_state.index.frames.empty()) {
        return 0;
    }

    const auto clamped = std::min<std::uint64_t>(frame, frame_count() - 1);
    for (std::uint64_t i = clamped + 1; i > 0; --i) {
        const auto candidate = i - 1;
        if (g_state.index.frames[static_cast<std::size_t>(candidate)].keyframe) {
            return candidate;
        }
    }
    return 0;
}

double playback_fps() {
    if (g_state.index.summary.estimated_fps >= 1.0 && g_state.index.summary.estimated_fps <= 120.0) {
        return g_state.index.summary.estimated_fps;
    }
    return 30.0;
}

double playback_speed_multiplier_from_index(int index) {
    switch (std::clamp(index, 0, 4)) {
    case 1:
        return 2.0;
    case 2:
        return 4.0;
    case 3:
        return 8.0;
    case 4:
        return 16.0;
    default:
        return 1.0;
    }
}

double playback_speed_multiplier() {
    return playback_speed_multiplier_from_index(g_state.playback_speed_index.load());
}

std::wstring playback_speed_label(double speed) {
    if (speed >= 15.5) {
        return L"x16";
    }
    if (speed >= 7.5) {
        return L"x8";
    }
    if (speed >= 3.5) {
        return L"x4";
    }
    if (speed >= 1.5) {
        return L"x2";
    }
    return L"x1";
}

std::wstring playback_speed_label() {
    return playback_speed_label(playback_speed_multiplier());
}

std::wstring next_speed_button_text() {
    switch (std::clamp(g_state.playback_speed_index.load(), 0, 4)) {
    case 0:
        return L"x2";
    case 1:
        return L"x4";
    case 2:
        return L"x8";
    case 3:
        return L"x16";
    default:
        return L"x1";
    }
}

int details_panel_width_for_client_width(int client_width) {
    return std::clamp(client_width / 3, 320, 370);
}

int minimum_video_width_for_controls() {
    constexpr int button_width = 104;
    constexpr int button_gap = 8;
    return button_width * 5 + button_gap * 4;
}

int minimum_client_width() {
    constexpr int padding = 14;
    if (!g_state.details_visible) {
        return minimum_video_width_for_controls() + padding * 2;
    }
    return minimum_video_width_for_controls() + details_panel_width_for_client_width(960) + padding * 3;
}

int minimum_client_height() {
    return 430;
}

SIZE window_size_for_client_size(HWND hwnd, int client_width, int client_height) {
    RECT window_rect = {0, 0, client_width, client_height};
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const auto ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    AdjustWindowRectEx(&window_rect, style, FALSE, ex_style);
    return {
        window_rect.right - window_rect.left,
        window_rect.bottom - window_rect.top
    };
}

int client_width_for_video_width(int video_width, bool details_visible) {
    constexpr int padding = 14;
    const int preserved_video_width = std::max(video_width, minimum_video_width_for_controls());
    if (!details_visible) {
        return preserved_video_width + padding * 2;
    }

    int details_width = 370;
    int client_width = preserved_video_width + padding * 3 + details_width;
    for (int i = 0; i < 6; ++i) {
        details_width = details_panel_width_for_client_width(client_width);
        client_width = preserved_video_width + padding * 3 + details_width;
    }
    return client_width;
}

double seconds_for_frame(std::uint64_t frame) {
    if (g_state.index.frames.empty()) {
        return 0.0;
    }

    const auto clamped = std::min<std::uint64_t>(frame, frame_count() - 1);
    const auto& record = g_state.index.frames[static_cast<std::size_t>(clamped)];
    if (record.has_elapsed_seconds) {
        return record.elapsed_seconds;
    }
    const auto first = g_state.index.frames.front().timestamp;
    const auto timestamp = record.timestamp;
    if (timestamp < first) {
        const double fps = playback_fps();
        return fps > 0.0 ? static_cast<double>(clamped) / fps : 0.0;
    }
    const double timebase = g_state.index.summary.timestamp_units_per_second > 0.0
        ? g_state.index.summary.timestamp_units_per_second
        : 39062.5;
    const double timestamp_seconds = static_cast<double>(timestamp - first) / timebase;
    const double fallback_seconds = playback_fps() > 0.0 ? static_cast<double>(clamped) / playback_fps() : 0.0;
    if (timestamp_seconds <= std::max(0.5, fallback_seconds * 4.0 + 0.5)) {
        return timestamp_seconds;
    }
    return fallback_seconds;
}

double total_duration_seconds() {
    if (g_state.index.summary.duration_seconds > 0.0) {
        return g_state.index.summary.duration_seconds;
    }
    if (frame_count() > 1 && playback_fps() > 0.0) {
        return static_cast<double>(frame_count() - 1) / playback_fps();
    }
    return 0.0;
}

double media_seconds_from_start(
    const dat_player::DatFrameIndex& index,
    std::uint64_t start_frame,
    std::uint64_t frame,
    double fallback_fps) {
    if (!index.frames.empty() && start_frame < index.frames.size() && frame < index.frames.size()) {
        const auto& target_record = index.frames[static_cast<std::size_t>(frame)];
        const auto& start_record = index.frames[static_cast<std::size_t>(start_frame)];
        if (target_record.has_elapsed_seconds && start_record.has_elapsed_seconds &&
            target_record.elapsed_seconds >= start_record.elapsed_seconds) {
            return target_record.elapsed_seconds - start_record.elapsed_seconds;
        }
        const auto start_timestamp = index.frames[static_cast<std::size_t>(start_frame)].timestamp;
        const auto timestamp = index.frames[static_cast<std::size_t>(frame)].timestamp;
        if (timestamp >= start_timestamp) {
            const double timebase = index.summary.timestamp_units_per_second > 0.0
                ? index.summary.timestamp_units_per_second
                : 39062.5;
            const double seconds = static_cast<double>(timestamp - start_timestamp) / timebase;
            const double frame_delta = static_cast<double>(frame - start_frame);
            const double fallback_seconds = fallback_fps > 0.0 ? frame_delta / fallback_fps : frame_delta / 30.0;
            if (seconds >= 0.0 && seconds <= std::max(0.5, fallback_seconds * 4.0 + 0.5)) {
                return seconds;
            }
        }
    }

    const double fps = fallback_fps > 0.0 ? fallback_fps : 30.0;
    return static_cast<double>(frame - start_frame) / fps;
}

double stable_playback_seconds_from_start(std::uint64_t start_frame, std::uint64_t frame, double fps) {
    const auto clamped_fps = (fps >= 1.0 && fps <= 120.0) ? fps : 30.0;
    if (frame < start_frame) {
        return 0.0;
    }
    return static_cast<double>(frame - start_frame) / clamped_fps;
}

int timeline_position_from_frame(std::uint64_t frame) {
    const auto count = frame_count();
    if (count <= 1) {
        return 0;
    }

    const auto clamped_frame = std::min(frame, count - 1);
    return static_cast<int>((clamped_frame * static_cast<std::uint64_t>(kTrackbarMax)) / (count - 1));
}

std::uint64_t frame_from_timeline_position(int position) {
    const auto count = frame_count();
    if (count <= 1) {
        return 0;
    }

    const auto clamped_position = std::clamp(position, 0, kTrackbarMax);
    return (static_cast<std::uint64_t>(clamped_position) * (count - 1)) / kTrackbarMax;
}

int timeline_position_from_scroll(WPARAM wparam) {
    const int code = LOWORD(wparam);
    if (code == TB_THUMBTRACK || code == TB_THUMBPOSITION) {
        return static_cast<int>(HIWORD(wparam));
    }
    return g_state.timeline ? static_cast<int>(SendMessageW(g_state.timeline, TBM_GETPOS, 0, 0)) : 0;
}

std::wstring frame_time_label(std::uint64_t frame) {
    std::wostringstream text;
    text << L"frame " << (frame + 1) << L" / " << frame_count()
         << L" (" << format_double(seconds_for_frame(frame), 2) << L" / "
         << format_double(total_duration_seconds(), 2) << L" sec)";
    return text.str();
}

std::wstring format_clock_time(double seconds) {
    if (seconds <= 0.0) {
        return L"00:00";
    }

    const auto whole_seconds = static_cast<std::uint64_t>(seconds + 0.5);
    const auto hours = whole_seconds / 3600;
    const auto minutes = (whole_seconds / 60) % 60;
    const auto secs = whole_seconds % 60;

    std::wostringstream text;
    text << std::setfill(L'0');
    if (hours > 0) {
        text << hours << L":" << std::setw(2) << minutes << L":" << std::setw(2) << secs;
    } else {
        text << std::setw(2) << minutes << L":" << std::setw(2) << secs;
    }
    return text.str();
}

bool archive_display_time_available() {
    const auto& metadata = g_state.index.summary.recording_metadata;
    if (metadata.confidence == dat_player::RecordingMetadataConfidence::None ||
        metadata.confidence == dat_player::RecordingMetadataConfidence::Low) {
        return false;
    }
    if (!metadata.sidecar.available || !metadata.sidecar.has_display_offset_minutes) {
        return false;
    }
    return !metadata.has_dat_frame_ticks ||
        metadata.confidence == dat_player::RecordingMetadataConfidence::High;
}

bool recording_time_display_enabled() {
    const auto& metadata = g_state.index.summary.recording_metadata;
    if (!archive_display_time_available()) {
        return false;
    }
    return metadata.has_dat_frame_ticks ||
        (metadata.sidecar.has_start_ticks && metadata.sidecar.has_end_ticks);
}

bool raw_recording_start_ticks(std::uint64_t& ticks) {
    const auto& metadata = g_state.index.summary.recording_metadata;
    if (metadata.has_dat_frame_ticks) {
        ticks = metadata.first_frame_ticks;
        return true;
    }
    if (metadata.sidecar.has_start_ticks) {
        ticks = metadata.sidecar.start_ticks;
        return true;
    }
    return false;
}

bool raw_recording_end_ticks(std::uint64_t& ticks) {
    const auto& metadata = g_state.index.summary.recording_metadata;
    if (metadata.has_dat_frame_ticks) {
        ticks = metadata.last_frame_ticks;
        return true;
    }
    if (metadata.sidecar.has_end_ticks) {
        ticks = metadata.sidecar.end_ticks;
        return true;
    }
    return false;
}

bool raw_recording_ticks_for_frame(std::uint64_t frame, std::uint64_t& ticks) {
    if (g_state.index.frames.empty()) {
        return false;
    }

    const auto clamped = std::min<std::uint64_t>(frame, frame_count() - 1);
    const auto& record = g_state.index.frames[static_cast<std::size_t>(clamped)];
    if (record.has_recording_ticks) {
        ticks = record.recording_ticks;
        return true;
    }

    std::uint64_t start_ticks = 0;
    std::uint64_t end_ticks = 0;
    if (!raw_recording_start_ticks(start_ticks)) {
        return false;
    }
    if (raw_recording_end_ticks(end_ticks) && end_ticks <= start_ticks) {
        return false;
    }

    constexpr double ticks_per_second = 10'000'000.0;
    const auto elapsed_ticks = static_cast<std::uint64_t>(std::max(0.0, seconds_for_frame(clamped)) * ticks_per_second + 0.5);
    if (std::numeric_limits<std::uint64_t>::max() - start_ticks < elapsed_ticks) {
        return false;
    }
    ticks = start_ticks + elapsed_ticks;
    if (end_ticks != 0 && ticks > end_ticks) {
        ticks = end_ticks;
    }
    return true;
}

bool recording_start_ticks(std::uint64_t& ticks) {
    return recording_time_display_enabled() && raw_recording_start_ticks(ticks);
}

bool recording_end_ticks(std::uint64_t& ticks) {
    return recording_time_display_enabled() && raw_recording_end_ticks(ticks);
}

bool recording_ticks_for_frame(std::uint64_t frame, std::uint64_t& ticks) {
    return recording_time_display_enabled() && raw_recording_ticks_for_frame(frame, ticks);
}

bool display_parts_for_recording_ticks(std::uint64_t ticks, dat_player::RecordingDateTimeParts& parts) {
    const auto& metadata = g_state.index.summary.recording_metadata;
    const auto& sidecar = metadata.sidecar;
    std::uint64_t display_ticks = ticks;
    if (!archive_display_time_available() ||
        !dat_player::offset_dotnet_ticks(ticks, sidecar.display_offset_minutes, display_ticks)) {
        return false;
    }

    constexpr std::uint64_t half_second_ticks = 5'000'000ULL;
    if (display_ticks <= std::numeric_limits<std::uint64_t>::max() - half_second_ticks) {
        display_ticks += half_second_ticks;
    }
    display_ticks -= display_ticks % 10'000'000ULL;
    return dat_player::dotnet_ticks_to_parts(display_ticks, parts);
}

std::wstring format_recording_date(const dat_player::RecordingDateTimeParts& parts) {
    std::wostringstream text;
    text << parts.month << L"/" << parts.day << L"/" << parts.year;
    return text.str();
}

std::wstring format_recording_time(const dat_player::RecordingDateTimeParts& parts) {
    int hour = parts.hour % 12;
    if (hour == 0) {
        hour = 12;
    }
    std::wostringstream text;
    text << hour << L":" << std::setfill(L'0') << std::setw(2) << parts.minute
         << L":" << std::setw(2) << parts.second << L" "
         << (parts.hour >= 12 ? L"PM" : L"AM");
    return text.str();
}

std::wstring format_recording_timeline_label(std::uint64_t ticks) {
    dat_player::RecordingDateTimeParts parts;
    if (!display_parts_for_recording_ticks(ticks, parts)) {
        return L"";
    }
    return format_recording_date(parts) + L"\r\n" + format_recording_time(parts);
}

std::wstring format_recording_thumb_label(std::uint64_t frame) {
    std::uint64_t ticks = 0;
    dat_player::RecordingDateTimeParts parts;
    if (!recording_ticks_for_frame(frame, ticks) || !display_parts_for_recording_ticks(ticks, parts)) {
        return L"";
    }
    return format_recording_time(parts);
}

std::wstring format_recording_datetime(std::uint64_t ticks) {
    dat_player::RecordingDateTimeParts parts;
    if (!display_parts_for_recording_ticks(ticks, parts)) {
        return L"";
    }
    return format_recording_date(parts) + L" " + format_recording_time(parts);
}

std::wstring metadata_source_label(const dat_player::RecordingMetadata& metadata) {
    if (metadata.source == "DAT frame ticks + .sef2 data") {
        return L"SEF2 + DAT";
    }
    if (metadata.source == "DAT frame ticks") {
        return L"DAT only";
    }
    if (metadata.source == ".sef2 data") {
        return L"SEF2 only";
    }
    if (metadata.source == "legacy DAT timing" ||
        metadata.confidence == dat_player::RecordingMetadataConfidence::Low) {
        return L"fallback elapsed only";
    }
    return L"none";
}

std::wstring integrity_cache_key(const dat_player::RecordingSidecarMetadata& sidecar) {
    if (!sidecar.available || sidecar.path.empty()) {
        return L"";
    }

    std::error_code ec;
    const auto file_size = std::filesystem::file_size(sidecar.path, ec);
    if (ec) {
        return sidecar.path.wstring();
    }
    const auto write_time = std::filesystem::last_write_time(sidecar.path, ec);
    const auto write_ticks = ec ? 0 : write_time.time_since_epoch().count();
    std::wostringstream key;
    key << sidecar.path.wstring() << L"|" << file_size << L"|" << write_ticks;
    return key.str();
}

std::wstring sef2_signature_label(const dat_player::RecordingSidecarMetadata& sidecar) {
    return widen(dat_player::to_string(sidecar.signature_status));
}

std::wstring video_authenticity_label(const dat_player::RecordingSidecarMetadata& sidecar) {
    return sidecar.available ? L"Not fully verified" : L"Not checked";
}

std::wstring format_offset_minutes(int minutes) {
    const int magnitude = std::abs(minutes);
    std::wostringstream text;
    text << L"UTC" << (minutes >= 0 ? L"+" : L"-")
         << std::setfill(L'0') << std::setw(2) << (magnitude / 60)
         << L":" << std::setw(2) << (magnitude % 60);
    return text.str();
}

bool set_window_text_if_changed(HWND window, const std::wstring& text) {
    if (!window) {
        return false;
    }

    const int length = GetWindowTextLengthW(window);
    std::wstring current(static_cast<std::size_t>(std::max(0, length)) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(window, current.data(), length + 1);
    }
    current.resize(static_cast<std::size_t>(std::max(0, length)));
    if (current == text) {
        return false;
    }

    SetWindowTextW(window, text.c_str());
    return true;
}

void set_status(const std::wstring& text) {
    if (g_state.status_label) {
        set_window_text_if_changed(g_state.status_label, text);
    }
}

void move_window_if_changed(HWND window, int x, int y, int width, int height, BOOL repaint) {
    if (!window) {
        return;
    }

    RECT current = {};
    GetWindowRect(window, &current);
    POINT top_left = {current.left, current.top};
    MapWindowPoints(nullptr, GetParent(window), &top_left, 1);
    const int current_width = current.right - current.left;
    const int current_height = current.bottom - current.top;
    if (top_left.x == x && top_left.y == y && current_width == width && current_height == height) {
        return;
    }

    MoveWindow(window, x, y, width, height, repaint);
}

RECT padded_rect(RECT rect, int padding) {
    rect.left -= padding;
    rect.top -= padding;
    rect.right += padding;
    rect.bottom += padding;
    return rect;
}

void update_play_button() {
    if (g_state.play_button) {
        const bool pause_would_cancel_scrub_resume =
            (g_state.timeline_dragging && g_state.resume_after_timeline_drag) ||
            (g_state.seeking && g_state.pending_seek_resume_after_completion);
        SetWindowTextW(g_state.play_button, (g_state.playing || pause_would_cancel_scrub_resume) ? L"Pause" : L"Play");
    }
}

void update_details_toggle_button() {
    if (g_state.details_toggle_button) {
        SetWindowTextW(g_state.details_toggle_button, g_state.details_visible ? L"Hide Details" : L"Show Details");
    }
}

int current_integrity_dot_state() {
    if (g_state.loaded_path.empty()) {
        return 0;
    }
    return g_state.index.summary.recording_metadata.sidecar.available ? 1 : 2;
}

void update_integrity_dot(bool force = false) {
    if (!g_state.integrity_dot) {
        return;
    }

    const int state = current_integrity_dot_state();
    if (!force && state == g_state.integrity_dot_state) {
        return;
    }
    g_state.integrity_dot_state = state;
    InvalidateRect(g_state.integrity_dot, nullptr, TRUE);
}

void order_details_controls() {
    if (!g_state.details_visible) {
        return;
    }

    SetWindowPos(g_state.info_label, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(g_state.decode_test_button, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(g_state.render_first_frame_button, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void update_speed_button() {
    if (g_state.speed_button) {
        const auto text = next_speed_button_text();
        SetWindowTextW(g_state.speed_button, text.c_str());
    }
}

void update_actual_size_button() {
    if (g_state.actual_size_button) {
        SetWindowTextW(g_state.actual_size_button, g_state.actual_size_applied ? L"Default Size" : L"Actual Size");
    }
}

void start_integrity_verification_if_needed() {
    if (!g_state.hwnd) {
        return;
    }

    auto& sidecar = g_state.index.summary.recording_metadata.sidecar;
    if (!sidecar.available || sidecar.path.empty() ||
        sidecar.signature_status != dat_player::Sef2SignatureStatus::Pending) {
        return;
    }

    const auto cache_key = integrity_cache_key(sidecar);
    if (!cache_key.empty()) {
        const auto cached = g_state.integrity_cache.find(cache_key);
        if (cached != g_state.integrity_cache.end()) {
            sidecar = cached->second;
            update_info(true);
            return;
        }
    }

    const auto generation = ++g_state.integrity_generation;
    const HWND hwnd = g_state.hwnd;
    auto sidecar_copy = sidecar;
    std::thread([hwnd, generation, cache_key, sidecar_copy = std::move(sidecar_copy)]() mutable {
        dat_player::verify_sef2_signature(sidecar_copy);
        auto* message = new IntegrityFinishedMessage;
        message->generation = generation;
        message->cache_key = cache_key;
        message->sidecar = std::move(sidecar_copy);
        if (!PostMessageW(hwnd, kIntegrityFinishedMessage, 0, reinterpret_cast<LPARAM>(message))) {
            delete message;
        }
    }).detach();
}

void refresh_after_resize() {
    if (g_state.details_visible) {
        RedrawWindow(
            g_state.details_group,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
        RedrawWindow(
            g_state.info_label,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
        RedrawWindow(
            g_state.decode_test_button,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
        RedrawWindow(
            g_state.render_first_frame_button,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
    }
    if (g_state.video_panel) {
        RedrawWindow(
            g_state.video_panel,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
    }
    if (g_state.hwnd) {
        RedrawWindow(
            g_state.hwnd,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}

void arm_resize_refresh() {
    if (g_state.hwnd) {
        SetTimer(
            g_state.hwnd,
            kResizeRefreshTimerId,
            static_cast<UINT>(kResizeRefreshDelay.count()),
            nullptr);
    }
}

void stop_playback() {
    g_state.stop_playback_requested = true;
    ++g_state.playback_generation;
    if (g_state.playback_thread.joinable() &&
        g_state.playback_thread.get_id() != std::this_thread::get_id()) {
        g_state.playback_thread.join();
    }

    if (g_state.playing) {
        g_state.playing = false;
        update_play_button();
    }
    g_state.seeking = false;
    g_state.pending_seek_resume_after_completion = false;
    g_state.pending_seek_resume_generation = 0;
    update_play_button();
}

void stop_preview(bool join_worker = true) {
    g_state.stop_preview_requested = true;
    ++g_state.preview_generation;
    if (g_state.hwnd) {
        KillTimer(g_state.hwnd, kTimelinePreviewTimerId);
    }
    g_state.preview_timer_armed = false;

    if (join_worker && g_state.preview_thread.joinable() &&
        g_state.preview_thread.get_id() != std::this_thread::get_id()) {
        g_state.preview_thread.join();
    }

    g_state.preview_in_flight = false;
}

void update_thumb_time_label(std::uint64_t display_frame) {
    if (!g_state.hwnd || !g_state.thumb_time_label || !g_state.timeline) {
        return;
    }

    RECT timeline_rect = {};
    GetWindowRect(g_state.timeline, &timeline_rect);
    POINT timeline_origin = {timeline_rect.left, timeline_rect.top};
    MapWindowPoints(nullptr, g_state.hwnd, &timeline_origin, 1);

    constexpr int elapsed_label_width = 54;
    constexpr int recording_label_width = 86;
    const auto recording_text = format_recording_thumb_label(display_frame);
    const bool show_recording_time = !recording_text.empty();
    const int label_width = show_recording_time ? recording_label_width : elapsed_label_width;
    constexpr int label_height = 18;
    const int timeline_width = std::max<int>(1, timeline_rect.right - timeline_rect.left);
    const int timeline_height = std::max<int>(1, timeline_rect.bottom - timeline_rect.top);
    const int timeline_position = timeline_position_from_frame(display_frame);
    const int thumb_center = timeline_origin.x + static_cast<int>(
        (static_cast<double>(timeline_position) / static_cast<double>(kTrackbarMax)) * timeline_width + 0.5);

    RECT client = {};
    GetClientRect(g_state.hwnd, &client);
    constexpr int padding = 14;
    const int min_x = padding;
    const int max_x = std::max<int>(min_x, client.right - padding - label_width);
    const int x = std::clamp(thumb_center - label_width / 2, min_x, max_x);
    const int y = timeline_origin.y + timeline_height - 2;
    const RECT new_rect = {x, y, x + label_width, y + label_height};
    const auto new_text = show_recording_time ? recording_text : format_clock_time(seconds_for_frame(display_frame));

    const bool moved =
        !g_state.has_thumb_time_label_rect ||
        g_state.thumb_time_label_rect.left != new_rect.left ||
        g_state.thumb_time_label_rect.top != new_rect.top ||
        g_state.thumb_time_label_rect.right != new_rect.right ||
        g_state.thumb_time_label_rect.bottom != new_rect.bottom;
    const bool text_changed = g_state.displayed_thumb_time_text != new_text;

    if (!moved && !text_changed) {
        return;
    }

    constexpr int repaint_padding = 4;
    const bool had_old_rect = g_state.has_thumb_time_label_rect;
    const RECT old_rect = g_state.thumb_time_label_rect;

    MoveWindow(g_state.thumb_time_label, new_rect.left, new_rect.top, label_width, label_height, TRUE);
    set_window_text_if_changed(g_state.thumb_time_label, new_text);

    if (had_old_rect) {
        const RECT old_padded = padded_rect(old_rect, repaint_padding);
        RedrawWindow(g_state.hwnd, &old_padded, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    }
    const RECT new_padded = padded_rect(new_rect, repaint_padding);
    InvalidateRect(g_state.hwnd, &new_padded, TRUE);
    InvalidateRect(g_state.thumb_time_label, nullptr, TRUE);

    g_state.thumb_time_label_rect = new_rect;
    g_state.has_thumb_time_label_rect = true;
    g_state.displayed_thumb_time_text = new_text;
}

void update_timeline() {
    const auto display_frame = g_state.has_timeline_preview
        ? g_state.timeline_preview_frame
        : (g_state.has_pending_seek ? g_state.pending_seek_frame : g_state.current_frame);
    if (g_state.timeline) {
        SendMessageW(g_state.timeline, TBM_SETRANGE, TRUE, MAKELPARAM(0, kTrackbarMax));
        SendMessageW(g_state.timeline, TBM_SETPOS, TRUE, timeline_position_from_frame(display_frame));
        EnableWindow(g_state.timeline, frame_count() > 0);
    }
    if (g_state.current_time_label) {
        std::uint64_t start_ticks = 0;
        const auto text = recording_start_ticks(start_ticks)
            ? format_recording_timeline_label(start_ticks)
            : std::wstring(L"00:00");
        set_window_text_if_changed(g_state.current_time_label, text);
    }
    if (g_state.total_time_label) {
        std::uint64_t end_ticks = 0;
        const auto text = recording_end_ticks(end_ticks)
            ? format_recording_timeline_label(end_ticks)
            : format_clock_time(total_duration_seconds());
        set_window_text_if_changed(g_state.total_time_label, text);
    }
    update_thumb_time_label(display_frame);
}

void update_file_path_text() {
    if (!g_state.file_path_edit) {
        return;
    }
    std::wstring text = g_state.loaded_path.empty()
        ? L"No DAT file loaded"
        : g_state.loaded_path.filename().wstring();
    set_window_text_if_changed(g_state.file_path_edit, text);
}

void arm_preview_timer(HWND hwnd, std::chrono::milliseconds delay) {
    if (!hwnd) {
        return;
    }
    const auto ms = static_cast<UINT>(std::max<std::chrono::milliseconds::rep>(1, delay.count()));
    SetTimer(hwnd, kTimelinePreviewTimerId, ms, nullptr);
    g_state.preview_timer_armed = true;
}

RECT fitted_rect(const RECT& bounds, std::uint32_t source_width, std::uint32_t source_height) {
    RECT result = bounds;
    const int bounds_width = std::max<int>(1, static_cast<int>(bounds.right - bounds.left));
    const int bounds_height = std::max<int>(1, static_cast<int>(bounds.bottom - bounds.top));
    if (source_width == 0 || source_height == 0) {
        return result;
    }

    const double source_aspect = static_cast<double>(source_width) / static_cast<double>(source_height);
    int draw_width = bounds_width;
    int draw_height = static_cast<int>(draw_width / source_aspect);
    if (draw_height > bounds_height) {
        draw_height = bounds_height;
        draw_width = static_cast<int>(draw_height * source_aspect);
    }

    result.left = bounds.left + (bounds_width - draw_width) / 2;
    result.top = bounds.top + (bounds_height - draw_height) / 2;
    result.right = result.left + draw_width;
    result.bottom = result.top + draw_height;
    return result;
}

COLORREF integrity_dot_color() {
    const int state = current_integrity_dot_state();
    if (state == 0) {
        return RGB(150, 150, 150);
    }
    return state == 1 ? RGB(45, 150, 72) : RGB(190, 55, 55);
}

LRESULT CALLBACK integrity_dot_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rect = {};
        GetClientRect(hwnd, &rect);

        FillRect(hdc, &rect, g_state.window_background_brush);
        const int dot_width = rect.right - rect.left;
        const int dot_height = rect.bottom - rect.top;
        const int diameter = std::max(6, std::min(dot_width, dot_height) - 6);
        const int left = (dot_width - diameter) / 2;
        const int top = (dot_height - diameter) / 2;
        const COLORREF color = integrity_dot_color();
        HBRUSH dot_brush = CreateSolidBrush(color);
        HGDIOBJ old_brush = SelectObject(hdc, dot_brush);
        HGDIOBJ old_pen = SelectObject(hdc, GetStockObject(NULL_PEN));
        Ellipse(hdc, left, top, left + diameter, top + diameter);
        SelectObject(hdc, old_pen);
        SelectObject(hdc, old_brush);
        DeleteObject(dot_brush);
        EndPaint(hwnd, &ps);
        return 0;
    }

    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

void layout_details_panel() {
    if (!g_state.details_group) {
        return;
    }

    RECT rect = {};
    GetClientRect(g_state.details_group, &rect);
    constexpr int padding = 12;
    constexpr int title_height = 24;
    constexpr int button_height = 32;
    constexpr int button_width = 104;
    constexpr int button_gap = 8;
    const int width = std::max<int>(1, rect.right - rect.left);
    const int height = std::max<int>(1, rect.bottom - rect.top);
    const int inner_left = padding;
    const int inner_width = std::max(1, width - padding * 2);
    const int buttons_top = std::max(title_height + padding, height - padding - button_height);
    const int edit_top = title_height + padding;
    const int edit_height = std::max(60, buttons_top - edit_top - 10);

    MoveWindow(g_state.info_label, inner_left, edit_top, inner_width, edit_height, TRUE);
    MoveWindow(g_state.decode_test_button, inner_left, buttons_top, button_width, button_height, TRUE);
    MoveWindow(
        g_state.render_first_frame_button,
        inner_left + button_width + button_gap,
        buttons_top,
        button_width,
        button_height,
        TRUE);
}

LRESULT CALLBACK details_panel_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_SIZE:
        layout_details_panel();
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;

    case WM_COMMAND:
        if (g_state.hwnd) {
            return SendMessageW(g_state.hwnd, message, wparam, lparam);
        }
        return 0;

    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wparam);
        SetBkColor(hdc, RGB(255, 255, 255));
        SetTextColor(hdc, RGB(35, 43, 51));
        return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wparam);
        SetTextColor(hdc, RGB(35, 43, 51));
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(g_state.window_background_brush);
    }

    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wparam);
        RECT rect = {};
        GetClientRect(hwnd, &rect);
        FillRect(hdc, &rect, g_state.window_background_brush);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rect = {};
        GetClientRect(hwnd, &rect);
        FillRect(hdc, &rect, g_state.window_background_brush);

        RECT border = rect;
        border.left += 1;
        border.top += 10;
        border.right -= 1;
        border.bottom -= 1;
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(160, 160, 160));
        HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, pen));
        HBRUSH old_brush = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
        Rectangle(hdc, border.left, border.top, border.right, border.bottom);
        SelectObject(hdc, old_brush);
        SelectObject(hdc, old_pen);
        DeleteObject(pen);

        RECT title_rect = {12, 0, rect.right - 12, 22};
        SetBkColor(hdc, RGB(240, 240, 240));
        SetTextColor(hdc, RGB(35, 43, 51));
        SetBkMode(hdc, OPAQUE);
        HFONT old_font = static_cast<HFONT>(SelectObject(hdc, g_state.ui_font));
        DrawTextW(hdc, L"Details / Diagnostics", -1, &title_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, old_font);
        EndPaint(hwnd, &ps);
        return 0;
    }

    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

LRESULT CALLBACK video_panel_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_DROPFILES:
        handle_drop_files(g_state.hwnd ? g_state.hwnd : hwnd, reinterpret_cast<HDROP>(wparam));
        return 0;

    case WM_PAINT: {
        const auto paint_started = std::chrono::steady_clock::now();
        PAINTSTRUCT ps = {};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rect = {};
        GetClientRect(hwnd, &rect);

        const int width = std::max<int>(1, rect.right - rect.left);
        const int height = std::max<int>(1, rect.bottom - rect.top);
        HDC memory_dc = CreateCompatibleDC(hdc);
        HBITMAP memory_bitmap = CreateCompatibleBitmap(hdc, width, height);
        HGDIOBJ old_bitmap = SelectObject(memory_dc, memory_bitmap);

        HBRUSH background = CreateSolidBrush(RGB(18, 18, 18));
        FillRect(memory_dc, &rect, background);
        DeleteObject(background);

        const auto& frame = g_state.rendered_frame;
        if (!frame.pixels.empty() && frame.display_width > 0 && frame.display_height > 0) {
            const RECT draw_rect = fitted_rect(rect, frame.display_width, frame.display_height);
            BITMAPINFO bitmap_info = {};
            bitmap_info.bmiHeader.biSize = sizeof(bitmap_info.bmiHeader);
            bitmap_info.bmiHeader.biWidth = static_cast<LONG>(frame.display_width);
            bitmap_info.bmiHeader.biHeight = -static_cast<LONG>(frame.display_height);
            bitmap_info.bmiHeader.biPlanes = 1;
            bitmap_info.bmiHeader.biBitCount = 32;
            bitmap_info.bmiHeader.biCompression = BI_RGB;

            SetStretchBltMode(memory_dc, HALFTONE);
            SetBrushOrgEx(memory_dc, 0, 0, nullptr);
            StretchDIBits(
                memory_dc,
                draw_rect.left,
                draw_rect.top,
                draw_rect.right - draw_rect.left,
                draw_rect.bottom - draw_rect.top,
                0,
                0,
                static_cast<int>(frame.display_width),
                static_cast<int>(frame.display_height),
                frame.pixels.data(),
                &bitmap_info,
                DIB_RGB_COLORS,
                SRCCOPY);
        } else {
            SetBkMode(memory_dc, TRANSPARENT);
            SetTextColor(memory_dc, RGB(210, 210, 210));
            DrawTextW(memory_dc, L"No rendered frame", -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        BitBlt(hdc, 0, 0, width, height, memory_dc, 0, 0, SRCCOPY);
        const auto paint_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - paint_started).count();
        g_state.last_paint_ms = paint_ms;
        g_state.average_paint_ms = g_state.average_paint_ms <= 0.0
            ? paint_ms
            : (g_state.average_paint_ms * 0.9 + paint_ms * 0.1);
        g_state.max_paint_ms = std::max(g_state.max_paint_ms, paint_ms);
        SelectObject(memory_dc, old_bitmap);
        DeleteObject(memory_bitmap);
        DeleteDC(memory_dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

std::wstring build_info_text() {
    if (g_state.index.frames.empty()) {
        return L"No DAT file loaded.\r\n\r\nOpen a compatible recording .dat file to index it.";
    }

    const auto& first = g_state.index.frames.front();
    const auto keys = keyframe_count();
    const auto total = frame_count();
    const auto interframes = total >= keys ? total - keys : 0;

    std::wostringstream text;
    text << L"File: " << g_state.loaded_path.filename().wstring() << L"\r\n"
         << L"Path: " << g_state.loaded_path.wstring() << L"\r\n"
         << L"Frames: " << total << L"\r\n"
         << L"First resolution: " << first.width << L" x " << first.height << L"\r\n"
         << L"Estimated duration: " << format_double(g_state.index.summary.duration_seconds, 2) << L" sec\r\n"
         << L"Estimated FPS: " << format_double(g_state.index.summary.estimated_fps, 2) << L"\r\n"
         << L"Speed: " << playback_speed_label() << L"\r\n"
         << L"Keyframes: " << keys << L"\r\n"
         << L"Interframes: " << interframes << L"\r\n"
         << L"Timeline frame: " << (g_state.current_frame + 1) << L" / " << total;
    const auto& metadata = g_state.index.summary.recording_metadata;
    const auto& sidecar = metadata.sidecar;
    text << L"\r\n\r\nIntegrity:\r\n"
         << L"Integrity metadata: " << (sidecar.available ? L"Present" : L"Not found") << L"\r\n"
         << L"SEF2 signature: " << sef2_signature_label(sidecar) << L"\r\n"
         << L"Video authenticity: " << video_authenticity_label(sidecar) << L"\r\n";
    if (sidecar.available) {
        if (!sidecar.path.empty()) {
            text << L"Metadata path: " << sidecar.path.wstring() << L"\r\n";
        }
        if (!sidecar.referenced_dat_filename.empty()) {
            text << L"Referenced DAT: " << widen(sidecar.referenced_dat_filename)
                 << (sidecar.sidecar_references_selected_dat ? L" (matches selected file)" : L" (does not match selected file)")
                 << L"\r\n";
            text << L"Referenced DAT exists: " << (sidecar.referenced_dat_exists ? L"Yes" : L"No") << L"\r\n";
        }
        if (!sidecar.channel_id.empty()) {
            text << L"Channel/camera id: " << widen(sidecar.channel_id) << L"\r\n";
        }
        if (!sidecar.signature_method.empty()) {
            text << L"Signature method: " << widen(sidecar.signature_method) << L"\r\n";
        }
        if (!sidecar.digest_method.empty()) {
            text << L"Digest method: " << widen(sidecar.digest_method) << L"\r\n";
        }
        if (!sidecar.public_key_source.empty()) {
            text << L"Public key source: " << widen(sidecar.public_key_source) << L"\r\n";
        }
        if (!sidecar.verification_error.empty()) {
            text << L"Verification detail: " << widen(sidecar.verification_error) << L"\r\n";
        }
        if (sidecar.has_data_size_bytes) {
            text << L"DAT size check: " << (sidecar.dat_size_plausible ? L"Plausible" : L"Warning") << L"\r\n";
        }
        for (const auto& warning : sidecar.integrity_warnings) {
            text << L"Warning: " << widen(warning) << L"\r\n";
        }
    }

    if (metadata.has_dat_frame_ticks || metadata.sidecar.available ||
        metadata.confidence == dat_player::RecordingMetadataConfidence::Low) {
        text << L"\r\n\r\nRecording data:\r\n";
        std::uint64_t start_ticks = 0;
        std::uint64_t end_ticks = 0;
        const bool has_start_ticks = raw_recording_start_ticks(start_ticks);
        const bool has_end_ticks = raw_recording_end_ticks(end_ticks);
        if (start_ticks != 0) {
            const auto display = format_recording_datetime(start_ticks);
            if (!display.empty()) {
                text << L"Recording start (archive display): " << display << L"\r\n";
            }
        }
        if (end_ticks != 0) {
            const auto display = format_recording_datetime(end_ticks);
            if (!display.empty()) {
                text << L"Recording end (archive display): " << display << L"\r\n";
            }
        }
        std::uint64_t current_ticks = 0;
        if (recording_ticks_for_frame(g_state.current_frame, current_ticks)) {
            const auto display = format_recording_datetime(current_ticks);
            if (!display.empty()) {
                text << L"Current frame recording time (archive display): " << display << L"\r\n";
            }
        }
        std::uint64_t raw_current_ticks = 0;
        if (raw_recording_ticks_for_frame(g_state.current_frame, raw_current_ticks)) {
            text << (metadata.has_dat_frame_ticks ? L"Current frame raw DAT time: " : L"Current frame raw data time: ")
                 << widen(dat_player::format_dotnet_ticks(raw_current_ticks)) << L"\r\n";
        }
        if (has_start_ticks) {
            text << L"Raw data start (unadjusted): " << widen(dat_player::format_dotnet_ticks(start_ticks)) << L"\r\n";
        }
        if (has_end_ticks) {
            text << L"Raw data end (unadjusted): " << widen(dat_player::format_dotnet_ticks(end_ticks)) << L"\r\n";
        }
        if (metadata.has_dat_frame_ticks) {
            text << L"DAT first-frame ticks: " << metadata.first_frame_ticks << L"\r\n";
            if (raw_current_ticks != 0) {
                text << L"DAT current-frame ticks: " << raw_current_ticks << L"\r\n";
            }
            text << L"DAT last-frame ticks: " << metadata.last_frame_ticks << L"\r\n";
        }
        text << L"Recording duration: " << format_double(total_duration_seconds(), 3) << L" sec\r\n";
        if (!metadata.sidecar.camera_name.empty()) {
            text << L"Camera: " << widen(metadata.sidecar.camera_name) << L"\r\n";
        }
        if (!metadata.sidecar.manufacturer.empty() || !metadata.sidecar.model.empty()) {
            text << L"Device: " << widen(metadata.sidecar.manufacturer);
            if (!metadata.sidecar.manufacturer.empty() && !metadata.sidecar.model.empty()) {
                text << L" ";
            }
            text << widen(metadata.sidecar.model) << L"\r\n";
        }
        text << L"Data source: " << metadata_source_label(metadata) << L"\r\n"
             << L"Raw data status: " << widen(dat_player::to_string(metadata.confidence)) << L"\r\n"
             << L"Display time status: "
             << (archive_display_time_available() ? widen(dat_player::to_string(metadata.confidence)) : L"None") << L"\r\n";
        if (archive_display_time_available()) {
            text << L"Display offset: " << format_offset_minutes(metadata.sidecar.display_offset_minutes) << L"\r\n";
        } else {
            text << L"Archive display: unavailable";
            if (!metadata.sidecar.available) {
                text << L" (timezone data not found)";
            } else if (!metadata.sidecar.has_display_offset_minutes) {
                text << L" (timezone/offset not found)";
            } else {
                text << L" (timezone data not trusted for DAT ticks)";
            }
            text << L"\r\n";
        }
        if (!metadata.sidecar.timezone_offset_minutes_candidates.empty()) {
            text << L"Timezone candidates: ";
            for (std::size_t i = 0; i < metadata.sidecar.timezone_offset_minutes_candidates.size(); ++i) {
                if (i > 0) {
                    text << L", ";
                }
                text << format_offset_minutes(metadata.sidecar.timezone_offset_minutes_candidates[i]);
            }
        } else {
            text << L"Timezone/offset info: not available";
        }
    }
    if (!g_state.decode_test_text.empty()) {
        text << L"\r\n\r\nDiagnostics:\r\n" << g_state.decode_test_text;
    }
    return text.str();
}

void update_info(bool force) {
    update_file_path_text();
    update_integrity_dot();
    update_timeline();
    if (!g_state.info_label || !g_state.details_visible) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!force && g_state.last_info_update_time.time_since_epoch().count() != 0 &&
        now - g_state.last_info_update_time < kDiagnosticsUpdateThrottle) {
        return;
    }

    const auto info = build_info_text();
    g_state.last_info_update_time = now;
    if (info == g_state.displayed_info_text) {
        return;
    }

    const auto first_visible_line = static_cast<int>(SendMessageW(g_state.info_label, EM_GETFIRSTVISIBLELINE, 0, 0));
    DWORD selection_start = 0;
    DWORD selection_end = 0;
    SendMessageW(
        g_state.info_label,
        EM_GETSEL,
        reinterpret_cast<WPARAM>(&selection_start),
        reinterpret_cast<LPARAM>(&selection_end));

    SendMessageW(g_state.info_label, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(g_state.info_label, info.c_str());
    SendMessageW(g_state.info_label, EM_SETSEL, selection_start, selection_end);
    const auto restored_first_line = static_cast<int>(SendMessageW(g_state.info_label, EM_GETFIRSTVISIBLELINE, 0, 0));
    if (first_visible_line != restored_first_line) {
        SendMessageW(g_state.info_label, EM_LINESCROLL, 0, first_visible_line - restored_first_line);
    }
    SendMessageW(g_state.info_label, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(
        g_state.info_label,
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
    g_state.displayed_info_text = info;
}

void set_enabled_after_load(bool enabled) {
    EnableWindow(g_state.play_button, enabled);
    EnableWindow(g_state.actual_size_button, enabled);
    EnableWindow(g_state.decode_test_button, enabled);
    EnableWindow(g_state.render_first_frame_button, enabled);
    EnableWindow(g_state.timeline, enabled);
    EnableWindow(g_state.current_time_label, enabled);
    EnableWindow(g_state.total_time_label, enabled);
    EnableWindow(g_state.thumb_time_label, enabled);
}

void reset_loaded_state() {
    stop_playback();
    stop_preview();
    ++g_state.integrity_generation;
    g_state.index = {};
    g_state.loaded_path.clear();
    g_state.decode_test_text.clear();
    g_state.rendered_frame = {};
    g_state.frames_rendered = 0;
    g_state.frames_decoded = 0;
    g_state.late_frames = 0;
    g_state.clock_reanchors = 0;
    g_state.effective_playback_fps = 0.0;
    g_state.target_frame_interval_ms = 0.0;
    g_state.frame_interval_ms = 0.0;
    g_state.average_scheduled_sleep_ms = 0.0;
    g_state.max_scheduled_sleep_ms = 0.0;
    g_state.recent_frame_interval_ms = 0.0;
    g_state.max_recent_frame_interval_ms = 0.0;
    g_state.average_ui_delay_ms = 0.0;
    g_state.max_ui_delay_ms = 0.0;
    g_state.average_convert_ms = 0.0;
    g_state.max_convert_ms = 0.0;
    g_state.last_paint_ms = 0.0;
    g_state.average_paint_ms = 0.0;
    g_state.max_paint_ms = 0.0;
    g_state.last_ui_frame_time = {};
    g_state.current_frame = 0;
    g_state.timeline_dragging = false;
    g_state.resume_after_timeline_drag = false;
    g_state.pending_seek_resume_after_completion = false;
    g_state.pending_seek_resume_generation = 0;
    g_state.force_native_video_size = false;
    g_state.requested_video_width = 0;
    g_state.requested_video_height = 0;
    g_state.has_timeline_preview = false;
    g_state.timeline_preview_frame = 0;
    g_state.has_pending_seek = false;
    g_state.pending_seek_frame = 0;
    g_state.seeking = false;
    g_state.preview_in_flight = false;
    g_state.preview_timer_armed = false;
    g_state.latest_preview_frame = 0;
    g_state.active_preview_frame = 0;
    g_state.preview_keyframe_frame = 0;
    g_state.preview_rendered_frame = 0;
    g_state.preview_frames_decoded = 0;
    g_state.preview_superseded = 0;
    g_state.displayed_info_text.clear();
    g_state.last_info_update_time = {};
    g_state.thumb_time_label_rect = {};
    g_state.has_thumb_time_label_rect = false;
    g_state.displayed_thumb_time_text.clear();
    g_state.actual_size_applied = false;
    update_actual_size_button();
    set_enabled_after_load(false);
    if (g_state.video_panel) {
        InvalidateRect(g_state.video_panel, nullptr, TRUE);
    }
    update_info(true);
}

std::filesystem::path ask_for_dat_file(HWND owner) {
    wchar_t file_name[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"DAT files (*.dat)\0*.dat\0";
    ofn.lpstrFile = file_name;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Open DAT File";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = L"dat";

    if (!GetOpenFileNameW(&ofn)) {
        return {};
    }

    return std::filesystem::path(file_name);
}

bool load_dat_path(HWND owner, const std::filesystem::path& path, bool dropped_file) {
    stop_playback();
    stop_preview();
    ++g_state.integrity_generation;
    if (path.empty()) {
        return false;
    }

    std::wstring validation_error;
    if (!is_existing_readable_dat_file(path, validation_error)) {
        reset_loaded_state();
        set_status(validation_error.empty() ? L"Unsupported drop: please drop one .dat file." : validation_error);
        if (!dropped_file) {
            MessageBoxW(owner, L"Only readable .dat files are supported.", L"Unsupported File", MB_OK | MB_ICONWARNING);
        }
        return false;
    }

    try {
        set_status(dropped_file ? L"Loading dropped file..." : L"Indexing DAT file...");
        dat_player::DatFrameIndexer indexer;
        auto index = indexer.index_file(path);

        g_state.index = std::move(index);
        g_state.loaded_path = path;
        g_state.decode_test_text.clear();
        g_state.rendered_frame = {};
        g_state.frames_rendered = 0;
        g_state.frames_decoded = 0;
        g_state.late_frames = 0;
        g_state.clock_reanchors = 0;
        g_state.effective_playback_fps = 0.0;
        g_state.target_frame_interval_ms = 0.0;
        g_state.frame_interval_ms = 0.0;
        g_state.average_scheduled_sleep_ms = 0.0;
        g_state.max_scheduled_sleep_ms = 0.0;
        g_state.recent_frame_interval_ms = 0.0;
        g_state.max_recent_frame_interval_ms = 0.0;
        g_state.average_ui_delay_ms = 0.0;
        g_state.max_ui_delay_ms = 0.0;
        g_state.average_convert_ms = 0.0;
        g_state.max_convert_ms = 0.0;
        g_state.last_paint_ms = 0.0;
        g_state.average_paint_ms = 0.0;
        g_state.max_paint_ms = 0.0;
        g_state.last_ui_frame_time = {};
        g_state.current_frame = 0;
        g_state.timeline_dragging = false;
        g_state.resume_after_timeline_drag = false;
        g_state.pending_seek_resume_after_completion = false;
        g_state.pending_seek_resume_generation = 0;
        g_state.has_timeline_preview = false;
        g_state.timeline_preview_frame = 0;
        g_state.has_pending_seek = false;
        g_state.pending_seek_frame = 0;
        g_state.seeking = false;
        g_state.preview_in_flight = false;
        g_state.preview_timer_armed = false;
        g_state.latest_preview_frame = 0;
        g_state.active_preview_frame = 0;
        g_state.preview_keyframe_frame = 0;
        g_state.preview_rendered_frame = 0;
        g_state.preview_frames_decoded = 0;
        g_state.preview_superseded = 0;
        g_state.displayed_info_text.clear();
        g_state.last_info_update_time = {};
        g_state.thumb_time_label_rect = {};
        g_state.has_thumb_time_label_rect = false;
        g_state.displayed_thumb_time_text.clear();
        g_state.actual_size_applied = false;
        update_actual_size_button();
        set_enabled_after_load(!g_state.index.frames.empty());
        if (g_state.video_panel) {
            InvalidateRect(g_state.video_panel, nullptr, TRUE);
        }
        update_info(true);
        start_integrity_verification_if_needed();

        if (g_state.index.frames.empty()) {
            reset_loaded_state();
            set_status(dropped_file
                ? L"Dropped .dat file could not be interpreted as compatible video."
                : L"No valid H264/I264 frame records were found.");
            return false;
        } else {
            set_status(L"Loaded index. Starting playback...");
            start_forward_playback();
            return true;
        }
    } catch (const std::exception& ex) {
        reset_loaded_state();
        const auto message = L"Unable to index DAT file:\r\n" + widen(ex.what());
        if (!dropped_file) {
            MessageBoxW(owner, message.c_str(), L"Index Error", MB_OK | MB_ICONERROR);
        }
        set_status(dropped_file
            ? L"Dropped .dat file could not be interpreted as compatible video."
            : L"Indexing failed.");
        return false;
    }
}

void load_dat_file(HWND owner) {
    const auto path = ask_for_dat_file(owner);
    if (path.empty()) {
        return;
    }
    load_dat_path(owner, path, false);
}

void handle_drop_files(HWND hwnd, HDROP drop) {
    const UINT file_count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (file_count != 1) {
        stop_playback();
        stop_preview();
        DragFinish(drop);
        reset_loaded_state();
        set_status(L"Unsupported drop: please drop one .dat file.");
        return;
    }

    const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
    std::wstring dropped_path(static_cast<std::size_t>(length) + 1, L'\0');
    DragQueryFileW(drop, 0, dropped_path.data(), length + 1);
    dropped_path.resize(length);
    DragFinish(drop);

    load_dat_path(hwnd, std::filesystem::path(dropped_path), true);
}

std::wstring format_decode_test_result(const dat_player::playback::DecodeTestResult& result) {
    std::wostringstream text;
    text << L"Media Foundation initialized: " << (result.media_foundation_initialized ? L"yes" : L"no") << L"\r\n"
         << L"H.264 decoder found: " << (result.decoder_found ? L"yes" : L"no") << L"\r\n"
         << L"Annex B start codes found: " << (result.inspection.saw_start_code ? L"yes" : L"no") << L"\r\n"
         << L"SPS/PPS found: " << ((result.inspection.saw_sps && result.inspection.saw_pps) ? L"yes" : L"no") << L"\r\n"
         << L"IDR found: " << (result.inspection.saw_idr ? L"yes" : L"no") << L"\r\n"
         << L"Payload format accepted for test: " << (result.payload_format_supported ? L"yes" : L"no") << L"\r\n"
         << L"Payloads inspected: " << result.inspection.payloads_inspected << L"\r\n"
         << L"Frames submitted: " << result.frames_submitted << L"\r\n"
         << L"Frames decoded: " << result.frames_decoded << L"\r\n";

    if (result.decoded_width > 0 && result.decoded_height > 0) {
        text << L"First decoded format: " << result.decoded_width << L" x " << result.decoded_height;
        if (!result.decoded_subtype.empty()) {
            text << L" " << result.decoded_subtype;
        }
        text << L"\r\n";
    } else {
        text << L"First decoded format: n/a\r\n";
    }

    text << L"Result: " << result.message;
    return text.str();
}

void run_decode_test() {
    if (g_state.loaded_path.empty() || g_state.index.frames.empty()) {
        MessageBoxW(g_state.hwnd, L"Open and index a .dat file before running the decode test.", L"Decode Test", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const bool was_playing = g_state.playing;
    stop_playback();
    stop_preview();
    set_status(L"Running Media Foundation decode test...");
    dat_player::playback::H264DecodeTester tester;
    const auto result = tester.run(g_state.loaded_path, g_state.index);
    g_state.decode_test_text = format_decode_test_result(result);
    update_info(true);
    set_status(result.decoded_any_frame
        ? (was_playing ? L"Playback stopped; decode test produced decoded samples." : L"Decode test produced decoded samples.")
        : L"Decode test did not produce decoded samples.");
}

std::wstring format_render_result(const dat_player::playback::FirstFrameRenderResult& result) {
    std::wostringstream text;
    text << L"First-frame render test\r\n"
         << L"Media Foundation initialized: " << (result.decode.media_foundation_initialized ? L"yes" : L"no") << L"\r\n"
         << L"H.264 decoder found: " << (result.decode.decoder_found ? L"yes" : L"no") << L"\r\n"
         << L"Frames submitted: " << result.decode.frames_submitted << L"\r\n"
         << L"Frames decoded: " << result.decode.frames_decoded << L"\r\n";

    if (!g_state.index.frames.empty()) {
        text << L"Indexed resolution: " << g_state.index.frames.front().width << L" x " << g_state.index.frames.front().height << L"\r\n";
    }

    if (result.decode.decoded_width > 0 && result.decode.decoded_height > 0) {
        text << L"Decoded output: " << result.decode.decoded_width << L" x " << result.decode.decoded_height;
        if (!result.decode.decoded_subtype.empty()) {
            text << L" " << result.decode.decoded_subtype;
        }
        text << L"\r\n";
    } else {
        text << L"Decoded output: n/a\r\n";
    }

    if (result.frame_available) {
        text << L"Rendered display: " << result.frame.display_width << L" x " << result.frame.display_height << L" BGRA\r\n";
    } else {
        text << L"Rendered display: n/a\r\n";
    }

    text << L"Result: " << result.message;
    return text.str();
}

void run_first_frame_render_test() {
    if (g_state.loaded_path.empty() || g_state.index.frames.empty()) {
        MessageBoxW(g_state.hwnd, L"Open and index a .dat file before rendering the first frame.", L"Render First Frame", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const bool was_playing = g_state.playing;
    stop_playback();
    stop_preview();
    set_status(L"Rendering first decoded frame...");
    dat_player::playback::H264DecodeTester tester;
    const auto result = tester.render_first_frame(g_state.loaded_path, g_state.index);
    g_state.rendered_frame = result.frame;
    g_state.decode_test_text = format_render_result(result);
    if (g_state.video_panel) {
        InvalidateRect(g_state.video_panel, nullptr, TRUE);
    }
    update_info(true);
    set_status(result.frame_available
        ? (was_playing ? L"Playback stopped; first decoded frame rendered." : L"First decoded frame rendered.")
        : L"First-frame render test failed.");
}

std::wstring format_playback_diagnostics(const std::wstring& state) {
    std::wostringstream text;
    text << L"Forward playback\r\n"
         << L"State: " << state << L"\r\n"
         << L"Current frame: " << (g_state.current_frame + 1) << L" / " << frame_count() << L"\r\n"
         << L"Current time: " << format_double(seconds_for_frame(g_state.current_frame), 2)
         << L" / " << format_double(total_duration_seconds(), 2) << L" sec\r\n"
         << L"Frames decoded: " << g_state.frames_decoded << L"\r\n"
         << L"Frames rendered: " << g_state.frames_rendered << L"\r\n"
         << L"Late frames: " << g_state.late_frames << L"\r\n"
         << L"Clock re-anchors: " << g_state.clock_reanchors << L"\r\n"
         << L"Speed: " << playback_speed_label() << L"\r\n"
         << L"Target FPS: " << format_double(playback_fps() * playback_speed_multiplier(), 2) << L"\r\n"
         << L"Actual FPS: " << format_double(g_state.effective_playback_fps, 2) << L"\r\n"
         << L"Target frame interval: " << format_double(g_state.target_frame_interval_ms, 2) << L" ms\r\n"
         << L"Worker interval: " << format_double(g_state.frame_interval_ms, 2) << L" ms\r\n"
         << L"Scheduled sleep avg/max: " << format_nonnegative_double(g_state.average_scheduled_sleep_ms, 2)
         << L" / " << format_nonnegative_double(g_state.max_scheduled_sleep_ms, 2) << L" ms\r\n"
         << L"UI interval avg/max: " << format_double(g_state.recent_frame_interval_ms, 2)
         << L" / " << format_double(g_state.max_recent_frame_interval_ms, 2) << L" ms\r\n"
         << L"UI post delay avg/max: " << format_double(g_state.average_ui_delay_ms, 2)
         << L" / " << format_double(g_state.max_ui_delay_ms, 2) << L" ms\r\n"
         << L"NV12->BGRA avg/max: " << format_double(g_state.average_convert_ms, 2)
         << L" / " << format_double(g_state.max_convert_ms, 2) << L" ms\r\n"
         << L"Paint avg/max: " << format_double(g_state.average_paint_ms, 2)
         << L" / " << format_double(g_state.max_paint_ms, 2) << L" ms\r\n";
    if (!g_state.index.frames.empty() && g_state.current_frame < frame_count()) {
        const auto& frame = g_state.index.frames[static_cast<std::size_t>(g_state.current_frame)];
        const auto timestamp = frame.timestamp;
        const double seconds = seconds_for_frame(g_state.current_frame);
        text << L"Current timestamp: " << timestamp << L"\r\n"
             << L"Approx time: " << format_double(seconds, 2) << L" sec\r\n";
        if (frame.has_recording_ticks) {
            text << L"Current recording time: " << widen(dat_player::format_dotnet_ticks(frame.recording_ticks)) << L"\r\n";
        }
    }
    text << L"Timing: stable cadence from estimated FPS; DAT timestamps used for time display";
    return text.str();
}

std::wstring format_seek_diagnostics(
    const std::wstring& state,
    std::uint64_t requested_frame,
    std::uint64_t keyframe_frame,
    std::uint64_t frames_decoded,
    const std::wstring& result_text = L"",
    bool resume_after_seek = false) {
    std::wostringstream text;
    text << L"Scrub seek\r\n"
         << L"State: " << state << L"\r\n"
         << L"Requested: " << frame_time_label(requested_frame) << L"\r\n"
         << L"Keyframe used: " << (keyframe_frame + 1) << L"\r\n"
         << L"Frames decoded during seek: " << frames_decoded << L"\r\n"
         << L"Resume after seek: " << (resume_after_seek ? L"yes" : L"no") << L"\r\n"
         << L"Committed current frame: " << (g_state.current_frame + 1) << L" / " << frame_count() << L"\r\n";
    if (!result_text.empty()) {
        text << L"Result: " << result_text;
    }
    return text.str();
}

std::wstring format_preview_diagnostics(const std::wstring& state, std::uint64_t target_frame) {
    std::wostringstream text;
    text << L"Live scrub preview\r\n"
         << L"State: " << state << L"\r\n"
         << L"Preview target: " << frame_time_label(target_frame) << L"\r\n"
         << L"Preview keyframe: " << (g_state.preview_keyframe_frame + 1) << L"\r\n"
         << L"Preview rendered frame: " << (g_state.preview_rendered_frame + 1) << L" / " << frame_count() << L"\r\n"
         << L"Preview frames decoded: " << g_state.preview_frames_decoded << L"\r\n"
         << L"Preview requests superseded: " << g_state.preview_superseded << L"\r\n"
         << L"Committed current frame: " << (g_state.current_frame + 1) << L" / " << frame_count();
    return text.str();
}

void start_seek_to_frame(std::uint64_t target_frame, bool resume_after_seek) {
    if (g_state.loaded_path.empty() || g_state.index.frames.empty()) {
        return;
    }

    stop_playback();
    const auto clamped_target = std::min<std::uint64_t>(target_frame, frame_count() - 1);
    const auto keyframe = nearest_previous_keyframe(clamped_target);
    const std::uint64_t generation = ++g_state.playback_generation;
    g_state.stop_playback_requested = false;
    g_state.playing = false;
    g_state.seeking = true;
    g_state.has_timeline_preview = false;
    g_state.timeline_preview_frame = 0;
    g_state.has_pending_seek = true;
    g_state.pending_seek_frame = clamped_target;
    g_state.pending_seek_resume_after_completion = resume_after_seek;
    g_state.pending_seek_resume_generation = generation;
    update_play_button();
    g_state.decode_test_text = format_seek_diagnostics(L"seeking", clamped_target, keyframe, 0, L"", resume_after_seek);
    update_info(true);
    const auto seek_status = L"Seeking to " + frame_time_label(clamped_target) + L"...";
    set_status(seek_status);

    const HWND hwnd = g_state.hwnd;
    const auto path = g_state.loaded_path;
    const auto index = g_state.index;

    g_state.playback_thread = std::thread([hwnd, path, index, clamped_target, keyframe, generation, resume_after_seek]() {
        dat_player::playback::H264DecodeTester tester;
        dat_player::playback::ForwardPlaybackOptions options;
        options.start_frame = clamped_target;
        options.fallback_fps = 30.0;

        std::uint64_t decoded_at_target = 0;
        const auto result = tester.play_forward(path, index, options, [&](dat_player::playback::ForwardPlaybackFrame&& frame) {
            if (g_state.stop_playback_requested || g_state.playback_generation.load() != generation) {
                return false;
            }

            decoded_at_target = frame.frames_decoded;
            auto ui_frame = std::make_unique<UiPlaybackFrame>();
            ui_frame->frame = std::move(frame.frame);
            ui_frame->generation = generation;
            ui_frame->frame_index = frame.frame_index;
            ui_frame->timestamp = frame.timestamp;
            ui_frame->frames_submitted = frame.frames_submitted;
            ui_frame->frames_decoded = frame.frames_decoded;
            ui_frame->seek_frame = true;
            if (!PostMessageW(hwnd, kPlaybackFrameMessage, 0, reinterpret_cast<LPARAM>(ui_frame.release()))) {
                return false;
            }
            return false;
        });

        auto message = std::make_unique<SeekFinishedMessage>();
        message->generation = generation;
        message->decoded_any_frame = result.decoded_any_frame;
        message->resume_after_seek = resume_after_seek;
        message->requested_frame = clamped_target;
        message->keyframe_frame = keyframe;
        message->frames_decoded = decoded_at_target != 0 ? decoded_at_target : result.frames_decoded;
        message->text = result.message;
        if (!PostMessageW(hwnd, kSeekFinishedMessage, 0, reinterpret_cast<LPARAM>(message.get()))) {
            return;
        }
        message.release();
    });
}

void start_preview_to_frame(std::uint64_t target_frame) {
    if (g_state.loaded_path.empty() || g_state.index.frames.empty() || !g_state.timeline_dragging) {
        return;
    }
    if (g_state.preview_in_flight || g_state.seeking || g_state.playing) {
        ++g_state.preview_superseded;
        return;
    }

    if (g_state.preview_thread.joinable()) {
        g_state.preview_thread.join();
    }

    const auto clamped_target = std::min<std::uint64_t>(target_frame, frame_count() - 1);
    const auto keyframe = nearest_previous_keyframe(clamped_target);
    const std::uint64_t generation = ++g_state.preview_generation;
    g_state.stop_preview_requested = false;
    g_state.preview_in_flight = true;
    g_state.active_preview_frame = clamped_target;
    g_state.preview_keyframe_frame = keyframe;
    g_state.last_preview_started = std::chrono::steady_clock::now();
    g_state.decode_test_text = format_preview_diagnostics(L"decoding", clamped_target);
    update_info();

    const HWND hwnd = g_state.hwnd;
    const auto path = g_state.loaded_path;
    const auto index = g_state.index;

    g_state.preview_thread = std::thread([hwnd, path, index, clamped_target, keyframe, generation]() {
        dat_player::playback::H264DecodeTester tester;
        dat_player::playback::ForwardPlaybackOptions options;
        options.start_frame = clamped_target;
        options.fallback_fps = 30.0;

        std::uint64_t decoded_at_target = 0;
        std::uint64_t rendered_frame = clamped_target;
        const auto result = tester.play_forward(path, index, options, [&](dat_player::playback::ForwardPlaybackFrame&& frame) {
            if (g_state.stop_preview_requested || g_state.preview_generation.load() != generation) {
                return false;
            }

            decoded_at_target = frame.frames_decoded;
            rendered_frame = frame.frame_index;
            auto ui_frame = std::make_unique<UiPlaybackFrame>();
            ui_frame->frame = std::move(frame.frame);
            ui_frame->generation = generation;
            ui_frame->frame_index = frame.frame_index;
            ui_frame->timestamp = frame.timestamp;
            ui_frame->frames_submitted = frame.frames_submitted;
            ui_frame->frames_decoded = frame.frames_decoded;
            if (!PostMessageW(hwnd, kPreviewFrameMessage, 0, reinterpret_cast<LPARAM>(ui_frame.release()))) {
                return false;
            }
            return false;
        });

        auto message = std::make_unique<PreviewFinishedMessage>();
        message->generation = generation;
        message->target_frame = clamped_target;
        message->keyframe_frame = keyframe;
        message->rendered_frame = rendered_frame;
        message->frames_decoded = decoded_at_target != 0 ? decoded_at_target : result.frames_decoded;
        message->text = result.message;
        if (!PostMessageW(hwnd, kPreviewFinishedMessage, 0, reinterpret_cast<LPARAM>(message.get()))) {
            return;
        }
        message.release();
    });
}

void schedule_preview_for_frame(std::uint64_t target_frame) {
    g_state.latest_preview_frame = target_frame;
    if (g_state.preview_in_flight) {
        ++g_state.preview_superseded;
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = now - g_state.last_preview_started;
    if (elapsed >= kPreviewThrottle) {
        start_preview_to_frame(target_frame);
        return;
    }

    arm_preview_timer(g_state.hwnd, std::chrono::duration_cast<std::chrono::milliseconds>(kPreviewThrottle - elapsed));
}

void start_forward_playback() {
    if (g_state.loaded_path.empty() || g_state.index.frames.empty()) {
        MessageBoxW(g_state.hwnd, L"Open and index a .dat file before playing.", L"Play", MB_OK | MB_ICONINFORMATION);
        return;
    }

    stop_playback();
    if (g_state.current_frame + 1 >= frame_count()) {
        g_state.current_frame = 0;
    }

    g_state.frames_rendered = 0;
    g_state.frames_decoded = 0;
    g_state.late_frames = 0;
    g_state.clock_reanchors = 0;
    g_state.effective_playback_fps = 0.0;
    g_state.target_frame_interval_ms = 1000.0 / (playback_fps() * playback_speed_multiplier());
    g_state.frame_interval_ms = g_state.target_frame_interval_ms;
    g_state.average_scheduled_sleep_ms = 0.0;
    g_state.max_scheduled_sleep_ms = 0.0;
    g_state.recent_frame_interval_ms = 0.0;
    g_state.max_recent_frame_interval_ms = 0.0;
    g_state.average_ui_delay_ms = 0.0;
    g_state.max_ui_delay_ms = 0.0;
    g_state.average_convert_ms = 0.0;
    g_state.max_convert_ms = 0.0;
    g_state.last_paint_ms = 0.0;
    g_state.average_paint_ms = 0.0;
    g_state.max_paint_ms = 0.0;
    g_state.last_ui_frame_time = {};
    g_state.stop_playback_requested = false;
    const std::uint64_t generation = ++g_state.playback_generation;
    g_state.playing = true;
    update_play_button();
    set_status(L"Starting forward playback at Speed: " + playback_speed_label() + L".");
    g_state.decode_test_text = format_playback_diagnostics(L"starting");
    update_info(true);

    const HWND hwnd = g_state.hwnd;
    const auto path = g_state.loaded_path;
    const auto index = g_state.index;
    const auto start_frame = g_state.current_frame;
    const double fallback_fps = playback_fps();
    const double fallback_interval_ms = 1000.0 / fallback_fps;
    const std::uint64_t timing_generation = g_state.playback_timing_generation.load();

    g_state.playback_thread = std::thread([hwnd, path, index, start_frame, fallback_fps, fallback_interval_ms, generation, timing_generation]() {
        dat_player::playback::H264DecodeTester tester;
        dat_player::playback::ForwardPlaybackOptions options;
        options.start_frame = start_frame;
        options.fallback_fps = fallback_fps;

        auto anchor_time = std::chrono::steady_clock::now();
        auto last_post_time = anchor_time;
        std::uint64_t anchor_frame = start_frame;
        bool first_frame = true;
        std::uint64_t local_timing_generation = timing_generation;
        std::uint64_t rendered_callbacks = 0;
        std::uint64_t late_frames = 0;
        std::uint64_t clock_reanchors = 0;

        const auto result = tester.play_forward(path, index, options, [&](dat_player::playback::ForwardPlaybackFrame&& frame) {
            if (g_state.stop_playback_requested || g_state.playback_generation.load() != generation) {
                return false;
            }

            double speed = playback_speed_multiplier_from_index(g_state.playback_speed_index.load());
            double target_interval_ms = fallback_interval_ms / std::max(1.0, speed);
            auto now = std::chrono::steady_clock::now();
            const auto current_timing_generation = g_state.playback_timing_generation.load();
            if (current_timing_generation != local_timing_generation) {
                local_timing_generation = current_timing_generation;
                anchor_time = now;
                anchor_frame = frame.frame_index;
                last_post_time = now;
                rendered_callbacks = 0;
                ++clock_reanchors;
                first_frame = false;
            }

            auto target_post_time = now;
            if (first_frame) {
                anchor_time = now;
                anchor_frame = frame.frame_index;
                last_post_time = now;
                first_frame = false;
            } else {
                const std::uint64_t frame_delta = frame.frame_index >= anchor_frame
                    ? frame.frame_index - anchor_frame
                    : 0;
                target_post_time = anchor_time + std::chrono::microseconds(
                    static_cast<long long>(static_cast<double>(frame_delta) * target_interval_ms * 1000.0));
                const double lateness_ms = std::chrono::duration<double, std::milli>(now - target_post_time).count();
                const double reanchor_threshold_ms = std::max(120.0, target_interval_ms * 4.0);
                if (lateness_ms > reanchor_threshold_ms) {
                    anchor_time = now;
                    anchor_frame = frame.frame_index;
                    target_post_time = now;
                    ++clock_reanchors;
                }
            }

            double scheduled_sleep_ms = std::max(
                0.0,
                std::chrono::duration<double, std::milli>(target_post_time - now).count());
            if (scheduled_sleep_ms > 0.0) {
                while (!g_state.stop_playback_requested &&
                       g_state.playback_generation.load() == generation &&
                       g_state.playback_timing_generation.load() == local_timing_generation &&
                       std::chrono::steady_clock::now() + std::chrono::milliseconds(1) < target_post_time) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }

            if (g_state.stop_playback_requested || g_state.playback_generation.load() != generation) {
                return false;
            }

            now = std::chrono::steady_clock::now();
            if (g_state.playback_timing_generation.load() != local_timing_generation) {
                local_timing_generation = g_state.playback_timing_generation.load();
                speed = playback_speed_multiplier_from_index(g_state.playback_speed_index.load());
                target_interval_ms = fallback_interval_ms / std::max(1.0, speed);
                anchor_time = now;
                anchor_frame = frame.frame_index;
                last_post_time = now;
                target_post_time = now;
                scheduled_sleep_ms = 0.0;
                rendered_callbacks = 0;
                ++clock_reanchors;
            }
            if (now > target_post_time + std::chrono::milliseconds(20)) {
                ++late_frames;
            }
            ++rendered_callbacks;
            const double elapsed_seconds = std::chrono::duration<double>(now - anchor_time).count();
            const double effective_fps = elapsed_seconds > 0.0 && rendered_callbacks > 1
                ? static_cast<double>(rendered_callbacks - 1) / elapsed_seconds
                : 0.0;
            const double frame_interval_ms = rendered_callbacks > 1
                ? std::chrono::duration<double, std::milli>(now - last_post_time).count()
                : target_interval_ms;
            last_post_time = now;

            auto ui_frame = std::make_unique<UiPlaybackFrame>();
            ui_frame->frame = std::move(frame.frame);
            ui_frame->generation = generation;
            ui_frame->frame_index = frame.frame_index;
            ui_frame->timestamp = frame.timestamp;
            ui_frame->frames_submitted = frame.frames_submitted;
            ui_frame->frames_decoded = frame.frames_decoded;
            ui_frame->late_frames = late_frames;
            ui_frame->clock_reanchors = clock_reanchors;
            ui_frame->effective_fps = effective_fps;
            ui_frame->target_interval_ms = target_interval_ms;
            ui_frame->frame_interval_ms = frame_interval_ms;
            ui_frame->scheduled_sleep_ms = scheduled_sleep_ms;
            ui_frame->convert_ms = frame.convert_ms;
            ui_frame->posted_at = std::chrono::steady_clock::now();
            if (!PostMessageW(hwnd, kPlaybackFrameMessage, 0, reinterpret_cast<LPARAM>(ui_frame.release()))) {
                return false;
            }
            return true;
        });

        auto message = std::make_unique<PlaybackFinishedMessage>();
        message->generation = generation;
        message->decoded_any_frame = result.decoded_any_frame;
        message->text = result.message;
        if (!PostMessageW(hwnd, kPlaybackFinishedMessage, 0, reinterpret_cast<LPARAM>(message.get()))) {
            return;
        }
        message.release();
    });
}

void toggle_playback() {
    if (g_state.index.frames.empty()) {
        return;
    }

    if (g_state.timeline_dragging) {
        if (g_state.resume_after_timeline_drag) {
            g_state.resume_after_timeline_drag = false;
            g_state.decode_test_text = format_seek_diagnostics(
                L"dragging, resume canceled",
                g_state.has_timeline_preview ? g_state.timeline_preview_frame : g_state.current_frame,
                nearest_previous_keyframe(g_state.has_timeline_preview ? g_state.timeline_preview_frame : g_state.current_frame),
                0);
            update_play_button();
            update_info(true);
            set_status(L"Scrub resume canceled; release will keep playback paused.");
        }
        return;
    }

    if (g_state.seeking) {
        if (g_state.pending_seek_resume_after_completion) {
            g_state.pending_seek_resume_after_completion = false;
            g_state.pending_seek_resume_generation = 0;
            g_state.decode_test_text = format_seek_diagnostics(
                L"seeking, resume canceled",
                g_state.has_pending_seek ? g_state.pending_seek_frame : g_state.current_frame,
                nearest_previous_keyframe(g_state.has_pending_seek ? g_state.pending_seek_frame : g_state.current_frame),
                0);
            update_play_button();
            update_info(true);
            set_status(L"Seek will complete paused.");
        }
        return;
    }

    if (g_state.playing) {
        stop_playback();
        g_state.decode_test_text = format_playback_diagnostics(L"paused");
        update_info(true);
        set_status(L"Playback paused.");
    } else {
        start_forward_playback();
    }
}

void toggle_details() {
    if (!g_state.hwnd) {
        return;
    }

    RECT client_rect = {};
    RECT window_rect = {};
    RECT video_rect = {};
    GetClientRect(g_state.hwnd, &client_rect);
    GetWindowRect(g_state.hwnd, &window_rect);
    if (g_state.video_panel) {
        GetWindowRect(g_state.video_panel, &video_rect);
    }

    const bool show_details = !g_state.details_visible;
    const int current_video_width = g_state.video_panel
        ? std::max(1, static_cast<int>(video_rect.right - video_rect.left))
        : std::max(minimum_video_width_for_controls(), static_cast<int>(client_rect.right - client_rect.left) - 28);
    const int target_client_width = client_width_for_video_width(current_video_width, show_details);
    const int target_client_height = std::max(1, static_cast<int>(client_rect.bottom - client_rect.top));

    RECT target_window = {0, 0, target_client_width, target_client_height};
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(g_state.hwnd, GWL_STYLE));
    const auto ex_style = static_cast<DWORD>(GetWindowLongPtrW(g_state.hwnd, GWL_EXSTYLE));
    AdjustWindowRectEx(&target_window, style, FALSE, ex_style);
    int desired_width = target_window.right - target_window.left;
    int desired_height = target_window.bottom - target_window.top;

    HMONITOR monitor = MonitorFromWindow(g_state.hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info = {};
    monitor_info.cbSize = sizeof(monitor_info);
    GetMonitorInfoW(monitor, &monitor_info);
    const RECT work = monitor_info.rcWork;
    const int work_width = work.right - work.left;
    const int work_height = work.bottom - work.top;

    bool clamped = false;
    if (desired_width > work_width) {
        desired_width = work_width;
        clamped = true;
    }
    if (desired_height > work_height) {
        desired_height = work_height;
        clamped = true;
    }

    int x = window_rect.left;
    int y = window_rect.top;
    if (show_details && x + desired_width > work.right) {
        x = work.right - desired_width;
        clamped = true;
    }
    const int min_x = static_cast<int>(work.left);
    const int min_y = static_cast<int>(work.top);
    const int max_x = std::max(min_x, static_cast<int>(work.right) - desired_width);
    const int max_y = std::max(min_y, static_cast<int>(work.bottom) - desired_height);
    x = std::clamp(x, min_x, max_x);
    y = std::clamp(y, min_y, max_y);

    if (IsZoomed(g_state.hwnd)) {
        ShowWindow(g_state.hwnd, SW_RESTORE);
    }

    g_state.details_visible = show_details;
    update_details_toggle_button();
    SetWindowPos(g_state.hwnd, nullptr, x, y, desired_width, desired_height, SWP_NOZORDER | SWP_NOACTIVATE);
    layout_controls(g_state.hwnd);
    if (show_details) {
        g_state.displayed_info_text.clear();
        order_details_controls();
    }
    update_info(show_details);
    if (show_details) {
        RedrawWindow(g_state.info_label, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
        RedrawWindow(g_state.decode_test_button, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
        RedrawWindow(g_state.render_first_frame_button, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
    }
    InvalidateRect(g_state.hwnd, nullptr, TRUE);
    if (g_state.video_panel) {
        InvalidateRect(g_state.video_panel, nullptr, TRUE);
    }
    if (clamped && show_details) {
        set_status(L"Details shown; window was fit to the available screen space.");
    } else {
        set_status(show_details ? L"Details shown." : L"Details hidden.");
    }
}

void cycle_playback_speed() {
    const int next_index = (std::clamp(g_state.playback_speed_index.load(), 0, 4) + 1) % 5;
    g_state.playback_speed_index.store(next_index);
    ++g_state.playback_timing_generation;
    g_state.last_ui_frame_time = {};
    g_state.recent_frame_interval_ms = 0.0;
    g_state.max_recent_frame_interval_ms = 0.0;
    g_state.average_ui_delay_ms = 0.0;
    g_state.max_ui_delay_ms = 0.0;
    g_state.target_frame_interval_ms = 1000.0 / (playback_fps() * playback_speed_multiplier());
    g_state.frame_interval_ms = g_state.target_frame_interval_ms;
    g_state.average_scheduled_sleep_ms = 0.0;
    g_state.max_scheduled_sleep_ms = 0.0;
    update_speed_button();
    update_info(true);
    std::wostringstream status;
    status << L"Speed: " << playback_speed_label();
    set_status(status.str());
}

bool resize_to_actual_size() {
    if (!g_state.hwnd || g_state.index.frames.empty()) {
        return false;
    }

    const auto& first_frame = g_state.index.frames.front();
    const int source_width = static_cast<int>(first_frame.width);
    const int source_height = static_cast<int>(first_frame.height);
    if (source_width <= 0 || source_height <= 0) {
        set_status(L"Actual Size is unavailable because no source resolution was detected.");
        return false;
    }

    const int padding = 14;
    const int header_height = 0;
    const int file_row_height = 22;
    const int button_height = 32;
    const int timeline_height = 58;
    const int status_height = 24;
    const int file_top = header_height + 12;
    const int content_top = file_top + file_row_height + 8;
    const int content_bottom_margin = status_height + 16;
    const int controls_height = button_height + 8;

    int details_width = 0;
    if (g_state.details_visible) {
        details_width = 370;
        int visible_client_width = 0;
        for (int i = 0; i < 4; ++i) {
            visible_client_width = source_width + padding * 3 + details_width;
            details_width = std::clamp(visible_client_width / 3, 320, 370);
        }
    }
    const int video_side_width = std::max(source_width, minimum_video_width_for_controls());
    const int client_width = video_side_width + padding * 2 + (g_state.details_visible ? padding + details_width : 0);
    const int client_height = content_top + source_height + timeline_height + controls_height + 16 + content_bottom_margin;

    RECT window_rect = {0, 0, client_width, client_height};
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(g_state.hwnd, GWL_STYLE));
    const auto ex_style = static_cast<DWORD>(GetWindowLongPtrW(g_state.hwnd, GWL_EXSTYLE));
    AdjustWindowRectEx(&window_rect, style, FALSE, ex_style);
    int desired_width = window_rect.right - window_rect.left;
    int desired_height = window_rect.bottom - window_rect.top;

    HMONITOR monitor = MonitorFromWindow(g_state.hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info = {};
    monitor_info.cbSize = sizeof(monitor_info);
    GetMonitorInfoW(monitor, &monitor_info);
    const RECT work = monitor_info.rcWork;
    const int work_width = work.right - work.left;
    const int work_height = work.bottom - work.top;

    const bool extends_beyond_screen = desired_width > work_width || desired_height > work_height;
    const int x = extends_beyond_screen
        ? static_cast<int>(work.left)
        : work.left + std::max(0, (work_width - desired_width) / 2);
    const int y = extends_beyond_screen
        ? static_cast<int>(work.top)
        : work.top + std::max(0, (work_height - desired_height) / 2);

    if (IsZoomed(g_state.hwnd)) {
        ShowWindow(g_state.hwnd, SW_RESTORE);
    }
    g_state.force_native_video_size = true;
    g_state.requested_video_width = source_width;
    g_state.requested_video_height = source_height;
    SetWindowPos(g_state.hwnd, nullptr, x, y, desired_width, desired_height, SWP_NOZORDER | SWP_NOACTIVATE);
    layout_controls(g_state.hwnd);

    if (extends_beyond_screen) {
        set_status(g_state.details_visible
            ? L"Video set to actual size; window may extend beyond the screen."
            : L"Video set to actual size.");
    } else {
        set_status(L"Video set to actual size.");
    }
    return true;
}

bool resize_to_default_size() {
    if (!g_state.hwnd) {
        return false;
    }

    HMONITOR monitor = MonitorFromWindow(g_state.hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info = {};
    monitor_info.cbSize = sizeof(monitor_info);
    GetMonitorInfoW(monitor, &monitor_info);
    const RECT work = monitor_info.rcWork;
    const int work_width = work.right - work.left;
    const int work_height = work.bottom - work.top;

    const SIZE min_window = window_size_for_client_size(g_state.hwnd, minimum_client_width(), minimum_client_height());
    const int desired_width = std::min<int>(std::max<int>(kDefaultWindowWidth, min_window.cx), work_width);
    const int desired_height = std::min<int>(std::max<int>(kDefaultWindowHeight, min_window.cy), work_height);
    const int x = work.left + std::max(0, (work_width - desired_width) / 2);
    const int y = work.top + std::max(0, (work_height - desired_height) / 2);

    if (IsZoomed(g_state.hwnd)) {
        ShowWindow(g_state.hwnd, SW_RESTORE);
    }
    g_state.force_native_video_size = false;
    g_state.requested_video_width = 0;
    g_state.requested_video_height = 0;
    SetWindowPos(g_state.hwnd, nullptr, x, y, desired_width, desired_height, SWP_NOZORDER | SWP_NOACTIVATE);
    set_status(L"Default Size applied.");
    return true;
}

void toggle_actual_size() {
    const bool resized = g_state.actual_size_applied ? resize_to_default_size() : resize_to_actual_size();
    if (!resized) {
        return;
    }

    g_state.actual_size_applied = !g_state.actual_size_applied;
    update_actual_size_button();
}

void apply_default_font(HWND control) {
    if (control) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_state.ui_font), TRUE);
    }
}

void layout_controls(HWND hwnd) {
    RECT rect = {};
    GetClientRect(hwnd, &rect);
    const int width = rect.right - rect.left;
    const int padding = 14;
    const int header_height = 0;
    const int file_row_height = 22;
    const int button_width = 104;
    const int button_gap = 8;
    const int open_button_width = button_width;
    const int play_button_width = button_width;
    const int speed_button_width = button_width;
    const int actual_size_button_width = button_width;
    const int details_toggle_button_width = button_width;
    const int button_height = 32;
    const int timeline_height = 58;
    const int trackbar_height = 28;
    const int status_height = 24;
    const int time_label_width = 92;
    const int integrity_dot_size = 16;
    const int integrity_dot_gap = 6;
    const int file_top = header_height + 12;
    const int content_top = file_top + file_row_height + 8;
    const int status_top = rect.bottom - status_height - 6;
    const int content_bottom = status_top - 10;
    const bool force_native_video =
        g_state.force_native_video_size &&
        g_state.requested_video_width > 0 &&
        g_state.requested_video_height > 0;
    int details_width = 0;
    if (g_state.details_visible) {
        if (force_native_video) {
            details_width = 370;
        } else {
            details_width = details_panel_width_for_client_width(width);
            const int max_details_width = width - padding * 3 - 1;
            if (max_details_width < details_width) {
                details_width = std::max(1, max_details_width);
            }
        }
    }
    const int controls_min_width = minimum_video_width_for_controls();
    const int available_video_width = width - padding * 2 - (g_state.details_visible ? padding + details_width : 0);
    const int video_width = force_native_video
        ? g_state.requested_video_width
        : (g_state.details_visible
            ? std::max(1, available_video_width)
            : std::max(controls_min_width, available_video_width));
    const int content_height = std::max(220, content_bottom - content_top);
    const int controls_height = button_height + 8;
    const int video_height = force_native_video
        ? g_state.requested_video_height
        : std::max(120, content_height - timeline_height - controls_height - 16);
    const int timeline_top = content_top + video_height + 8;
    const int controls_top = timeline_top + timeline_height + 2;
    const int details_left = padding + video_width + padding;

    MoveWindow(g_state.file_label, 0, 0, 0, 0, TRUE);
    ShowWindow(g_state.file_label, SW_HIDE);
    const int file_text_width = std::max(80, video_width - integrity_dot_size - integrity_dot_gap);
    MoveWindow(g_state.file_path_edit, padding, file_top, file_text_width, file_row_height, TRUE);
    MoveWindow(
        g_state.integrity_dot,
        padding + video_width - integrity_dot_size,
        file_top + std::max(0, (file_row_height - integrity_dot_size) / 2),
        integrity_dot_size,
        integrity_dot_size,
        TRUE);
    MoveWindow(g_state.video_panel, padding, content_top, video_width, video_height, TRUE);
    MoveWindow(g_state.current_time_label, padding, timeline_top + 2, time_label_width, 38, TRUE);
    MoveWindow(g_state.timeline, padding + time_label_width + 6, timeline_top, video_width - time_label_width * 2 - 12, trackbar_height, TRUE);
    MoveWindow(g_state.total_time_label, padding + video_width - time_label_width, timeline_top + 2, time_label_width, 38, TRUE);
    const int button_group_width =
        open_button_width + actual_size_button_width + play_button_width + speed_button_width + details_toggle_button_width +
        button_gap * 4;
    const int button_group_left = padding + std::max(0, (video_width - button_group_width) / 2);
    int button_left = button_group_left;
    MoveWindow(g_state.open_button, button_left, controls_top, open_button_width, button_height, TRUE);
    button_left += open_button_width + button_gap;
    MoveWindow(g_state.actual_size_button, button_left, controls_top, actual_size_button_width, button_height, TRUE);
    button_left += actual_size_button_width + button_gap;
    MoveWindow(g_state.play_button, button_left, controls_top, play_button_width, button_height, TRUE);
    button_left += play_button_width + button_gap;
    MoveWindow(g_state.speed_button, button_left, controls_top, speed_button_width, button_height, TRUE);
    button_left += speed_button_width + button_gap;
    MoveWindow(g_state.details_toggle_button, button_left, controls_top, details_toggle_button_width, button_height, TRUE);
    if (g_state.details_visible) {
        MoveWindow(g_state.details_group, details_left, content_top, details_width, content_height, TRUE);
        layout_details_panel();
    }
    ShowWindow(g_state.details_group, g_state.details_visible ? SW_SHOW : SW_HIDE);
    ShowWindow(g_state.info_label, g_state.details_visible ? SW_SHOW : SW_HIDE);
    ShowWindow(g_state.decode_test_button, g_state.details_visible ? SW_SHOW : SW_HIDE);
    ShowWindow(g_state.render_first_frame_button, g_state.details_visible ? SW_SHOW : SW_HIDE);
    order_details_controls();
    ShowWindow(g_state.integrity_dot, SW_SHOW);
    update_integrity_dot(true);
    update_actual_size_button();
    update_details_toggle_button();
    update_speed_button();
    MoveWindow(g_state.status_label, padding, status_top, width - padding * 2, status_height, TRUE);
    update_thumb_time_label(g_state.has_timeline_preview
        ? g_state.timeline_preview_frame
        : (g_state.has_pending_seek ? g_state.pending_seek_frame : g_state.current_frame));
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        g_state.hwnd = hwnd;
        g_state.ui_font = CreateFontW(
            -12,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");

        g_state.file_label = CreateWindowW(L"STATIC", L"", WS_CHILD,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFileLabelId)), nullptr, nullptr);
        g_state.file_path_edit = CreateWindowW(L"STATIC", L"No DAT file loaded",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFilePathId)), nullptr, nullptr);
        g_state.integrity_dot = CreateWindowW(L"DATIntegrityDot", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIntegrityDotId)), nullptr, nullptr);
        g_state.open_button = CreateWindowW(L"BUTTON", L"Open .dat", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOpenButtonId)), nullptr, nullptr);
        g_state.play_button = CreateWindowW(L"BUTTON", L"Play", WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPlayButtonId)), nullptr, nullptr);
        g_state.speed_button = CreateWindowW(L"BUTTON", L"x2", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSpeedButtonId)), nullptr, nullptr);
        g_state.actual_size_button = CreateWindowW(L"BUTTON", L"Actual Size", WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kActualSizeButtonId)), nullptr, nullptr);
        g_state.details_toggle_button = CreateWindowW(L"BUTTON", L"Show Details", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDetailsToggleButtonId)), nullptr, nullptr);
        g_state.details_group = CreateWindowW(L"DATDetailsPanel", L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDetailsGroupId)), nullptr, nullptr);
        g_state.decode_test_button = CreateWindowW(L"BUTTON", L"Decode Test", WS_CHILD | WS_CLIPSIBLINGS | WS_DISABLED | BS_PUSHBUTTON,
            0, 0, 0, 0, g_state.details_group, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDecodeButtonId)), nullptr, nullptr);
        g_state.render_first_frame_button = CreateWindowW(L"BUTTON", L"Render Frame", WS_CHILD | WS_CLIPSIBLINGS | WS_DISABLED | BS_PUSHBUTTON,
            0, 0, 0, 0, g_state.details_group, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRenderFirstFrameButtonId)), nullptr, nullptr);
        g_state.timeline = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_DISABLED | TBS_NOTICKS,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTimelineId)), nullptr, nullptr);
        g_state.current_time_label = CreateWindowW(L"STATIC", L"00:00", WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCurrentTimeLabelId)), nullptr, nullptr);
        g_state.total_time_label = CreateWindowW(L"STATIC", L"00:00", WS_CHILD | WS_VISIBLE | SS_RIGHT,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTotalTimeLabelId)), nullptr, nullptr);
        g_state.thumb_time_label = CreateWindowW(L"STATIC", L"00:00", WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kThumbTimeLabelId)), nullptr, nullptr);
        g_state.video_panel = CreateWindowW(L"DATVideoPanel", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_CLIPSIBLINGS,
            0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        DragAcceptFiles(hwnd, TRUE);
        DragAcceptFiles(g_state.video_panel, TRUE);
        g_state.info_label = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_CLIPSIBLINGS | ES_LEFT | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
            0, 0, 0, 0, g_state.details_group, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInfoLabelId)), nullptr, nullptr);
        g_state.status_label = CreateWindowW(L"STATIC", L"Ready. Open a compatible .dat file to begin.", WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusLabelId)), nullptr, nullptr);

        apply_default_font(g_state.file_label);
        apply_default_font(g_state.file_path_edit);
        apply_default_font(g_state.integrity_dot);
        apply_default_font(g_state.open_button);
        apply_default_font(g_state.play_button);
        apply_default_font(g_state.speed_button);
        apply_default_font(g_state.actual_size_button);
        apply_default_font(g_state.details_toggle_button);
        apply_default_font(g_state.details_group);
        apply_default_font(g_state.decode_test_button);
        apply_default_font(g_state.render_first_frame_button);
        apply_default_font(g_state.current_time_label);
        apply_default_font(g_state.total_time_label);
        apply_default_font(g_state.thumb_time_label);
        apply_default_font(g_state.info_label);
        apply_default_font(g_state.status_label);
        SendMessageW(g_state.info_label, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));

        SendMessageW(g_state.timeline, TBM_SETRANGE, TRUE, MAKELPARAM(0, kTrackbarMax));
        SendMessageW(g_state.timeline, TBM_SETPAGESIZE, 0, 500);
        SendMessageW(g_state.timeline, TBM_SETLINESIZE, 0, 100);
        update_info(true);
        layout_controls(hwnd);
        return 0;

    case WM_ENTERSIZEMOVE:
        g_state.live_resizing = true;
        g_state.force_native_video_size = false;
        g_state.requested_video_width = 0;
        g_state.requested_video_height = 0;
        return 0;

    case WM_EXITSIZEMOVE:
        g_state.live_resizing = false;
        KillTimer(hwnd, kResizeRefreshTimerId);
        layout_controls(hwnd);
        refresh_after_resize();
        return 0;

    case WM_GETMINMAXINFO: {
        auto* minmax = reinterpret_cast<MINMAXINFO*>(lparam);
        const SIZE min_window = window_size_for_client_size(hwnd, minimum_client_width(), minimum_client_height());
        minmax->ptMinTrackSize.x = min_window.cx;
        minmax->ptMinTrackSize.y = min_window.cy;
        minmax->ptMaxTrackSize.x = 32767;
        minmax->ptMaxTrackSize.y = 32767;
        return 0;
    }

    case WM_SIZE:
        layout_controls(hwnd);
        if (g_state.video_panel) {
            InvalidateRect(g_state.video_panel, nullptr, FALSE);
        }
        if (g_state.details_visible) {
            RedrawWindow(g_state.details_group, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
            RedrawWindow(g_state.info_label, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
            RedrawWindow(g_state.decode_test_button, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
            RedrawWindow(g_state.render_first_frame_button, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
        }
        arm_resize_refresh();
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wparam);
        SetTextColor(hdc, RGB(35, 43, 51));
        if (reinterpret_cast<HWND>(lparam) == g_state.info_label) {
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
        }
        if (reinterpret_cast<HWND>(lparam) == g_state.thumb_time_label) {
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, RGB(240, 240, 240));
        } else {
            SetBkMode(hdc, TRANSPARENT);
        }
        return reinterpret_cast<LRESULT>(g_state.window_background_brush);
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wparam);
        SetBkColor(hdc, RGB(255, 255, 255));
        SetTextColor(hdc, RGB(35, 43, 51));
        return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
    }

    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wparam);
        RECT rect = {};
        GetClientRect(hwnd, &rect);
        FillRect(hdc, &rect, g_state.window_background_brush);
        return 1;
    }

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case kOpenButtonId:
            load_dat_file(hwnd);
            return 0;
        case kPlayButtonId:
            toggle_playback();
            return 0;
        case kSpeedButtonId:
            cycle_playback_speed();
            return 0;
        case kActualSizeButtonId:
            toggle_actual_size();
            return 0;
        case kDetailsToggleButtonId:
            toggle_details();
            return 0;
        case kDecodeButtonId:
            run_decode_test();
            return 0;
        case kRenderFirstFrameButtonId:
            run_first_frame_render_test();
            return 0;
        default:
            break;
        }
        break;

    case WM_DROPFILES:
        handle_drop_files(hwnd, reinterpret_cast<HDROP>(wparam));
        return 0;

    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lparam) == g_state.timeline && !g_state.index.frames.empty()) {
            const int code = LOWORD(wparam);
            if (code == TB_THUMBTRACK) {
                if (!g_state.timeline_dragging) {
                    g_state.resume_after_timeline_drag = g_state.playing;
                    if (g_state.playing || g_state.seeking) {
                        stop_playback();
                    }
                    g_state.timeline_dragging = true;
                    update_play_button();
                }
                const int position = timeline_position_from_scroll(wparam);
                const auto target = frame_from_timeline_position(position);
                g_state.timeline_preview_frame = target;
                g_state.has_timeline_preview = true;
                update_thumb_time_label(target);
                g_state.decode_test_text = format_seek_diagnostics(
                    g_state.resume_after_timeline_drag ? L"dragging, will resume" : L"dragging",
                    target,
                    nearest_previous_keyframe(target),
                    0,
                    L"",
                    g_state.resume_after_timeline_drag);
                update_info();
                schedule_preview_for_frame(target);
                set_status(L"Scrubbing preview. Release the timeline to commit seek.");
                return 0;
            }

            if (code == TB_ENDTRACK || code == TB_THUMBPOSITION) {
                if (!g_state.timeline_dragging) {
                    if (code == TB_ENDTRACK || (g_state.seeking && g_state.has_pending_seek)) {
                        return 0;
                    }
                } else {
                    const int position = timeline_position_from_scroll(wparam);
                    const auto target = g_state.has_timeline_preview
                        ? g_state.timeline_preview_frame
                        : frame_from_timeline_position(position);
                    const bool resume = g_state.resume_after_timeline_drag;
                    g_state.timeline_dragging = false;
                    g_state.resume_after_timeline_drag = false;
                    g_state.has_timeline_preview = false;
                    stop_preview(false);
                    start_seek_to_frame(target, resume);
                    return 0;
                }
            }

            const bool resume_after_click_seek =
                g_state.playing ||
                (g_state.seeking && g_state.pending_seek_resume_after_completion);
            if (g_state.playing || g_state.seeking) {
                stop_playback();
            }
            const int position = static_cast<int>(SendMessageW(g_state.timeline, TBM_GETPOS, 0, 0));
            const auto target = frame_from_timeline_position(position);
            g_state.has_timeline_preview = false;
            stop_preview(false);
            start_seek_to_frame(target, resume_after_click_seek);
            return 0;
        }
        break;

    case WM_TIMER:
        if (wparam == kTimelinePreviewTimerId) {
            KillTimer(hwnd, kTimelinePreviewTimerId);
            g_state.preview_timer_armed = false;
            if (g_state.timeline_dragging && g_state.has_timeline_preview && !g_state.preview_in_flight) {
                start_preview_to_frame(g_state.latest_preview_frame);
            }
            return 0;
        }
        if (wparam == kResizeRefreshTimerId) {
            KillTimer(hwnd, kResizeRefreshTimerId);
            if (!g_state.live_resizing) {
                refresh_after_resize();
            }
            return 0;
        }
        break;

    case kIntegrityFinishedMessage: {
        std::unique_ptr<IntegrityFinishedMessage> integrity_message(reinterpret_cast<IntegrityFinishedMessage*>(lparam));
        if (!integrity_message || integrity_message->generation != g_state.integrity_generation.load()) {
            return 0;
        }

        auto& current_sidecar = g_state.index.summary.recording_metadata.sidecar;
        if (!current_sidecar.available || current_sidecar.path != integrity_message->sidecar.path) {
            return 0;
        }

        current_sidecar = std::move(integrity_message->sidecar);
        if (!integrity_message->cache_key.empty()) {
            g_state.integrity_cache[integrity_message->cache_key] = current_sidecar;
        }
        update_info(true);
        return 0;
    }

    case kPlaybackFrameMessage: {
        std::unique_ptr<UiPlaybackFrame> frame(reinterpret_cast<UiPlaybackFrame*>(lparam));
        if (frame && frame->generation == g_state.playback_generation.load()) {
            const auto handled_at = std::chrono::steady_clock::now();
            g_state.rendered_frame = std::move(frame->frame);
            g_state.current_frame = frame->frame_index;
            g_state.frames_decoded = frame->frames_decoded;
            ++g_state.frames_rendered;
            if (frame->seek_frame) {
                g_state.has_pending_seek = false;
                g_state.pending_seek_frame = 0;
            } else {
                g_state.late_frames = frame->late_frames;
                g_state.clock_reanchors = frame->clock_reanchors;
                g_state.effective_playback_fps = frame->effective_fps;
                g_state.target_frame_interval_ms = frame->target_interval_ms;
                g_state.frame_interval_ms = frame->frame_interval_ms;
                g_state.average_scheduled_sleep_ms =
                    (g_state.average_scheduled_sleep_ms <= 0.0 && g_state.max_scheduled_sleep_ms <= 0.0)
                    ? frame->scheduled_sleep_ms
                    : (g_state.average_scheduled_sleep_ms * 0.9 + frame->scheduled_sleep_ms * 0.1);
                g_state.max_scheduled_sleep_ms = std::max(g_state.max_scheduled_sleep_ms, frame->scheduled_sleep_ms);
                if (g_state.last_ui_frame_time.time_since_epoch().count() != 0) {
                    const double ui_interval_ms = std::chrono::duration<double, std::milli>(
                        handled_at - g_state.last_ui_frame_time).count();
                    g_state.recent_frame_interval_ms = g_state.recent_frame_interval_ms <= 0.0
                        ? ui_interval_ms
                        : (g_state.recent_frame_interval_ms * 0.9 + ui_interval_ms * 0.1);
                    g_state.max_recent_frame_interval_ms = std::max(g_state.max_recent_frame_interval_ms, ui_interval_ms);
                }
                g_state.last_ui_frame_time = handled_at;
                if (frame->posted_at.time_since_epoch().count() != 0) {
                    const double ui_delay_ms = std::chrono::duration<double, std::milli>(
                        handled_at - frame->posted_at).count();
                    g_state.average_ui_delay_ms = g_state.average_ui_delay_ms <= 0.0
                        ? ui_delay_ms
                        : (g_state.average_ui_delay_ms * 0.9 + ui_delay_ms * 0.1);
                    g_state.max_ui_delay_ms = std::max(g_state.max_ui_delay_ms, ui_delay_ms);
                }
                if (frame->convert_ms > 0.0) {
                    g_state.average_convert_ms = g_state.average_convert_ms <= 0.0
                        ? frame->convert_ms
                        : (g_state.average_convert_ms * 0.9 + frame->convert_ms * 0.1);
                    g_state.max_convert_ms = std::max(g_state.max_convert_ms, frame->convert_ms);
                }
                g_state.decode_test_text = format_playback_diagnostics(L"playing");
            }
            if (g_state.video_panel) {
                InvalidateRect(g_state.video_panel, nullptr, FALSE);
            }
            update_info();
            std::wostringstream status;
            status << L"Speed: " << playback_speed_label();
            set_status(status.str());
        }
        return 0;
    }

    case kPlaybackFinishedMessage: {
        std::unique_ptr<PlaybackFinishedMessage> playback_message(reinterpret_cast<PlaybackFinishedMessage*>(lparam));
        if (!playback_message || playback_message->generation != g_state.playback_generation.load()) {
            return 0;
        }
        if (g_state.playback_thread.joinable() &&
            g_state.playback_thread.get_id() != std::this_thread::get_id()) {
            g_state.playback_thread.join();
        }
        const bool completed = playback_message->decoded_any_frame && !g_state.stop_playback_requested;
        g_state.playing = false;
        update_play_button();
        g_state.decode_test_text = format_playback_diagnostics(completed ? L"completed" : L"stopped");
        if (!playback_message->text.empty()) {
            g_state.decode_test_text += L"\r\nResult: " + playback_message->text;
        }
        update_info(true);
        set_status(completed ? L"Playback completed or reached decoder end." : L"Playback stopped.");
        return 0;
    }

    case kPreviewFrameMessage: {
        std::unique_ptr<UiPlaybackFrame> frame(reinterpret_cast<UiPlaybackFrame*>(lparam));
        if (frame &&
            frame->generation == g_state.preview_generation.load() &&
            g_state.timeline_dragging &&
            g_state.has_timeline_preview) {
            g_state.rendered_frame = std::move(frame->frame);
            g_state.preview_rendered_frame = frame->frame_index;
            g_state.preview_frames_decoded = frame->frames_decoded;
            g_state.decode_test_text = format_preview_diagnostics(L"rendered", g_state.timeline_preview_frame);
            if (g_state.video_panel) {
                InvalidateRect(g_state.video_panel, nullptr, FALSE);
            }
            update_info();
            std::wostringstream status;
            status << L"Preview frame " << (frame->frame_index + 1) << L" / " << frame_count()
                   << L". Release timeline to commit.";
            set_status(status.str());
        }
        return 0;
    }

    case kPreviewFinishedMessage: {
        std::unique_ptr<PreviewFinishedMessage> preview_message(reinterpret_cast<PreviewFinishedMessage*>(lparam));
        const bool valid = preview_message &&
            preview_message->generation == g_state.preview_generation.load();
        if (g_state.preview_thread.joinable() &&
            g_state.preview_thread.get_id() != std::this_thread::get_id()) {
            g_state.preview_thread.join();
        }
        if (!valid) {
            return 0;
        }

        g_state.preview_in_flight = false;
        g_state.preview_keyframe_frame = preview_message->keyframe_frame;
        g_state.preview_rendered_frame = preview_message->rendered_frame;
        g_state.preview_frames_decoded = preview_message->frames_decoded;

        if (g_state.timeline_dragging && g_state.has_timeline_preview) {
            g_state.decode_test_text = format_preview_diagnostics(L"ready", g_state.timeline_preview_frame);
            update_info();
            if (g_state.latest_preview_frame != g_state.active_preview_frame) {
                arm_preview_timer(hwnd, std::chrono::milliseconds(1));
            }
        }
        return 0;
    }

    case kSeekFinishedMessage: {
        std::unique_ptr<SeekFinishedMessage> seek_message(reinterpret_cast<SeekFinishedMessage*>(lparam));
        if (!seek_message || seek_message->generation != g_state.playback_generation.load()) {
            return 0;
        }
        if (g_state.playback_thread.joinable() &&
            g_state.playback_thread.get_id() != std::this_thread::get_id()) {
            g_state.playback_thread.join();
        }

        g_state.seeking = false;
        g_state.playing = false;
        if (!seek_message->decoded_any_frame || g_state.stop_playback_requested) {
            g_state.has_pending_seek = false;
            g_state.pending_seek_frame = 0;
        }
        const bool seek_ok = seek_message->decoded_any_frame && !g_state.stop_playback_requested;
        const bool resume_after_seek =
            seek_ok &&
            seek_message->resume_after_seek &&
            g_state.pending_seek_resume_after_completion &&
            g_state.pending_seek_resume_generation == seek_message->generation;
        g_state.pending_seek_resume_after_completion = false;
        g_state.pending_seek_resume_generation = 0;
        update_play_button();
        g_state.decode_test_text = format_seek_diagnostics(
            seek_ok ? L"completed" : L"failed",
            seek_message->requested_frame,
            seek_message->keyframe_frame,
            seek_message->frames_decoded,
            seek_message->text,
            resume_after_seek);
        update_info(true);

        if (resume_after_seek) {
            set_status(L"Seek completed; resuming playback.");
            start_forward_playback();
        } else {
            set_status(seek_ok ? L"Seek completed." : L"Seek failed; keeping last rendered frame.");
        }
        return 0;
    }

    case WM_DESTROY:
        DragAcceptFiles(g_state.video_panel, FALSE);
        DragAcceptFiles(hwnd, FALSE);
        stop_playback();
        stop_preview();
        ++g_state.integrity_generation;
        if (g_state.ui_font) {
            DeleteObject(g_state.ui_font);
            g_state.ui_font = nullptr;
        }
        if (g_state.window_background_brush) {
            DeleteObject(g_state.window_background_brush);
            g_state.window_background_brush = nullptr;
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    INITCOMMONCONTROLSEX controls = {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&controls);

    g_state.window_background_brush = CreateSolidBrush(RGB(240, 240, 240));

    const wchar_t class_name[] = L"DATPlayerShellWindow";
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_DATPLAYER_ICON));
    window_class.hIconSm = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(IDI_DATPLAYER_ICON),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));
    window_class.hbrBackground = g_state.window_background_brush;
    window_class.lpszClassName = class_name;

    if (!RegisterClassExW(&window_class)) {
        MessageBoxW(nullptr, L"Unable to register DAT Player window class.", L"DAT Player", MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW integrity_dot_class = {};
    integrity_dot_class.cbSize = sizeof(integrity_dot_class);
    integrity_dot_class.style = CS_HREDRAW | CS_VREDRAW;
    integrity_dot_class.lpfnWndProc = integrity_dot_proc;
    integrity_dot_class.hInstance = instance;
    integrity_dot_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    integrity_dot_class.hbrBackground = g_state.window_background_brush;
    integrity_dot_class.lpszClassName = L"DATIntegrityDot";

    if (!RegisterClassExW(&integrity_dot_class)) {
        MessageBoxW(nullptr, L"Unable to register DAT Player status indicator.", L"DAT Player", MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW details_panel_class = {};
    details_panel_class.cbSize = sizeof(details_panel_class);
    details_panel_class.style = CS_HREDRAW | CS_VREDRAW;
    details_panel_class.lpfnWndProc = details_panel_proc;
    details_panel_class.hInstance = instance;
    details_panel_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    details_panel_class.hbrBackground = g_state.window_background_brush;
    details_panel_class.lpszClassName = L"DATDetailsPanel";

    if (!RegisterClassExW(&details_panel_class)) {
        MessageBoxW(nullptr, L"Unable to register DAT Player details panel.", L"DAT Player", MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW video_class = {};
    video_class.cbSize = sizeof(video_class);
    video_class.style = CS_HREDRAW | CS_VREDRAW;
    video_class.lpfnWndProc = video_panel_proc;
    video_class.hInstance = instance;
    video_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    video_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    video_class.lpszClassName = L"DATVideoPanel";

    if (!RegisterClassExW(&video_class)) {
        MessageBoxW(nullptr, L"Unable to register DAT Player video panel.", L"DAT Player", MB_OK | MB_ICONERROR);
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0,
        class_name,
        L"DAT Player",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        kDefaultWindowWidth,
        kDefaultWindowHeight,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!hwnd) {
        MessageBoxW(nullptr, L"Unable to create DAT Player window.", L"DAT Player", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, show_command);
    UpdateWindow(hwnd);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}
