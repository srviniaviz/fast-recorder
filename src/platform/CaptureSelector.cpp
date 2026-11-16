#include "platform/CaptureSelector.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wincrypt.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace fastrecord::platform {

namespace {

constexpr wchar_t kRegionPickerClass[] = L"FastRecord.CaptureRegionPicker";
using Microsoft::WRL::ComPtr;
constexpr UINT kIconPixels = 32;

struct RegionPickerState {
    HWND owner{};
    HWND overlay{};
    RECT monitorRect{};
    POINT start{};
    POINT current{};
    RECT selected{};
    bool dragging{false};
    bool accepted{false};
};

std::wstring fileDescription(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) {
        return {};
    }
    std::vector<std::uint8_t> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
        return {};
    }

    struct Translation {
        WORD language;
        WORD codePage;
    };
    Translation* translations = nullptr;
    UINT translationBytes = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
            reinterpret_cast<void**>(&translations), &translationBytes) ||
        translationBytes < sizeof(Translation)) {
        return {};
    }

    wchar_t query[64]{};
    swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\FileDescription",
        translations[0].language, translations[0].codePage);
    wchar_t* description = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), query, reinterpret_cast<void**>(&description), &length) ||
        description == nullptr || length <= 1) {
        return {};
    }
    std::wstring text(description);
    while (!text.empty() && std::iswspace(text.back())) {
        text.pop_back();
    }
    return text;
}

std::wstring processImagePath(HWND window) {
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == 0) {
        return {};
    }
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return {};
    }
    std::wstring path(MAX_PATH * 2, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path.data(), &length);
    CloseHandle(process);
    if (!ok) {
        return {};
    }
    path.resize(length);
    return path;
}

std::wstring friendlyAppName(const std::wstring& imagePath, const std::wstring& title) {
    if (imagePath.empty()) {
        return {};
    }
    const std::filesystem::path path(imagePath);
    // UWP apps are hosted by ApplicationFrameHost, whose description says nothing useful.
    if (_wcsicmp(path.filename().c_str(), L"ApplicationFrameHost.exe") == 0) {
        return title;
    }
    std::wstring description = fileDescription(imagePath);
    return description.empty() ? path.stem().wstring() : description;
}

HICON windowIcon(HWND window, const std::wstring& imagePath, bool& owned) {
    owned = false;
    const auto ask = [window](WPARAM kind) -> HICON {
        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(window, WM_GETICON, kind, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 60, &result) == 0) {
            return nullptr;
        }
        return reinterpret_cast<HICON>(result);
    };
    HICON icon = ask(ICON_BIG);
    if (icon == nullptr) {
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICON));
    }
    if (icon == nullptr) {
        icon = ask(ICON_SMALL2);
    }
    if (icon == nullptr) {
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICONSM));
    }
    if (icon == nullptr && !imagePath.empty() &&
        ExtractIconExW(imagePath.c_str(), 0, &icon, nullptr, 1) > 0 && icon != nullptr) {
        owned = true;
    }
    return icon;
}

std::wstring iconAsPngDataUrl(IWICImagingFactory* factory, HICON icon) {
    if (factory == nullptr || icon == nullptr) {
        return {};
    }

    ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromHICON(icon, &bitmap))) {
        return {};
    }
    UINT width = 0;
    UINT height = 0;
    bitmap->GetSize(&width, &height);
    ComPtr<IWICBitmapSource> source = bitmap;
    if (width != kIconPixels || height != kIconPixels) {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(bitmap.Get(), kIconPixels, kIconPixels,
                WICBitmapInterpolationModeHighQualityCubic))) {
            return {};
        }
        source = scaler;
    }

    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(kIconPixels, kIconPixels)) ||
        FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(frame->WriteSource(source.Get(), nullptr)) ||
        FAILED(frame->Commit()) ||
        FAILED(encoder->Commit())) {
        return {};
    }

    HGLOBAL memory = nullptr;
    STATSTG stat{};
    if (FAILED(GetHGlobalFromStream(stream.Get(), &memory)) ||
        FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart == 0) {
        return {};
    }
    const auto byteCount = static_cast<DWORD>(stat.cbSize.QuadPart);
    const auto* bytes = static_cast<const BYTE*>(GlobalLock(memory));
    if (bytes == nullptr) {
        return {};
    }
    const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    DWORD textLength = 0;
    std::wstring base64;
    if (CryptBinaryToStringW(bytes, byteCount, flags, nullptr, &textLength) && textLength > 0) {
        base64.resize(textLength);
        if (CryptBinaryToStringW(bytes, byteCount, flags, base64.data(), &textLength)) {
            base64.resize(textLength);
        } else {
            base64.clear();
        }
    }
    GlobalUnlock(memory);
    return base64.empty() ? std::wstring{} : L"data:image/png;base64," + base64;
}

struct Enumeration {
    HWND excluded{};
    std::vector<HWND> windows;
};

BOOL CALLBACK collectWindow(HWND window, LPARAM parameter) {
    auto& enumeration = *reinterpret_cast<Enumeration*>(parameter);
    if (isCaptureWindowCandidate(window, enumeration.excluded)) {
        enumeration.windows.push_back(window);
    }
    return TRUE;
}

RECT normalizedRect(POINT first, POINT second) {
    return RECT{
        std::min(first.x, second.x),
        std::min(first.y, second.y),
        std::max(first.x, second.x),
        std::max(first.y, second.y),
    };
}

LRESULT CALLBACK regionPickerProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<RegionPickerState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<RegionPickerState*>(create->lpCreateParams);
        state->overlay = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH background = CreateSolidBrush(RGB(8, 10, 14));
        FillRect(dc, &client, background);
        DeleteObject(background);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(245, 246, 248));
        RECT instruction{20, 18, client.right - 20, 44};
        DrawTextW(dc, L"Arraste para selecionar a região  •  ESC cancela", -1,
            &instruction, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

        if (state != nullptr && state->dragging) {
            const RECT selection = normalizedRect(state->start, state->current);
            HBRUSH fill = CreateSolidBrush(RGB(255, 59, 69));
            FillRect(dc, &selection, fill);
            DeleteObject(fill);
            HPEN border = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            const HGDIOBJ oldPen = SelectObject(dc, border);
            const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(dc, selection.left, selection.top, selection.right, selection.bottom);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(border);
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (state != nullptr) {
            state->start = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            state->current = state->start;
            state->dragging = true;
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (state != nullptr && state->dragging) {
            state->current = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (state != nullptr && state->dragging) {
            state->current = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            state->dragging = false;
            ReleaseCapture();
            const RECT clientSelection = normalizedRect(state->start, state->current);
            if (clientSelection.right - clientSelection.left < 16 ||
                clientSelection.bottom - clientSelection.top < 16) {
                MessageBoxW(window, L"A região precisa ter pelo menos 16×16 pixels.",
                    L"Fast Record", MB_OK | MB_ICONINFORMATION);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }

            POINT topLeft{clientSelection.left, clientSelection.top};
            POINT bottomRight{clientSelection.right, clientSelection.bottom};
            ClientToScreen(window, &topLeft);
            ClientToScreen(window, &bottomRight);
            state->selected = RECT{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
            state->accepted = true;
            DestroyWindow(window);
        }
        return 0;
    case WM_RBUTTONDOWN:
    case WM_KEYDOWN:
        if (message == WM_RBUTTONDOWN || wParam == VK_ESCAPE) {
            if (state != nullptr) {
                state->dragging = false;
            }
            ReleaseCapture();
            DestroyWindow(window);
        }
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

bool registerPickerClass(const wchar_t* name, WNDPROC procedure, HBRUSH background) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpfnWndProc = procedure;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = background;
    windowClass.lpszClassName = name;
    if (RegisterClassExW(&windowClass) != 0) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

bool isCaptureWindowCandidate(HWND window, HWND excludedWindow) {
    if (window == nullptr || window == excludedWindow || !IsWindow(window) ||
        !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) {
        return false;
    }

    const LONG_PTR extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((extendedStyle & WS_EX_TOOLWINDOW) != 0) {
        return false;
    }

    // Suspended UWP apps and windows on other virtual desktops are "visible" but cloaked.
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked != 0) {
        return false;
    }

    return GetWindowTextLengthW(window) > 0;
}

std::vector<CaptureWindowInfo> listCaptureWindows(HWND excludedWindow) {
    Enumeration enumeration{excludedWindow, {}};
    EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&enumeration));

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory)))) {
        factory.Reset();
    }

    std::vector<CaptureWindowInfo> windows;
    windows.reserve(enumeration.windows.size());
    for (const HWND window : enumeration.windows) {
        CaptureWindowInfo info;
        info.handle = window;
        const int titleLength = GetWindowTextLengthW(window);
        info.title.resize(static_cast<std::size_t>(titleLength) + 1);
        info.title.resize(static_cast<std::size_t>(
            GetWindowTextW(window, info.title.data(), titleLength + 1)));
        if (info.title.empty()) {
            continue;
        }
        info.minimized = IsIconic(window) != FALSE;

        const std::wstring imagePath = processImagePath(window);
        info.appName = friendlyAppName(imagePath, info.title);
        bool ownedIcon = false;
        const HICON icon = windowIcon(window, imagePath, ownedIcon);
        info.iconDataUrl = iconAsPngDataUrl(factory.Get(), icon);
        if (ownedIcon) {
            DestroyIcon(icon);
        }
        windows.push_back(std::move(info));
    }
    return windows;
}

bool chooseCaptureRegion(HWND owner, HMONITOR monitor, RECT& region) {
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo)) {
        return false;
    }

    if (!registerPickerClass(kRegionPickerClass, regionPickerProcedure,
            reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1))) {
        return false;
    }

    RegionPickerState state;
    state.owner = owner;
    state.monitorRect = monitorInfo.rcMonitor;
    if (owner != nullptr) {
        EnableWindow(owner, FALSE);
    }

    const HWND overlay = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        kRegionPickerClass,
        L"Selecionar região · Fast Record",
        WS_POPUP,
        monitorInfo.rcMonitor.left,
        monitorInfo.rcMonitor.top,
        monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
        monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
        owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (overlay == nullptr) {
        if (owner != nullptr) {
            EnableWindow(owner, TRUE);
        }
        return false;
    }

    SetLayeredWindowAttributes(overlay, 0, 92, LWA_ALPHA);
    ShowWindow(overlay, SW_SHOWNORMAL);
    UpdateWindow(overlay);
    SetForegroundWindow(overlay);
    SetFocus(overlay);

    MSG message{};
    while (IsWindow(overlay)) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (owner != nullptr) {
        EnableWindow(owner, TRUE);
        if (IsWindowVisible(owner)) {
            SetForegroundWindow(owner);
        }
    }
    if (state.accepted) {
        region = state.selected;
    }
    return state.accepted;
}

} // namespace fastrecord::platform
