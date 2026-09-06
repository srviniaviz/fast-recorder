#pragma once

#include "recording/RecordingBackend.h"

#include <atomic>
#include <future>
#include <mutex>
#include <thread>

namespace fastrecord::recording {

// Direct NVIDIA H.264 path. The class is still present when the SDK headers
// are unavailable so an explicit NVENC selection can report a useful error.
class NvencBackend final : public IRecordingBackend {
public:
    NvencBackend() = default;
    ~NvencBackend() override;

    NvencBackend(const NvencBackend&) = delete;
    NvencBackend& operator=(const NvencBackend&) = delete;

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
};

} // namespace fastrecord::recording
