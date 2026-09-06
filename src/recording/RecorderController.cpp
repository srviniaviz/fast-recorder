#include "recording/RecorderController.h"

namespace fastrecord::recording {

OperationResult RecorderController::start() {
    if (m_state == RecorderState::Recording || m_state == RecorderState::Starting) {
        return {false, L"Já existe uma gravação em andamento."};
    }

    // This is deliberately explicit until the Windows Graphics Capture and
    // Media Foundation pipeline is connected. The shell is already usable,
    // but it must not pretend to have created a video file.
    m_lastError = L"O backend de captura ainda não foi conectado.";
    return {false, m_lastError};
}

OperationResult RecorderController::pause() {
    if (m_state != RecorderState::Recording) {
        return {false, L"Não há uma gravação ativa para pausar."};
    }

    m_state = RecorderState::Paused;
    return {true, L"Gravação pausada."};
}

OperationResult RecorderController::resume() {
    if (m_state != RecorderState::Paused) {
        return {false, L"A gravação não está pausada."};
    }

    m_state = RecorderState::Recording;
    return {true, L"Gravação retomada."};
}

OperationResult RecorderController::stop() {
    if (m_state != RecorderState::Recording && m_state != RecorderState::Paused) {
        return {false, L"Não há uma gravação ativa."};
    }

    m_state = RecorderState::Stopping;
    m_state = RecorderState::Idle;
    return {true, L"Gravação finalizada."};
}

} // namespace fastrecord::recording
