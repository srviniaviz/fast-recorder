#pragma once

#include "recording/RecorderController.h"

#include <string>

namespace fastrecord {

struct AppSettings {
    recording::EncoderEngine engine{recording::EncoderEngine::Automatic};
    std::wstring captureArea{L"monitor"};
    std::wstring resolution{L"1920x1080"};
    int bitrateMbps{20};
    bool microphoneEnabled{true};
    bool webcamEnabled{false};
    bool alwaysOnTop{false};
};

AppSettings loadAppSettings();
bool saveAppSettings(const AppSettings& settings);

} // namespace fastrecord
