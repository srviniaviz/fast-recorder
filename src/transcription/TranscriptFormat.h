#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fastrecord::transcription {

struct Segment {
    std::int64_t startMilliseconds{0};
    std::int64_t endMilliseconds{0};
    std::wstring text;
};

std::wstring formatPlainText(const std::vector<Segment>& segments);
std::wstring formatSrt(const std::vector<Segment>& segments);
std::string encodeUtf8(const std::wstring& text);
std::filesystem::path srtPathForVideo(const std::filesystem::path& videoPath);
bool saveTranscriptSrt(
    const std::filesystem::path& destination,
    const std::vector<Segment>& segments,
    std::wstring& error);

} // namespace fastrecord::transcription
