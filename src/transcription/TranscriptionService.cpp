#include "transcription/TranscriptionService.h"

#include <Windows.h>
#include <bcrypt.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <ks.h>
#include <ksmedia.h>
#include <shlobj.h>
#include <winhttp.h>
#include <wrl/client.h>
#include <whisper.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

namespace fastrecord::transcription {

namespace {

using Microsoft::WRL::ComPtr;

struct ModelArtifact {
    Model model;
    std::uint64_t size;
    const wchar_t* sha256;
};

constexpr wchar_t kModelRepositoryRevision[] = L"5359861c739e955e79d9a303bcbc70fb988958b1";
constexpr std::array<ModelArtifact, 3> kModelArtifacts{{
    {Model::Tiny, 77'691'713, L"be07e048e1e599ad46341c8d2a135645097a538221678b7acdd1b1919c6e1b21"},
    {Model::Base, 147'951'465, L"60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe"},
    {Model::Small, 487'601'967, L"1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b"},
}};
constexpr std::uint64_t kSampleRate = 16'000;
constexpr std::uint64_t kMaximumAudioSeconds = 2 * 60 * 60;
constexpr DWORD kAllReaderStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD kFirstAudioStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
constexpr DWORD kMediaSource = static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);

struct WinHttpCloser {
    void operator()(void* handle) const noexcept {
        if (handle != nullptr) {
            WinHttpCloseHandle(handle);
        }
    }
};

using WinHttpHandle = std::unique_ptr<void, WinHttpCloser>;

const ModelArtifact& modelArtifact(Model model) noexcept {
    for (const auto& artifact : kModelArtifacts) {
        if (artifact.model == model) {
            return artifact;
        }
    }
    return kModelArtifacts[1];
}

std::wstring modelDownloadUrl(Model model) {
    return L"https://huggingface.co/ggerganov/whisper.cpp/resolve/" +
        std::wstring(kModelRepositoryRevision) + L"/ggml-" +
        std::wstring(modelPresentation(model).id) + L".bin";
}

class MediaFoundationLifetime final {
public:
    bool start(std::wstring& error) {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
            error = L"Não foi possível inicializar o decodificador de mídia do Windows.";
            return false;
        }
        m_uninitializeCom = SUCCEEDED(comResult);

        const HRESULT mfResult = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(mfResult)) {
            if (m_uninitializeCom) {
                CoUninitialize();
                m_uninitializeCom = false;
            }
            error = L"O Media Foundation do Windows não pôde ser iniciado.";
            return false;
        }
        m_started = true;
        return true;
    }

    ~MediaFoundationLifetime() {
        if (m_started) {
            MFShutdown();
        }
        if (m_uninitializeCom) {
            CoUninitialize();
        }
    }

private:
    bool m_started{false};
    bool m_uninitializeCom{false};
};

class BCryptHash final {
public:
    bool start() {
        if (BCryptOpenAlgorithmProvider(&m_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
            return false;
        }
        DWORD objectLength = 0;
        DWORD bytesReturned = 0;
        if (BCryptGetProperty(
                m_algorithm,
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objectLength),
                sizeof(objectLength),
                &bytesReturned,
                0) < 0) {
            return false;
        }
        m_object.resize(objectLength);
        return BCryptCreateHash(
            m_algorithm,
            &m_hash,
            m_object.data(),
            static_cast<ULONG>(m_object.size()),
            nullptr,
            0,
            0) >= 0;
    }

    bool append(const std::uint8_t* data, std::size_t size) {
        return size <= std::numeric_limits<ULONG>::max() &&
            BCryptHashData(m_hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) >= 0;
    }

    bool finish(std::wstring& hex) {
        std::array<std::uint8_t, 32> digest{};
        if (BCryptFinishHash(m_hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
            return false;
        }
        constexpr wchar_t digits[] = L"0123456789abcdef";
        hex.clear();
        hex.reserve(digest.size() * 2);
        for (const std::uint8_t byte : digest) {
            hex.push_back(digits[byte >> 4]);
            hex.push_back(digits[byte & 0x0f]);
        }
        return true;
    }

    ~BCryptHash() {
        if (m_hash != nullptr) {
            BCryptDestroyHash(m_hash);
        }
        if (m_algorithm != nullptr) {
            BCryptCloseAlgorithmProvider(m_algorithm, 0);
        }
    }

private:
    BCRYPT_ALG_HANDLE m_algorithm{nullptr};
    BCRYPT_HASH_HANDLE m_hash{nullptr};
    std::vector<std::uint8_t> m_object;
};

bool downloadModelFile(
    Model model,
    std::stop_token cancellation,
    const ProgressCallback& progress,
    std::wstring& error) {
    const ModelArtifact& artifact = modelArtifact(model);
    const std::filesystem::path target = modelPath(model);
    if (target.empty()) {
        error = L"Não foi possível localizar a pasta de dados do usuário.";
        return false;
    }

    std::error_code filesystemError;
    std::filesystem::create_directories(target.parent_path(), filesystemError);
    if (filesystemError) {
        error = L"Não foi possível criar a pasta local do modelo.";
        return false;
    }
    if (std::filesystem::exists(target, filesystemError) && !filesystemError &&
        std::filesystem::file_size(target, filesystemError) == artifact.size && !filesystemError) {
        return true;
    }

    const std::filesystem::path partial = target.wstring() + L".download";
    std::filesystem::remove(partial, filesystemError);

    const std::wstring url = modelDownloadUrl(model);
    URL_COMPONENTS urlParts{};
    urlParts.dwStructSize = sizeof(urlParts);
    urlParts.dwSchemeLength = static_cast<DWORD>(-1);
    urlParts.dwHostNameLength = static_cast<DWORD>(-1);
    urlParts.dwUrlPathLength = static_cast<DWORD>(-1);
    urlParts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &urlParts)) {
        error = L"O endereço de download do modelo está inválido.";
        return false;
    }

    const std::wstring host(urlParts.lpszHostName, urlParts.dwHostNameLength);
    std::wstring requestPath(urlParts.lpszUrlPath, urlParts.dwUrlPathLength);
    if (urlParts.dwExtraInfoLength > 0) {
        requestPath.append(urlParts.lpszExtraInfo, urlParts.dwExtraInfoLength);
    }
    WinHttpHandle session(WinHttpOpen(
        L"FastRecord transcription model/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session) {
        error = L"Não foi possível iniciar o download do modelo.";
        return false;
    }
    WinHttpSetTimeouts(session.get(), 15'000, 15'000, 30'000, 30'000);
    WinHttpHandle connection(WinHttpConnect(session.get(), host.c_str(), urlParts.nPort, 0));
    if (!connection) {
        error = L"Não foi possível conectar ao servidor do modelo.";
        return false;
    }
    const DWORD flags = urlParts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(WinHttpOpenRequest(
        connection.get(),
        L"GET",
        requestPath.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags));
    if (!request || !WinHttpSendRequest(
            request.get(),
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0) || !WinHttpReceiveResponse(request.get(), nullptr)) {
        error = L"Falha na conexão. Verifique a internet e tente novamente.";
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(
            request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX) || statusCode != 200) {
        error = L"O servidor não disponibilizou o modelo Whisper.";
        return false;
    }

    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = L"Não foi possível gravar o arquivo do modelo no disco.";
        return false;
    }
    BCryptHash hash;
    if (!hash.start()) {
        error = L"Não foi possível verificar a integridade do modelo.";
        output.close();
        std::filesystem::remove(partial, filesystemError);
        return false;
    }

    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::uint64_t received = 0;
    bool failed = false;
    while (!cancellation.stop_requested()) {
        DWORD bytesRead = 0;
        if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead)) {
            error = L"O download foi interrompido. Verifique sua conexão e tente novamente.";
            failed = true;
            break;
        }
        if (bytesRead == 0) {
            break;
        }
        if (received + bytesRead > artifact.size || !hash.append(buffer.data(), bytesRead)) {
            error = L"O arquivo recebido não corresponde ao modelo esperado.";
            failed = true;
            break;
        }
        output.write(reinterpret_cast<const char*>(buffer.data()), bytesRead);
        if (!output) {
            error = L"Não foi possível gravar o modelo no disco.";
            failed = true;
            break;
        }
        received += bytesRead;
        if (progress) {
            const int percent = static_cast<int>((received * 100) / artifact.size);
            const auto& presentation = modelPresentation(model);
            progress(
                std::min(percent, 99),
                L"Baixando Whisper " + std::wstring(presentation.name) + L"…");
        }
    }

    if (cancellation.stop_requested()) {
        error = L"Download cancelado.";
        failed = true;
    }
    output.flush();
    if (!output) {
        error = L"Não foi possível finalizar a gravação do modelo.";
        failed = true;
    }
    output.close();

    std::wstring actualHash;
    if (!failed && (received != artifact.size || !hash.finish(actualHash) || actualHash != artifact.sha256)) {
        error = L"A verificação de integridade do modelo falhou. Tente baixar novamente.";
        failed = true;
    }
    if (failed) {
        std::filesystem::remove(partial, filesystemError);
        return false;
    }

    std::filesystem::remove(target, filesystemError);
    filesystemError.clear();
    std::filesystem::rename(partial, target, filesystemError);
    if (filesystemError) {
        error = L"O modelo foi baixado, mas não pôde ser instalado na pasta local.";
        std::filesystem::remove(partial, filesystemError);
        return false;
    }
    if (progress) {
        progress(100, L"Modelo Whisper pronto.");
    }
    return true;
}

std::wstring decodeUtf8(const char* text) {
    if (text == nullptr || *text == '\0') {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (required <= 1) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result.data(), required);
    if (written <= 1) {
        return {};
    }
    result.resize(static_cast<std::size_t>(written - 1));
    return result;
}

struct WhisperProgressContext {
    std::stop_token cancellation;
    const ProgressCallback* progress{nullptr};
};

bool decodeAudio(
    const std::filesystem::path& videoPath,
    std::stop_token cancellation,
    const ProgressCallback& progress,
    std::vector<float>& audio,
    std::wstring& error) {
    MediaFoundationLifetime mediaFoundation;
    if (!mediaFoundation.start(error)) {
        return false;
    }

    ComPtr<IMFSourceReader> reader;
    HRESULT result = MFCreateSourceReaderFromURL(videoPath.c_str(), nullptr, &reader);
    if (FAILED(result) || !reader) {
        error = L"O Windows não conseguiu abrir esse vídeo. Tente um MP4 ou MOV compatível.";
        return false;
    }

    result = reader->SetStreamSelection(kAllReaderStreams, FALSE);
    if (FAILED(result) || FAILED(reader->SetStreamSelection(kFirstAudioStream, TRUE))) {
        error = L"Este vídeo não contém uma faixa de áudio reconhecida pelo Windows.";
        return false;
    }

    ComPtr<IMFMediaType> outputType;
    result = MFCreateMediaType(&outputType);
    if (FAILED(result) ||
        FAILED(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
        FAILED(outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float)) ||
        FAILED(outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1)) ||
        FAILED(outputType->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK, SPEAKER_FRONT_CENTER)) ||
        FAILED(outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(kSampleRate))) ||
        FAILED(outputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32)) ||
        FAILED(outputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, static_cast<UINT32>(sizeof(float)))) ||
        FAILED(outputType->SetUINT32(
            MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
            static_cast<UINT32>(kSampleRate * sizeof(float)))) ||
        FAILED(outputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE)) ||
        FAILED(reader->SetCurrentMediaType(kFirstAudioStream, nullptr, outputType.Get()))) {
        error = L"O formato de áudio deste vídeo não pôde ser convertido pelo Windows.";
        return false;
    }

    std::uint64_t duration100ns = 0;
    PROPVARIANT duration{};
    PropVariantInit(&duration);
    if (SUCCEEDED(reader->GetPresentationAttribute(
            kMediaSource, MF_PD_DURATION, &duration))) {
        if (duration.vt == VT_UI8) {
            duration100ns = duration.uhVal.QuadPart;
        } else if (duration.vt == VT_I8 && duration.hVal.QuadPart > 0) {
            duration100ns = static_cast<std::uint64_t>(duration.hVal.QuadPart);
        }
    }
    PropVariantClear(&duration);

    const std::uint64_t maxSamples = kSampleRate * kMaximumAudioSeconds;
    audio.clear();
    if (duration100ns > 0 && duration100ns / 10'000'000 <= kMaximumAudioSeconds) {
        audio.reserve(static_cast<std::size_t>((duration100ns * kSampleRate) / 10'000'000));
    }
    std::uint64_t lastReported = 0;
    if (progress) {
        progress(1, L"Lendo o áudio do vídeo…");
    }

    for (;;) {
        if (cancellation.stop_requested()) {
            error = L"Transcrição cancelada.";
            return false;
        }
        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        result = reader->ReadSample(
            kFirstAudioStream,
            0,
            &streamIndex,
            &flags,
            &timestamp,
            &sample);
        if (FAILED(result)) {
            error = L"O Windows encontrou um erro ao decodificar o áudio.";
            return false;
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            break;
        }
        if (sample) {
            ComPtr<IMFMediaBuffer> mediaBuffer;
            if (FAILED(sample->ConvertToContiguousBuffer(&mediaBuffer))) {
                error = L"Não foi possível ler um trecho do áudio do vídeo.";
                return false;
            }
            BYTE* data = nullptr;
            DWORD maximumLength = 0;
            DWORD currentLength = 0;
            result = mediaBuffer->Lock(&data, &maximumLength, &currentLength);
            if (FAILED(result)) {
                error = L"Não foi possível acessar o áudio decodificado.";
                return false;
            }
            if (currentLength % sizeof(float) != 0) {
                mediaBuffer->Unlock();
                error = L"O Windows retornou áudio em um formato inesperado.";
                return false;
            }
            const std::size_t sampleCount = currentLength / sizeof(float);
            if (audio.size() + sampleCount > maxSamples) {
                mediaBuffer->Unlock();
                error = L"A versão atual aceita vídeos com até 2 horas de áudio.";
                return false;
            }
            const std::size_t oldSize = audio.size();
            audio.resize(oldSize + sampleCount);
            if (sampleCount > 0) {
                std::memcpy(audio.data() + oldSize, data, currentLength);
            }
            mediaBuffer->Unlock();
        }

        if (progress && duration100ns > 0 && timestamp > 0) {
            const std::uint64_t currentTime = static_cast<std::uint64_t>(timestamp);
            if (currentTime >= lastReported + 10'000'000) {
                lastReported = currentTime;
                const int percent = static_cast<int>(std::min<std::uint64_t>(
                    15,
                    (currentTime * 15) / duration100ns));
                progress(percent, L"Lendo o áudio do vídeo…");
            }
        }
    }

    if (audio.size() < kSampleRate / 4) {
        error = L"Não foi encontrada uma faixa de áudio com duração suficiente.";
        return false;
    }
    return true;
}

} // namespace

std::filesystem::path modelPath(Model model) {
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData)) ||
        localAppData == nullptr) {
        return {};
    }
    std::filesystem::path result(localAppData);
    CoTaskMemFree(localAppData);
    return result / L"Fast Record" / L"Models" /
        (L"ggml-" + std::wstring(modelPresentation(model).id) + L".bin");
}

bool modelAvailable(Model model) {
    std::error_code error;
    const auto path = modelPath(model);
    return !path.empty() && std::filesystem::is_regular_file(path, error) && !error &&
        std::filesystem::file_size(path, error) == modelArtifact(model).size && !error;
}

bool downloadModel(
    Model model,
    std::stop_token cancellation,
    const ProgressCallback& progress,
    std::wstring& error) {
    error.clear();
    return downloadModelFile(model, cancellation, progress, error);
}

bool transcribeVideo(
    Model model,
    const std::filesystem::path& videoPath,
    std::stop_token cancellation,
    const ProgressCallback& progress,
    std::vector<Segment>& segments,
    std::wstring& error) {
    error.clear();
    segments.clear();
    if (!modelAvailable(model)) {
        error = L"Baixe o modelo Whisper antes de iniciar a transcrição.";
        return false;
    }

    std::vector<float> audio;
    if (!decodeAudio(videoPath, cancellation, progress, audio, error)) {
        return false;
    }
    if (cancellation.stop_requested()) {
        error = L"Transcrição cancelada.";
        return false;
    }

    const std::string modelFile = encodeUtf8(modelPath(model).wstring());
    whisper_context_params contextParams = whisper_context_default_params();
    contextParams.use_gpu = false;
    whisper_context* rawContext = whisper_init_from_file_with_params(modelFile.c_str(), contextParams);
    if (rawContext == nullptr) {
        error = L"O modelo Whisper não pôde ser carregado. Tente baixá-lo novamente.";
        return false;
    }
    const std::unique_ptr<whisper_context, decltype(&whisper_free)> context(rawContext, &whisper_free);

    WhisperProgressContext callbackContext{cancellation, &progress};
    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads = static_cast<int>(std::clamp(
        std::thread::hardware_concurrency(), 2u, 8u));
    params.language = "auto";
    // `language = "auto"` detects the language and continues with decoding;
    // detect_language=true returns after classification without transcription.
    params.detect_language = false;
    params.no_context = true;
    params.print_realtime = false;
    params.print_progress = false;
    params.print_timestamps = false;
    params.progress_callback = [](whisper_context*, whisper_state*, int percent, void* userData) {
        const auto* state = static_cast<const WhisperProgressContext*>(userData);
        if (state != nullptr && state->progress != nullptr && *state->progress) {
            const int mapped = 16 + std::clamp(percent, 0, 100) * 83 / 100;
            (*state->progress)(mapped, L"Transcrevendo com Whisper…");
        }
    };
    params.progress_callback_user_data = &callbackContext;
    params.abort_callback = [](void* userData) {
        const auto* state = static_cast<const WhisperProgressContext*>(userData);
        return state != nullptr && state->cancellation.stop_requested();
    };
    params.abort_callback_user_data = &callbackContext;

    if (audio.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = L"O vídeo é longo demais para a transcrição atual.";
        return false;
    }
    const int result = whisper_full(
        context.get(), params, audio.data(), static_cast<int>(audio.size()));
    if (result != 0) {
        error = cancellation.stop_requested()
            ? L"Transcrição cancelada."
            : L"O Whisper não conseguiu transcrever este áudio.";
        return false;
    }

    const int count = whisper_full_n_segments(context.get());
    segments.reserve(static_cast<std::size_t>(std::max(count, 0)));
    for (int index = 0; index < count; ++index) {
        const char* text = whisper_full_get_segment_text(context.get(), index);
        if (text == nullptr || *text == '\0') {
            continue;
        }
        const std::int64_t start = static_cast<std::int64_t>(
            whisper_full_get_segment_t0(context.get(), index)) * 10;
        const std::int64_t end = static_cast<std::int64_t>(
            whisper_full_get_segment_t1(context.get(), index)) * 10;
        segments.push_back({start, std::max(start, end), decodeUtf8(text)});
    }
    if (segments.empty()) {
        error = L"O Whisper terminou, mas não encontrou fala neste vídeo.";
        return false;
    }
    if (progress) {
        progress(100, L"Transcrição concluída.");
    }
    return true;
}

} // namespace fastrecord::transcription
