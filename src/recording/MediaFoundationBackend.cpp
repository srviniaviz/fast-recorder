#include "recording/MediaFoundationBackend.h"
#include "recording/AudioCapture.h"
#include "recording/GraphicsCapture.h"
#include "recording/QsvProbe.h"

#include <Windows.h>
#include <mfapi.h>
#include <roapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <system_error>
#include <utility>

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
        // This worker owns the last WinRT apartment in the standalone recorder.
        // Release cached activation factories before Windows unloads their modules.
        winrt::clear_factory_cache();
        MFShutdown();
        RoUninitialize();
    }
};

std::wstring hresultMessage(const wchar_t* operation, HRESULT result) {
    wchar_t buffer[96]{};
    swprintf_s(buffer, L"%s (HRESULT 0x%08X).", operation, static_cast<unsigned int>(result));
    return buffer;
}

std::filesystem::path createOutputPath(const std::filesystem::path& directory) {
    SYSTEMTIME now{};
    GetLocalTime(&now);

    wchar_t name[96]{};
    swprintf_s(
        name,
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

} // namespace

MediaFoundationBackend::~MediaFoundationBackend() {
    std::wstring ignored;
    stop(ignored);
}

bool MediaFoundationBackend::start(const RecordingSettings& settings, std::wstring& error) {
    if (m_worker.joinable() || m_running.load()) {
        error = L"Já existe uma captura em andamento.";
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
                setWorkerError(L"Falha inesperada na captura de vídeo.");
            }
            m_running = false;
        });
        error = startupFuture.get();
    } catch (...) {
        if (m_worker.joinable()) m_worker.join();
        std::scoped_lock lock(m_mutex);
        error = m_workerError.empty() ? L"Não foi possível iniciar a captura." : m_workerError;
        return false;
    }
    if (!error.empty()) {
        if (m_worker.joinable()) {
            m_worker.join();
        }
        return false;
    }
    return true;
}

bool MediaFoundationBackend::pause(std::wstring& error) {
    if (!m_running.load() || m_paused.load()) {
        std::scoped_lock lock(m_mutex);
        error = m_workerError.empty() ? L"A gravação não está ativa." : m_workerError;
        return false;
    }
    m_paused = true;
    return true;
}

bool MediaFoundationBackend::resume(std::wstring& error) {
    if (!m_running.load() || !m_paused.load()) {
        std::scoped_lock lock(m_mutex);
        error = m_workerError.empty() ? L"A gravação não está pausada." : m_workerError;
        return false;
    }
    m_paused = false;
    return true;
}

bool MediaFoundationBackend::stop(std::wstring& error) {
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

std::filesystem::path MediaFoundationBackend::outputPath() const {
    std::scoped_lock lock(m_mutex);
    return m_outputPath;
}

void MediaFoundationBackend::setWorkerError(std::wstring error) {
    std::scoped_lock lock(m_mutex);
    m_workerError = std::move(error);
}

void MediaFoundationBackend::recordLoop(
    RecordingSettings settings,
    std::promise<std::wstring> startupResult) {
    bool startupReported = false;
    auto reportStartup = [&](std::wstring message) {
        if (!startupReported) {
            startupResult.set_value(std::move(message));
            startupReported = true;
        }
    };

    MediaRuntime runtime;

    const auto width = settings.width;
    const auto height = settings.height;
    const auto fps = settings.framesPerSecond;
    const auto bitrate = settings.bitrateMbps * 1'000'000u;
    const DWORD frameBytes = width * height * 4u;
    std::unique_ptr<GraphicsCapture> capture;
    std::vector<BYTE> pixels;
    try {
        capture = std::make_unique<GraphicsCapture>(settings);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!capture->read(pixels)) {
            if (m_stopRequested || std::chrono::steady_clock::now() >= deadline) {
                throw winrt::hresult_error(E_ABORT, L"O Windows não entregou frames do monitor.");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    } catch (const winrt::hresult_error& failure) {
        reportStartup(failure.message().c_str());
        capture.reset();
        return;
    }

    std::unique_ptr<AudioCapture> audioCapture;
    if (settings.captureMicrophone || settings.captureSystemAudio) {
        audioCapture = std::make_unique<AudioCapture>();
    }

    QsvDeviceManager qsvDevice;
    if (m_requireQsv) {
        std::wstring qsvError;
        if (!createQsvDeviceManager(qsvDevice, qsvError)) {
            reportStartup(qsvError);
            capture.reset();
            return;
        }
    }

    ComPtr<IMFAttributes> attributes;
    HRESULT result = MFCreateAttributes(&attributes, 2);
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(
            MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
            settings.engine == EncoderEngine::Software ? FALSE : TRUE);
    }
    if (SUCCEEDED(result) && m_requireQsv) {
        result = attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, qsvDevice.manager.Get());
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    }

    ComPtr<IMFSinkWriter> writer;
    const auto output = outputPath();
    if (SUCCEEDED(result)) {
        result = MFCreateSinkWriterFromURL(output.c_str(), nullptr, attributes.Get(), &writer);
    }

    ComPtr<IMFMediaType> outputType;
    if (SUCCEEDED(result)) {
        result = MFCreateMediaType(&outputType);
    }
    if (SUCCEEDED(result)) {
        result = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    }
    if (SUCCEEDED(result)) {
        result = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    }
    if (SUCCEEDED(result)) {
        result = outputType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    }
    if (SUCCEEDED(result)) {
        result = outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    }
    if (SUCCEEDED(result)) {
        result = MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, width, height);
    }
    if (SUCCEEDED(result)) {
        result = MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, fps, 1);
    }
    if (SUCCEEDED(result)) {
        result = MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    }

    DWORD streamIndex = 0;
    if (SUCCEEDED(result)) {
        result = writer->AddStream(outputType.Get(), &streamIndex);
    }

    ComPtr<IMFMediaType> inputType;
    if (SUCCEEDED(result)) {
        result = MFCreateMediaType(&inputType);
    }
    if (SUCCEEDED(result)) {
        result = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    }
    if (SUCCEEDED(result)) {
        result = inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    }
    if (SUCCEEDED(result)) {
        result = inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    }
    if (SUCCEEDED(result)) {
        result = inputType->SetUINT32(MF_MT_DEFAULT_STRIDE, width * 4u);
    }
    if (SUCCEEDED(result)) {
        result = MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, width, height);
    }
    if (SUCCEEDED(result)) {
        result = MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, fps, 1);
    }
    if (SUCCEEDED(result)) {
        result = MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    }
    if (SUCCEEDED(result)) {
        result = writer->SetInputMediaType(streamIndex, inputType.Get(), nullptr);
    }
    DWORD audioStreamIndex = static_cast<DWORD>(MF_SINK_WRITER_INVALID_STREAM_INDEX);
    if (SUCCEEDED(result) && audioCapture) {
        result = configureAudioStream(writer.Get(), audioStreamIndex);
    }
    if (SUCCEEDED(result)) {
        result = writer->BeginWriting();
    }

    if (FAILED(result)) {
        reportStartup(hresultMessage(
            m_requireQsv
                ? L"Não foi possível iniciar o encoder Intel QSV H.264"
                : L"Não foi possível iniciar o encoder H.264",
            result));
        writer.Reset();
        inputType.Reset();
        outputType.Reset();
        attributes.Reset();
        capture.reset();
        return;
    }

    std::wstring audioStartupError;
    if (audioCapture && !audioCapture->start(
            settings.captureMicrophone,
            settings.captureSystemAudio,
            audioStartupError)) {
        writer->Finalize();
        reportStartup(audioStartupError);
        writer.Reset();
        inputType.Reset();
        outputType.Reset();
        attributes.Reset();
        capture.reset();
        return;
    }

    m_running = true;
    reportStartup({});

    const LONGLONG frameDuration = kHundredNanosecondsPerSecond / fps;
    LONGLONG timestamp = 0;
    auto nextFrame = std::chrono::steady_clock::now();
    HRESULT writeResult = S_OK;
    const wchar_t* writeStage = L"o encoder de vídeo";
    std::uint64_t audioFramesWritten = 0;
    std::vector<std::uint8_t> audioPcm;
    std::wstring audioReadError;

    const auto captureAudio = [&](bool writeSamples) {
        if (!audioCapture) {
            return true;
        }
        std::uint32_t audioFrameCount = 0;
        if (!audioCapture->read(audioPcm, audioFrameCount, audioReadError)) {
            return false;
        }
        if (!writeSamples || audioFrameCount == 0) {
            return true;
        }
        writeStage = L"o áudio do microfone";
        writeResult = writeAudioSample(
            writer.Get(),
            audioStreamIndex,
            audioPcm,
            audioFrameCount,
            audioFramesWritten);
        if (SUCCEEDED(writeResult)) {
            audioFramesWritten += audioFrameCount;
        }
        return SUCCEEDED(writeResult);
    };

    while (!m_stopRequested.load()) {
        if (m_paused.load()) {
            if (!captureAudio(false)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            nextFrame = std::chrono::steady_clock::now();
            continue;
        }

        if (!captureAudio(true)) {
            break;
        }

        try {
            capture->read(pixels);
        } catch (const winrt::hresult_error& failure) {
            setWorkerError(failure.message().c_str());
            break;
        }

        ComPtr<IMFMediaBuffer> buffer;
        writeStage = L"o buffer de vídeo";
        writeResult = MFCreateMemoryBuffer(frameBytes, &buffer);
        BYTE* destination = nullptr;
        if (SUCCEEDED(writeResult)) {
            writeResult = buffer->Lock(&destination, nullptr, nullptr);
        }
        if (SUCCEEDED(writeResult)) {
            std::memcpy(destination, pixels.data(), frameBytes);
            buffer->Unlock();
            destination = nullptr;
            writeResult = buffer->SetCurrentLength(frameBytes);
        }
        if (destination != nullptr) {
            buffer->Unlock();
        }

        ComPtr<IMFSample> sample;
        writeStage = L"a amostra de vídeo";
        if (SUCCEEDED(writeResult)) {
            writeResult = MFCreateSample(&sample);
        }
        if (SUCCEEDED(writeResult)) {
            writeResult = sample->AddBuffer(buffer.Get());
        }
        if (SUCCEEDED(writeResult)) {
            writeResult = sample->SetSampleTime(timestamp);
        }
        if (SUCCEEDED(writeResult)) {
            writeResult = sample->SetSampleDuration(frameDuration);
        }
        if (SUCCEEDED(writeResult)) {
            writeStage = L"o frame no encoder";
            writeResult = writer->WriteSample(streamIndex, sample.Get());
        }
        if (FAILED(writeResult)) {
            break;
        }

        timestamp += frameDuration;
        nextFrame += std::chrono::nanoseconds(frameDuration * 100);
        const auto now = std::chrono::steady_clock::now();
        if (nextFrame > now) {
            std::this_thread::sleep_until(nextFrame);
        } else if (now - nextFrame > std::chrono::milliseconds(250)) {
            nextFrame = now;
        }
    }

    if (SUCCEEDED(writeResult) && audioReadError.empty()) {
        captureAudio(true);
    }

    const HRESULT finalizeResult = writer->Finalize();
    if (!audioReadError.empty()) {
        setWorkerError(audioReadError);
    } else if (FAILED(writeResult)) {
        setWorkerError(hresultMessage(writeStage, writeResult));
    } else if (FAILED(finalizeResult)) {
        setWorkerError(hresultMessage(L"Não foi possível finalizar o arquivo MP4", finalizeResult));
    }

    m_running = false;
    writer.Reset();
    inputType.Reset();
    outputType.Reset();
    attributes.Reset();
    audioCapture.reset();
    capture.reset();
}

} // namespace fastrecord::recording
