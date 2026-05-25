#include "dat_player/RecordingMetadata.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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

std::string find_first_file_element(const std::string& xml) {
    const auto start = xml.find("<file ");
    if (start == std::string::npos) {
        return {};
    }
    const auto end = xml.find('>', start);
    if (end == std::string::npos) {
        return {};
    }
    return xml.substr(start, end - start + 1);
}

bool contains_xml_signature(const std::string& xml) {
    return xml.find("<Signature") != std::string::npos &&
        xml.find("http://www.w3.org/2000/09/xmldsig#") != std::string::npos;
}

std::string find_signature_algorithm(const std::string& xml, const std::string& element_name) {
    const auto tag = "<" + element_name;
    const auto start = xml.find(tag);
    if (start == std::string::npos) {
        return {};
    }
    const auto end = xml.find('>', start);
    if (end == std::string::npos) {
        return {};
    }
    return find_attribute(xml.substr(start, end - start + 1), "Algorithm");
}

bool parse_u64_decimal(const std::string& text, std::uint64_t& value) {
    if (text.empty()) {
        return false;
    }

    std::uint64_t result = 0;
    for (const char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10ULL) {
            return false;
        }
        result = result * 10ULL + digit;
    }
    value = result;
    return true;
}

std::string lower_ascii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

bool filename_matches(const std::filesystem::path& selected_dat, const std::string& referenced_dat_filename) {
    if (referenced_dat_filename.empty()) {
        return false;
    }

    return lower_ascii(selected_dat.filename().string()) ==
        lower_ascii(std::filesystem::path(referenced_dat_filename).filename().string());
}

void apply_sidecar_consistency_checks(RecordingSidecarMetadata& metadata, const std::filesystem::path& selected_dat_path) {
    if (!metadata.available) {
        return;
    }

    if (!metadata.referenced_dat_filename.empty()) {
        metadata.sidecar_references_selected_dat = filename_matches(selected_dat_path, metadata.referenced_dat_filename);
        metadata.references_different_dat = !metadata.sidecar_references_selected_dat;

        const auto referenced_path = selected_dat_path.parent_path() / std::filesystem::path(metadata.referenced_dat_filename).filename();
        std::error_code ec;
        metadata.referenced_dat_exists = std::filesystem::is_regular_file(referenced_path, ec) && !ec;
        if (metadata.references_different_dat) {
            metadata.integrity_warnings.push_back("SEF2 data references a different DAT filename.");
        }
        if (!metadata.referenced_dat_exists) {
            metadata.integrity_warnings.push_back("SEF2 referenced DAT file was not found next to the selected DAT.");
        }
    }

    if (metadata.has_data_size_bytes) {
        std::error_code ec;
        const auto selected_dat_size = std::filesystem::file_size(selected_dat_path, ec);
        if (!ec) {
            metadata.dat_size_plausible = selected_dat_size >= metadata.data_size_bytes;
            if (!metadata.dat_size_plausible) {
                metadata.integrity_warnings.push_back("Selected DAT file is smaller than the SEF2 channel data size.");
            }
        }
    }
}

std::filesystem::path verifier_from_environment() {
#ifdef _WIN32
    char* verifier = nullptr;
    std::size_t verifier_length = 0;
    if (_dupenv_s(&verifier, &verifier_length, "DAT_PLAYER_SEF2_VERIFIER") != 0 || verifier == nullptr) {
        return {};
    }
    std::filesystem::path path(verifier);
    std::free(verifier);
    return path;
#else
    const char* verifier = std::getenv("DAT_PLAYER_SEF2_VERIFIER");
    return verifier == nullptr ? std::filesystem::path{} : std::filesystem::path(verifier);
#endif
}

std::filesystem::path default_verifier_path() {
#ifdef _WIN32
    wchar_t module_path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, module_path, static_cast<DWORD>(std::size(module_path)));
    if (length > 0 && length < std::size(module_path)) {
        return std::filesystem::path(module_path).parent_path() / "DatPlayer.Sef2SignatureVerifier.exe";
    }
    return std::filesystem::current_path() / "DatPlayer.Sef2SignatureVerifier.exe";
#else
    return {};
#endif
}

#ifdef _WIN32
std::wstring quote_process_arg(const std::filesystem::path& path) {
    std::wstring value = path.wstring();
    std::wstring quoted = L"\"";
    for (const wchar_t ch : value) {
        if (ch == L'"') {
            quoted += L'\\';
        }
        quoted += ch;
    }
    quoted += L"\"";
    return quoted;
}

std::string read_all_from_pipe(HANDLE pipe) {
    std::string output;
    std::array<char, 512> buffer{};
    DWORD bytes_read = 0;
    while (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr) && bytes_read > 0) {
        output.append(buffer.data(), buffer.data() + bytes_read);
    }
    return output;
}
#endif

std::map<std::string, std::string> parse_key_value_lines(const std::string& text) {
    std::map<std::string, std::string> values;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xef &&
            static_cast<unsigned char>(line[1]) == 0xbb &&
            static_cast<unsigned char>(line[2]) == 0xbf) {
            line.erase(0, 3);
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        values[line.substr(0, separator)] = line.substr(separator + 1);
    }
    return values;
}

Sef2SignatureStatus parse_signature_status(const std::string& status) {
    if (status == "Valid") {
        return Sef2SignatureStatus::Valid;
    }
    if (status == "Invalid") {
        return Sef2SignatureStatus::Invalid;
    }
    if (status == "MissingSignature") {
        return Sef2SignatureStatus::MissingSignature;
    }
    if (status == "Error") {
        return Sef2SignatureStatus::Error;
    }
    return Sef2SignatureStatus::NotAvailable;
}

} // namespace

void verify_sef2_signature(RecordingSidecarMetadata& metadata) {
    if (!metadata.available || metadata.path.empty()) {
        return;
    }

#ifdef _WIN32
    auto verifier_path = verifier_from_environment();
    if (verifier_path.empty()) {
        verifier_path = default_verifier_path();
    }
    std::error_code ec;
    if (verifier_path.empty() || !std::filesystem::is_regular_file(verifier_path, ec) || ec) {
        metadata.signature_status = metadata.has_signature
            ? Sef2SignatureStatus::NotAvailable
            : Sef2SignatureStatus::MissingSignature;
        return;
    }

    SECURITY_ATTRIBUTES security = {};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
        metadata.signature_status = Sef2SignatureStatus::NotAvailable;
        metadata.verification_error = "Could not start SEF2 signature verifier.";
        return;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process = {};
    std::wstring command_line = quote_process_arg(verifier_path) + L" " + quote_process_arg(metadata.path);
    const auto verifier_directory = verifier_path.parent_path().wstring();
    const BOOL started = CreateProcessW(
        nullptr,
        command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        verifier_directory.empty() ? nullptr : verifier_directory.c_str(),
        &startup,
        &process);

    CloseHandle(write_pipe);
    write_pipe = nullptr;

    if (!started) {
        CloseHandle(read_pipe);
        metadata.signature_status = Sef2SignatureStatus::NotAvailable;
        metadata.verification_error = "Could not start SEF2 signature verifier.";
        return;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            AssignProcessToJobObject(job, process.hProcess);
        }
    }

    const DWORD wait_result = WaitForSingleObject(process.hProcess, 8000);
    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 1000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (job) {
            CloseHandle(job);
        }
        CloseHandle(read_pipe);
        metadata.signature_status = Sef2SignatureStatus::NotAvailable;
        metadata.verification_error = "SEF2 signature verifier timed out.";
        return;
    }

    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (job) {
        CloseHandle(job);
    }

    const auto output = read_all_from_pipe(read_pipe);
    CloseHandle(read_pipe);

    const auto values = parse_key_value_lines(output);
    const auto status = values.find("Status");
    metadata.signature_status = status == values.end()
        ? Sef2SignatureStatus::Error
        : parse_signature_status(status->second);
    if (status == values.end()) {
        metadata.verification_error = output;
    }

    if (const auto value = values.find("SignatureMethod"); value != values.end() && metadata.signature_method.empty()) {
        metadata.signature_method = value->second;
    }
    if (const auto value = values.find("DigestMethod"); value != values.end() && metadata.digest_method.empty()) {
        metadata.digest_method = value->second;
    }
    if (const auto value = values.find("PublicKeySource"); value != values.end()) {
        metadata.public_key_source = value->second;
    }
    if (const auto value = values.find("Exception"); value != values.end()) {
        metadata.verification_error = value->second;
    }
    if (exit_code != 0) {
        metadata.verification_error = metadata.verification_error.empty()
            ? "SEF2 signature verifier exited unsuccessfully."
            : metadata.verification_error + " SEF2 signature verifier exited unsuccessfully.";
    }
#else
    metadata.signature_status = metadata.has_signature
        ? Sef2SignatureStatus::NotAvailable
        : Sef2SignatureStatus::MissingSignature;
#endif
}

namespace {

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

bool derive_display_offset_minutes(const std::vector<int>& offsets, int& display_offset_minutes) {
    if (offsets.empty()) {
        return false;
    }

    bool has_standard_offset = false;
    int standard_offset = 0;
    bool has_daylight_delta = false;
    int daylight_delta = 0;
    for (const int offset : offsets) {
        if (offset < 0 && (!has_standard_offset || offset < standard_offset)) {
            standard_offset = offset;
            has_standard_offset = true;
        } else if (offset > 0 && offset <= 120 && offset > daylight_delta) {
            daylight_delta = offset;
            has_daylight_delta = true;
        }
    }

    if (has_standard_offset) {
        display_offset_minutes = standard_offset + (has_daylight_delta ? daylight_delta : 0);
        return true;
    }

    if (offsets.size() == 1 && offsets.front() != 0) {
        display_offset_minutes = offsets.front();
        return true;
    }

    return false;
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

bool offset_dotnet_ticks(std::uint64_t ticks, int offset_minutes, std::uint64_t& adjusted_ticks) {
    const auto offset_ticks = static_cast<std::int64_t>(offset_minutes) * 60LL * static_cast<std::int64_t>(kTicksPerSecond);
    if (offset_ticks < 0) {
        const auto magnitude = static_cast<std::uint64_t>(-offset_ticks);
        if (ticks < magnitude) {
            return false;
        }
        adjusted_ticks = ticks - magnitude;
        return true;
    }

    const auto magnitude = static_cast<std::uint64_t>(offset_ticks);
    if (ticks > std::numeric_limits<std::uint64_t>::max() - magnitude) {
        return false;
    }
    adjusted_ticks = ticks + magnitude;
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

std::string to_string(Sef2SignatureStatus status) {
    switch (status) {
    case Sef2SignatureStatus::Valid:
        return "Valid";
    case Sef2SignatureStatus::Pending:
        return "Checking...";
    case Sef2SignatureStatus::Invalid:
        return "Invalid";
    case Sef2SignatureStatus::MissingSignature:
        return "Missing";
    case Sef2SignatureStatus::Error:
        return "Invalid";
    default:
        return "Not available";
    }
}

RecordingSidecarMetadata parse_sef2_metadata_xml(const std::string& xml) {
    RecordingSidecarMetadata metadata;
    if (xml.empty()) {
        return metadata;
    }

    metadata.has_signature = contains_xml_signature(xml);
    metadata.signature_method = find_signature_algorithm(xml, "SignatureMethod");
    metadata.digest_method = find_signature_algorithm(xml, "DigestMethod");
    if (metadata.has_signature) {
        metadata.signature_status = Sef2SignatureStatus::Pending;
        metadata.available = true;
    } else if (xml.find("<archive2") != std::string::npos || xml.find("<archive") != std::string::npos) {
        metadata.signature_status = Sef2SignatureStatus::MissingSignature;
        metadata.available = true;
    }

    const auto file = find_first_file_element(xml);
    if (!file.empty()) {
        metadata.referenced_dat_filename = find_attribute(file, "name");
        if (!metadata.referenced_dat_filename.empty()) {
            metadata.available = true;
        }
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
        metadata.channel_id = find_attribute(channel, "channelId");
        const auto encoded_name = find_attribute(channel, "name");
        if (!encoded_name.empty()) {
            const auto decoded = base64_decode_utf8(encoded_name);
            metadata.camera_name = decoded.empty() ? encoded_name : decoded;
            metadata.available = true;
        }
        metadata.manufacturer = find_attribute(channel, "manufacturer");
        metadata.model = find_attribute(channel, "model");
        const auto data_size = find_attribute(channel, "dataSize");
        if (parse_u64_decimal(data_size, metadata.data_size_bytes)) {
            metadata.has_data_size_bytes = true;
        }
        const auto timezone = find_attribute(channel, "timezone");
        if (!timezone.empty()) {
            metadata.timezone_offset_minutes_candidates = timezone_offsets_from_blob(timezone);
            metadata.has_display_offset_minutes =
                derive_display_offset_minutes(metadata.timezone_offset_minutes_candidates, metadata.display_offset_minutes);
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

    std::vector<RecordingSidecarMetadata> candidates;
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
        if (extension == L".sef2" || extension == L".sef") {
            auto metadata = parse_sef2_metadata_file(entry.path());
            if (metadata.available) {
                apply_sidecar_consistency_checks(metadata, dat_path);
                candidates.push_back(std::move(metadata));
            }
        }
    }

    for (auto& metadata : candidates) {
        if (metadata.sidecar_references_selected_dat) {
            return metadata;
        }
    }

    return candidates.empty() ? RecordingSidecarMetadata{} : candidates.front();
}

} // namespace dat_player
