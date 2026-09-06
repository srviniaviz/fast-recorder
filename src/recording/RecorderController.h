#pragma once

#include "recording/RecordingBackend.h"

#include <filesystem>
#include <memory>
#include <string>

namespace fastrecord::recording {

enum class RecorderState {
    Idle,
    Starting,
    Recording,
    Paused,
    Stopping,
};

struct OperationResult {
    bool success{false};
    std::wstring message;
};

class RecorderController final {
public:
    RecorderController();
    ~RecorderController();

    RecorderController(const RecorderController&) = delete;
    RecorderController& operator=(const RecorderController&) = delete;

    OperationResult start(const RecordingSettings& settings);
    OperationResult pause();
    OperationResult resume();
    OperationResult stop();

    bool isRecording() const noexcept {
        return m_state == RecorderState::Recording || m_state == RecorderState::Paused;
    }
    bool isPaused() const noexcept { return m_state == RecorderState::Paused; }
    RecorderState state() const noexcept { return m_state; }
    const std::wstring& lastError() const noexcept { return m_lastError; }

    void setEngine(EncoderEngine engine) noexcept { m_engine = engine; }
    EncoderEngine engine() const noexcept { return m_engine; }
    std::filesystem::path outputPath() const;

private:
    RecorderState m_state{RecorderState::Idle};
    EncoderEngine m_engine{EncoderEngine::Automatic};
    std::wstring m_lastError;
    std::unique_ptr<IRecordingBackend> m_backend;
};

} // namespace fastrecord::recording
