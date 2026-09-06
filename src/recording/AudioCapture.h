#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fastrecord::recording {

class AudioCapture final {
public:
    static constexpr std::uint32_t kSampleRate = 48'000;
    static constexpr std::uint16_t kChannels = 2;
    static constexpr std::uint16_t kBitsPerSample = 16;
    static constexpr std::uint16_t kBlockAlignment =
        kChannels * (kBitsPerSample / 8);

    AudioCapture();
    ~AudioCapture();

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    bool start(bool captureMicrophone, bool captureSystemAudio, std::wstring& error);
    bool read(std::vector<std::uint8_t>& pcm, std::uint32_t& frameCount, std::wstring& error);
    bool discard(std::wstring& error);
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fastrecord::recording
