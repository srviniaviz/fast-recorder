#pragma once

#include <cstdint>
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

} // namespace fastrecord::transcription
