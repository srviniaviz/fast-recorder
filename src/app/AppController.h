#pragma once

#include "platform/TrayIcon.h"
#include "recording/AmfProbe.h"
#include "recording/NvencProbe.h"
#include "recording/RecorderController.h"
#include "ui/WebViewHost.h"

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace fastrecord {

class AppController final {
public:
    explicit AppController(HINSTANCE instance);
    ~AppController();

    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    bool initialize(bool startHidden = false);
    int run();
    void shutdown();

private:
    static constexpr int kRecordHotkeyId = 1;
    static constexpr int kPauseHotkeyId = 2;
    static constexpr int kStopHotkeyId = 3;
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
    bool registerHotkeys();
    void unregisterHotkeys();
    void dockWindowToBottom();
    void showMainWindow();
    void handleTrayEvent(WPARAM eventCode, LPARAM eventData);
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
    std::wstring codecValue() const;
    std::wstring selectedEngineStatus() const;
    std::wstring statusText() const;
    std::uint64_t recordingElapsedSeconds() const;
    std::filesystem::path recordingsDirectory() const;
    void refreshRecordings();

    HINSTANCE m_instance{nullptr};
    HWND m_window{nullptr};
    HICON m_icon{nullptr};
    HANDLE m_singleInstanceMutex{nullptr};
    bool m_comInitialized{false};
    bool m_recordHotkeyRegistered{false};
    bool m_pauseHotkeyRegistered{false};
    bool m_stopHotkeyRegistered{false};
    bool m_shutdownStarted{false};
    bool m_settingsLoaded{false};
    bool m_webViewReady{false};
    bool m_microphoneEnabled{true};
    bool m_systemAudioEnabled{true};
    bool m_alwaysOnTop{false};
    bool m_startWithWindows{false};
    int m_framesPerSecond{30};
    int m_bitrateMbps{20};
    recording::VideoCodec m_codec{recording::VideoCodec::H264};
    std::wstring m_area{L"monitor"};
    std::wstring m_resolution{L"1920x1080"};
    std::wstring m_status{L"Pronto para gravar"};
    std::chrono::steady_clock::time_point m_recordingResumedAt{};
    std::chrono::milliseconds m_recordingElapsed{0};
    bool m_recordingClockRunning{false};
    std::wstring m_recordingsJson{L"[]"};

    platform::TrayIcon m_tray;
    recording::RecorderController m_recorder;
    recording::AmfProbeResult m_amfProbe;
    recording::NvencProbeResult m_nvencProbe;
    ui::WebViewHost m_webView;
};

} // namespace fastrecord
