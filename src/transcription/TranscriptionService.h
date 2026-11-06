#pragma once

#include "transcription/TranscriptFormat.h"
#include "transcription/TranscriptionModel.h"

#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace fastrecord::transcription {

using ProgressCallback = std::function<void(int percent, const std::wstring& status)>;

std::filesystem::path modelPath(Model model);
bool modelAvailable(Model model);

bool downloadModel(
    Model model,
    std::stop_token cancellation,
    const ProgressCallback& progress,
    std::wstring& error);

bool transcribeVideo(
    Model model,
    const std::filesystem::path& videoPath,
    std::stop_token cancellation,
    const ProgressCallback& progress,
    std::vector<Segment>& segments,
    std::wstring& error);

} // namespace fastrecord::transcription
