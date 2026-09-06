#include "platform/TrayIcon.h"

#include <cwchar>
#include <utility>

namespace fastrecord::platform {

namespace {

template <size_t Size>
void copyText(wchar_t (&destination)[Size], const std::wstring& source) {
    wcsncpy_s(destination, Size, source.c_str(), _TRUNCATE);
}

} // namespace

TrayIcon::~TrayIcon() {
    destroy();
}

bool TrayIcon::create(
    HWND owner,
    HICON icon,
    const std::wstring& tooltip,
    Callback callback) {
    if (m_created || owner == nullptr || icon == nullptr) {
        return false;
    }

    m_data = {};
    m_data.cbSize = sizeof(m_data);
    m_data.hWnd = owner;
    m_data.uID = 1;
    m_data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    m_data.uCallbackMessage = kCallbackMessage;
    m_data.hIcon = icon;
    copyText(m_data.szTip, tooltip);

    if (!Shell_NotifyIconW(NIM_ADD, &m_data)) {
        m_data = {};
        return false;
    }

    m_data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &m_data);

    m_callback = std::move(callback);
    m_created = true;
    return true;
}

void TrayIcon::destroy() {
    if (!m_created) {
        return;
    }

    Shell_NotifyIconW(NIM_DELETE, &m_data);
    m_callback = {};
    m_data = {};
    m_created = false;
}

bool TrayIcon::setTooltip(const std::wstring& tooltip) {
    if (!m_created) {
        return false;
    }

    copyText(m_data.szTip, tooltip);
    m_data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    return Shell_NotifyIconW(NIM_MODIFY, &m_data) == TRUE;
}

bool TrayIcon::showBalloon(
    const std::wstring& title,
    const std::wstring& message,
    DWORD infoFlags) {
    if (!m_created) {
        return false;
    }

    m_data.uFlags = NIF_INFO;
    copyText(m_data.szInfoTitle, title);
    copyText(m_data.szInfo, message);
    m_data.dwInfoFlags = infoFlags;
    return Shell_NotifyIconW(NIM_MODIFY, &m_data) == TRUE;
}

} // namespace fastrecord::platform
