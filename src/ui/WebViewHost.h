#pragma once

#include <Windows.h>
#include <Unknwn.h>
#include <WebView2.h>
#include <wrl.h>

#include <functional>
#include <string>

namespace fastrecord::ui {

class WebViewHost final {
public:
    using MessageCallback = std::function<void(const std::wstring&)>;
    using ReadyCallback = std::function<void(bool)>;

    WebViewHost() = default;
    WebViewHost(const WebViewHost&) = delete;
    WebViewHost& operator=(const WebViewHost&) = delete;
    ~WebViewHost();

    bool initialize(
        HWND owner,
        std::wstring html,
        MessageCallback messageCallback,
        ReadyCallback readyCallback);
    void resize();
    void executeScript(const std::wstring& script);
    void close();

private:
    HWND m_owner{nullptr};
    std::wstring m_html;
    MessageCallback m_messageCallback;
    ReadyCallback m_readyCallback;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> m_webView;
    EventRegistrationToken m_messageToken{};
    bool m_messageHandlerRegistered{false};
};

} // namespace fastrecord::ui
