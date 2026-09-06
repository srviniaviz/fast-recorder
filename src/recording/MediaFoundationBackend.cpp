#include "recording/MediaFoundationBackend.h"

#include <Windows.h>
#include <mfapi.h>
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

void drawCursor(HDC target, const RECT& source, std::uint32_t width, std::uint32_t height) {
    CURSORINFO cursor{};
    cursor.cbSize = sizeof(cursor);
    if (!GetCursorInfo(&cursor) || (cursor.flags & CURSOR_SHOWING) == 0 || cursor.hCursor == nullptr) {
        return;
    }

    ICONINFO icon{};
    if (!GetIconInfo(cursor.hCursor, &icon)) {
        return;
    }

    const int sourceWidth = std::max(1L, source.right - source.left);
    const int sourceHeight = std::max(1L, source.bottom - source.top);
    const int x = static_cast<int>(
        (cursor.ptScreenPos.x - source.left - static_cast<LONG>(icon.xHotspot)) *
        static_cast<double>(width) / sourceWidth);
    const int y = static_cast<int>(
        (cursor.ptScreenPos.y - source.top - static_cast<LONG>(icon.yHotspot)) *
        static_cast<double>(height) / sourceHeight);

    DrawIconEx(target, x, y, cursor.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
    if (icon.hbmColor != nullptr) {
        DeleteObject(icon.hbmColor);
    }
    if (icon.hbmMask != nullptr) {
        DeleteObject(icon.hbmMask);
    }
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
    if (settings.outputDirectory.empty() || settings.width == 0 || settings.height == 0 ||
        settings.framesPerSecond == 0) {
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
    m_worker = std::thread(
        &MediaFoundationBackend::recordLoop,
        this,
        settings,
        std::move(startupResult));

    error = startupFuture.get();
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
        error = L"A gravação não está ativa.";
        return false;
    }
    m_paused = true;
    return true;
}

bool MediaFoundationBackend::resume(std::wstring& error) {
    if (!m_running.load() || !m_paused.load()) {
        error = L"A gravação não está pausada.";
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

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        reportStartup(hresultMessage(L"Falha ao inicializar COM", comResult));
        return;
    }

    const HRESULT mfResult = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(mfResult)) {
        reportStartup(hresultMessage(L"Falha ao iniciar o Media Foundation", mfResult));
        if (comInitialized) {
            CoUninitialize();
        }
        return;
    }

    HMONITOR monitor = settings.target.monitor;
    if (monitor == nullptr) {
        POINT origin{};
        monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    }

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo)) {
        reportStartup(L"Não foi possível localizar o monitor que será gravado.");
        MFShutdown();
        if (comInitialized) {
            CoUninitialize();
        }
        return;
    }

    const RECT source = monitorInfo.rcMonitor;
    const auto width = settings.width & ~1u;
    const auto height = settings.height & ~1u;
    const auto fps = std::clamp(settings.framesPerSecond, 1u, 120u);
    const auto bitrate = std::clamp(settings.bitrateMbps, 4u, 100u) * 1'000'000u;
    const DWORD frameBytes = width * height * 4u;

    HDC screen = GetDC(nullptr);
    HDC frameDc = screen != nullptr ? CreateCompatibleDC(screen) : nullptr;
    void* pixels = nullptr;
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(width);
    bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(height);
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    HBITMAP frameBitmap = frameDc != nullptr
        ? CreateDIBSection(frameDc, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0)
        : nullptr;
    HGDIOBJ previousBitmap = frameBitmap != nullptr ? SelectObject(frameDc, frameBitmap) : nullptr;

    const auto releaseGraphics = [&]() {
        if (frameDc != nullptr && previousBitmap != nullptr) {
            SelectObject(frameDc, previousBitmap);
        }
        if (frameBitmap != nullptr) {
            DeleteObject(frameBitmap);
        }
        if (frameDc != nullptr) {
            DeleteDC(frameDc);
        }
        if (screen != nullptr) {
            ReleaseDC(nullptr, screen);
        }
    };

    if (screen == nullptr || frameDc == nullptr || frameBitmap == nullptr || pixels == nullptr) {
        reportStartup(L"Não foi possível preparar a superfície de captura.");
        releaseGraphics();
        MFShutdown();
        if (comInitialized) {
            CoUninitialize();
        }
        return;
    }
    SetStretchBltMode(frameDc, HALFTONE);

    ComPtr<IMFAttributes> attributes;
    HRESULT result = MFCreateAttributes(&attributes, 2);
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(
            MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
            settings.engine == EncoderEngine::Software ? FALSE : TRUE);
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
    if (SUCCEEDED(result)) {
        result = writer->BeginWriting();
    }

    if (FAILED(result)) {
        reportStartup(hresultMessage(L"Não foi possível iniciar o encoder H.264", result));
        releaseGraphics();
        MFShutdown();
        if (comInitialized) {
            CoUninitialize();
        }
        return;
    }

    m_running = true;
    reportStartup({});

    const LONGLONG frameDuration = kHundredNanosecondsPerSecond / fps;
    LONGLONG timestamp = 0;
    auto nextFrame = std::chrono::steady_clock::now();
    HRESULT writeResult = S_OK;

    while (!m_stopRequested.load()) {
        if (m_paused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            nextFrame = std::chrono::steady_clock::now();
            continue;
        }

        const int sourceWidth = source.right - source.left;
        const int sourceHeight = source.bottom - source.top;
        if (!StretchBlt(
                frameDc,
                0,
                0,
                static_cast<int>(width),
                static_cast<int>(height),
                screen,
                source.left,
                source.top,
                sourceWidth,
                sourceHeight,
                SRCCOPY | CAPTUREBLT)) {
            writeResult = HRESULT_FROM_WIN32(GetLastError());
            break;
        }
        if (settings.captureCursor) {
            drawCursor(frameDc, source, width, height);
        }

        ComPtr<IMFMediaBuffer> buffer;
        writeResult = MFCreateMemoryBuffer(frameBytes, &buffer);
        BYTE* destination = nullptr;
        if (SUCCEEDED(writeResult)) {
            writeResult = buffer->Lock(&destination, nullptr, nullptr);
        }
        if (SUCCEEDED(writeResult)) {
            std::memcpy(destination, pixels, frameBytes);
            buffer->Unlock();
            destination = nullptr;
            writeResult = buffer->SetCurrentLength(frameBytes);
        }
        if (destination != nullptr) {
            buffer->Unlock();
        }

        ComPtr<IMFSample> sample;
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

    const HRESULT finalizeResult = writer->Finalize();
    if (FAILED(writeResult)) {
        setWorkerError(hresultMessage(L"A captura foi interrompida", writeResult));
    } else if (FAILED(finalizeResult)) {
        setWorkerError(hresultMessage(L"Não foi possível finalizar o arquivo MP4", finalizeResult));
    }

    m_running = false;
    releaseGraphics();
    MFShutdown();
    if (comInitialized) {
        CoUninitialize();
    }
}

} // namespace fastrecord::recording
