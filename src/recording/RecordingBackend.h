#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace fastrecord::recording {

enum class EncoderEngine {
    Automatic,
    Nvenc,
    Amf,
    Software,
    // Kept after Software so settings written by older builds keep their
    // original numeric values.
    Qsv,
};

enum class VideoCodec {
    H264,
    Av1,
};

enum class CaptureTargetKind {
    CurrentMonitor,
    SelectedMonitor,
    SelectedWindow,
    SelectedRegion,
};

struct CaptureTarget {
    CaptureTargetKind kind{CaptureTargetKind::CurrentMonitor};
    HMONITOR monitor{nullptr};
    HWND window{nullptr};
};

// Lets the UI thread ask a running window capture to follow another window.
// The capture worker polls take() between frames.
class CaptureRetarget final {
public:
    void request(HWND window) {
        std::scoped_lock lock(m_mutex);
        m_pending = window;
    }

    HWND take() {
        std::scoped_lock lock(m_mutex);
        return std::exchange(m_pending, nullptr);
    }

private:
    std::mutex m_mutex;
    HWND m_pending{nullptr};
};

struct RecordingSettings {
    CaptureTarget target{};
    std::filesystem::path outputDirectory{};
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    std::uint32_t framesPerSecond{30};
    std::uint32_t bitrateMbps{20};
    // Desktop coordinates used when target.kind is SelectedRegion.
    RECT captureRegion{};
    EncoderEngine engine{EncoderEngine::Automatic};
    VideoCodec codec{VideoCodec::H264};
    bool captureCursor{true};
    bool captureSystemAudio{true};
    bool captureMicrophone{false};
    // Optional; only honoured when target.kind is SelectedWindow.
    std::shared_ptr<CaptureRetarget> retarget;
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
