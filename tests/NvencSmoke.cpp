#include "recording/NvencBackend.h"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

using namespace fastrecord::recording;

namespace {

struct VideoTimeline {
    std::uint64_t frames{0};
    LONGLONG firstTimestamp{LLONG_MAX};
    LONGLONG lastTimestamp{0};
};

constexpr DWORD kAllReaderStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD kFirstVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

bool inspectVideoTimeline(
    const std::filesystem::path& path,
    VideoTimeline& timeline,
    std::wstring& error) {
    const HRESULT startup = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(startup)) {
        error = L"Media Foundation não iniciou para validar a duração do vídeo.";
        return false;
    }

    HRESULT result = S_OK;
    {
        Microsoft::WRL::ComPtr<IMFSourceReader> reader;
        result = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
        if (SUCCEEDED(result)) {
            result = reader->SetStreamSelection(kAllReaderStreams, FALSE);
        }
        if (SUCCEEDED(result)) {
            result = reader->SetStreamSelection(kFirstVideoStream, TRUE);
        }

        while (SUCCEEDED(result)) {
            DWORD streamIndex = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            Microsoft::WRL::ComPtr<IMFSample> sample;
            result = reader->ReadSample(
                kFirstVideoStream,
                0,
                &streamIndex,
                &flags,
                &timestamp,
                &sample);
            if (FAILED(result) || (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                break;
            }
            if (!sample) {
                continue;
            }

            LONGLONG sampleTimestamp = timestamp;
            if (FAILED(sample->GetSampleTime(&sampleTimestamp))) {
                continue;
            }
            ++timeline.frames;
            timeline.firstTimestamp = (std::min)(timeline.firstTimestamp, sampleTimestamp);
            timeline.lastTimestamp = (std::max)(timeline.lastTimestamp, sampleTimestamp);
        }
    }

    MFShutdown();
    if (FAILED(result)) {
        error = L"Não foi possível decodificar os frames do arquivo NVENC: " +
            std::to_wstring(static_cast<unsigned long>(result));
        return false;
    }
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const std::filesystem::path outputDirectory = argc > 1
        ? argv[1]
        : std::filesystem::current_path() / L"nvenc-smoke-output";
    const std::wstring mode = argc > 2 ? argv[2] : L"";
    const bool av1 = mode == L"av1";
    const bool systemOnly = mode == L"system";

    RecordingSettings settings{};
    settings.target.kind = CaptureTargetKind::CurrentMonitor;
    settings.outputDirectory = outputDirectory;
    settings.width = 1280;
    settings.height = 720;
    settings.framesPerSecond = 60;
    settings.bitrateMbps = 12;
    settings.engine = EncoderEngine::Nvenc;
    settings.codec = av1 ? VideoCodec::Av1 : VideoCodec::H264;
    settings.captureCursor = true;
    settings.captureMicrophone = !systemOnly;
    settings.captureSystemAudio = true;

    NvencBackend backend;
    std::wstring error;
    if (!backend.start(settings, error)) {
        std::wcerr << L"NVENC smoke falhou ao iniciar: " << error << L"\n";
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));
    if (!backend.stop(error)) {
        std::wcerr << L"NVENC smoke falhou ao parar: " << error << L"\n";
        return 2;
    }

    VideoTimeline timeline;
    if (!inspectVideoTimeline(backend.outputPath(), timeline, error)) {
        std::wcerr << L"NVENC smoke não validou a duração dos frames: " << error << L"\n";
        return 3;
    }
    if (timeline.frames < 120 || timeline.firstTimestamp == LLONG_MAX ||
        timeline.lastTimestamp - timeline.firstTimestamp < 25'000'000) {
        std::wcerr << L"NVENC smoke encontrou somente " << timeline.frames
                   << L" frames em "
                   << (timeline.firstTimestamp == LLONG_MAX
                        ? 0.0
                        : static_cast<double>(timeline.lastTimestamp - timeline.firstTimestamp) / 10'000'000.0)
                   << L" s; o desktop estático precisa manter a cadência de vídeo.\n";
        return 4;
    }

    std::wcout << backend.outputPath().wstring() << L"\n"
               << L"Frames: " << timeline.frames << L" · duração do vídeo: "
               << static_cast<double>(timeline.lastTimestamp - timeline.firstTimestamp) / 10'000'000.0
               << L" s\n";
    return 0;
}
