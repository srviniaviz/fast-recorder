#pragma once

#include "recording/RecorderController.h"

#include <string>

namespace fastrecord {

struct AppSettings {
    recording::EncoderEngine engine{recording::EncoderEngine::Automatic};
    recording::VideoCodec codec{recording::VideoCodec::H264};
    std::wstring captureArea{L"monitor"};
    std::wstring resolution{L"1920x1080"};
    int framesPerSecond{30};
    int bitrateMbps{20};
    bool microphoneEnabled{true};
    bool systemAudioEnabled{true};
    bool alwaysOnTop{false};
};

// The optional registry path keeps the production key stable while allowing
// isolated round-trip tests to use their own temporary key.
AppSettings loadAppSettings(const std::wstring& registryPath = L"Software\\Fast Record");
bool saveAppSettings(
    const AppSettings& settings,
    const std::wstring& registryPath = L"Software\\Fast Record");

} // namespace fastrecord
