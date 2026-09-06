#pragma once

#include "recording/RecordingBackend.h"

#include <atomic>
#include <future>
#include <mutex>
#include <thread>

namespace fastrecord::recording {

class MediaFoundationBackend final : public IRecordingBackend {
public:
    explicit MediaFoundationBackend(bool requireQsv = false)
        : m_requireQsv(requireQsv) {}
    ~MediaFoundationBackend() override;

    MediaFoundationBackend(const MediaFoundationBackend&) = delete;
    MediaFoundationBackend& operator=(const MediaFoundationBackend&) = delete;

    bool start(const RecordingSettings& settings, std::wstring& error) override;
    bool pause(std::wstring& error) override;
    bool resume(std::wstring& error) override;
    bool stop(std::wstring& error) override;
    std::filesystem::path outputPath() const override;
    bool running() const noexcept override { return m_running.load(); }

private:
    void recordLoop(RecordingSettings settings, std::promise<std::wstring> startupResult);
    void setWorkerError(std::wstring error);

    std::atomic_bool m_stopRequested{false};
    std::atomic_bool m_paused{false};
    std::atomic_bool m_running{false};
    mutable std::mutex m_mutex;
    std::thread m_worker;
    std::filesystem::path m_outputPath;
    std::wstring m_workerError;
    bool m_requireQsv{false};
};

} // namespace fastrecord::recording
