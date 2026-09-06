#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace fastrecord::recording {

enum class EncoderEngine {
    Automatic,
    Nvenc,
    Amf,
    Software,
};

enum class VideoCodec {
    H264,
    Av1,
};

enum class CaptureTargetKind {
    CurrentMonitor,
    SelectedMonitor,
    SelectedWindow,
};

struct CaptureTarget {
    CaptureTargetKind kind{CaptureTargetKind::CurrentMonitor};
    HMONITOR monitor{nullptr};
    HWND window{nullptr};
};

struct RecordingSettings {
    CaptureTarget target{};
    std::filesystem::path outputDirectory{};
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    std::uint32_t framesPerSecond{30};
    std::uint32_t bitrateMbps{20};
    EncoderEngine engine{EncoderEngine::Automatic};
    VideoCodec codec{VideoCodec::H264};
    bool captureCursor{true};
    bool captureSystemAudio{true};
    bool captureMicrophone{false};
};

class IRecordingBackend {
public:
    virtual ~IRecordingBackend() = default;

    virtual bool start(const RecordingSettings& settings, std::wstring& error) = 0;
    virtual bool pause(std::wstring& error) = 0;
    virtual bool resume(std::wstring& error) = 0;
    virtual bool stop(std::wstring& error) = 0;
    virtual std::filesystem::path outputPath() const = 0;
    virtual bool running() const noexcept = 0;
};

} // namespace fastrecord::recording
