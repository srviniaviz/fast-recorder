// Records a test window while it is resized, maximized and restored, and
// checks that the recording keeps going until it is stopped.
//
//   window-resize-smoke.exe <output-dir> [nvenc|software]
#include "recording/MediaFoundationBackend.h"
#include "recording/NvencBackend.h"

#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace fastrecord::recording;
using namespace std::chrono_literals;

namespace {

LRESULT CALLBACK testWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        const auto tick = GetTickCount64() / 16;
        HBRUSH brush = CreateSolidBrush(RGB(tick % 256, (tick * 3) % 256, 160));
        FillRect(dc, &client, brush);
        DeleteObject(brush);
        const std::wstring text = L"Fast Record resize smoke " + std::to_wstring(tick);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, text.c_str(), -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

double inspectVideoSeconds(const std::filesystem::path& path, std::uint64_t& frames) {
    frames = 0;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) return -1;
    LONGLONG first = LLONG_MAX;
    LONGLONG last = 0;
    {
        Microsoft::WRL::ComPtr<IMFSourceReader> reader;
        if (SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader))) {
            const auto video = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
            reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
            reader->SetStreamSelection(video, TRUE);
            for (;;) {
                DWORD stream = 0, flags = 0;
                LONGLONG time = 0;
                Microsoft::WRL::ComPtr<IMFSample> sample;
                if (FAILED(reader->ReadSample(video, 0, &stream, &flags, &time, &sample)) ||
                    (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                    break;
                }
                if (!sample) continue;
                ++frames;
                first = (std::min)(first, time);
                last = (std::max)(last, time);
            }
        }
    }
    MFShutdown();
    return first == LLONG_MAX ? 0.0 : static_cast<double>(last - first) / 10'000'000.0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::filesystem::path outputDirectory = argc > 1
        ? argv[1]
        : std::filesystem::current_path() / L"window-resize-smoke-output";
    const bool software = argc > 2 && std::wstring(argv[2]) == L"software";
    std::filesystem::create_directories(outputDirectory);

    std::atomic<HWND> window{nullptr};
    std::thread ui([&window] {
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.lpfnWndProc = testWindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"FastRecordResizeSmoke";
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&windowClass);
        HWND hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"Fast Record resize smoke",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640, 400,
            nullptr, nullptr, windowClass.hInstance, nullptr);
        SetTimer(hwnd, 1, 16, nullptr);
        window = hwnd;
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    });
    while (!window.load()) std::this_thread::sleep_for(10ms);
    const HWND hwnd = window.load();
    SetForegroundWindow(hwnd);
    std::this_thread::sleep_for(500ms);

    RecordingSettings settings{};
    settings.target.kind = CaptureTargetKind::SelectedWindow;
    settings.target.window = hwnd;
    settings.outputDirectory = outputDirectory;
    settings.width = 1280;
    settings.height = 720;
    settings.framesPerSecond = 30;
    settings.bitrateMbps = 8;
    settings.engine = software ? EncoderEngine::Software : EncoderEngine::Nvenc;
    settings.captureMicrophone = false;
    settings.captureSystemAudio = false;

    std::unique_ptr<IRecordingBackend> backend;
    if (software) backend = std::make_unique<MediaFoundationBackend>();
    else backend = std::make_unique<NvencBackend>();

    std::wstring error;
    if (!backend->start(settings, error)) {
        std::wcerr << L"Falhou ao iniciar: " << error << L"\n";
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        ui.join();
        return 1;
    }

    struct Step { const wchar_t* name; int action; int width; int height; };
    const Step steps[] = {
        {L"aumentar para 1100x700", 0, 1100, 700},
        {L"diminuir para 500x300", 0, 500, 300},
        {L"maximizar", 1, 0, 0},
        {L"restaurar", 2, 0, 0},
        {L"retrato 400x800", 0, 400, 800},
        {L"voltar para 800x500", 0, 800, 500},
    };
    bool stoppedEarly = false;
    const auto checkRunning = [&](const wchar_t* after) {
        for (int i = 0; i < 10; ++i) {
            std::this_thread::sleep_for(100ms);
            if (!backend->running()) {
                std::wcerr << L"A gravação parou sozinha depois de: " << after << L"\n";
                stoppedEarly = true;
                return false;
            }
        }
        return true;
    };
    checkRunning(L"início");
    for (const auto& step : steps) {
        if (stoppedEarly) break;
        if (step.action == 0) {
            SetWindowPos(hwnd, nullptr, 0, 0, step.width, step.height, SWP_NOMOVE | SWP_NOZORDER);
        } else {
            ShowWindow(hwnd, step.action == 1 ? SW_MAXIMIZE : SW_RESTORE);
        }
        std::wcout << L"  passo: " << step.name << L"\n";
        checkRunning(step.name);
    }

    const bool stopped = backend->stop(error);
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    ui.join();
    if (!stopped && !error.empty()) {
        std::wcerr << L"Erro ao parar: " << error << L"\n";
    }

    std::uint64_t frames = 0;
    const double seconds = inspectVideoSeconds(backend->outputPath(), frames);
    std::wcout << backend->outputPath().wstring() << L"\n"
               << L"Frames: " << frames << L" · duração do vídeo: " << seconds << L" s\n";
    if (stoppedEarly) return 2;
    if (seconds < 6.0) {
        std::wcerr << L"O vídeo ficou curto demais para ~7 s de gravação.\n";
        return 3;
    }
    return 0;
}
