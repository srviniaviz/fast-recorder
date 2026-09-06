#pragma once

#include <d3d11.h>
#include <mfidl.h>

#include <string>

#include <wrl/client.h>

namespace fastrecord::recording {

struct QsvProbeResult {
    bool hardwareMftFound{false};
    bool deviceReady{false};
    bool h264Supported{false};
    bool av1Supported{false};
    std::wstring adapterName;
    std::wstring encoderName;
    std::wstring description;
};

struct QsvDeviceManager {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> manager;
    std::wstring adapterName;
};

QsvProbeResult probeQsv();

// Creates a Media Foundation device manager bound to a physical Intel
// adapter. The sink writer uses it to match the hardware encoder to QSV.
bool createQsvDeviceManager(QsvDeviceManager& result, std::wstring& error);

} // namespace fastrecord::recording
