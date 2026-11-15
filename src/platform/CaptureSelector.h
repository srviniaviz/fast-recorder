#pragma once

#include <Windows.h>

#include <string>
#include <vector>

namespace fastrecord::platform {

struct CaptureWindowInfo {
    HWND handle{};
    std::wstring title;
    // Friendly program name (file description, or the executable name).
    std::wstring appName;
    // 32x32 PNG as a data: URL, or empty when the window has no usable icon.
    std::wstring iconDataUrl;
    bool minimized{false};
};

// True when the window is a visible, uncloaked top-level app window with a title.
// The excluded window is rejected so Fast Record cannot capture itself.
bool isCaptureWindowCandidate(HWND window, HWND excludedWindow);

// Lists the windows that can be recorded, in Z order (most recently used first).
// Requires COM to be initialized on the calling thread for icon encoding.
std::vector<CaptureWindowInfo> listCaptureWindows(HWND excludedWindow);

// Shows a full-monitor drag overlay and returns the selected desktop rectangle.
bool chooseCaptureRegion(HWND owner, HMONITOR monitor, RECT& region);

} // namespace fastrecord::platform
