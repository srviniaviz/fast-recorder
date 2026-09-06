#include "recording/QsvProbe.h"

#include <Windows.h>

#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>

#include <algorithm>
#include <cwctype>

namespace fastrecord::recording {

namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kIntelVendorId = 0x8086;

class MediaFoundationRuntime final {
public:
    MediaFoundationRuntime() {
        const HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        m_started = SUCCEEDED(result);
    }

    ~MediaFoundationRuntime() {
        if (m_started) {
            MFShutdown();
        }
    }

    MediaFoundationRuntime(const MediaFoundationRuntime&) = delete;
    MediaFoundationRuntime& operator=(const MediaFoundationRuntime&) = delete;

    bool started() const noexcept { return m_started; }

private:
    bool m_started{false};
};

class ComRuntime final {
public:
    ComRuntime() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_initialized = SUCCEEDED(result);
        m_usable = m_initialized || result == RPC_E_CHANGED_MODE;
    }

    ~ComRuntime() {
        if (m_initialized) {
            CoUninitialize();
        }
    }

    ComRuntime(const ComRuntime&) = delete;
    ComRuntime& operator=(const ComRuntime&) = delete;

    bool usable() const noexcept { return m_usable; }

private:
    bool m_initialized{false};
    bool m_usable{false};
};

std::wstring toLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

bool looksLikeIntelEncoder(const std::wstring& name, const std::wstring& hardwareUrl) {
    const std::wstring lowerName = toLower(name);
    const std::wstring lowerUrl = toLower(hardwareUrl);
    return lowerName.find(L"intel") != std::wstring::npos ||
        lowerName.find(L"quick sync") != std::wstring::npos ||
        lowerName.find(L"quicksync") != std::wstring::npos ||
        lowerUrl.find(L"ven_8086") != std::wstring::npos ||
        lowerUrl.find(L"8086") != std::wstring::npos;
}

std::wstring activationString(IMFActivate* activation, REFGUID attribute) {
    PWSTR value = nullptr;
    UINT32 length = 0;
    if (activation == nullptr ||
        FAILED(activation->GetAllocatedString(attribute, &value, &length)) ||
        value == nullptr) {
        return {};
    }

    std::wstring result(value, length);
    CoTaskMemFree(value);
    return result;
}

struct EncoderEnumeration {
    bool found{false};
    std::wstring name;
};

EncoderEnumeration enumerateIntelEncoder(REFGUID codec) {
    EncoderEnumeration result{};
    MFT_REGISTER_TYPE_INFO outputType{MFMediaType_Video, codec};
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    const HRESULT enumerationResult = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr,
        &outputType,
        &activations,
        &count);
    if (FAILED(enumerationResult) || count == 0 || activations == nullptr) {
        return result;
    }

    for (UINT32 index = 0; index < count; ++index) {
        IMFActivate* activation = activations[index];
        const std::wstring name = activationString(activation, MFT_FRIENDLY_NAME_Attribute);
        const std::wstring hardwareUrl = activationString(
            activation,
            MFT_ENUM_HARDWARE_URL_Attribute);
        if (!looksLikeIntelEncoder(name, hardwareUrl)) {
            continue;
        }

        ComPtr<IMFTransform> transform;
        if (FAILED(activation->ActivateObject(IID_PPV_ARGS(&transform)))) {
            continue;
        }

        result.found = true;
        if (result.name.empty()) {
            result.name = name.empty() ? L"Intel Quick Sync Video" : name;
        }
        break;
    }

    for (UINT32 index = 0; index < count; ++index) {
        if (activations[index] != nullptr) {
            activations[index]->Release();
        }
    }
    CoTaskMemFree(activations);
    return result;
}

bool createIntelDevice(
    ComPtr<ID3D11Device>& device,
    std::wstring& adapterName) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT enumResult = factory->EnumAdapters1(index, &adapter);
        if (enumResult == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(enumResult) || adapter == nullptr) {
            continue;
        }

        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description)) ||
            description.VendorId != kIntelVendorId ||
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }

        constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL createdLevel{};
        const HRESULT deviceResult = D3D11CreateDevice(
            adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            levels,
            ARRAYSIZE(levels),
            D3D11_SDK_VERSION,
            device.GetAddressOf(),
            &createdLevel,
            nullptr);
        if (FAILED(deviceResult) || device == nullptr) {
            device.Reset();
            continue;
        }

        adapterName = description.Description;
        return true;
    }

    return false;
}

} // namespace

bool createQsvDeviceManager(QsvDeviceManager& result, std::wstring& error) {
    result = {};
    error.clear();

    ComPtr<ID3D11Device> device;
    if (!createIntelDevice(device, result.adapterName)) {
        error = L"Intel QSV não disponível: nenhuma GPU Intel compatível foi encontrada.";
        return false;
    }

    UINT resetToken = 0;
    ComPtr<IMFDXGIDeviceManager> manager;
    HRESULT hr = MFCreateDXGIDeviceManager(&resetToken, &manager);
    if (SUCCEEDED(hr)) {
        hr = manager->ResetDevice(device.Get(), resetToken);
    }
    if (FAILED(hr)) {
        error = L"Intel QSV não disponível: não foi possível preparar o dispositivo de vídeo.";
        return false;
    }

    result.device = std::move(device);
    result.manager = std::move(manager);
    return true;
}

QsvProbeResult probeQsv() {
    QsvProbeResult result{};
    ComRuntime com;
    if (!com.usable()) {
        result.description = L"Intel QSV não testado: não foi possível inicializar COM.";
        return result;
    }

    MediaFoundationRuntime mediaFoundation;
    if (!mediaFoundation.started()) {
        result.description = L"Intel QSV não testado: não foi possível inicializar o Media Foundation.";
        return result;
    }

    const EncoderEnumeration h264 = enumerateIntelEncoder(MFVideoFormat_H264);
    const EncoderEnumeration av1 = enumerateIntelEncoder(MFVideoFormat_AV1);
    result.hardwareMftFound = h264.found || av1.found;
    result.h264Supported = h264.found;
    result.av1Supported = av1.found;
    result.encoderName = !h264.name.empty() ? h264.name : av1.name;

    QsvDeviceManager device;
    std::wstring deviceError;
    result.deviceReady = createQsvDeviceManager(device, deviceError);
    result.adapterName = device.adapterName;
    result.h264Supported = result.h264Supported && result.deviceReady;
    result.av1Supported = result.av1Supported && result.deviceReady;

    if (result.h264Supported) {
        result.description = L"Intel QSV disponível";
        if (!result.adapterName.empty()) {
            result.description += L" em " + result.adapterName;
        }
        result.description += L" para H.264";
        if (result.av1Supported) {
            result.description += L"; AV1 também foi anunciado, mas permanece reservado ao NVENC nesta versão";
        }
        result.description += L".";
    } else if (result.av1Supported) {
        result.description = L"Intel QSV foi detectado, mas o encoder H.264 não foi anunciado";
        if (!result.adapterName.empty()) {
            result.description += L" em " + result.adapterName;
        }
        result.description += L".";
    } else if (result.hardwareMftFound && !result.deviceReady) {
        result.description = deviceError.empty()
            ? L"Intel QSV encontrado, mas o dispositivo Intel não pôde ser preparado."
            : deviceError;
    } else {
        result.description = L"Intel QSV não detectado; o fallback será usado.";
    }

    return result;
}

} // namespace fastrecord::recording
