#include "recording/RecorderController.h"
#include "recording/MediaFoundationBackend.h"

#include <utility>

namespace fastrecord::recording {

RecorderController::RecorderController()
    : m_backend(std::make_unique<MediaFoundationBackend>()) {}

RecorderController::~RecorderController() = default;

OperationResult RecorderController::start(const RecordingSettings& settings) {
    if (m_state != RecorderState::Idle) {
        return {false, L"Já existe uma gravação em andamento."};
    }

    m_state = RecorderState::Starting;
    RecordingSettings effectiveSettings = settings;
    effectiveSettings.engine = m_engine;
    if (!m_backend->start(effectiveSettings, m_lastError)) {
        m_state = RecorderState::Idle;
        return {false, m_lastError};
    }

    m_lastError.clear();
    m_state = RecorderState::Recording;
    return {true, L"Gravação iniciada."};
}

OperationResult RecorderController::pause() {
    if (m_state != RecorderState::Recording) {
        return {false, L"Não há uma gravação ativa para pausar."};
    }

    if (!m_backend->pause(m_lastError)) {
        return {false, m_lastError};
    }
    m_state = RecorderState::Paused;
    return {true, L"Gravação pausada."};
}

OperationResult RecorderController::resume() {
    if (m_state != RecorderState::Paused) {
        return {false, L"A gravação não está pausada."};
    }

    if (!m_backend->resume(m_lastError)) {
        return {false, m_lastError};
    }
    m_state = RecorderState::Recording;
    return {true, L"Gravação retomada."};
}

OperationResult RecorderController::stop() {
    if (m_state != RecorderState::Recording && m_state != RecorderState::Paused) {
        return {false, L"Não há uma gravação ativa."};
    }

    m_state = RecorderState::Stopping;
    if (!m_backend->stop(m_lastError)) {
        m_state = RecorderState::Idle;
        return {false, m_lastError};
    }
    m_state = RecorderState::Idle;
    return {true, L"Gravação finalizada."};
}

std::filesystem::path RecorderController::outputPath() const {
    return m_backend->outputPath();
}

} // namespace fastrecord::recording
