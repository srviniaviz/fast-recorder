#include "recording/AmfBackend.h"

#include "recording/AmfProbe.h"
#include "recording/AudioCapture.h"
#include "recording/GraphicsCapture.h"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <roapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifndef FASTRECORD_HAS_AMF
#define FASTRECORD_HAS_AMF 0
#endif

#if FASTRECORD_HAS_AMF
#include <core/Factory.h>
#include <components/VideoEncoderVCE.h>
#endif

namespace fastrecord::recording {

namespace {

using Microsoft::WRL::ComPtr;

constexpr LONGLONG kHundredNanosecondsPerSecond = 10'000'000;
constexpr std::uint32_t kAudioBitrate = 192'000;

struct MediaRuntime {
    MediaRuntime() {
        winrt::check_hresult(RoInitialize(RO_INIT_MULTITHREADED));
        const HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(result)) {
            RoUninitialize();
            winrt::check_hresult(result);
        }
    }

    ~MediaRuntime() {
        winrt::clear_factory_cache();
        MFShutdown();
        RoUninitialize();
    }
};

std::wstring hresultMessage(const wchar_t* operation, HRESULT result) {
    wchar_t buffer[128]{};
    swprintf_s(
        buffer,
        _countof(buffer),
        L"%s (HRESULT 0x%08X).",
        operation,
        static_cast<unsigned int>(result));
    return buffer;
}

std::filesystem::path createOutputPath(const std::filesystem::path& directory) {
    SYSTEMTIME now{};
    GetLocalTime(&now);

    wchar_t name[96]{};
    swprintf_s(
        name,
        _countof(name),
        L"Fast Record %04u-%02u-%02u %02u-%02u-%02u-%03u.mp4",
        now.wYear,
        now.wMonth,
        now.wDay,
        now.wHour,
        now.wMinute,
        now.wSecond,
        now.wMilliseconds);
    return directory / name;
}

HRESULT configureAudioStream(IMFSinkWriter* writer, DWORD& streamIndex) {
    ComPtr<IMFMediaType> outputType;
    HRESULT result = MFCreateMediaType(&outputType);
    if (SUCCEEDED(result)) result = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(result)) result = outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    if (SUCCEEDED(result)) result = outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, AudioCapture::kChannels);
    if (SUCCEEDED(result)) result = outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AudioCapture::kSampleRate);
    if (SUCCEEDED(result)) result = outputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, AudioCapture::kBitsPerSample);
    if (SUCCEEDED(result)) result = outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAudioBitrate / 8);
    if (SUCCEEDED(result)) result = outputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
    if (SUCCEEDED(result)) result = outputType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    if (SUCCEEDED(result)) result = outputType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    if (SUCCEEDED(result)) result = writer->AddStream(outputType.Get(), &streamIndex);

    ComPtr<IMFMediaType> inputType;
    if (SUCCEEDED(result)) result = MFCreateMediaType(&inputType);
    if (SUCCEEDED(result)) result = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(result)) result = inputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (SUCCEEDED(result)) result = inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, AudioCapture::kChannels);
    if (SUCCEEDED(result)) result = inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AudioCapture::kSampleRate);
    if (SUCCEEDED(result)) result = inputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, AudioCapture::kBitsPerSample);
    if (SUCCEEDED(result)) result = inputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, AudioCapture::kBlockAlignment);
    if (SUCCEEDED(result)) {
        result = inputType->SetUINT32(
            MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
            AudioCapture::kSampleRate * AudioCapture::kBlockAlignment);
    }
    if (SUCCEEDED(result)) result = inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (SUCCEEDED(result)) result = writer->SetInputMediaType(streamIndex, inputType.Get(), nullptr);
    return result;
}

HRESULT writeAudioSample(
    IMFSinkWriter* writer,
    DWORD streamIndex,
    const std::vector<std::uint8_t>& pcm,
    std::uint32_t frameCount,
    std::uint64_t firstFrame) {
    if (pcm.empty() || frameCount == 0) {
        return S_OK;
    }

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT result = MFCreateMemoryBuffer(static_cast<DWORD>(pcm.size()), &buffer);
    BYTE* destination = nullptr;
    if (SUCCEEDED(result)) result = buffer->Lock(&destination, nullptr, nullptr);
    if (SUCCEEDED(result)) {
        std::memcpy(destination, pcm.data(), pcm.size());
        buffer->Unlock();
        destination = nullptr;
        result = buffer->SetCurrentLength(static_cast<DWORD>(pcm.size()));
    }
    if (destination != nullptr) buffer->Unlock();

    ComPtr<IMFSample> sample;
    if (SUCCEEDED(result)) result = MFCreateSample(&sample);
    if (SUCCEEDED(result)) result = sample->AddBuffer(buffer.Get());
    if (SUCCEEDED(result)) {
        result = sample->SetSampleTime(
            static_cast<LONGLONG>(firstFrame * kHundredNanosecondsPerSecond / AudioCapture::kSampleRate));
    }
    if (SUCCEEDED(result)) {
        result = sample->SetSampleDuration(
            static_cast<LONGLONG>(frameCount) * kHundredNanosecondsPerSecond / AudioCapture::kSampleRate);
    }
    if (SUCCEEDED(result)) result = writer->WriteSample(streamIndex, sample.Get());
    return result;
}

size_t findStartCode(
    const std::vector<std::uint8_t>& data,
    size_t offset,
    size_t& codeSize) {
    for (size_t index = offset; index + 3 <= data.size(); ++index) {
        if (data[index] != 0 || data[index + 1] != 0) continue;
        if (data[index + 2] == 1) {
            codeSize = 3;
            return index;
        }
        if (index + 4 <= data.size() && data[index + 2] == 0 && data[index + 3] == 1) {
            codeSize = 4;
            return index;
        }
    }
    codeSize = 0;
    return std::numeric_limits<size_t>::max();
}

bool normalizeH264ToAnnexB(
    const std::vector<std::uint8_t>& encoded,
    std::vector<std::uint8_t>& packet) {
    packet.clear();
    if (encoded.empty()) return false;

    size_t codeSize = 0;
    if (findStartCode(encoded, 0, codeSize) != std::numeric_limits<size_t>::max()) {
        packet = encoded;
        return true;
    }

    // Some AMF versions return AVC length-prefixed access units. Convert each
    // NAL to Annex B before handing the sample to the Media Foundation muxer.
    for (size_t offset = 0; offset + sizeof(DWORD) <= encoded.size();) {
        const DWORD nalSize =
            (static_cast<DWORD>(encoded[offset]) << 24) |
            (static_cast<DWORD>(encoded[offset + 1]) << 16) |
            (static_cast<DWORD>(encoded[offset + 2]) << 8) |
            static_cast<DWORD>(encoded[offset + 3]);
        offset += sizeof(DWORD);
        if (nalSize == 0 || nalSize > encoded.size() - offset) {
            packet.clear();
            return false;
        }
        packet.insert(packet.end(), {0, 0, 0, 1});
        packet.insert(
            packet.end(),
            encoded.begin() + static_cast<std::ptrdiff_t>(offset),
            encoded.begin() + static_cast<std::ptrdiff_t>(offset + nalSize));
        offset += nalSize;
    }
    return !packet.empty();
}

bool extractH264SequenceHeader(
    const std::vector<std::uint8_t>& encoded,
    std::vector<std::uint8_t>& sequenceHeader) {
    sequenceHeader.clear();
    std::vector<std::uint8_t> annexB;
    if (!normalizeH264ToAnnexB(encoded, annexB)) return false;

    size_t codeSize = 0;
    size_t start = findStartCode(annexB, 0, codeSize);
    bool hasSps = false;
    bool hasPps = false;
    while (start != std::numeric_limits<size_t>::max()) {
        const size_t nalStart = start + codeSize;
        size_t nextCodeSize = 0;
        const size_t next = findStartCode(annexB, nalStart, nextCodeSize);
        size_t nalEnd = next == std::numeric_limits<size_t>::max() ? annexB.size() : next;
        while (nalEnd > nalStart && annexB[nalEnd - 1] == 0) --nalEnd;
        if (nalEnd > nalStart) {
            const auto type = static_cast<std::uint8_t>(annexB[nalStart] & 0x1f);
            if (type == 7 || type == 8) {
                sequenceHeader.insert(sequenceHeader.end(), {0, 0, 0, 1});
                sequenceHeader.insert(
                    sequenceHeader.end(),
                    annexB.begin() + static_cast<std::ptrdiff_t>(nalStart),
                    annexB.begin() + static_cast<std::ptrdiff_t>(nalEnd));
                hasSps = hasSps || type == 7;
                hasPps = hasPps || type == 8;
            }
        }
        start = next;
        codeSize = nextCodeSize;
    }
    return hasSps && hasPps;
}

bool containsH264KeyFrame(const std::vector<std::uint8_t>& encoded) {
    std::vector<std::uint8_t> annexB;
    if (!normalizeH264ToAnnexB(encoded, annexB)) return false;
    size_t codeSize = 0;
    size_t start = findStartCode(annexB, 0, codeSize);
    while (start != std::numeric_limits<size_t>::max()) {
        const size_t nalStart = start + codeSize;
        if (nalStart < annexB.size() && (annexB[nalStart] & 0x1f) == 5) return true;
        size_t nextCodeSize = 0;
        start = findStartCode(annexB, nalStart, nextCodeSize);
        codeSize = nextCodeSize;
    }
    return false;
}

#if FASTRECORD_HAS_AMF

class AmfLibrary final {
public:
    AmfLibrary() {
#if defined(_WIN64)
        m_handle = LoadLibraryW(L"amfrt64.dll");
        if (m_handle == nullptr) m_handle = LoadLibraryW(L"amfrtlt64.dll");
#else
        m_handle = LoadLibraryW(L"amfrt32.dll");
        if (m_handle == nullptr) m_handle = LoadLibraryW(L"amfrtlt32.dll");
#endif
    }

    ~AmfLibrary() {
        if (m_handle != nullptr) FreeLibrary(m_handle);
    }

    AmfLibrary(const AmfLibrary&) = delete;
    AmfLibrary& operator=(const AmfLibrary&) = delete;

    HMODULE handle() const noexcept { return m_handle; }

private:
    HMODULE m_handle{nullptr};
};

std::wstring amfResultMessage(const wchar_t* operation, AMF_RESULT result) {
    std::wstring message = operation;
    message += L" (código AMF ";
    message += std::to_wstring(static_cast<int>(result));
    message += L").";
    return message;
}

struct AmfVideoPacket {
    std::vector<std::uint8_t> bytes;
    LONGLONG timestamp{0};
    bool keyFrame{false};
};

void clampByte(int& value) {
    value = (std::max)(0, (std::min)(255, value));
}

bool fillNv12(
    amf::AMFSurface* surface,
    const std::vector<BYTE>& bgra,
    std::uint32_t width,
    std::uint32_t height,
    std::wstring& error) {
    if (surface == nullptr || bgra.size() < static_cast<size_t>(width) * height * 4) {
        error = L"A superfície AMF recebeu um frame inválido.";
        return false;
    }

    amf::AMFPlane* yPlane = surface->GetPlane(amf::AMF_PLANE_Y);
    amf::AMFPlane* uvPlane = surface->GetPlane(amf::AMF_PLANE_UV);
    if (yPlane == nullptr || uvPlane == nullptr ||
        yPlane->GetNative() == nullptr || uvPlane->GetNative() == nullptr) {
        error = L"O AMF não expôs as superfícies NV12 para escrita.";
        return false;
    }

    auto* y = static_cast<std::uint8_t*>(yPlane->GetNative());
    auto* uv = static_cast<std::uint8_t*>(uvPlane->GetNative());
    const auto yPitch = static_cast<size_t>(yPlane->GetHPitch());
    const auto uvPitch = static_cast<size_t>(uvPlane->GetHPitch());
    if (yPitch < width || uvPitch < width || yPlane->GetHeight() < static_cast<amf_int32>(height) ||
        uvPlane->GetHeight() < static_cast<amf_int32>((height + 1) / 2)) {
        error = L"A superfície NV12 retornada pelo AMF tem dimensões inválidas.";
        return false;
    }

    const auto pixel = [&](std::uint32_t x, std::uint32_t row, int& red, int& green, int& blue) {
        const auto* source = bgra.data() + (static_cast<size_t>(row) * width + x) * 4;
        blue = source[0];
        green = source[1];
        red = source[2];
    };

    for (std::uint32_t row = 0; row < height; ++row) {
        auto* yRow = y + static_cast<size_t>(row) * yPitch;
        for (std::uint32_t column = 0; column < width; ++column) {
            int red = 0;
            int green = 0;
            int blue = 0;
            pixel(column, row, red, green, blue);
            int value = ((66 * red + 129 * green + 25 * blue + 128) >> 8) + 16;
            clampByte(value);
            yRow[column] = static_cast<std::uint8_t>(value);
        }
    }

    for (std::uint32_t row = 0; row < height; row += 2) {
        auto* uvRow = uv + static_cast<size_t>(row / 2) * uvPitch;
        for (std::uint32_t column = 0; column < width; column += 2) {
            int redTotal = 0;
            int greenTotal = 0;
            int blueTotal = 0;
            unsigned count = 0;
            for (std::uint32_t dy = 0; dy < 2 && row + dy < height; ++dy) {
                for (std::uint32_t dx = 0; dx < 2 && column + dx < width; ++dx) {
                    int red = 0;
                    int green = 0;
                    int blue = 0;
                    pixel(column + dx, row + dy, red, green, blue);
                    redTotal += red;
                    greenTotal += green;
                    blueTotal += blue;
                    ++count;
                }
            }
            const int red = redTotal / static_cast<int>(count);
            const int green = greenTotal / static_cast<int>(count);
            const int blue = blueTotal / static_cast<int>(count);
            int u = ((-38 * red - 74 * green + 112 * blue + 128) >> 8) + 128;
            int v = ((112 * red - 94 * green - 18 * blue + 128) >> 8) + 128;
            clampByte(u);
            clampByte(v);
            uvRow[column] = static_cast<std::uint8_t>(u);
            uvRow[column + 1] = static_cast<std::uint8_t>(v);
        }
    }
    return true;
}

class AmfEncoder final {
public:
    ~AmfEncoder() {
        if (m_encoder != nullptr) {
            m_encoder->Terminate();
            m_encoder = nullptr;
        }
        if (m_context != nullptr) {
            m_context->Terminate();
            m_context = nullptr;
        }
        m_factory = nullptr;
        m_library.reset();
    }

    bool initialize(
        ID3D11Device* device,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t fps,
        std::uint32_t bitrate,
        std::wstring& error) {
        m_width = width;
        m_height = height;
        m_frameDuration = kHundredNanosecondsPerSecond / fps;

        m_library = std::make_unique<AmfLibrary>();
        if (m_library->handle() == nullptr) {
            error = L"AMF não disponível: amfrt64.dll não foi encontrada no driver AMD.";
            return false;
        }

        const auto init = reinterpret_cast<AMFInit_Fn>(
            GetProcAddress(m_library->handle(), AMF_INIT_FUNCTION_NAME));
        if (init == nullptr) {
            error = L"AMF não disponível: a DLL não exporta AMFInit.";
            return false;
        }

        if (init(AMF_FULL_VERSION, &m_factory) != AMF_OK || m_factory == nullptr) {
            error = L"AMF não disponível: AMFInit recusou a versão dos bindings.";
            return false;
        }
        if (m_factory->CreateContext(&m_context) != AMF_OK || m_context == nullptr) {
            error = L"AMF não disponível: não foi possível criar o contexto.";
            return false;
        }
        if (device == nullptr || m_context->InitDX11(device) != AMF_OK) {
            error = L"AMF não disponível: o contexto não aceitou o dispositivo Direct3D 11.";
            return false;
        }
        if (m_factory->CreateComponent(m_context, AMFVideoEncoderVCE_AVC, &m_encoder) != AMF_OK ||
            m_encoder == nullptr) {
            error = L"AMF não disponível: o encoder AMD H.264 não foi encontrado nesta GPU/driver.";
            return false;
        }

        const auto set = [&](const wchar_t* property, const auto& value) {
            return m_encoder->SetProperty(property, value) == AMF_OK;
        };
        if (!set(AMF_VIDEO_ENCODER_USAGE, static_cast<amf_int64>(AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY)) ||
            !set(AMF_VIDEO_ENCODER_PROFILE, static_cast<amf_int64>(AMF_VIDEO_ENCODER_PROFILE_HIGH)) ||
            !set(AMF_VIDEO_ENCODER_FRAMERATE, AMFConstructRate(fps, 1)) ||
            !set(AMF_VIDEO_ENCODER_TARGET_BITRATE, static_cast<amf_int64>(bitrate)) ||
            !set(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
                static_cast<amf_int64>(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR)) ||
            !set(AMF_VIDEO_ENCODER_B_PIC_PATTERN, static_cast<amf_int64>(0)) ||
            !set(AMF_VIDEO_ENCODER_IDR_PERIOD, static_cast<amf_int64>(fps * 2)) ||
            !set(AMF_VIDEO_ENCODER_FRAMESIZE, AMFConstructSize(static_cast<amf_int32>(width), static_cast<amf_int32>(height)))) {
            error = L"AMF não aceitou a configuração de H.264 da gravação.";
            return false;
        }

        const AMF_RESULT initResult = m_encoder->Init(amf::AMF_SURFACE_NV12, width, height);
        if (initResult != AMF_OK) {
            error = amfResultMessage(L"Não foi possível inicializar o encoder H.264 AMF", initResult);
            return false;
        }

        // ExtraData is the most reliable source of SPS/PPS on drivers that do
        // not repeat parameter sets in the first access unit.
        amf::AMFVariant extraData;
        if (m_encoder->GetProperty(AMF_VIDEO_ENCODER_EXTRADATA, &extraData) == AMF_OK &&
            extraData.type == amf::AMF_VARIANT_INTERFACE && extraData.pInterface != nullptr) {
            amf::AMFBufferPtr buffer(extraData.pInterface);
            if (buffer != nullptr && buffer->GetNative() != nullptr && buffer->GetSize() > 0) {
                const auto* bytes = static_cast<const std::uint8_t*>(buffer->GetNative());
                m_sequenceHeader.assign(bytes, bytes + buffer->GetSize());
            }
        }
        return true;
    }

    const std::vector<std::uint8_t>& sequenceHeader() const noexcept {
        return m_sequenceHeader;
    }

    bool encode(
        const std::vector<BYTE>& bgra,
        LONGLONG timestamp,
        std::vector<AmfVideoPacket>& packets,
        std::wstring& error) {
        packets.clear();
        if (m_encoder == nullptr || m_context == nullptr) {
            error = L"O encoder AMF não está pronto para receber frames.";
            return false;
        }

        amf::AMFSurfacePtr surface;
        AMF_RESULT result = m_context->AllocSurface(
            amf::AMF_MEMORY_HOST,
            amf::AMF_SURFACE_NV12,
            static_cast<amf_int32>(m_width),
            static_cast<amf_int32>(m_height),
            &surface);
        if (result != AMF_OK || surface == nullptr) {
            error = amfResultMessage(L"AMF não conseguiu alocar uma superfície NV12", result);
            return false;
        }
        if (!fillNv12(surface, bgra, m_width, m_height, error)) return false;
        surface->SetPts(timestamp);
        surface->SetDuration(m_frameDuration);

        for (;;) {
            result = m_encoder->SubmitInput(surface);
            if (result != AMF_INPUT_FULL) break;
            if (!collectOutput(packets, false, error)) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (result != AMF_OK && result != AMF_NEED_MORE_INPUT) {
            error = amfResultMessage(L"AMF não conseguiu enviar o frame", result);
            return false;
        }

        // Low-latency mode normally returns one packet immediately. The small
        // wait also handles drivers that schedule the first GPU submission.
        return collectOutput(packets, true, error);
    }

    bool drain(std::vector<AmfVideoPacket>& packets, std::wstring& error) {
        packets.clear();
        if (m_encoder == nullptr) return true;

        AMF_RESULT result = AMF_INPUT_FULL;
        while (result == AMF_INPUT_FULL) {
            result = m_encoder->Drain();
            if (result == AMF_INPUT_FULL) {
                if (!collectOutput(packets, false, error)) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (result != AMF_OK && result != AMF_EOF) {
            error = amfResultMessage(L"Não foi possível finalizar o encoder AMF", result);
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (;;) {
            const size_t before = packets.size();
            if (!collectOutput(packets, false, error)) return false;
            if (std::chrono::steady_clock::now() >= deadline || packets.size() == before) break;
        }
        return true;
    }

private:
    bool collectOutput(
        std::vector<AmfVideoPacket>& packets,
        bool waitForOne,
        std::wstring& error) {
        const auto deadline = std::chrono::steady_clock::now() +
            (waitForOne ? std::chrono::seconds(2) : std::chrono::milliseconds(2));
        bool received = false;
        for (;;) {
            amf::AMFData* data = nullptr;
            const AMF_RESULT result = m_encoder->QueryOutput(&data);
            if (result == AMF_OK && data != nullptr) {
                amf::AMFBufferPtr buffer(data);
                if (buffer == nullptr || buffer->GetNative() == nullptr || buffer->GetSize() == 0) {
                    error = L"O encoder AMF retornou um pacote H.264 vazio.";
                    return false;
                }
                const auto* bytes = static_cast<const std::uint8_t*>(buffer->GetNative());
                std::vector<std::uint8_t> encoded(bytes, bytes + buffer->GetSize());
                std::vector<std::uint8_t> annexB;
                if (!normalizeH264ToAnnexB(encoded, annexB)) {
                    error = L"O encoder AMF retornou um pacote H.264 inválido.";
                    return false;
                }
                if (m_sequenceHeader.empty()) extractH264SequenceHeader(annexB, m_sequenceHeader);
                packets.push_back({
                    std::move(annexB),
                    static_cast<LONGLONG>(buffer->GetPts()),
                    containsH264KeyFrame(encoded)});
                received = true;
                if (waitForOne) return true;
                continue;
            }
            if (result == AMF_EOF || result == AMF_REPEAT || result == AMF_NEED_MORE_INPUT) {
                if (!waitForOne || received || std::chrono::steady_clock::now() >= deadline) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            error = amfResultMessage(L"Não foi possível obter o pacote H.264 do AMF", result);
            return false;
        }
    }

    std::unique_ptr<AmfLibrary> m_library;
    amf::AMFFactory* m_factory{nullptr};
    amf::AMFContextPtr m_context;
    amf::AMFComponentPtr m_encoder;
    std::vector<std::uint8_t> m_sequenceHeader;
    std::uint32_t m_width{0};
    std::uint32_t m_height{0};
    LONGLONG m_frameDuration{0};
};

HRESULT configureAmfVideoStream(
    IMFSinkWriter* writer,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t fps,
    std::uint32_t bitrate,
    const std::vector<std::uint8_t>& sequenceHeader,
    DWORD& streamIndex) {
    ComPtr<IMFMediaType> outputType;
    HRESULT result = MFCreateMediaType(&outputType);
    if (SUCCEEDED(result)) result = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(result)) result = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(result)) result = outputType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    if (SUCCEEDED(result)) result = outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(result)) result = MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, width, height);
    if (SUCCEEDED(result)) result = MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(result)) result = MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(result)) result = writer->AddStream(outputType.Get(), &streamIndex);

    ComPtr<IMFMediaType> inputType;
    if (SUCCEEDED(result)) result = MFCreateMediaType(&inputType);
    if (SUCCEEDED(result)) result = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(result)) result = inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(result)) result = inputType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    if (SUCCEEDED(result)) result = inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(result)) result = MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, width, height);
    if (SUCCEEDED(result)) result = MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(result)) result = MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(result) && !sequenceHeader.empty()) {
        result = inputType->SetBlob(
            MF_MT_MPEG_SEQUENCE_HEADER,
            sequenceHeader.data(),
            static_cast<UINT32>(sequenceHeader.size()));
    }
    if (SUCCEEDED(result)) result = writer->SetInputMediaType(streamIndex, inputType.Get(), nullptr);
    return result;
}

HRESULT writeAmfVideoSample(
    IMFSinkWriter* writer,
    DWORD streamIndex,
    const AmfVideoPacket& packet,
    LONGLONG fallbackTimestamp,
    LONGLONG frameDuration) {
    if (packet.bytes.empty()) return S_OK;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT result = MFCreateMemoryBuffer(static_cast<DWORD>(packet.bytes.size()), &buffer);
    BYTE* destination = nullptr;
    if (SUCCEEDED(result)) result = buffer->Lock(&destination, nullptr, nullptr);
    if (SUCCEEDED(result)) {
        std::memcpy(destination, packet.bytes.data(), packet.bytes.size());
        buffer->Unlock();
        destination = nullptr;
        result = buffer->SetCurrentLength(static_cast<DWORD>(packet.bytes.size()));
    }
    if (destination != nullptr) buffer->Unlock();

    ComPtr<IMFSample> sample;
    if (SUCCEEDED(result)) result = MFCreateSample(&sample);
    if (SUCCEEDED(result)) result = sample->AddBuffer(buffer.Get());
    if (SUCCEEDED(result)) {
        const LONGLONG timestamp = packet.timestamp >= 0 ? packet.timestamp : fallbackTimestamp;
        result = sample->SetSampleTime(timestamp);
    }
    if (SUCCEEDED(result)) result = sample->SetSampleDuration(frameDuration);
    if (SUCCEEDED(result)) result = writer->WriteSample(streamIndex, sample.Get());
    return result;
}

#endif // FASTRECORD_HAS_AMF

} // namespace

AmfBackend::~AmfBackend() {
    std::wstring ignored;
    stop(ignored);
}

bool AmfBackend::start(const RecordingSettings& settings, std::wstring& error) {
#if !FASTRECORD_HAS_AMF
    error = L"Esta build não inclui os headers do AMF. Reconfigure com FASTRECORD_FETCH_AMF_HEADERS=ON.";
    return false;
#else
    if (m_worker.joinable() || m_running.load()) {
        error = L"Já existe uma captura em andamento.";
        return false;
    }
    const auto capability = probeAmf();
    if (!capability.h264Supported) {
        error = capability.description.empty()
            ? L"AMF não está disponível nesta máquina."
            : capability.description;
        return false;
    }
    if (settings.codec != VideoCodec::H264) {
        error = L"O backend AMF desta versão grava H.264; AV1 continua disponível pelo NVENC.";
        return false;
    }
    if (settings.outputDirectory.empty() || settings.width < 2 || settings.height < 2 ||
        settings.width > 3840 || settings.height > 2160 ||
        (settings.width % 2) != 0 || (settings.height % 2) != 0 ||
        settings.framesPerSecond == 0 || settings.framesPerSecond > 120 ||
        settings.bitrateMbps < 4 || settings.bitrateMbps > 80) {
        error = L"As configurações da gravação são inválidas.";
        return false;
    }

    std::error_code directoryError;
    std::filesystem::create_directories(settings.outputDirectory, directoryError);
    if (directoryError) {
        error = L"Não foi possível criar a pasta de gravações.";
        return false;
    }

    {
        std::scoped_lock lock(m_mutex);
        m_outputPath = createOutputPath(settings.outputDirectory);
        m_workerError.clear();
    }
    m_stopRequested = false;
    m_paused = false;

    std::promise<std::wstring> startupResult;
    auto startupFuture = startupResult.get_future();
    try {
        m_worker = std::thread([this, settings, promise = std::move(startupResult)]() mutable {
            try {
                recordLoop(settings, std::move(promise));
            } catch (const winrt::hresult_error& failure) {
                setWorkerError(failure.message().c_str());
            } catch (...) {
                setWorkerError(L"Falha inesperada no backend AMF.");
            }
            m_running = false;
        });
        error = startupFuture.get();
    } catch (...) {
        if (m_worker.joinable()) m_worker.join();
        std::scoped_lock lock(m_mutex);
        error = m_workerError.empty() ? L"Não foi possível iniciar o AMF." : m_workerError;
        return false;
    }
    if (!error.empty()) {
        if (m_worker.joinable()) m_worker.join();
        return false;
    }
    return true;
#endif
}

bool AmfBackend::pause(std::wstring& error) {
    if (!m_running.load() || m_paused.load()) {
        std::scoped_lock lock(m_mutex);
        error = m_workerError.empty() ? L"A gravação não está ativa." : m_workerError;
        return false;
    }
    m_paused = true;
    return true;
}

bool AmfBackend::resume(std::wstring& error) {
    if (!m_running.load() || !m_paused.load()) {
        std::scoped_lock lock(m_mutex);
        error = m_workerError.empty() ? L"A gravação não está pausada." : m_workerError;
        return false;
    }
    m_paused = false;
    return true;
}

bool AmfBackend::stop(std::wstring& error) {
    if (!m_worker.joinable()) {
        error = L"Não há uma captura ativa.";
        return false;
    }

    m_stopRequested = true;
    m_paused = false;
    m_worker.join();

    std::scoped_lock lock(m_mutex);
    error = m_workerError;
    return error.empty();
}

std::filesystem::path AmfBackend::outputPath() const {
    std::scoped_lock lock(m_mutex);
    return m_outputPath;
}

void AmfBackend::setWorkerError(std::wstring error) {
    std::scoped_lock lock(m_mutex);
    if (m_workerError.empty()) m_workerError = std::move(error);
}

void AmfBackend::recordLoop(
    RecordingSettings settings,
    std::promise<std::wstring> startupResult) {
    bool startupReported = false;
    auto reportStartup = [&](std::wstring message) {
        if (!startupReported) {
            startupResult.set_value(std::move(message));
            startupReported = true;
        }
    };

#if FASTRECORD_HAS_AMF
    try {
        MediaRuntime runtime;
        const auto width = settings.width;
        const auto height = settings.height;
        const auto fps = settings.framesPerSecond;
        const auto bitrate = settings.bitrateMbps * 1'000'000u;
        const LONGLONG frameDuration = kHundredNanosecondsPerSecond / fps;

        auto capture = std::make_unique<GraphicsCapture>(settings);
        std::vector<BYTE> pixels;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!capture->read(pixels)) {
            if (m_stopRequested || std::chrono::steady_clock::now() >= deadline) {
                throw winrt::hresult_error(E_ABORT, L"O Windows não entregou frames do monitor.");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        AmfEncoder encoder;
        std::wstring encoderError;
        if (!encoder.initialize(capture->device(), width, height, fps, bitrate, encoderError)) {
            reportStartup(encoderError);
            setWorkerError(encoderError);
            return;
        }

        std::vector<AmfVideoPacket> firstPackets;
        LONGLONG timestamp = 0;
        for (std::uint32_t attempt = 0; attempt < 8 && firstPackets.empty(); ++attempt) {
            if (attempt > 0) {
                while (!capture->read(pixels)) {
                    if (m_stopRequested) return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                timestamp = static_cast<LONGLONG>(attempt) * frameDuration;
            }
            if (!encoder.encode(pixels, timestamp, firstPackets, encoderError)) {
                reportStartup(encoderError);
                setWorkerError(encoderError);
                return;
            }
        }
        if (firstPackets.empty()) {
            const std::wstring message = L"O AMF não produziu o primeiro frame H.264.";
            reportStartup(message);
            setWorkerError(message);
            return;
        }

        std::vector<std::uint8_t> sequenceHeader = encoder.sequenceHeader();
        if (sequenceHeader.empty()) {
            for (const auto& packet : firstPackets) {
                if (extractH264SequenceHeader(packet.bytes, sequenceHeader)) break;
            }
        }
        if (sequenceHeader.empty()) {
            const std::wstring message = L"O AMF não retornou SPS/PPS para montar o arquivo H.264.";
            reportStartup(message);
            setWorkerError(message);
            return;
        }

        ComPtr<IMFAttributes> attributes;
        HRESULT result = MFCreateAttributes(&attributes, 2);
        if (SUCCEEDED(result)) result = attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        ComPtr<IMFSinkWriter> writer;
        const auto output = outputPath();
        if (SUCCEEDED(result)) result = MFCreateSinkWriterFromURL(output.c_str(), nullptr, attributes.Get(), &writer);

        DWORD videoStreamIndex = 0;
        if (SUCCEEDED(result)) {
            result = configureAmfVideoStream(
                writer.Get(), width, height, fps, bitrate, sequenceHeader, videoStreamIndex);
        }
        DWORD audioStreamIndex = static_cast<DWORD>(MF_SINK_WRITER_INVALID_STREAM_INDEX);
        std::unique_ptr<AudioCapture> audioCapture;
        if (SUCCEEDED(result) && (settings.captureMicrophone || settings.captureSystemAudio)) {
            audioCapture = std::make_unique<AudioCapture>();
            result = configureAudioStream(writer.Get(), audioStreamIndex);
        }
        if (SUCCEEDED(result)) result = writer->BeginWriting();
        if (FAILED(result)) {
            const auto message = hresultMessage(L"Não foi possível abrir o contêiner MP4 para AMF", result);
            reportStartup(message);
            setWorkerError(message);
            return;
        }

        if (audioCapture) {
            std::wstring audioError;
            if (!audioCapture->start(settings.captureMicrophone, settings.captureSystemAudio, audioError)) {
                writer->Finalize();
                reportStartup(audioError);
                setWorkerError(audioError);
                return;
            }
        }

        std::wstring loopError;
        const auto writeVideoPackets = [&](const std::vector<AmfVideoPacket>& packets) {
            for (const auto& packet : packets) {
                const HRESULT writeResult = writeAmfVideoSample(
                    writer.Get(), videoStreamIndex, packet, timestamp, frameDuration);
                if (FAILED(writeResult)) {
                    loopError = hresultMessage(L"Não foi possível gravar o frame H.264 do AMF", writeResult);
                    return false;
                }
            }
            return true;
        };
        if (!writeVideoPackets(firstPackets)) {
            writer->Finalize();
            setWorkerError(loopError);
            return;
        }

        m_running = true;
        reportStartup({});

        std::uint64_t audioFramesWritten = 0;
        std::vector<std::uint8_t> audioPcm;
        std::wstring audioReadError;
        const auto captureAudio = [&](bool writeSamples) {
            if (!audioCapture) return true;
            std::uint32_t audioFrameCount = 0;
            if (!audioCapture->read(audioPcm, audioFrameCount, audioReadError)) {
                loopError = audioReadError;
                return false;
            }
            if (!writeSamples || audioFrameCount == 0) return true;
            const HRESULT audioResult = writeAudioSample(
                writer.Get(), audioStreamIndex, audioPcm, audioFrameCount, audioFramesWritten);
            if (FAILED(audioResult)) {
                loopError = hresultMessage(L"Não foi possível gravar o áudio da captura AMF", audioResult);
                return false;
            }
            audioFramesWritten += audioFrameCount;
            return true;
        };

        timestamp = (std::max)(timestamp, static_cast<LONGLONG>(firstPackets.back().timestamp));
        timestamp += frameDuration;
        auto nextFrame = std::chrono::steady_clock::now();
        while (!m_stopRequested.load()) {
            if (m_paused.load()) {
                if (!captureAudio(false)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                nextFrame = std::chrono::steady_clock::now();
                continue;
            }
            if (!captureAudio(true)) break;
            if (!capture->read(pixels)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            std::vector<AmfVideoPacket> packets;
            if (!encoder.encode(pixels, timestamp, packets, loopError)) break;
            if (!writeVideoPackets(packets)) break;
            timestamp += frameDuration;

            nextFrame += std::chrono::nanoseconds(frameDuration * 100);
            const auto now = std::chrono::steady_clock::now();
            if (nextFrame > now) {
                std::this_thread::sleep_until(nextFrame);
            } else if (now - nextFrame > std::chrono::milliseconds(250)) {
                nextFrame = now;
            }
        }

        if (loopError.empty() && !captureAudio(true)) {
            // Keep the first audio error.
        }
        std::vector<AmfVideoPacket> finalPackets;
        if (loopError.empty() && !encoder.drain(finalPackets, loopError)) {
            // Keep the encoder error.
        }
        if (loopError.empty() && !writeVideoPackets(finalPackets)) {
            // Keep the first muxing error.
        }
        const HRESULT finalizeResult = writer->Finalize();
        if (FAILED(finalizeResult) && loopError.empty()) {
            loopError = hresultMessage(L"Não foi possível finalizar a gravação AMF", finalizeResult);
        }
        if (!loopError.empty()) setWorkerError(std::move(loopError));
    } catch (const winrt::hresult_error& failure) {
        const std::wstring message = failure.message().c_str();
        reportStartup(message);
        setWorkerError(message);
    } catch (...) {
        const std::wstring message = L"Falha inesperada no backend AMF.";
        reportStartup(message);
        setWorkerError(message);
    }
#else
    reportStartup(L"Esta build não inclui suporte ao AMF.");
#endif
}

} // namespace fastrecord::recording
