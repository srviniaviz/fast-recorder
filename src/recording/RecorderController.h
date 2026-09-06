#pragma once

#include "recording/RecordingBackend.h"

#include <string>

namespace fastrecord::recording {

enum class EncoderEngine {
    Automatic,
    Nvenc,
    Amf,
    Software,
};

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
    OperationResult start();
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

private:
    RecorderState m_state{RecorderState::Idle};
    EncoderEngine m_engine{EncoderEngine::Automatic};
    std::wstring m_lastError;
};

} // namespace fastrecord::recording
