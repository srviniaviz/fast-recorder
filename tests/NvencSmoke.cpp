#include "recording/NvencBackend.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

using namespace fastrecord::recording;

int wmain(int argc, wchar_t** argv) {
    const std::filesystem::path outputDirectory = argc > 1
        ? argv[1]
        : std::filesystem::current_path() / L"nvenc-smoke-output";

    RecordingSettings settings{};
    settings.target.kind = CaptureTargetKind::CurrentMonitor;
    settings.outputDirectory = outputDirectory;
    settings.width = 1280;
    settings.height = 720;
    settings.framesPerSecond = 30;
    settings.bitrateMbps = 12;
    settings.engine = EncoderEngine::Nvenc;
    settings.captureCursor = true;

    NvencBackend backend;
    std::wstring error;
    if (!backend.start(settings, error)) {
        std::wcerr << L"NVENC smoke falhou ao iniciar: " << error << L"\n";
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));
    if (!backend.stop(error)) {
        std::wcerr << L"NVENC smoke falhou ao parar: " << error << L"\n";
        return 2;
    }

    std::wcout << backend.outputPath().wstring() << L"\n";
    return 0;
}
