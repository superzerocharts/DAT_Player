#include "dat_player/DatFrameIndexer.h"
#include "playback/H264Decoder.h"
#include "player/resource.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

constexpr int kOpenButtonId = 1001;
constexpr int kPlayButtonId = 1002;
constexpr int kDecodeSmokeButtonId = 1003;
constexpr int kRenderFirstFrameButtonId = 1004;
constexpr int kTimelineId = 1005;
constexpr int kInfoLabelId = 1006;
constexpr int kStatusLabelId = 1007;
constexpr int kBrandHeaderId = 1008;
constexpr int kFilePathId = 1009;
constexpr int kFileLabelId = 1010;
constexpr int kDetailsGroupId = 1011;
constexpr UINT kPlaybackFrameMessage = WM_APP + 1;
constexpr UINT kPlaybackFinishedMessage = WM_APP + 2;
constexpr UINT kSeekFinishedMessage = WM_APP + 3;
constexpr int kTrackbarMax = 10000;

struct UiPlaybackFrame {
    dat_player::playback::BgraVideoFrame frame;
    std::uint64_t generation = 0;
    std::uint64_t frame_index = 0;
    std::uint64_t timestamp = 0;
    std::uint64_t frames_submitted = 0;
    std::uint64_t frames_decoded = 0;
    std::uint64_t late_frames = 0;
    double effective_fps = 0.0;
    double frame_interval_ms = 0.0;
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

struct PlayerState {
    HWND hwnd = nullptr;
    HWND open_button = nullptr;
    HWND play_button = nullptr;
    HWND decode_smoke_button = nullptr;
    HWND render_first_frame_button = nullptr;
    HWND timeline = nullptr;
    HWND brand_header = nullptr;
    HWND file_label = nullptr;
    HWND file_path_edit = nullptr;
    HWND details_group = nullptr;
    HWND video_panel = nullptr;
    HWND info_label = nullptr;
    HWND status_label = nullptr;
    HFONT ui_font = nullptr;
    HBRUSH window_background_brush = nullptr;
    ULONG_PTR gdiplus_token = 0;
    std::unique_ptr<Gdiplus::Bitmap> brand_logo;
    dat_player::DatFrameIndex index;
    std::filesystem::path loaded_path;
    std::wstring decode_smoke_text;
    dat_player::playback::BgraVideoFrame rendered_frame;
    std::thread playback_thread;
    std::atomic_bool stop_playback_requested = false;
    std::atomic<std::uint64_t> playback_generation = 0;
    std::uint64_t frames_rendered = 0;
    std::uint64_t frames_decoded = 0;
    std::uint64_t late_frames = 0;
    double effective_playback_fps = 0.0;
    double frame_interval_ms = 0.0;
    std::uint64_t current_frame = 0;
    bool timeline_dragging = false;
    bool resume_after_timeline_drag = false;
    bool seeking = false;
    bool playing = false;
};

PlayerState g_state;

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

bool has_dat_extension(const std::filesystem::path& path) {
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return extension == L".dat";
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

double seconds_for_frame(std::uint64_t frame) {
    if (g_state.index.frames.empty()) {
        return 0.0;
    }

    const auto clamped = std::min<std::uint64_t>(frame, frame_count() - 1);
    const auto first = g_state.index.frames.front().timestamp;
    const auto timestamp = g_state.index.frames[static_cast<std::size_t>(clamped)].timestamp;
    if (timestamp < first) {
        const double fps = playback_fps();
        return fps > 0.0 ? static_cast<double>(clamped) / fps : 0.0;
    }
    const double timestamp_seconds = static_cast<double>(timestamp - first) / 39062.5;
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
        const auto start_timestamp = index.frames[static_cast<std::size_t>(start_frame)].timestamp;
        const auto timestamp = index.frames[static_cast<std::size_t>(frame)].timestamp;
        if (timestamp >= start_timestamp) {
            const double seconds = static_cast<double>(timestamp - start_timestamp) / 39062.5;
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

std::wstring frame_time_label(std::uint64_t frame) {
    std::wostringstream text;
    text << L"frame " << (frame + 1) << L" / " << frame_count()
         << L" (" << format_double(seconds_for_frame(frame), 2) << L" / "
         << format_double(total_duration_seconds(), 2) << L" sec)";
    return text.str();
}

void set_status(const std::wstring& text) {
    if (g_state.status_label) {
        SetWindowTextW(g_state.status_label, text.c_str());
    }
}

void update_play_button() {
    if (g_state.play_button) {
        SetWindowTextW(g_state.play_button, g_state.playing ? L"Pause" : L"Play");
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
}

void update_timeline() {
    if (g_state.timeline) {
        SendMessageW(g_state.timeline, TBM_SETRANGE, TRUE, MAKELPARAM(0, kTrackbarMax));
        SendMessageW(g_state.timeline, TBM_SETPOS, TRUE, timeline_position_from_frame(g_state.current_frame));
        EnableWindow(g_state.timeline, frame_count() > 0);
    }
}

void update_file_path_text() {
    if (!g_state.file_path_edit) {
        return;
    }
    const std::wstring text = g_state.loaded_path.empty()
        ? L"No .dat file selected"
        : g_state.loaded_path.wstring();
    SetWindowTextW(g_state.file_path_edit, text.c_str());
}

std::unique_ptr<Gdiplus::Bitmap> load_png_resource(HINSTANCE instance, int resource_id) {
    HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (!resource) {
        return nullptr;
    }

    HGLOBAL resource_handle = LoadResource(instance, resource);
    if (!resource_handle) {
        return nullptr;
    }

    const DWORD resource_size = SizeofResource(instance, resource);
    const void* resource_data = LockResource(resource_handle);
    if (!resource_data || resource_size == 0) {
        return nullptr;
    }

    HGLOBAL copy_handle = GlobalAlloc(GMEM_MOVEABLE, resource_size);
    if (!copy_handle) {
        return nullptr;
    }

    void* copy_data = GlobalLock(copy_handle);
    if (!copy_data) {
        GlobalFree(copy_handle);
        return nullptr;
    }

    std::memcpy(copy_data, resource_data, resource_size);
    GlobalUnlock(copy_handle);

    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(copy_handle, TRUE, &stream) != S_OK || !stream) {
        GlobalFree(copy_handle);
        return nullptr;
    }

    std::unique_ptr<Gdiplus::Bitmap> bitmap(Gdiplus::Bitmap::FromStream(stream));
    stream->Release();
    if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok) {
        return nullptr;
    }
    return bitmap;
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

Gdiplus::PointF point_toward(const Gdiplus::PointF& from, const Gdiplus::PointF& to, float distance) {
    const float dx = to.X - from.X;
    const float dy = to.Y - from.Y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length <= 0.001f) {
        return from;
    }
    const float clamped = std::min(distance, length * 0.45f);
    return Gdiplus::PointF(from.X + dx * clamped / length, from.Y + dy * clamped / length);
}

void add_rounded_triangle(
    Gdiplus::GraphicsPath& path,
    const Gdiplus::PointF& a,
    const Gdiplus::PointF& b,
    const Gdiplus::PointF& c,
    float radius) {
    const Gdiplus::PointF points[3] = { a, b, c };
    Gdiplus::PointF current = point_toward(points[0], points[1], radius);
    path.StartFigure();
    for (int i = 0; i < 3; ++i) {
        const auto& vertex = points[(i + 1) % 3];
        const auto& previous = points[i];
        const auto& next = points[(i + 2) % 3];
        const auto before = point_toward(vertex, previous, radius);
        const auto after = point_toward(vertex, next, radius);
        path.AddLine(current, before);
        path.AddBezier(before, vertex, vertex, after);
        current = after;
    }
    path.CloseFigure();
}

void fill_rounded_play_shape(
    Gdiplus::Graphics& graphics,
    const Gdiplus::RectF& rect,
    const Gdiplus::Color& color,
    float radius) {
    Gdiplus::GraphicsPath path;
    add_rounded_triangle(
        path,
        Gdiplus::PointF(rect.X, rect.Y),
        Gdiplus::PointF(rect.X + rect.Width, rect.Y + rect.Height * 0.5f),
        Gdiplus::PointF(rect.X, rect.Y + rect.Height),
        radius);
    Gdiplus::SolidBrush brush(color);
    graphics.FillPath(&brush, &path);
}

void draw_fallback_player_logo(Gdiplus::Graphics& graphics, const Gdiplus::RectF& bounds) {
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    const float scale = std::min(bounds.Width / 190.0f, bounds.Height / 142.0f);
    const float origin_x = bounds.X + (bounds.Width - 190.0f * scale) * 0.5f;
    const float origin_y = bounds.Y + (bounds.Height - 142.0f * scale) * 0.5f;
    const auto map_rect = [&](float x, float y, float w, float h) {
        return Gdiplus::RectF(origin_x + x * scale, origin_y + y * scale, w * scale, h * scale);
    };

    const Gdiplus::Color charcoal(255, 32, 40, 48);
    const Gdiplus::Color slate(255, 48, 58, 68);
    const Gdiplus::Color green(255, 111, 198, 75);
    const Gdiplus::Color teal(255, 24, 158, 156);

    fill_rounded_play_shape(graphics, map_rect(18, 18, 44, 36), charcoal, 7.0f * scale);
    fill_rounded_play_shape(graphics, map_rect(46, 8, 118, 60), green, 10.0f * scale);
    fill_rounded_play_shape(graphics, map_rect(158, 30, 32, 32), charcoal, 6.0f * scale);
    fill_rounded_play_shape(graphics, map_rect(10, 58, 92, 64), teal, 10.0f * scale);
    fill_rounded_play_shape(graphics, map_rect(82, 74, 98, 66), slate, 11.0f * scale);

    Gdiplus::Pen white_pen(Gdiplus::Color(255, 245, 248, 248), 4.0f * scale);
    graphics.DrawLine(&white_pen, origin_x + 62.0f * scale, origin_y + 28.0f * scale, origin_x + 150.0f * scale, origin_y + 28.0f * scale);
    graphics.DrawLine(&white_pen, origin_x + 28.0f * scale, origin_y + 72.0f * scale, origin_x + 84.0f * scale, origin_y + 72.0f * scale);

    Gdiplus::FontFamily font_family(L"Segoe UI");
    Gdiplus::Font logo_font(&font_family, 40.0f * scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush text_brush(Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::StringFormat text_format;
    text_format.SetAlignment(Gdiplus::StringAlignmentCenter);
    text_format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    graphics.DrawString(L"DAT", -1, &logo_font, map_rect(50, 72, 116, 58), &text_format, &text_brush);
}

void draw_player_logo(Gdiplus::Graphics& graphics, const Gdiplus::RectF& bounds) {
    if (g_state.brand_logo) {
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.DrawImage(g_state.brand_logo.get(), bounds);
        return;
    }

    draw_fallback_player_logo(graphics, bounds);
}

LRESULT CALLBACK brand_header_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rect = {};
        GetClientRect(hwnd, &rect);

        Gdiplus::Graphics graphics(hdc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::SolidBrush background(Gdiplus::Color(255, 240, 240, 240));
        graphics.FillRectangle(&background, 0, 0, rect.right - rect.left, rect.bottom - rect.top);

        draw_player_logo(graphics, Gdiplus::RectF(14.0f, 7.0f, 50.0f, 50.0f));

        Gdiplus::FontFamily font_family(L"Segoe UI");
        Gdiplus::Font title_font(&font_family, 22.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font subtitle_font(&font_family, 12.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush title_brush(Gdiplus::Color(255, 35, 43, 51));
        Gdiplus::SolidBrush subtitle_brush(Gdiplus::Color(255, 88, 96, 104));
        graphics.DrawString(L"DAT Player", -1, &title_font, Gdiplus::PointF(76.0f, 12.0f), &title_brush);
        graphics.DrawString(
            L"Lightweight companion utility for compatible Mirasys/Spotter DAT files",
            -1,
            &subtitle_font,
            Gdiplus::PointF(78.0f, 40.0f),
            &subtitle_brush);

        Gdiplus::Pen divider(Gdiplus::Color(255, 210, 210, 210), 1.0f);
        graphics.DrawLine(&divider, 0, rect.bottom - 1, rect.right, rect.bottom - 1);
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

    case WM_PAINT: {
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
            Gdiplus::Graphics graphics(memory_dc);
            graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
            graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighSpeed);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            Gdiplus::Bitmap bitmap(
                static_cast<INT>(frame.display_width),
                static_cast<INT>(frame.display_height),
                static_cast<INT>(frame.stride_bytes),
                PixelFormat32bppRGB,
                const_cast<BYTE*>(frame.pixels.data()));
            graphics.DrawImage(
                &bitmap,
                Gdiplus::Rect(
                    draw_rect.left,
                    draw_rect.top,
                    draw_rect.right - draw_rect.left,
                    draw_rect.bottom - draw_rect.top),
                0,
                0,
                static_cast<INT>(frame.display_width),
                static_cast<INT>(frame.display_height),
                Gdiplus::UnitPixel);
        } else {
            SetBkMode(memory_dc, TRANSPARENT);
            SetTextColor(memory_dc, RGB(210, 210, 210));
            DrawTextW(memory_dc, L"No rendered frame", -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        BitBlt(hdc, 0, 0, width, height, memory_dc, 0, 0, SRCCOPY);
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
        return L"No DAT file loaded.\r\n\r\nOpen a compatible Mirasys/Spotter-style .dat file to index it.";
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
         << L"Keyframes: " << keys << L"\r\n"
         << L"Interframes: " << interframes << L"\r\n"
         << L"Timeline frame: " << (g_state.current_frame + 1) << L" / " << total;
    if (!g_state.decode_smoke_text.empty()) {
        text << L"\r\n\r\nDiagnostics:\r\n" << g_state.decode_smoke_text;
    }
    return text.str();
}

void update_info() {
    const auto info = build_info_text();
    SetWindowTextW(g_state.info_label, info.c_str());
    update_file_path_text();
    update_timeline();
}

void set_enabled_after_load(bool enabled) {
    EnableWindow(g_state.play_button, enabled);
    EnableWindow(g_state.decode_smoke_button, enabled);
    EnableWindow(g_state.render_first_frame_button, enabled);
    EnableWindow(g_state.timeline, enabled);
}

void reset_loaded_state() {
    stop_playback();
    g_state.index = {};
    g_state.loaded_path.clear();
    g_state.decode_smoke_text.clear();
    g_state.rendered_frame = {};
    g_state.frames_rendered = 0;
    g_state.frames_decoded = 0;
    g_state.late_frames = 0;
    g_state.effective_playback_fps = 0.0;
    g_state.frame_interval_ms = 0.0;
    g_state.current_frame = 0;
    g_state.timeline_dragging = false;
    g_state.resume_after_timeline_drag = false;
    g_state.seeking = false;
    set_enabled_after_load(false);
    if (g_state.video_panel) {
        InvalidateRect(g_state.video_panel, nullptr, TRUE);
    }
    update_info();
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

void load_dat_file(HWND owner) {
    stop_playback();
    const auto path = ask_for_dat_file(owner);
    if (path.empty()) {
        return;
    }

    if (!has_dat_extension(path)) {
        MessageBoxW(owner, L"Only .dat files are supported in this phase.", L"Unsupported File", MB_OK | MB_ICONWARNING);
        return;
    }

    try {
        set_status(L"Indexing DAT file...");
        dat_player::DatFrameIndexer indexer;
        auto index = indexer.index_file(path);

        g_state.index = std::move(index);
        g_state.loaded_path = path;
        g_state.decode_smoke_text.clear();
        g_state.rendered_frame = {};
        g_state.frames_rendered = 0;
        g_state.frames_decoded = 0;
        g_state.late_frames = 0;
        g_state.effective_playback_fps = 0.0;
        g_state.frame_interval_ms = 0.0;
        g_state.current_frame = 0;
        g_state.timeline_dragging = false;
        g_state.resume_after_timeline_drag = false;
        g_state.seeking = false;
        set_enabled_after_load(!g_state.index.frames.empty());
        if (g_state.video_panel) {
            InvalidateRect(g_state.video_panel, nullptr, TRUE);
        }
        update_info();

        if (g_state.index.frames.empty()) {
            set_status(L"No valid H264/I264 frame records were found.");
        } else {
            set_status(L"Loaded index. Simple forward playback and diagnostics are available.");
        }
    } catch (const std::exception& ex) {
        reset_loaded_state();
        const auto message = L"Unable to index DAT file:\r\n" + widen(ex.what());
        MessageBoxW(owner, message.c_str(), L"Index Error", MB_OK | MB_ICONERROR);
        set_status(L"Indexing failed.");
    }
}

std::wstring format_decode_smoke_result(const dat_player::playback::DecodeSmokeTestResult& result) {
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

void run_decode_smoke_test() {
    if (g_state.loaded_path.empty() || g_state.index.frames.empty()) {
        MessageBoxW(g_state.hwnd, L"Open and index a .dat file before running the decode smoke test.", L"Decode Smoke Test", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const bool was_playing = g_state.playing;
    stop_playback();
    set_status(L"Running Media Foundation decode smoke test...");
    dat_player::playback::H264DecodeSmokeTester tester;
    const auto result = tester.run(g_state.loaded_path, g_state.index);
    g_state.decode_smoke_text = format_decode_smoke_result(result);
    update_info();
    set_status(result.decoded_any_frame
        ? (was_playing ? L"Playback stopped; decode smoke test produced decoded samples." : L"Decode smoke test produced decoded samples.")
        : L"Decode smoke test did not produce decoded samples.");
}

std::wstring format_render_result(const dat_player::playback::FirstFrameRenderResult& result) {
    std::wostringstream text;
    text << L"First-frame render smoke test\r\n"
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

void run_first_frame_render_smoke_test() {
    if (g_state.loaded_path.empty() || g_state.index.frames.empty()) {
        MessageBoxW(g_state.hwnd, L"Open and index a .dat file before rendering the first frame.", L"Render First Frame", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const bool was_playing = g_state.playing;
    stop_playback();
    set_status(L"Rendering first decoded frame...");
    dat_player::playback::H264DecodeSmokeTester tester;
    const auto result = tester.render_first_frame(g_state.loaded_path, g_state.index);
    g_state.rendered_frame = result.frame;
    g_state.decode_smoke_text = format_render_result(result);
    if (g_state.video_panel) {
        InvalidateRect(g_state.video_panel, nullptr, TRUE);
    }
    update_info();
    set_status(result.frame_available
        ? (was_playing ? L"Playback stopped; first decoded frame rendered." : L"First decoded frame rendered.")
        : L"First-frame render smoke test failed.");
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
         << L"Effective playback FPS: " << format_double(g_state.effective_playback_fps, 2) << L"\r\n"
         << L"Frame interval: " << format_double(g_state.frame_interval_ms, 2) << L" ms\r\n";
    if (!g_state.index.frames.empty() && g_state.current_frame < frame_count()) {
        const auto timestamp = g_state.index.frames[static_cast<std::size_t>(g_state.current_frame)].timestamp;
        const double seconds = static_cast<double>(timestamp - g_state.index.frames.front().timestamp) / 39062.5;
        text << L"Current timestamp: " << timestamp << L"\r\n"
             << L"Approx time: " << format_double(seconds, 2) << L" sec\r\n";
    }
    text << L"Timing: timestamp deltas, fallback " << format_double(playback_fps(), 2) << L" FPS";
    return text.str();
}

std::wstring format_seek_diagnostics(
    const std::wstring& state,
    std::uint64_t requested_frame,
    std::uint64_t keyframe_frame,
    std::uint64_t frames_decoded,
    const std::wstring& result_text = L"") {
    std::wostringstream text;
    text << L"Scrub seek\r\n"
         << L"State: " << state << L"\r\n"
         << L"Requested: " << frame_time_label(requested_frame) << L"\r\n"
         << L"Keyframe used: " << (keyframe_frame + 1) << L"\r\n"
         << L"Frames decoded during seek: " << frames_decoded << L"\r\n";
    if (!result_text.empty()) {
        text << L"Result: " << result_text;
    }
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
    update_play_button();
    g_state.decode_smoke_text = format_seek_diagnostics(L"seeking", clamped_target, keyframe, 0);
    update_info();
    const auto seek_status = L"Seeking to " + frame_time_label(clamped_target) + L"...";
    set_status(seek_status);

    const HWND hwnd = g_state.hwnd;
    const auto path = g_state.loaded_path;
    const auto index = g_state.index;

    g_state.playback_thread = std::thread([hwnd, path, index, clamped_target, keyframe, generation, resume_after_seek]() {
        dat_player::playback::H264DecodeSmokeTester tester;
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
    g_state.effective_playback_fps = 0.0;
    g_state.frame_interval_ms = 1000.0 / playback_fps();
    g_state.stop_playback_requested = false;
    const std::uint64_t generation = ++g_state.playback_generation;
    g_state.playing = true;
    update_play_button();
    set_status(L"Starting forward playback...");
    g_state.decode_smoke_text = format_playback_diagnostics(L"starting");
    update_info();

    const HWND hwnd = g_state.hwnd;
    const auto path = g_state.loaded_path;
    const auto index = g_state.index;
    const auto start_frame = g_state.current_frame;
    const double fallback_fps = playback_fps();
    const double fallback_interval_ms = 1000.0 / fallback_fps;

    g_state.playback_thread = std::thread([hwnd, path, index, start_frame, fallback_fps, fallback_interval_ms, generation]() {
        dat_player::playback::H264DecodeSmokeTester tester;
        dat_player::playback::ForwardPlaybackOptions options;
        options.start_frame = start_frame;
        options.fallback_fps = fallback_fps;

        auto playback_started = std::chrono::steady_clock::now();
        auto last_post_time = playback_started;
        bool clock_started = false;
        std::uint64_t rendered_callbacks = 0;
        std::uint64_t late_frames = 0;

        const auto result = tester.play_forward(path, index, options, [&](dat_player::playback::ForwardPlaybackFrame&& frame) {
            if (g_state.stop_playback_requested || g_state.playback_generation.load() != generation) {
                return false;
            }

            const double media_seconds = media_seconds_from_start(index, start_frame, frame.frame_index, fallback_fps);
            const auto media_offset = std::chrono::microseconds(
                static_cast<long long>(std::max(0.0, media_seconds) * 1000000.0));
            if (!clock_started) {
                playback_started = std::chrono::steady_clock::now() - media_offset;
                last_post_time = std::chrono::steady_clock::now();
                clock_started = true;
            }
            const auto due_time = playback_started + media_offset;
            while (!g_state.stop_playback_requested &&
                   g_state.playback_generation.load() == generation &&
                   std::chrono::steady_clock::now() + std::chrono::milliseconds(1) < due_time) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            if (g_state.stop_playback_requested || g_state.playback_generation.load() != generation) {
                return false;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now > due_time + std::chrono::milliseconds(20)) {
                ++late_frames;
            }
            ++rendered_callbacks;
            const double elapsed_seconds = std::chrono::duration<double>(now - playback_started).count();
            const double effective_fps = elapsed_seconds > 0.0
                ? static_cast<double>(rendered_callbacks) / elapsed_seconds
                : 0.0;
            const double frame_interval_ms = rendered_callbacks > 1
                ? std::chrono::duration<double, std::milli>(now - last_post_time).count()
                : fallback_interval_ms;
            last_post_time = now;

            auto ui_frame = std::make_unique<UiPlaybackFrame>();
            ui_frame->frame = std::move(frame.frame);
            ui_frame->generation = generation;
            ui_frame->frame_index = frame.frame_index;
            ui_frame->timestamp = frame.timestamp;
            ui_frame->frames_submitted = frame.frames_submitted;
            ui_frame->frames_decoded = frame.frames_decoded;
            ui_frame->late_frames = late_frames;
            ui_frame->effective_fps = effective_fps;
            ui_frame->frame_interval_ms = frame_interval_ms;
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

    if (g_state.playing) {
        stop_playback();
        g_state.decode_smoke_text = format_playback_diagnostics(L"paused");
        update_info();
        set_status(L"Playback paused.");
    } else {
        start_forward_playback();
    }
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
    const int header_height = 64;
    const int file_row_height = 30;
    const int file_label_width = 64;
    const int open_button_width = 110;
    const int play_button_width = 104;
    const int smoke_button_width = 132;
    const int render_button_width = 132;
    const int button_height = 32;
    const int timeline_height = 34;
    const int status_height = 24;
    const int file_top = header_height + 12;
    const int content_top = file_top + file_row_height + 14;
    const int status_top = rect.bottom - status_height - 6;
    const int content_bottom = status_top - 10;
    const int details_width = std::clamp(width / 3, 320, 370);
    const int video_width = std::max(240, width - padding * 3 - details_width);
    const int content_height = std::max(220, content_bottom - content_top);
    const int controls_height = button_height + 8;
    const int video_height = std::max(120, content_height - timeline_height - controls_height - 18);
    const int timeline_top = content_top + video_height + 8;
    const int controls_top = timeline_top + timeline_height + 4;
    const int details_left = padding + video_width + padding;
    const int details_inner_left = details_left + 12;
    const int details_inner_top = content_top + 24;
    const int details_inner_width = details_width - 24;
    const int details_buttons_top = content_bottom - button_height - 12;
    const int details_text_height = std::max(80, details_buttons_top - details_inner_top - 10);

    MoveWindow(g_state.brand_header, 0, 0, width, header_height, TRUE);
    MoveWindow(g_state.file_label, padding, file_top + 5, file_label_width, 22, TRUE);
    MoveWindow(g_state.file_path_edit, padding + file_label_width, file_top, std::max(80, width - padding * 3 - file_label_width - open_button_width), file_row_height, TRUE);
    MoveWindow(g_state.open_button, width - padding - open_button_width, file_top - 1, open_button_width, button_height, TRUE);
    MoveWindow(g_state.video_panel, padding, content_top, video_width, video_height, TRUE);
    MoveWindow(g_state.timeline, padding, timeline_top, video_width, timeline_height, TRUE);
    MoveWindow(g_state.play_button, padding, controls_top, play_button_width, button_height, TRUE);
    MoveWindow(g_state.details_group, details_left, content_top, details_width, content_height, TRUE);
    MoveWindow(g_state.info_label, details_inner_left, details_inner_top, details_inner_width, details_text_height, TRUE);
    MoveWindow(g_state.decode_smoke_button, details_inner_left, details_buttons_top, smoke_button_width, button_height, TRUE);
    MoveWindow(g_state.render_first_frame_button, details_inner_left + smoke_button_width + 8, details_buttons_top, render_button_width, button_height, TRUE);
    MoveWindow(g_state.status_label, padding, status_top, width - padding * 2, status_height, TRUE);
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

        g_state.brand_header = CreateWindowW(L"DATBrandHeader", L"", WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBrandHeaderId)), nullptr, nullptr);
        g_state.file_label = CreateWindowW(L"STATIC", L"DAT file:", WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFileLabelId)), nullptr, nullptr);
        g_state.file_path_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"No .dat file selected",
            WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL | ES_READONLY,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFilePathId)), nullptr, nullptr);
        g_state.open_button = CreateWindowW(L"BUTTON", L"Open .dat", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOpenButtonId)), nullptr, nullptr);
        g_state.play_button = CreateWindowW(L"BUTTON", L"Play", WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPlayButtonId)), nullptr, nullptr);
        g_state.details_group = CreateWindowW(L"BUTTON", L"Details / Diagnostics", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDetailsGroupId)), nullptr, nullptr);
        g_state.decode_smoke_button = CreateWindowW(L"BUTTON", L"Decode Test", WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDecodeSmokeButtonId)), nullptr, nullptr);
        g_state.render_first_frame_button = CreateWindowW(L"BUTTON", L"Render Frame", WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRenderFirstFrameButtonId)), nullptr, nullptr);
        g_state.timeline = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_DISABLED | TBS_AUTOTICKS,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTimelineId)), nullptr, nullptr);
        g_state.video_panel = CreateWindowW(L"DATVideoPanel", L"", WS_CHILD | WS_VISIBLE | WS_BORDER,
            0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        g_state.info_label = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInfoLabelId)), nullptr, nullptr);
        g_state.status_label = CreateWindowW(L"STATIC", L"Ready. Open a compatible .dat file to begin.", WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusLabelId)), nullptr, nullptr);

        apply_default_font(g_state.brand_header);
        apply_default_font(g_state.file_label);
        apply_default_font(g_state.file_path_edit);
        apply_default_font(g_state.open_button);
        apply_default_font(g_state.play_button);
        apply_default_font(g_state.details_group);
        apply_default_font(g_state.decode_smoke_button);
        apply_default_font(g_state.render_first_frame_button);
        apply_default_font(g_state.info_label);
        apply_default_font(g_state.status_label);

        SendMessageW(g_state.timeline, TBM_SETRANGE, TRUE, MAKELPARAM(0, kTrackbarMax));
        SendMessageW(g_state.timeline, TBM_SETTICFREQ, 1000, 0);
        update_info();
        layout_controls(hwnd);
        return 0;

    case WM_SIZE:
        layout_controls(hwnd);
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wparam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(35, 43, 51));
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
        case kDecodeSmokeButtonId:
            run_decode_smoke_test();
            return 0;
        case kRenderFirstFrameButtonId:
            run_first_frame_render_smoke_test();
            return 0;
        default:
            break;
        }
        break;

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
                }
                const int position = static_cast<int>(SendMessageW(g_state.timeline, TBM_GETPOS, 0, 0));
                const auto target = frame_from_timeline_position(position);
                g_state.decode_smoke_text = format_seek_diagnostics(
                    g_state.resume_after_timeline_drag ? L"dragging, will resume" : L"dragging",
                    target,
                    nearest_previous_keyframe(target),
                    0);
                SetWindowTextW(g_state.info_label, build_info_text().c_str());
                set_status(L"Release the timeline to seek.");
                return 0;
            }

            if (code == TB_ENDTRACK || code == TB_THUMBPOSITION) {
                const int position = static_cast<int>(SendMessageW(g_state.timeline, TBM_GETPOS, 0, 0));
                const auto target = frame_from_timeline_position(position);
                const bool resume = g_state.timeline_dragging && g_state.resume_after_timeline_drag;
                g_state.timeline_dragging = false;
                g_state.resume_after_timeline_drag = false;
                start_seek_to_frame(target, resume);
                return 0;
            }

            if (g_state.playing || g_state.seeking) {
                stop_playback();
            }
            const int position = static_cast<int>(SendMessageW(g_state.timeline, TBM_GETPOS, 0, 0));
            const auto target = frame_from_timeline_position(position);
            start_seek_to_frame(target, false);
            return 0;
        }
        break;

    case kPlaybackFrameMessage: {
        std::unique_ptr<UiPlaybackFrame> frame(reinterpret_cast<UiPlaybackFrame*>(lparam));
        if (frame && frame->generation == g_state.playback_generation.load()) {
            g_state.rendered_frame = std::move(frame->frame);
            g_state.current_frame = frame->frame_index;
            g_state.frames_decoded = frame->frames_decoded;
            ++g_state.frames_rendered;
            if (!frame->seek_frame) {
                g_state.late_frames = frame->late_frames;
                g_state.effective_playback_fps = frame->effective_fps;
                g_state.frame_interval_ms = frame->frame_interval_ms;
                g_state.decode_smoke_text = format_playback_diagnostics(L"playing");
            }
            if (g_state.video_panel) {
                InvalidateRect(g_state.video_panel, nullptr, FALSE);
            }
            update_info();
            std::wostringstream status;
            status << L"Playing frame " << (g_state.current_frame + 1) << L" / " << frame_count()
                   << L" decoded=" << g_state.frames_decoded
                   << L" rendered=" << g_state.frames_rendered;
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
        g_state.decode_smoke_text = format_playback_diagnostics(completed ? L"completed" : L"stopped");
        if (!playback_message->text.empty()) {
            g_state.decode_smoke_text += L"\r\nResult: " + playback_message->text;
        }
        update_info();
        set_status(completed ? L"Playback completed or reached decoder end." : L"Playback stopped.");
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
        update_play_button();
        const bool seek_ok = seek_message->decoded_any_frame && !g_state.stop_playback_requested;
        g_state.decode_smoke_text = format_seek_diagnostics(
            seek_ok ? L"completed" : L"failed",
            seek_message->requested_frame,
            seek_message->keyframe_frame,
            seek_message->frames_decoded,
            seek_message->text);
        update_info();

        if (seek_ok && seek_message->resume_after_seek) {
            set_status(L"Seek completed; resuming playback.");
            start_forward_playback();
        } else {
            set_status(seek_ok ? L"Seek completed." : L"Seek failed; keeping last rendered frame.");
        }
        return 0;
    }

    case WM_DESTROY:
        stop_playback();
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

    Gdiplus::GdiplusStartupInput gdiplus_input;
    if (Gdiplus::GdiplusStartup(&g_state.gdiplus_token, &gdiplus_input, nullptr) != Gdiplus::Ok) {
        MessageBoxW(nullptr, L"Unable to initialize DAT Player graphics.", L"DAT Player", MB_OK | MB_ICONERROR);
        return 1;
    }

    g_state.window_background_brush = CreateSolidBrush(RGB(240, 240, 240));

    const wchar_t class_name[] = L"DATPlayerShellWindow";
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
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
        Gdiplus::GdiplusShutdown(g_state.gdiplus_token);
        return 1;
    }

    WNDCLASSEXW header_class = {};
    header_class.cbSize = sizeof(header_class);
    header_class.lpfnWndProc = brand_header_proc;
    header_class.hInstance = instance;
    header_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    header_class.hbrBackground = g_state.window_background_brush;
    header_class.lpszClassName = L"DATBrandHeader";

    if (!RegisterClassExW(&header_class)) {
        MessageBoxW(nullptr, L"Unable to register DAT Player header.", L"DAT Player", MB_OK | MB_ICONERROR);
        Gdiplus::GdiplusShutdown(g_state.gdiplus_token);
        return 1;
    }

    WNDCLASSEXW video_class = {};
    video_class.cbSize = sizeof(video_class);
    video_class.lpfnWndProc = video_panel_proc;
    video_class.hInstance = instance;
    video_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    video_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    video_class.lpszClassName = L"DATVideoPanel";

    if (!RegisterClassExW(&video_class)) {
        MessageBoxW(nullptr, L"Unable to register DAT Player video panel.", L"DAT Player", MB_OK | MB_ICONERROR);
        Gdiplus::GdiplusShutdown(g_state.gdiplus_token);
        return 1;
    }

    g_state.brand_logo = load_png_resource(instance, IDR_DATPLAYER_LOGO);

    HWND hwnd = CreateWindowExW(
        0,
        class_name,
        L"DAT Player",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1000,
        700,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!hwnd) {
        MessageBoxW(nullptr, L"Unable to create DAT Player window.", L"DAT Player", MB_OK | MB_ICONERROR);
        Gdiplus::GdiplusShutdown(g_state.gdiplus_token);
        return 1;
    }

    ShowWindow(hwnd, show_command);
    UpdateWindow(hwnd);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_state.gdiplus_token != 0) {
        Gdiplus::GdiplusShutdown(g_state.gdiplus_token);
        g_state.gdiplus_token = 0;
    }

    return static_cast<int>(message.wParam);
}
