#pragma once

#include "platform/TrayIcon.h"
#include "recording/AmfProbe.h"
#include "recording/NvencProbe.h"
#include "recording/RecorderController.h"
#include "ui/WebViewHost.h"

#include <Windows.h>

#include <filesystem>
#include <string>

namespace fastrecord {

class AppController final {
public:
    explicit AppController(HINSTANCE instance);
    ~AppController();

    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    bool initialize();
    int run();
    void shutdown();

private:
    static constexpr int kHotkeyId = 1;
    static constexpr UINT kShowWindowMessage = WM_APP + 20;
    static constexpr int kWindowWidth = 460;
    static constexpr int kWindowHeight = 570;
    static constexpr wchar_t kWindowClassName[] = L"FastRecord.WebView.MainWindow";

    enum Command : UINT_PTR {
        CommandShowWindow = 1001,
        CommandToggle = 1002,
        CommandOpenFolder = 1003,
        CommandAbout = 1004,
        CommandExit = 1005,
    };

    static LRESULT CALLBACK windowProcedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    bool createMainWindow();
    bool createTrayIcon();
    bool registerHotkey();
    void unregisterHotkey();
    void dockWindowToBottom();
    void showMainWindow();
    void handleTrayEvent(LPARAM event);
    void showTrayMenu();
    void handleCommand(UINT_PTR command);
    void handleWebMessage(const std::wstring& message);
    void syncInterface();
    void persistSettings();

    void startRecording();
    void togglePause();
    void stopRecording();
    void toggleRecording();
    void updateTrayTooltip();
    void openRecordingsFolder();
    void showAboutDialog();

    std::wstring engineValue() const;
    std::wstring selectedEngineStatus() const;
    std::wstring statusText() const;
    std::filesystem::path recordingsDirectory() const;

    HINSTANCE m_instance{nullptr};
    HWND m_window{nullptr};
    HICON m_icon{nullptr};
    HANDLE m_singleInstanceMutex{nullptr};
    bool m_comInitialized{false};
    bool m_hotkeyRegistered{false};
    bool m_shutdownStarted{false};
    bool m_settingsLoaded{false};
    bool m_webViewReady{false};
    bool m_microphoneEnabled{true};
    bool m_webcamEnabled{false};
    bool m_alwaysOnTop{false};
    int m_bitrateMbps{20};
    std::wstring m_area{L"monitor"};
    std::wstring m_resolution{L"1920x1080"};
    std::wstring m_status{L"Pronto para gravar"};

    platform::TrayIcon m_tray;
    recording::RecorderController m_recorder;
    recording::AmfProbeResult m_amfProbe;
    recording::NvencProbeResult m_nvencProbe;
    ui::WebViewHost m_webView;
};

} // namespace fastrecord
