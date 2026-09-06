#include "recording/NvencProbe.h"

#include <Windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <memory>
#include <vector>

#ifndef FASTRECORD_HAS_NVENC
#define FASTRECORD_HAS_NVENC 0
#endif

#if FASTRECORD_HAS_NVENC
#include <nvEncodeAPI.h>
#endif

namespace fastrecord::recording {

namespace {

#if FASTRECORD_HAS_NVENC

class Library final {
public:
    Library() : handle(LoadLibraryW(L"nvEncodeAPI.dll")) {}
    ~Library() {
        if (handle != nullptr) {
            FreeLibrary(handle);
        }
    }

    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    HMODULE handle{nullptr};
};

using NvEncodeAPICreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);

bool createD3D11Device(
    Microsoft::WRL::ComPtr<ID3D11Device>& device,
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>& context) {
    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0,
    };

    D3D_FEATURE_LEVEL createdLevel{};
    const HRESULT result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels,
        ARRAYSIZE(levels),
        D3D11_SDK_VERSION,
        device.GetAddressOf(),
        &createdLevel,
        context.GetAddressOf());

    return SUCCEEDED(result) && device != nullptr && context != nullptr;
}

#endif

} // namespace

NvencProbeResult probeNvenc() {
    NvencProbeResult result{};

#if FASTRECORD_HAS_NVENC
    result.sdkBindingsCompiled = true;

    Library library;
    if (library.handle == nullptr) {
        result.description = L"NVENC não disponível: nvEncodeAPI.dll não foi encontrada.";
        return result;
    }
    result.runtimeLibraryFound = true;

    const auto createInstance = reinterpret_cast<NvEncodeAPICreateInstanceFn>(
        GetProcAddress(library.handle, "NvEncodeAPICreateInstance"));
    if (createInstance == nullptr) {
        result.description = L"NVENC não disponível: API do driver não exportada.";
        return result;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    if (!createD3D11Device(device, context)) {
        result.description = L"NVENC não testado: não foi possível criar um dispositivo Direct3D 11.";
        return result;
    }

    NV_ENCODE_API_FUNCTION_LIST functions{};
    functions.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (createInstance(&functions) != NV_ENC_SUCCESS) {
        result.description = L"NVENC não disponível: falha ao inicializar a tabela da API.";
        return result;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openParams{};
    openParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    openParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    openParams.device = device.Get();

    void* session = nullptr;
    if (functions.nvEncOpenEncodeSessionEx(&openParams, &session) != NV_ENC_SUCCESS ||
        session == nullptr) {
        result.description = L"NVENC não disponível nesta GPU ou driver.";
        return result;
    }
    result.encodeSessionOpened = true;

    uint32_t guidCount = 0;
    if (functions.nvEncGetEncodeGUIDCount(session, &guidCount) == NV_ENC_SUCCESS &&
        guidCount > 0) {
        std::vector<GUID> guids(guidCount);
        uint32_t returnedCount = 0;
        if (functions.nvEncGetEncodeGUIDs(
                session,
                guids.data(),
                guidCount,
                &returnedCount) == NV_ENC_SUCCESS) {
            for (uint32_t index = 0; index < returnedCount; ++index) {
                if (IsEqualGUID(guids[index], NV_ENC_CODEC_H264_GUID)) {
                    result.h264Supported = true;
                    break;
                }
            }
        }
    }

    functions.nvEncCloseEncodeSession(session);

    result.description = result.h264Supported
        ? L"NVENC disponível para H.264."
        : L"NVENC encontrado, mas H.264 não foi anunciado pela API.";
#else
    HMODULE library = LoadLibraryW(L"nvEncodeAPI.dll");
    result.runtimeLibraryFound = library != nullptr;
    if (library != nullptr) {
        FreeLibrary(library);
    }

    result.description = result.runtimeLibraryFound
        ? L"Driver NVENC encontrada; configure o NVIDIA Video Codec SDK para a sonda completa."
        : L"NVENC não detectado; o fallback será usado.";
#endif

    return result;
}

} // namespace fastrecord::recording
