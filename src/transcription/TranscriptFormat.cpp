#include "transcription/TranscriptFormat.h"

#include <algorithm>
#include <fstream>
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

std::filesystem::path srtPathForVideo(const std::filesystem::path& videoPath) {
    if (videoPath.empty()) {
        return {};
    }

    const std::filesystem::path basePath = videoPath.parent_path() /
        (videoPath.stem().wstring() + L".srt");
    std::error_code filesystemError;
    if (!std::filesystem::exists(basePath, filesystemError) && !filesystemError) {
        return basePath;
    }
    if (filesystemError) {
        return {};
    }

    const std::wstring stem = videoPath.stem().wstring();
    for (std::uint32_t suffix = 2; suffix <= 10'000; ++suffix) {
        const std::filesystem::path candidate = videoPath.parent_path() /
            (stem + L" (" + std::to_wstring(suffix) + L").srt");
        filesystemError.clear();
        if (!std::filesystem::exists(candidate, filesystemError) && !filesystemError) {
            return candidate;
        }
        if (filesystemError) {
            return {};
        }
    }
    return {};
}

bool saveTranscriptSrt(
    const std::filesystem::path& destination,
    const std::vector<Segment>& segments,
    std::wstring& error) {
    if (destination.empty() || segments.empty()) {
        error = L"Não há transcrição para salvar.";
        return false;
    }

    const std::string content = encodeUtf8(formatSrt(segments));
    std::ofstream file(destination, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = L"Não foi possível abrir o arquivo SRT para gravação.";
        return false;
    }
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!file) {
        error = L"Não foi possível concluir a gravação do arquivo SRT.";
        return false;
    }
    error.clear();
    return true;
}

} // namespace fastrecord::transcription
