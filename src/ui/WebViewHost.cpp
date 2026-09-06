#include "ui/WebViewHost.h"

#include <WebView2EnvironmentOptions.h>
#include <shlobj.h>

#include <filesystem>
#include <utility>

namespace fastrecord::ui {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

std::wstring webViewDataDirectory() {
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData)) ||
        localAppData == nullptr) {
        return {};
    }

    std::filesystem::path directory(localAppData);
    CoTaskMemFree(localAppData);
    directory /= L"Fast Record";
    directory /= L"WebView2";

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return error ? std::wstring{} : directory.wstring();
}

} // namespace

WebViewHost::~WebViewHost() {
    close();
}

bool WebViewHost::initialize(
    HWND owner,
    std::wstring html,
    MessageCallback messageCallback,
    ReadyCallback readyCallback) {
    if (owner == nullptr) {
        return false;
    }

    m_owner = owner;
    m_html = std::move(html);
    m_messageCallback = std::move(messageCallback);
    m_readyCallback = std::move(readyCallback);

    const std::wstring dataDirectory = webViewDataDirectory();
    const HRESULT result = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        dataDirectory.empty() ? nullptr : dataDirectory.c_str(),
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT environmentResult, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(environmentResult) || environment == nullptr || m_owner == nullptr) {
                    if (m_readyCallback) {
                        m_readyCallback(false);
                    }
                    return S_OK;
                }

                return environment->CreateCoreWebView2Controller(
                    m_owner,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(controllerResult) || controller == nullptr || m_owner == nullptr) {
                                if (m_readyCallback) {
                                    m_readyCallback(false);
                                }
                                return S_OK;
                            }

                            m_controller = controller;
                            if (FAILED(m_controller->get_CoreWebView2(&m_webView)) || !m_webView) {
                                if (m_readyCallback) {
                                    m_readyCallback(false);
                                }
                                return S_OK;
                            }

                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(m_webView->get_Settings(&settings)) && settings) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                            }

                            ComPtr<ICoreWebView2Controller2> controller2;
                            if (SUCCEEDED(m_controller.As(&controller2)) && controller2) {
                                COREWEBVIEW2_COLOR background{255, 8, 9, 12};
                                controller2->put_DefaultBackgroundColor(background);
                            }

                            m_webView->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR rawMessage = nullptr;
                                        if (args != nullptr && SUCCEEDED(args->TryGetWebMessageAsString(&rawMessage)) &&
                                            rawMessage != nullptr) {
                                            const std::wstring message(rawMessage);
                                            CoTaskMemFree(rawMessage);
                                            if (m_messageCallback) {
                                                m_messageCallback(message);
                                            }
                                        }
                                        return S_OK;
                                    }).Get(),
                                &m_messageToken);
                            m_messageHandlerRegistered = true;

                            resize();
                            m_controller->put_IsVisible(TRUE);
                            m_webView->NavigateToString(m_html.c_str());
                            if (m_readyCallback) {
                                m_readyCallback(true);
                            }
                            return S_OK;
                        }).Get());
            }).Get());

    return SUCCEEDED(result);
}

void WebViewHost::resize() {
    if (!m_controller || m_owner == nullptr) {
        return;
    }

    RECT bounds{};
    GetClientRect(m_owner, &bounds);
    m_controller->put_Bounds(bounds);
}

void WebViewHost::executeScript(const std::wstring& script) {
    if (m_webView) {
        m_webView->ExecuteScript(script.c_str(), nullptr);
    }
}

void WebViewHost::close() {
    if (m_webView && m_messageHandlerRegistered) {
        m_webView->remove_WebMessageReceived(m_messageToken);
    }
    m_messageHandlerRegistered = false;
    m_webView.Reset();
    if (m_controller) {
        m_controller->Close();
    }
    m_controller.Reset();
    m_owner = nullptr;
    m_html.clear();
    m_messageCallback = {};
    m_readyCallback = {};
}

} // namespace fastrecord::ui
