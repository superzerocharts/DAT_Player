#include "playback/H264Decoder.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dat_player::playback {
namespace {

std::string hresult_to_string(HRESULT hr) {
    std::ostringstream stream;
    stream << "0x" << std::hex << static_cast<unsigned long>(hr);
    return stream.str();
}

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept
        : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    ~ComPtr() {
        reset();
    }

    T* get() const {
        return ptr_;
    }

    T** put() {
        reset();
        return &ptr_;
    }

    T* operator->() const {
        return ptr_;
    }

    explicit operator bool() const {
        return ptr_ != nullptr;
    }

    void reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T* ptr_ = nullptr;
};

class MfSession {
public:
    explicit MfSession(DecodeSmokeTestResult& result)
        : result_(result) {
        const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(com_hr)) {
            com_initialized_ = true;
        } else if (com_hr != RPC_E_CHANGED_MODE) {
            throw std::runtime_error("COM initialization failed: " + hresult_to_string(com_hr));
        }

        const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hr)) {
            throw std::runtime_error("Media Foundation startup failed: " + hresult_to_string(hr));
        }
        started_ = true;
        result_.media_foundation_initialized = true;
    }

    MfSession(const MfSession&) = delete;
    MfSession& operator=(const MfSession&) = delete;

    ~MfSession() {
        if (started_) {
            MFShutdown();
        }
        if (com_initialized_) {
            CoUninitialize();
        }
    }

private:
    DecodeSmokeTestResult& result_;
    bool started_ = false;
    bool com_initialized_ = false;
};

struct OutputFrameSink {
    const DatFrameIndex* index = nullptr;
    std::uint64_t next_output_frame = 0;
    std::uint64_t render_from_frame = 0;
    ForwardPlaybackCallback callback;
    bool stop_requested = false;
};

std::wstring widen_ascii(const std::string& value) {
    return std::wstring(value.begin(), value.end());
}

std::wstring guid_to_name(const GUID& guid) {
    if (guid == MFVideoFormat_NV12) {
        return L"NV12";
    }
    if (guid == MFVideoFormat_YV12) {
        return L"YV12";
    }
    if (guid == MFVideoFormat_IYUV) {
        return L"IYUV";
    }
    if (guid == MFVideoFormat_YUY2) {
        return L"YUY2";
    }

    wchar_t buffer[64] = {};
    StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
    return buffer;
}

std::uint8_t clamp_to_byte(int value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

void nv12_to_bgra(
    const std::uint8_t* y_plane,
    const std::uint8_t* uv_plane,
    LONG source_stride,
    std::uint32_t decoded_width,
    std::uint32_t decoded_height,
    std::uint32_t display_width,
    std::uint32_t display_height,
    BgraVideoFrame& frame) {
    if (!y_plane || !uv_plane || source_stride == 0 || decoded_width == 0 || decoded_height == 0 ||
        display_width == 0 || display_height == 0) {
        throw std::runtime_error("Invalid NV12 frame dimensions.");
    }

    const std::uint32_t crop_width = std::min(display_width, decoded_width);
    const std::uint32_t crop_height = std::min(display_height, decoded_height);
    const std::size_t abs_stride = static_cast<std::size_t>(std::abs(source_stride));
    if (crop_width > abs_stride) {
        throw std::runtime_error("Decoded NV12 stride is smaller than the display width.");
    }

    frame.display_width = crop_width;
    frame.display_height = crop_height;
    frame.decoded_width = decoded_width;
    frame.decoded_height = decoded_height;
    frame.stride_bytes = crop_width * 4;
    frame.decoded_subtype = L"NV12";
    frame.pixels.assign(static_cast<std::size_t>(frame.stride_bytes) * frame.display_height, 0);

    for (std::uint32_t y = 0; y < crop_height; ++y) {
        const std::uint8_t* y_row = y_plane + static_cast<std::ptrdiff_t>(y) * source_stride;
        const std::uint8_t* uv_row = uv_plane + static_cast<std::ptrdiff_t>(y / 2) * source_stride;
        std::uint8_t* dst = frame.pixels.data() + static_cast<std::size_t>(y) * frame.stride_bytes;

        for (std::uint32_t x = 0; x < crop_width; ++x) {
            const int yy = static_cast<int>(y_row[x]) - 16;
            const int uu = static_cast<int>(uv_row[(x / 2) * 2]) - 128;
            const int vv = static_cast<int>(uv_row[(x / 2) * 2 + 1]) - 128;
            const int c = std::max(0, yy);
            const int r = (298 * c + 409 * vv + 128) >> 8;
            const int g = (298 * c - 100 * uu - 208 * vv + 128) >> 8;
            const int b = (298 * c + 516 * uu + 128) >> 8;

            dst[x * 4 + 0] = clamp_to_byte(b);
            dst[x * 4 + 1] = clamp_to_byte(g);
            dst[x * 4 + 2] = clamp_to_byte(r);
            dst[x * 4 + 3] = 255;
        }
    }
}

std::vector<std::uint8_t> read_payload(
    std::ifstream& input,
    const DatFrameRecord& frame,
    std::uint64_t file_size) {
    if (frame.payload_size == 0 || frame.payload_offset > file_size ||
        frame.payload_size > file_size - frame.payload_offset) {
        throw std::runtime_error("Indexed payload range is outside the DAT file.");
    }

    if (frame.payload_size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("Payload is too large to read into one Media Foundation sample.");
    }
    if (frame.payload_offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw std::runtime_error("Payload offset is too large for this file stream.");
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(frame.payload_size));
    input.seekg(static_cast<std::streamoff>(frame.payload_offset), std::ios::beg);
    if (!input) {
        throw std::runtime_error("Unable to seek to indexed payload offset.");
    }

    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::size_t>(input.gcount()) != bytes.size()) {
        throw std::runtime_error("Unable to read complete indexed payload.");
    }

    return bytes;
}

std::vector<std::size_t> find_start_codes(const std::vector<std::uint8_t>& bytes) {
    std::vector<std::size_t> positions;
    for (std::size_t i = 0; i + 3 < bytes.size(); ++i) {
        const bool three_byte = bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1;
        const bool four_byte = i + 4 < bytes.size() && bytes[i] == 0 && bytes[i + 1] == 0 &&
                               bytes[i + 2] == 0 && bytes[i + 3] == 1;
        if (three_byte || four_byte) {
            positions.push_back(i);
            i += three_byte ? 2 : 3;
        }
    }
    return positions;
}

void inspect_payload(const std::vector<std::uint8_t>& bytes, H264PayloadInspection& inspection) {
    const auto start_codes = find_start_codes(bytes);
    if (start_codes.empty()) {
        return;
    }

    inspection.saw_start_code = true;
    for (const auto start : start_codes) {
        const std::size_t nal_offset = start + (start + 3 < bytes.size() && bytes[start + 2] == 1 ? 3 : 4);
        if (nal_offset >= bytes.size()) {
            continue;
        }

        const auto nal_type = bytes[nal_offset] & 0x1f;
        inspection.saw_sps = inspection.saw_sps || nal_type == 7;
        inspection.saw_pps = inspection.saw_pps || nal_type == 8;
        inspection.saw_idr = inspection.saw_idr || nal_type == 5;
    }
}

std::vector<std::vector<std::uint8_t>> collect_payloads(
    const std::filesystem::path& dat_path,
    const DatFrameIndex& index,
    std::size_t max_frames,
    DecodeSmokeTestResult& result) {
    if (index.frames.empty()) {
        throw std::runtime_error("No indexed H264/I264 frame records are loaded.");
    }

    std::ifstream input(dat_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Unable to open DAT file for decode smoke test.");
    }

    const auto file_size = static_cast<std::uint64_t>(std::filesystem::file_size(dat_path));
    const auto first_keyframe = std::find_if(index.frames.begin(), index.frames.end(), [](const DatFrameRecord& frame) {
        return frame.keyframe;
    });
    if (first_keyframe == index.frames.end()) {
        throw std::runtime_error("No H264 keyframe record is available to start the decode smoke test.");
    }

    std::vector<std::vector<std::uint8_t>> payloads;
    payloads.reserve(std::min(max_frames, index.frames.size()));
    std::vector<H264PayloadInspection> per_payload_inspections;
    per_payload_inspections.reserve(std::min(max_frames, index.frames.size()));

    for (auto it = first_keyframe; it != index.frames.end() && payloads.size() < max_frames; ++it) {
        auto bytes = read_payload(input, *it, file_size);
        H264PayloadInspection payload_inspection;
        inspect_payload(bytes, payload_inspection);
        result.inspection.saw_start_code = result.inspection.saw_start_code || payload_inspection.saw_start_code;
        result.inspection.saw_sps = result.inspection.saw_sps || payload_inspection.saw_sps;
        result.inspection.saw_pps = result.inspection.saw_pps || payload_inspection.saw_pps;
        result.inspection.saw_idr = result.inspection.saw_idr || payload_inspection.saw_idr;
        ++result.inspection.payloads_inspected;
        per_payload_inspections.push_back(payload_inspection);
        payloads.push_back(std::move(bytes));
    }

    if (!result.inspection.saw_start_code) {
        throw std::runtime_error("Indexed H.264 payloads do not appear to contain Annex B start codes.");
    }
    if (!result.inspection.saw_sps || !result.inspection.saw_pps) {
        throw std::runtime_error("No SPS/PPS NAL units were found in the submitted payload window.");
    }

    bool have_sps = false;
    bool have_pps = false;
    std::size_t first_parameterized_payload = 0;
    for (std::size_t i = 0; i < per_payload_inspections.size(); ++i) {
        have_sps = have_sps || per_payload_inspections[i].saw_sps;
        have_pps = have_pps || per_payload_inspections[i].saw_pps;
        if (have_sps && have_pps) {
            first_parameterized_payload = i;
            break;
        }
    }
    if (first_parameterized_payload > 0) {
        payloads.erase(payloads.begin(), payloads.begin() + static_cast<std::ptrdiff_t>(first_parameterized_payload));
    }

    result.payload_format_supported = true;
    return payloads;
}

ComPtr<IMFTransform> create_h264_decoder(DecodeSmokeTestResult& result) {
    MFT_REGISTER_TYPE_INFO input_type = {};
    input_type.guidMajorType = MFMediaType_Video;
    input_type.guidSubtype = MFVideoFormat_H264;

    IMFActivate** activates = nullptr;
    UINT32 activate_count = 0;
    const HRESULT enum_hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        &input_type,
        nullptr,
        &activates,
        &activate_count);

    if (FAILED(enum_hr) || activate_count == 0 || !activates) {
        if (activates) {
            CoTaskMemFree(activates);
        }
        throw std::runtime_error("No Media Foundation H.264 decoder transform was found.");
    }

    ComPtr<IMFTransform> decoder;
    const HRESULT activate_hr = activates[0]->ActivateObject(
        __uuidof(IMFTransform),
        reinterpret_cast<void**>(decoder.put()));
    for (UINT32 i = 0; i < activate_count; ++i) {
        activates[i]->Release();
    }
    CoTaskMemFree(activates);

    if (FAILED(activate_hr) || !decoder) {
        throw std::runtime_error("Failed to activate H.264 decoder transform: " + hresult_to_string(activate_hr));
    }

    result.decoder_found = true;
    return decoder;
}

void choose_output_type(IMFTransform* decoder) {
    ComPtr<IMFMediaType> output_type;
    DWORD type_index = 0;
    while (SUCCEEDED(decoder->GetOutputAvailableType(0, type_index, output_type.put()))) {
        GUID subtype = {};
        if (SUCCEEDED(output_type->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
            (subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_IYUV || subtype == MFVideoFormat_YV12)) {
            const HRESULT hr = decoder->SetOutputType(0, output_type.get(), 0);
            if (SUCCEEDED(hr)) {
                return;
            }
        }
        ++type_index;
    }

    throw std::runtime_error("No supported uncompressed decoder output type was accepted.");
}

void configure_decoder(IMFTransform* decoder, const DatFrameIndex& index) {
    ComPtr<IMFMediaType> input_type;
    HRESULT hr = MFCreateMediaType(input_type.put());
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create H.264 input media type: " + hresult_to_string(hr));
    }

    input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    if (!index.frames.empty() && index.frames.front().width > 0 && index.frames.front().height > 0) {
        MFSetAttributeSize(input_type.get(), MF_MT_FRAME_SIZE, index.frames.front().width, index.frames.front().height);
    }

    hr = decoder->SetInputType(0, input_type.get(), 0);
    if (FAILED(hr)) {
        throw std::runtime_error("H.264 decoder rejected the input media type: " + hresult_to_string(hr));
    }

    choose_output_type(decoder);
}

ComPtr<IMFSample> create_input_sample(const std::vector<std::uint8_t>& bytes, LONGLONG sample_time) {
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(bytes.size()), buffer.put());
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to allocate input sample buffer: " + hresult_to_string(hr));
    }

    BYTE* destination = nullptr;
    DWORD max_length = 0;
    DWORD current_length = 0;
    hr = buffer->Lock(&destination, &max_length, &current_length);
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to lock input sample buffer: " + hresult_to_string(hr));
    }

    std::copy(bytes.begin(), bytes.end(), destination);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(bytes.size()));

    ComPtr<IMFSample> sample;
    hr = MFCreateSample(sample.put());
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create input sample: " + hresult_to_string(hr));
    }

    sample->AddBuffer(buffer.get());
    sample->SetSampleTime(sample_time);
    return sample;
}

void decode_sample_to_bgra(
    IMFSample* sample,
    IMFMediaType* media_type,
    const DatFrameIndex& index,
    BgraVideoFrame& frame) {
    if (!sample || !media_type) {
        return;
    }

    GUID subtype = {};
    if (FAILED(media_type->GetGUID(MF_MT_SUBTYPE, &subtype))) {
        throw std::runtime_error("Decoded output media type has no subtype.");
    }
    if (subtype != MFVideoFormat_NV12) {
        throw std::runtime_error("First-frame render smoke test currently supports NV12 output only.");
    }

    UINT32 decoded_width = 0;
    UINT32 decoded_height = 0;
    if (FAILED(MFGetAttributeSize(media_type, MF_MT_FRAME_SIZE, &decoded_width, &decoded_height))) {
        throw std::runtime_error("Decoded output media type has no frame size.");
    }

    UINT32 stride_attribute = 0;
    LONG stride = static_cast<LONG>(decoded_width);
    if (SUCCEEDED(media_type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride_attribute))) {
        stride = static_cast<LONG>(stride_attribute);
    }

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = sample->GetBufferByIndex(0, buffer.put());
    if (FAILED(hr) || !buffer) {
        hr = sample->ConvertToContiguousBuffer(buffer.put());
    }
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to access decoded output sample buffer: " + hresult_to_string(hr));
    }

    try {
        const std::uint32_t display_width = !index.frames.empty() ? index.frames.front().width : decoded_width;
        const std::uint32_t display_height = !index.frames.empty() ? index.frames.front().height : decoded_height;

        ComPtr<IMF2DBuffer> buffer_2d;
        if (SUCCEEDED(buffer->QueryInterface(__uuidof(IMF2DBuffer), reinterpret_cast<void**>(buffer_2d.put()))) && buffer_2d) {
            BYTE* scanline0 = nullptr;
            LONG pitch = 0;
            hr = buffer_2d->Lock2D(&scanline0, &pitch);
            if (FAILED(hr)) {
                throw std::runtime_error("Failed to lock decoded NV12 2D buffer: " + hresult_to_string(hr));
            }
            try {
                const LONG actual_stride = pitch != 0 ? pitch : stride;
                if (actual_stride < 0) {
                    throw std::runtime_error("Negative NV12 output pitch is not supported yet.");
                }
                const std::uint8_t* y_plane = scanline0;
                const std::uint8_t* uv_plane = scanline0 + static_cast<std::size_t>(actual_stride) * decoded_height;
                nv12_to_bgra(y_plane, uv_plane, actual_stride, decoded_width, decoded_height, display_width, display_height, frame);
            } catch (...) {
                buffer_2d->Unlock2D();
                throw;
            }
            buffer_2d->Unlock2D();
            return;
        }

        BYTE* bytes = nullptr;
        DWORD max_length = 0;
        DWORD current_length = 0;
        hr = buffer->Lock(&bytes, &max_length, &current_length);
        if (FAILED(hr)) {
            throw std::runtime_error("Failed to lock decoded output sample buffer: " + hresult_to_string(hr));
        }

        try {
            const std::size_t abs_stride = static_cast<std::size_t>(std::abs(stride));
            const std::size_t required_size = abs_stride * decoded_height + abs_stride * ((decoded_height + 1) / 2);
            if (current_length < required_size) {
                throw std::runtime_error("Decoded NV12 buffer is smaller than expected for its stride and height.");
            }
            const std::uint8_t* y_plane = bytes;
            const std::uint8_t* uv_plane = bytes + abs_stride * decoded_height;
            nv12_to_bgra(y_plane, uv_plane, stride, decoded_width, decoded_height, display_width, display_height, frame);
        } catch (...) {
            buffer->Unlock();
            throw;
        }
        buffer->Unlock();
    } catch (...) {
        throw;
    }
}

bool try_process_output(
    IMFTransform* decoder,
    DecodeSmokeTestResult& result,
    const DatFrameIndex* capture_index = nullptr,
    BgraVideoFrame* capture_frame = nullptr,
    OutputFrameSink* output_sink = nullptr) {
    MFT_OUTPUT_STREAM_INFO stream_info = {};
    HRESULT hr = decoder->GetOutputStreamInfo(0, &stream_info);
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to query decoder output stream info: " + hresult_to_string(hr));
    }

    ComPtr<IMFSample> sample;
    if (!(stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        hr = MFCreateSample(sample.put());
        if (FAILED(hr)) {
            throw std::runtime_error("Failed to create output sample: " + hresult_to_string(hr));
        }

        ComPtr<IMFMediaBuffer> buffer;
        hr = MFCreateMemoryBuffer(stream_info.cbSize, buffer.put());
        if (FAILED(hr)) {
            throw std::runtime_error("Failed to create output buffer: " + hresult_to_string(hr));
        }
        sample->AddBuffer(buffer.get());
    }

    MFT_OUTPUT_DATA_BUFFER output = {};
    output.dwStreamID = 0;
    output.pSample = sample.get();
    DWORD status = 0;
    hr = decoder->ProcessOutput(0, 1, &output, &status);
    if (output.pEvents) {
        output.pEvents->Release();
    }

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        if ((stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) && output.pSample) {
            output.pSample->Release();
        }
        return false;
    }
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        if ((stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) && output.pSample) {
            output.pSample->Release();
        }
        choose_output_type(decoder);
        return false;
    }
    if (FAILED(hr)) {
        if ((stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) && output.pSample) {
            output.pSample->Release();
        }
        throw std::runtime_error("Decoder ProcessOutput failed: " + hresult_to_string(hr));
    }

    ++result.frames_decoded;
    result.decoded_any_frame = true;

    ComPtr<IMFMediaType> current_type;
    if (SUCCEEDED(decoder->GetOutputCurrentType(0, current_type.put())) && current_type) {
        GUID subtype = {};
        UINT32 width = 0;
        UINT32 height = 0;
        if (SUCCEEDED(current_type->GetGUID(MF_MT_SUBTYPE, &subtype))) {
            result.decoded_subtype = guid_to_name(subtype);
        }
        if (SUCCEEDED(MFGetAttributeSize(current_type.get(), MF_MT_FRAME_SIZE, &width, &height))) {
            result.decoded_width = width;
            result.decoded_height = height;
        }
    }

    IMFSample* decoded_sample = output.pSample ? output.pSample : sample.get();
    if (capture_index && capture_frame && current_type && capture_frame->pixels.empty()) {
        decode_sample_to_bgra(decoded_sample, current_type.get(), *capture_index, *capture_frame);
    }
    if (output_sink && output_sink->index && current_type) {
        const auto frame_index = output_sink->next_output_frame++;
        if (frame_index >= output_sink->render_from_frame && output_sink->callback) {
            BgraVideoFrame frame;
            decode_sample_to_bgra(decoded_sample, current_type.get(), *output_sink->index, frame);
            const auto bounded_index = std::min<std::uint64_t>(frame_index, output_sink->index->frames.size() - 1);
            ForwardPlaybackFrame playback_frame;
            playback_frame.frame = std::move(frame);
            playback_frame.frame_index = bounded_index;
            playback_frame.timestamp = output_sink->index->frames[static_cast<std::size_t>(bounded_index)].timestamp;
            playback_frame.frames_submitted = result.frames_submitted;
            playback_frame.frames_decoded = result.frames_decoded;
            if (!output_sink->callback(std::move(playback_frame))) {
                output_sink->stop_requested = true;
            }
        }
    }

    if ((stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) && output.pSample) {
        output.pSample->Release();
    }

    return true;
}

void submit_payloads(
    IMFTransform* decoder,
    const std::vector<std::vector<std::uint8_t>>& payloads,
    DecodeSmokeTestResult& result,
    const DatFrameIndex* capture_index = nullptr,
    BgraVideoFrame* capture_frame = nullptr) {
    decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    LONGLONG sample_time = 0;
    for (const auto& payload : payloads) {
        auto sample = create_input_sample(payload, sample_time);
        HRESULT hr = decoder->ProcessInput(0, sample.get(), 0);
        while (hr == MF_E_NOTACCEPTING) {
            if (!try_process_output(decoder, result, capture_index, capture_frame)) {
                break;
            }
            hr = decoder->ProcessInput(0, sample.get(), 0);
        }

        if (FAILED(hr)) {
            throw std::runtime_error("Decoder rejected an input sample: " + hresult_to_string(hr));
        }

        ++result.frames_submitted;
        sample_time += 333667;
        while (try_process_output(decoder, result, capture_index, capture_frame)) {
        }
    }

    decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    while (try_process_output(decoder, result, capture_index, capture_frame)) {
    }
}

std::size_t find_decode_start_frame(const DatFrameIndex& index, std::uint64_t requested_frame) {
    if (index.frames.empty()) {
        throw std::runtime_error("No indexed H264/I264 frame records are loaded.");
    }

    const auto clamped = static_cast<std::size_t>(
        std::min<std::uint64_t>(requested_frame, static_cast<std::uint64_t>(index.frames.size() - 1)));
    for (std::size_t i = clamped + 1; i > 0; --i) {
        const auto candidate = i - 1;
        if (index.frames[candidate].keyframe) {
            return candidate;
        }
    }

    throw std::runtime_error("No H264 keyframe record is available to start playback.");
}

bool pump_decoder_output(IMFTransform* decoder, DecodeSmokeTestResult& result, OutputFrameSink& sink) {
    while (!sink.stop_requested && try_process_output(decoder, result, nullptr, nullptr, &sink)) {
    }
    return !sink.stop_requested;
}

} // namespace

DecodeSmokeTestResult H264DecodeSmokeTester::run(
    const std::filesystem::path& dat_path,
    const DatFrameIndex& index,
    std::size_t max_frames_to_submit) const {
    DecodeSmokeTestResult result;

    try {
        MfSession session(result);
        auto payloads = collect_payloads(dat_path, index, std::max<std::size_t>(1, max_frames_to_submit), result);
        auto decoder = create_h264_decoder(result);
        configure_decoder(decoder.get(), index);
        submit_payloads(decoder.get(), payloads, result);

        if (result.decoded_any_frame) {
            std::wostringstream message;
            message << L"Decode smoke test produced " << result.frames_decoded << L" decoded sample(s).";
            result.message = message.str();
        } else {
            result.message = L"Media Foundation accepted input, but no decoded sample was produced.";
        }
    } catch (const std::exception& ex) {
        result.message = widen_ascii(ex.what());
    }

    return result;
}

FirstFrameRenderResult H264DecodeSmokeTester::render_first_frame(
    const std::filesystem::path& dat_path,
    const DatFrameIndex& index,
    std::size_t max_frames_to_submit) const {
    FirstFrameRenderResult render_result;

    try {
        MfSession session(render_result.decode);
        auto payloads = collect_payloads(dat_path, index, std::max<std::size_t>(1, max_frames_to_submit), render_result.decode);
        auto decoder = create_h264_decoder(render_result.decode);
        configure_decoder(decoder.get(), index);
        submit_payloads(decoder.get(), payloads, render_result.decode, &index, &render_result.frame);

        render_result.frame_available = !render_result.frame.pixels.empty();
        if (render_result.frame_available) {
            std::wostringstream message;
            message << L"Rendered first decoded frame as "
                    << render_result.frame.display_width << L" x " << render_result.frame.display_height
                    << L" BGRA.";
            render_result.message = message.str();
        } else if (render_result.decode.decoded_any_frame) {
            render_result.message = L"Decoder produced samples, but no renderable NV12 frame was captured.";
        } else {
            render_result.message = L"Decoder accepted input, but produced no frame to render.";
        }
        render_result.decode.message = render_result.message;
    } catch (const std::exception& ex) {
        render_result.message = widen_ascii(ex.what());
        render_result.decode.message = render_result.message;
    }

    return render_result;
}

DecodeSmokeTestResult H264DecodeSmokeTester::play_forward(
    const std::filesystem::path& dat_path,
    const DatFrameIndex& index,
    const ForwardPlaybackOptions& options,
    const ForwardPlaybackCallback& on_frame) const {
    DecodeSmokeTestResult result;

    try {
        if (!on_frame) {
            throw std::runtime_error("No playback frame callback was provided.");
        }

        MfSession session(result);
        const auto decode_start = find_decode_start_frame(index, options.start_frame);

        std::ifstream input(dat_path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Unable to open DAT file for forward playback.");
        }
        const auto file_size = static_cast<std::uint64_t>(std::filesystem::file_size(dat_path));

        auto decoder = create_h264_decoder(result);
        configure_decoder(decoder.get(), index);
        decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        OutputFrameSink sink;
        sink.index = &index;
        sink.next_output_frame = static_cast<std::uint64_t>(decode_start);
        sink.render_from_frame = std::min<std::uint64_t>(
            options.start_frame,
            static_cast<std::uint64_t>(index.frames.size() - 1));
        sink.callback = on_frame;

        LONGLONG sample_time = 0;
        std::size_t submitted_since_start = 0;
        for (std::size_t i = decode_start; i < index.frames.size() && !sink.stop_requested; ++i) {
            auto payload = read_payload(input, index.frames[i], file_size);
            H264PayloadInspection payload_inspection;
            inspect_payload(payload, payload_inspection);
            result.inspection.saw_start_code = result.inspection.saw_start_code || payload_inspection.saw_start_code;
            result.inspection.saw_sps = result.inspection.saw_sps || payload_inspection.saw_sps;
            result.inspection.saw_pps = result.inspection.saw_pps || payload_inspection.saw_pps;
            result.inspection.saw_idr = result.inspection.saw_idr || payload_inspection.saw_idr;
            ++result.inspection.payloads_inspected;

            if (submitted_since_start >= 120 && (!result.inspection.saw_start_code || !result.inspection.saw_sps || !result.inspection.saw_pps)) {
                throw std::runtime_error("Playback startup did not find Annex B SPS/PPS setup data in the first 120 submitted payloads.");
            }

            auto sample = create_input_sample(payload, sample_time);
            HRESULT hr = decoder->ProcessInput(0, sample.get(), 0);
            while (hr == MF_E_NOTACCEPTING && !sink.stop_requested) {
                if (!pump_decoder_output(decoder.get(), result, sink)) {
                    break;
                }
                hr = decoder->ProcessInput(0, sample.get(), 0);
            }

            if (sink.stop_requested) {
                break;
            }
            if (FAILED(hr)) {
                throw std::runtime_error("Decoder rejected an input sample during playback: " + hresult_to_string(hr));
            }

            ++result.frames_submitted;
            ++submitted_since_start;
            sample_time += 333667;
            pump_decoder_output(decoder.get(), result, sink);
        }

        if (!sink.stop_requested) {
            decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            pump_decoder_output(decoder.get(), result, sink);
        }

        result.payload_format_supported = result.inspection.saw_start_code && result.inspection.saw_sps && result.inspection.saw_pps;
        if (sink.stop_requested) {
            result.message = L"Forward playback stopped.";
        } else if (result.decoded_any_frame) {
            std::wostringstream message;
            message << L"Forward playback completed after decoding " << result.frames_decoded << L" frame(s).";
            result.message = message.str();
        } else {
            result.message = L"Decoder accepted input, but produced no frames during playback.";
        }
    } catch (const std::exception& ex) {
        result.message = widen_ascii(ex.what());
    }

    return result;
}

} // namespace dat_player::playback
