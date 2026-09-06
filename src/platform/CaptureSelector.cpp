#include "platform/CaptureSelector.h"

#include <windowsx.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace fastrecord::platform {

namespace {

constexpr wchar_t kWindowPickerClass[] = L"FastRecord.CaptureWindowPicker";
constexpr wchar_t kRegionPickerClass[] = L"FastRecord.CaptureRegionPicker";
constexpr int kWindowPickerOk = 1001;
constexpr int kWindowPickerCancel = 1002;

struct WindowEntry {
    HWND handle{};
    std::wstring title;
};

struct WindowPickerState {
    HWND owner{};
    HWND list{};
    HWND selected{};
    bool accepted{false};
    std::vector<WindowEntry> entries;
};

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

void setDefaultFont(HWND control) {
    SendMessageW(
        control,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
        TRUE);
}

bool isWindowCandidate(HWND window, HWND excludedWindow) {
    if (window == nullptr || window == excludedWindow || !IsWindow(window) ||
        !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) {
        return false;
    }

    const LONG_PTR extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((extendedStyle & WS_EX_TOOLWINDOW) != 0) {
        return false;
    }

    const int titleLength = GetWindowTextLengthW(window);
    return titleLength > 0;
}

BOOL CALLBACK enumerateWindows(HWND window, LPARAM parameter) {
    auto& state = *reinterpret_cast<std::pair<std::vector<WindowEntry>*, HWND>*>(parameter);
    if (!isWindowCandidate(window, state.second)) {
        return TRUE;
    }

    const int titleLength = GetWindowTextLengthW(window);
    std::wstring title(static_cast<size_t>(titleLength) + 1, L'\0');
    GetWindowTextW(window, title.data(), titleLength + 1);
    title.resize(static_cast<size_t>(titleLength));
    state.first->push_back({window, std::move(title)});
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

void closeWindowPicker(HWND dialog, WindowPickerState& state) {
    const LRESULT selection = SendMessageW(state.list, LB_GETCURSEL, 0, 0);
    if (selection == LB_ERR) {
        MessageBoxW(dialog, L"Selecione uma janela para continuar.", L"Fast Record", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const LRESULT handle = SendMessageW(state.list, LB_GETITEMDATA, selection, 0);
    if (handle == LB_ERR || handle == 0) {
        return;
    }

    state.selected = reinterpret_cast<HWND>(handle);
    state.accepted = IsWindow(state.selected) && IsWindowVisible(state.selected);
    DestroyWindow(dialog);
}

LRESULT CALLBACK windowPickerProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<WindowPickerState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<WindowPickerState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
    case WM_CREATE: {
        RECT client{};
        GetClientRect(window, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;

        HWND title = CreateWindowExW(
            0, L"STATIC", L"Escolha a janela que deseja gravar:",
            WS_CHILD | WS_VISIBLE, 16, 14, width - 32, 24,
            window, nullptr, GetModuleHandleW(nullptr), nullptr);
        state->list = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            16, 46, width - 32, height - 102,
            window, nullptr, GetModuleHandleW(nullptr), nullptr);
        HWND ok = CreateWindowExW(
            0, L"BUTTON", L"Gravar janela",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            width - 218, height - 44, 112, 28,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kWindowPickerOk)), GetModuleHandleW(nullptr), nullptr);
        HWND cancel = CreateWindowExW(
            0, L"BUTTON", L"Cancelar",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            width - 98, height - 44, 82, 28,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kWindowPickerCancel)), GetModuleHandleW(nullptr), nullptr);
        setDefaultFont(title);
        setDefaultFont(state->list);
        setDefaultFont(ok);
        setDefaultFont(cancel);

        for (const auto& entry : state->entries) {
            const LRESULT index = SendMessageW(state->list, LB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(entry.title.c_str()));
            SendMessageW(state->list, LB_SETITEMDATA, index,
                reinterpret_cast<LPARAM>(entry.handle));
        }
        if (!state->entries.empty()) {
            SendMessageW(state->list, LB_SETCURSEL, 0, 0);
        }
        return 0;
    }
    case WM_COMMAND:
        if (state == nullptr) {
            return 0;
        }
        if (LOWORD(wParam) == kWindowPickerOk ||
            (LOWORD(wParam) == 0 && HIWORD(wParam) == LBN_DBLCLK)) {
            closeWindowPicker(window, *state);
        } else if (LOWORD(wParam) == kWindowPickerCancel) {
            DestroyWindow(window);
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            DestroyWindow(window);
        } else if (wParam == VK_RETURN && state != nullptr) {
            closeWindowPicker(window, *state);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
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

RECT pickerBounds(HWND owner, int width, int height) {
    RECT reference{};
    HMONITOR monitor = nullptr;
    if (owner != nullptr && GetWindowRect(owner, &reference)) {
        monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    } else {
        POINT cursor{};
        GetCursorPos(&cursor);
        monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    }

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &info)) {
        info.rcWork = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }

    const int x = info.rcWork.left +
        ((info.rcWork.right - info.rcWork.left) - width) / 2;
    const int y = info.rcWork.top +
        ((info.rcWork.bottom - info.rcWork.top) - height) / 2;
    return RECT{x, y, x + width, y + height};
}

} // namespace

HWND chooseCaptureWindow(HWND owner, HWND excludedWindow) {
    WindowPickerState state;
    state.owner = owner;
    std::pair<std::vector<WindowEntry>*, HWND> enumeration{&state.entries, excludedWindow};
    EnumWindows(enumerateWindows, reinterpret_cast<LPARAM>(&enumeration));
    if (state.entries.empty()) {
        MessageBoxW(owner, L"Nenhuma janela visível disponível para captura.",
            L"Fast Record", MB_OK | MB_ICONINFORMATION);
        return nullptr;
    }

    if (!registerPickerClass(kWindowPickerClass, windowPickerProcedure,
            reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1))) {
        return nullptr;
    }

    const RECT bounds = pickerBounds(owner, 540, 390);
    if (owner != nullptr) {
        EnableWindow(owner, FALSE);
    }
    const HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        kWindowPickerClass,
        L"Selecionar janela · Fast Record",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (dialog == nullptr) {
        if (owner != nullptr) {
            EnableWindow(owner, TRUE);
        }
        return nullptr;
    }

    ShowWindow(dialog, SW_SHOWNORMAL);
    UpdateWindow(dialog);
    SetForegroundWindow(dialog);
    SetFocus(state.list);

    MSG message{};
    while (IsWindow(dialog)) {
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
    return state.accepted ? state.selected : nullptr;
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
