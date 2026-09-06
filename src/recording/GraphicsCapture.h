#pragma once

#include "recording/RecordingBackend.h"
#include <d3d11.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <vector>

namespace fastrecord::recording {

// Owned exclusively by the recording worker, including the immediate D3D context.
class GraphicsCapture final {
public:
    explicit GraphicsCapture(const RecordingSettings& settings);
    ~GraphicsCapture();
    GraphicsCapture(const GraphicsCapture&) = delete;
    GraphicsCapture& operator=(const GraphicsCapture&) = delete;
    bool read(std::vector<BYTE>& pixels);

private:
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_pool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{nullptr};
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_runtimeDevice{nullptr};
    winrt::com_ptr<ID3D11Device> m_device;
    winrt::com_ptr<ID3D11DeviceContext> m_context;
    winrt::com_ptr<ID3D11VideoDevice> m_videoDevice;
    winrt::com_ptr<ID3D11VideoContext> m_videoContext;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> m_enumerator;
    winrt::com_ptr<ID3D11VideoProcessor> m_processor;
    winrt::com_ptr<ID3D11Texture2D> m_output;
    winrt::com_ptr<ID3D11Texture2D> m_staging;
    winrt::com_ptr<ID3D11VideoProcessorOutputView> m_outputView;
    winrt::Windows::Graphics::SizeInt32 m_size{};
    UINT m_width{}, m_height{};
    HMONITOR m_monitor{};
};

} // namespace fastrecord::recording
