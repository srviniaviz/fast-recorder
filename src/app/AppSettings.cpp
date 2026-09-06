#include "app/AppSettings.h"

#include <Windows.h>

#include <algorithm>

namespace fastrecord {

namespace {

constexpr wchar_t kSettingsKey[] = L"Software\\Fast Record";

DWORD readDword(HKEY key, const wchar_t* name, DWORD fallback) {
    DWORD value = fallback;
    DWORD size = sizeof(value);
    return RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS
        ? value
        : fallback;
}

std::wstring readString(HKEY key, const wchar_t* name, const std::wstring& fallback) {
    DWORD size = 0;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size < sizeof(wchar_t)) {
        return fallback;
    }

    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS) {
        return fallback;
    }
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value.empty() ? fallback : value;
}

bool writeDword(HKEY key, const wchar_t* name, DWORD value) {
    return RegSetValueExW(
        key,
        name,
        0,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&value),
        sizeof(value)) == ERROR_SUCCESS;
}

bool writeString(HKEY key, const wchar_t* name, const std::wstring& value) {
    const DWORD size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(
        key,
        name,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        size) == ERROR_SUCCESS;
}

bool isValidArea(const std::wstring& area) {
    return area == L"monitor" || area == L"window" || area == L"region";
}

bool isValidResolution(const std::wstring& resolution) {
    return resolution == L"1280x720" || resolution == L"1920x1080" ||
        resolution == L"2560x1440" || resolution == L"3840x2160";
}

} // namespace

AppSettings loadAppSettings() {
    AppSettings settings;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return settings;
    }

    const DWORD engine = readDword(key, L"Encoder", 0);
    if (engine <= static_cast<DWORD>(recording::EncoderEngine::Software)) {
        settings.engine = static_cast<recording::EncoderEngine>(engine);
    }

    const std::wstring area = readString(key, L"CaptureArea", settings.captureArea);
    if (isValidArea(area)) {
        settings.captureArea = area;
    }

    const std::wstring resolution = readString(key, L"Resolution", settings.resolution);
    if (isValidResolution(resolution)) {
        settings.resolution = resolution;
    }

    settings.bitrateMbps = std::clamp(
        static_cast<int>(readDword(key, L"BitrateMbps", 20)),
        4,
        80);
    settings.microphoneEnabled = readDword(key, L"MicrophoneEnabled", 1) != 0;
    settings.webcamEnabled = readDword(key, L"WebcamEnabled", 0) != 0;
    settings.alwaysOnTop = readDword(key, L"AlwaysOnTop", 0) != 0;

    RegCloseKey(key);
    return settings;
}

bool saveAppSettings(const AppSettings& settings) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kSettingsKey,
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_WRITE,
            nullptr,
            &key,
            nullptr) != ERROR_SUCCESS) {
        return false;
    }

    const bool saved =
        writeDword(key, L"Encoder", static_cast<DWORD>(settings.engine)) &&
        writeString(key, L"CaptureArea", settings.captureArea) &&
        writeString(key, L"Resolution", settings.resolution) &&
        writeDword(key, L"BitrateMbps", static_cast<DWORD>(std::clamp(settings.bitrateMbps, 4, 80))) &&
        writeDword(key, L"MicrophoneEnabled", settings.microphoneEnabled ? 1u : 0u) &&
        writeDword(key, L"WebcamEnabled", settings.webcamEnabled ? 1u : 0u) &&
        writeDword(key, L"AlwaysOnTop", settings.alwaysOnTop ? 1u : 0u);

    RegCloseKey(key);
    return saved;
}

} // namespace fastrecord
