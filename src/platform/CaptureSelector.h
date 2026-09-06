#pragma once

#include <Windows.h>

namespace fastrecord::platform {

// Shows a small native picker and returns the selected top-level window.
// The excluded window is omitted from the list so Fast Record cannot capture itself.
HWND chooseCaptureWindow(HWND owner, HWND excludedWindow);

// Shows a full-monitor drag overlay and returns the selected desktop rectangle.
bool chooseCaptureRegion(HWND owner, HMONITOR monitor, RECT& region);

} // namespace fastrecord::platform
