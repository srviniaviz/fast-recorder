#include "recording/MediaFoundationBackend.h"
#include <chrono>
#include <iostream>
#include <thread>

int wmain(int argc, wchar_t** argv) {
    using namespace fastrecord::recording;
    using namespace std::chrono_literals;
    if (argc != 2) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    MediaFoundationBackend backend;
    RecordingSettings settings;
    settings.outputDirectory = argv[1];
    settings.captureSystemAudio = true;
    settings.captureMicrophone = true;
    settings.width = 1280;
    settings.height = 720;
    settings.engine = EncoderEngine::Software;
    std::wstring error;
    settings.width = 1;
    if (backend.start(settings, error)) return 3;
    settings.width = 1280;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!backend.start(settings, error)) { std::wcerr << error; return 4; }
        std::this_thread::sleep_for(2s);
        if (!backend.pause(error)) { std::wcerr << error; return 5; }
        std::this_thread::sleep_for(1s);
        if (!backend.resume(error)) { std::wcerr << error; return 6; }
        std::this_thread::sleep_for(2s);
        if (!backend.stop(error)) { std::wcerr << error; return 7; }
        std::wcout << backend.outputPath().wstring() << L'\n';
        settings.engine = EncoderEngine::Automatic;
    }
}
