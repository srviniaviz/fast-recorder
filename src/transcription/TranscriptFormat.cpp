#include "transcription/TranscriptFormat.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace fastrecord::transcription {

namespace {

std::wstring formatTimestamp(std::int64_t milliseconds) {
    const std::int64_t safe = std::max<std::int64_t>(0, milliseconds);
    const std::int64_t hours = safe / 3'600'000;
    const std::int64_t minutes = (safe / 60'000) % 60;
    const std::int64_t seconds = (safe / 1'000) % 60;
    const std::int64_t remainder = safe % 1'000;

    std::wostringstream output;
    output << std::setfill(L'0') << std::setw(2) << hours << L':'
           << std::setw(2) << minutes << L':' << std::setw(2) << seconds
           << L',' << std::setw(3) << remainder;
    return output.str();
}

void appendCodePoint(std::string& output, std::uint32_t codePoint) {
    if (codePoint <= 0x7f) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    } else if (codePoint <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    }
}

} // namespace

std::wstring formatPlainText(const std::vector<Segment>& segments) {
    std::wstring output;
    for (const auto& segment : segments) {
        if (segment.text.empty()) {
            continue;
        }
        if (!output.empty()) {
            output += L'\n';
        }
        output += segment.text;
    }
    return output;
}

std::wstring formatSrt(const std::vector<Segment>& segments) {
    std::wostringstream output;
    std::size_t index = 0;
    for (const auto& segment : segments) {
        if (segment.text.empty()) {
            continue;
        }
        const auto end = std::max(segment.startMilliseconds, segment.endMilliseconds);
        output << ++index << L'\n'
               << formatTimestamp(segment.startMilliseconds) << L" --> "
               << formatTimestamp(end) << L'\n'
               << segment.text << L"\n\n";
    }
    return output.str();
}

std::string encodeUtf8(const std::wstring& text) {
    std::string output;
    output.reserve(text.size() * 2);
    for (std::size_t index = 0; index < text.size(); ++index) {
        std::uint32_t codePoint = static_cast<std::uint32_t>(text[index]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (codePoint >= 0xd800 && codePoint <= 0xdbff && index + 1 < text.size()) {
                const std::uint32_t low = static_cast<std::uint32_t>(text[index + 1]);
                if (low >= 0xdc00 && low <= 0xdfff) {
                    codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + (low - 0xdc00);
                    ++index;
                } else {
                    codePoint = 0xfffd;
                }
            } else if (codePoint >= 0xdc00 && codePoint <= 0xdfff) {
                codePoint = 0xfffd;
            }
        }
        if (codePoint > 0x10ffff) {
            codePoint = 0xfffd;
        }
        appendCodePoint(output, codePoint);
    }
    return output;
}

} // namespace fastrecord::transcription
