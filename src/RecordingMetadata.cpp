#include "dat_player/RecordingMetadata.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>

namespace dat_player {
namespace {

constexpr std::uint64_t kTicksPerSecond = 10'000'000;
constexpr std::uint64_t kTicksPerMillisecond = 10'000;
constexpr std::uint64_t kTicksPerMicrosecond = 10;
constexpr std::uint64_t kPlausibleStartTicks = 630822816000000000ULL; // 2000-01-01
constexpr std::uint64_t kPlausibleEndTicks = 662380416000000000ULL; // 2100-01-01

bool is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month(int year, int month) {
    constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return days[month - 1];
}

std::uint64_t days_before_year(int year) {
    const auto previous_year = static_cast<std::uint64_t>(year - 1);
    return previous_year * 365 + previous_year / 4 - previous_year / 100 + previous_year / 400;
}

std::uint64_t days_before_month(int year, int month) {
    std::uint64_t days = 0;
    for (int current = 1; current < month; ++current) {
        days += static_cast<std::uint64_t>(days_in_month(year, current));
    }
    return days;
}

bool date_is_valid(int year, int month, int day) {
    return year >= 1 && year <= 9999 && month >= 1 && month <= 12 && day >= 1 && day <= days_in_month(year, month);
}

std::uint64_t days_from_date(int year, int month, int day) {
    return days_before_year(year) + days_before_month(year, month) + static_cast<std::uint64_t>(day - 1);
}

bool parse_int(const std::string& text, std::size_t offset, std::size_t length, int& value) {
    if (offset + length > text.size()) {
        return false;
    }
    int result = 0;
    for (std::size_t i = 0; i < length; ++i) {
        const char ch = text[offset + i];
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
        result = result * 10 + (ch - '0');
    }
    value = result;
    return true;
}

std::string xml_decode(std::string text) {
    struct Replacement {
        const char* from;
        const char* to;
    };
    constexpr Replacement replacements[] = {
        {"&quot;", "\""},
        {"&apos;", "'"},
        {"&lt;", "<"},
        {"&gt;", ">"},
        {"&amp;", "&"},
    };
    for (const auto& replacement : replacements) {
        std::size_t pos = 0;
        while ((pos = text.find(replacement.from, pos)) != std::string::npos) {
            text.replace(pos, std::char_traits<char>::length(replacement.from), replacement.to);
            pos += std::char_traits<char>::length(replacement.to);
        }
    }
    return text;
}

std::string find_tag_text(const std::string& xml, const std::string& tag) {
    const auto start_tag = "<" + tag + ">";
    const auto end_tag = "</" + tag + ">";
    const auto start = xml.find(start_tag);
    if (start == std::string::npos) {
        return {};
    }
    const auto value_start = start + start_tag.size();
    const auto end = xml.find(end_tag, value_start);
    if (end == std::string::npos) {
        return {};
    }
    return xml_decode(xml.substr(value_start, end - value_start));
}

std::string find_attribute(const std::string& element, const std::string& name) {
    const auto pattern = name + "=\"";
    const auto start = element.find(pattern);
    if (start == std::string::npos) {
        return {};
    }
    const auto value_start = start + pattern.size();
    const auto end = element.find('"', value_start);
    if (end == std::string::npos) {
        return {};
    }
    return xml_decode(element.substr(value_start, end - value_start));
}

std::string find_first_channel_element(const std::string& xml) {
    const auto start = xml.find("<channel ");
    if (start == std::string::npos) {
        return {};
    }
    const auto end = xml.find('>', start);
    if (end == std::string::npos) {
        return {};
    }
    return xml.substr(start, end - start + 1);
}

int base64_value(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    if (ch == '+') {
        return 62;
    }
    if (ch == '/') {
        return 63;
    }
    return -1;
}

std::vector<unsigned char> base64_decode_bytes(const std::string& value) {
    std::vector<unsigned char> output;
    int accumulator = 0;
    int bits = -8;
    for (char ch : value) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            continue;
        }
        if (ch == '=') {
            break;
        }
        const int decoded = base64_value(ch);
        if (decoded < 0) {
            return {};
        }
        accumulator = (accumulator << 6) | decoded;
        bits += 6;
        if (bits >= 0) {
            output.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xff));
            bits -= 8;
        }
    }
    return output;
}

std::string base64_decode_utf8(const std::string& value) {
    const auto bytes = base64_decode_bytes(value);
    return std::string(bytes.begin(), bytes.end());
}

std::int64_t read_i64_le(const std::vector<unsigned char>& data, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(data[offset + i]) << (i * 8);
    }
    return static_cast<std::int64_t>(value);
}

std::vector<int> timezone_offsets_from_blob(const std::string& base64_blob) {
    const auto blob = base64_decode_bytes(base64_blob);
    std::vector<int> offsets;
    if (blob.size() < 8) {
        return offsets;
    }

    for (std::size_t offset = 0; offset + 8 <= blob.size(); ++offset) {
        const auto ticks = read_i64_le(blob, offset);
        constexpr std::int64_t max_offset_ticks = 14LL * 60LL * 60LL * static_cast<std::int64_t>(kTicksPerSecond);
        if (ticks < -max_offset_ticks || ticks > max_offset_ticks || ticks % (60LL * static_cast<std::int64_t>(kTicksPerSecond)) != 0) {
            continue;
        }
        const auto minutes = static_cast<int>(ticks / (60LL * static_cast<std::int64_t>(kTicksPerSecond)));
        if (std::find(offsets.begin(), offsets.end(), minutes) == offsets.end()) {
            offsets.push_back(minutes);
        }
    }
    return offsets;
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

} // namespace

bool is_plausible_dotnet_ticks(std::uint64_t ticks) {
    return ticks >= kPlausibleStartTicks && ticks <= kPlausibleEndTicks;
}

bool parse_dotnet_iso_ticks(const std::string& value, std::uint64_t& ticks) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!parse_int(value, 0, 4, year) ||
        value.size() < 19 ||
        value[4] != '-' ||
        !parse_int(value, 5, 2, month) ||
        value[7] != '-' ||
        !parse_int(value, 8, 2, day) ||
        (value[10] != 'T' && value[10] != ' ') ||
        !parse_int(value, 11, 2, hour) ||
        value[13] != ':' ||
        !parse_int(value, 14, 2, minute) ||
        value[16] != ':' ||
        !parse_int(value, 17, 2, second)) {
        return false;
    }
    if (!date_is_valid(year, month, day) || hour > 23 || minute > 59 || second > 60) {
        return false;
    }

    std::uint64_t fraction_ticks = 0;
    if (value.size() > 19 && value[19] == '.') {
        std::size_t pos = 20;
        int digits = 0;
        while (pos < value.size() && std::isdigit(static_cast<unsigned char>(value[pos])) && digits < 7) {
            fraction_ticks = fraction_ticks * 10 + static_cast<std::uint64_t>(value[pos] - '0');
            ++pos;
            ++digits;
        }
        while (digits < 7) {
            fraction_ticks *= 10;
            ++digits;
        }
    }

    const auto days = days_from_date(year, month, day);
    ticks = days * 24ULL * 60ULL * 60ULL * kTicksPerSecond +
        static_cast<std::uint64_t>(hour) * 60ULL * 60ULL * kTicksPerSecond +
        static_cast<std::uint64_t>(minute) * 60ULL * kTicksPerSecond +
        static_cast<std::uint64_t>(second) * kTicksPerSecond +
        fraction_ticks;
    return true;
}

bool dotnet_ticks_to_parts(std::uint64_t ticks, RecordingDateTimeParts& parts) {
    constexpr std::uint64_t ticks_per_day = 24ULL * 60ULL * 60ULL * kTicksPerSecond;
    auto days = ticks / ticks_per_day;
    auto remainder = ticks % ticks_per_day;
    int year = 1;
    while (year <= 9999) {
        const auto year_days = static_cast<std::uint64_t>(is_leap_year(year) ? 366 : 365);
        if (days < year_days) {
            break;
        }
        days -= year_days;
        ++year;
    }
    if (year < 1 || year > 9999) {
        return false;
    }
    int month = 1;
    while (month <= 12) {
        const auto month_days = static_cast<std::uint64_t>(days_in_month(year, month));
        if (days < month_days) {
            break;
        }
        days -= month_days;
        ++month;
    }
    if (month < 1 || month > 12) {
        return false;
    }

    parts.year = year;
    parts.month = month;
    parts.day = static_cast<int>(days) + 1;
    parts.hour = static_cast<int>(remainder / (60ULL * 60ULL * kTicksPerSecond));
    remainder %= 60ULL * 60ULL * kTicksPerSecond;
    parts.minute = static_cast<int>(remainder / (60ULL * kTicksPerSecond));
    remainder %= 60ULL * kTicksPerSecond;
    parts.second = static_cast<int>(remainder / kTicksPerSecond);
    remainder %= kTicksPerSecond;
    parts.millisecond = static_cast<int>(remainder / kTicksPerMillisecond);
    parts.microsecond = static_cast<int>(remainder / kTicksPerMicrosecond);
    return true;
}

std::string format_dotnet_ticks(std::uint64_t ticks) {
    RecordingDateTimeParts parts;
    if (!dotnet_ticks_to_parts(ticks, parts)) {
        return {};
    }

    std::ostringstream text;
    text.fill('0');
    text << parts.year << '-'
         << std::setw(2) << parts.month << '-'
         << std::setw(2) << parts.day << ' '
         << std::setw(2) << parts.hour << ':'
         << std::setw(2) << parts.minute << ':'
         << std::setw(2) << parts.second << '.'
         << std::setw(3) << parts.millisecond;
    return text.str();
}

std::string to_string(RecordingMetadataConfidence confidence) {
    switch (confidence) {
    case RecordingMetadataConfidence::High:
        return "High";
    case RecordingMetadataConfidence::Medium:
        return "Medium";
    case RecordingMetadataConfidence::Low:
        return "Low";
    default:
        return "None";
    }
}

RecordingSidecarMetadata parse_sef2_metadata_xml(const std::string& xml) {
    RecordingSidecarMetadata metadata;
    if (xml.empty()) {
        return metadata;
    }

    std::uint64_t ticks = 0;
    const auto start = find_tag_text(xml, "start");
    if (!start.empty() && parse_dotnet_iso_ticks(start, ticks)) {
        metadata.start_ticks = ticks;
        metadata.has_start_ticks = true;
        metadata.available = true;
    }
    const auto end = find_tag_text(xml, "end");
    if (!end.empty() && parse_dotnet_iso_ticks(end, ticks)) {
        metadata.end_ticks = ticks;
        metadata.has_end_ticks = true;
        metadata.available = true;
    }

    const auto channel = find_first_channel_element(xml);
    if (!channel.empty()) {
        const auto encoded_name = find_attribute(channel, "name");
        if (!encoded_name.empty()) {
            const auto decoded = base64_decode_utf8(encoded_name);
            metadata.camera_name = decoded.empty() ? encoded_name : decoded;
            metadata.available = true;
        }
        metadata.manufacturer = find_attribute(channel, "manufacturer");
        metadata.model = find_attribute(channel, "model");
        const auto timezone = find_attribute(channel, "timezone");
        if (!timezone.empty()) {
            metadata.timezone_offset_minutes_candidates = timezone_offsets_from_blob(timezone);
        }
        if (!metadata.manufacturer.empty() || !metadata.model.empty() || !metadata.timezone_offset_minutes_candidates.empty()) {
            metadata.available = true;
        }
    }

    return metadata;
}

RecordingSidecarMetadata parse_sef2_metadata_file(const std::filesystem::path& path) {
    auto metadata = parse_sef2_metadata_xml(read_text_file(path));
    if (metadata.available) {
        metadata.path = path;
    }
    return metadata;
}

RecordingSidecarMetadata try_load_sef2_sidecar(const std::filesystem::path& dat_path) {
    std::error_code ec;
    const auto folder = dat_path.parent_path();
    if (folder.empty() || !std::filesystem::is_directory(folder, ec) || ec) {
        return {};
    }

    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (ec) {
            return {};
        }
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        auto extension = entry.path().extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        if (extension == L".sef2") {
            auto metadata = parse_sef2_metadata_file(entry.path());
            if (metadata.available) {
                return metadata;
            }
        }
    }

    return {};
}

} // namespace dat_player
