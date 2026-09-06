#include "app/AppController.h"

#include "app/AppSettings.h"
#include "BuildVersion.h"
#include "resources/resource.h"

#include "ui/AppPage.h"

#include <Windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cwchar>
#include <sstream>
#include <string>
#include <utility>

namespace fastrecord {

namespace {

constexpr wchar_t kSingleInstanceName[] = L"Local\\FastRecord.SingleInstance.WebView";
constexpr wchar_t kWindowTitle[] = L"Fast Record";
constexpr wchar_t kTrayTooltip[] = L"Fast Record - Ctrl+Shift+R para gravar";

std::wstring escapeJavaScriptString(const std::wstring& input) {
    std::wostringstream output;
    for (const wchar_t character : input) {
        switch (character) {
        case L'\\': output << L"\\\\"; break;
        case L'"': output << L"\\\""; break;
        case L'\n': output << L"\\n"; break;
        case L'\r': output << L"\\r"; break;
        case L'\t': output << L"\\t"; break;
        default:
            if (character < 0x20) {
                output << L' ';
            } else {
                output << character;
            }
            break;
        }
    }
    return output.str();
}

const wchar_t* jsonBoolean(bool value) {
    return value ? L"true" : L"false";
}

} // namespace

AppController::AppController(HINSTANCE instance)
    : m_instance(instance) {}

AppController::~AppController() {
    shutdown();
}

bool AppController::initialize() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    SetLastError(ERROR_SUCCESS);
    m_singleInstanceMutex = CreateMutexW(nullptr, TRUE, kSingleInstanceName);
    if (m_singleInstanceMutex == nullptr) {
        return false;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        const HWND existingWindow = FindWindowW(kWindowClassName, kWindowTitle);
        if (existingWindow != nullptr) {
            DWORD existingProcessId = 0;
            GetWindowThreadProcessId(existingWindow, &existingProcessId);
            if (existingProcessId != 0) {
                AllowSetForegroundWindow(existingProcessId);
            }
            ShowWindowAsync(existingWindow, SW_SHOWNORMAL);
            SetWindowPos(
                existingWindow,
                HWND_TOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            DWORD_PTR activationResult = 0;
            SendMessageTimeoutW(
                existingWindow,
                kShowWindowMessage,
                0,
                0,
                SMTO_ABORTIFHUNG,
                2000,
                &activationResult);
            BringWindowToTop(existingWindow);
            SetForegroundWindow(existingWindow);
        }
        CloseHandle(m_singleInstanceMutex);
        m_singleInstanceMutex = nullptr;
        return false;
    }

    const AppSettings savedSettings = loadAppSettings();
    m_recorder.setEngine(savedSettings.engine);
    m_area = savedSettings.captureArea;
    m_resolution = savedSettings.resolution;
    m_bitrateMbps = savedSettings.bitrateMbps;
    m_microphoneEnabled = savedSettings.microphoneEnabled;
    m_webcamEnabled = savedSettings.webcamEnabled;
    m_alwaysOnTop = savedSettings.alwaysOnTop;
    m_settingsLoaded = true;

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_comInitialized = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        shutdown();
        return false;
    }

    m_nvencProbe = recording::probeNvenc();
    m_amfProbe = recording::probeAmf();

    if (!createMainWindow() || !createTrayIcon()) {
        shutdown();
        return false;
    }

    if (!registerHotkeys()) {
        m_tray.showBalloon(
            L"Fast Record",
            L"Um ou mais atalhos globais já estão em uso. Os botões da janela continuam disponíveis.",
            NIIF_WARNING);
    }

    std::wstring appPage(ui::kAppPage);
    const std::wstring versionToken = L"__FASTRECORD_VERSION__";
    const size_t versionPosition = appPage.find(versionToken);
    if (versionPosition != std::wstring::npos) {
        appPage.replace(versionPosition, versionToken.size(), FASTRECORD_VERSION);
    }

    const bool webViewStarted = m_webView.initialize(
        m_window,
        std::move(appPage),
        [this](const std::wstring& message) { handleWebMessage(message); },
        [this](bool ready) {
            if (!ready) {
                m_status = L"Não foi possível iniciar a interface WebView2";
                m_tray.showBalloon(
                    L"Fast Record",
                    L"Falha ao abrir a interface. Repare ou instale o Microsoft Edge WebView2 Runtime.",
                    NIIF_ERROR);
            }
        });

    if (!webViewStarted) {
        shutdown();
        return false;
    }

    showMainWindow();
    return true;
}

int AppController::run() {
    MSG message{};
    while (true) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status == -1) {
            return 1;
        }
        if (status == 0) {
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void AppController::shutdown() {
    if (m_shutdownStarted) {
        return;
    }
    m_shutdownStarted = true;

    if (m_recorder.isRecording()) {
        m_recorder.stop();
    }

    persistSettings();

    unregisterHotkeys();
    m_tray.destroy();
    m_webView.close();

    if (m_window != nullptr) {
        DestroyWindow(m_window);
        m_window = nullptr;
    }

    if (m_singleInstanceMutex != nullptr) {
        ReleaseMutex(m_singleInstanceMutex);
        CloseHandle(m_singleInstanceMutex);
        m_singleInstanceMutex = nullptr;
    }

    if (m_comInitialized) {
        CoUninitialize();
        m_comInitialized = false;
    }
}

bool AppController::createMainWindow() {
    m_icon = LoadIconW(m_instance, MAKEINTRESOURCEW(IDI_FAST_RECORD));
    if (m_icon == nullptr) {
        return false;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.hInstance = m_instance;
    windowClass.lpfnWndProc = &AppController::windowProcedure;
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = m_icon;
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hbrBackground = CreateSolidBrush(RGB(8, 9, 12));

    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    m_window = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kWindowClassName,
        kWindowTitle,
        WS_POPUP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        kWindowWidth,
        kWindowHeight,
        nullptr,
        nullptr,
        m_instance,
        this);

    if (m_window == nullptr) {
        return false;
    }

    const HRGN roundedRegion = CreateRoundRectRgn(
        0, 0, kWindowWidth + 1, kWindowHeight + 1, 36, 36);
    SetWindowRgn(m_window, roundedRegion, TRUE);
    return true;
}

bool AppController::createTrayIcon() {
    return m_icon != nullptr && m_tray.create(
        m_window,
        m_icon,
        kTrayTooltip,
        [this](LPARAM event) { handleTrayEvent(event); });
}

bool AppController::registerHotkeys() {
    m_recordHotkeyRegistered = RegisterHotKey(
        m_window,
        kRecordHotkeyId,
        MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT,
        'R') == TRUE;
    m_pauseHotkeyRegistered = RegisterHotKey(
        m_window,
        kPauseHotkeyId,
        MOD_CONTROL | MOD_NOREPEAT,
        'P') == TRUE;
    m_stopHotkeyRegistered = RegisterHotKey(
        m_window,
        kStopHotkeyId,
        MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT,
        'S') == TRUE;
    return m_recordHotkeyRegistered && m_pauseHotkeyRegistered && m_stopHotkeyRegistered;
}

void AppController::unregisterHotkeys() {
    if (m_window != nullptr) {
        if (m_recordHotkeyRegistered) {
            UnregisterHotKey(m_window, kRecordHotkeyId);
        }
        if (m_pauseHotkeyRegistered) {
            UnregisterHotKey(m_window, kPauseHotkeyId);
        }
        if (m_stopHotkeyRegistered) {
            UnregisterHotKey(m_window, kStopHotkeyId);
        }
    }
    m_recordHotkeyRegistered = false;
    m_pauseHotkeyRegistered = false;
    m_stopHotkeyRegistered = false;
}

void AppController::dockWindowToBottom() {
    if (m_window == nullptr) {
        return;
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo)) {
        return;
    }

    const RECT workArea = monitorInfo.rcWork;
    const int left = std::max(workArea.left + 10, workArea.right - kWindowWidth - 18);
    const int top = std::max(workArea.top + 10, workArea.bottom - kWindowHeight - 14);
    SetWindowPos(
        m_window,
        m_alwaysOnTop ? HWND_TOPMOST : HWND_TOP,
        left,
        top,
        kWindowWidth,
        kWindowHeight,
        SWP_NOACTIVATE);
}

void AppController::showMainWindow() {
    if (m_window == nullptr) {
        return;
    }
    dockWindowToBottom();
    ShowWindowAsync(m_window, SW_RESTORE);
    SetWindowPos(
        m_window,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(m_window);
    SetForegroundWindow(m_window);
    if (!m_alwaysOnTop) {
        SetWindowPos(
            m_window,
            HWND_NOTOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
    UpdateWindow(m_window);
}

void AppController::handleTrayEvent(LPARAM event) {
    switch (event) {
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
        showMainWindow();
        break;
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        showTrayMenu();
        break;
    default:
        break;
    }
}

void AppController::showTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    AppendMenuW(menu, MF_STRING, CommandShowWindow, L"Abrir Fast Record");
    AppendMenuW(
        menu,
        MF_STRING,
        CommandToggle,
        m_recorder.isRecording() ? L"Parar gravação" : L"Gravar monitor atual");
    AppendMenuW(menu, MF_STRING, CommandOpenFolder, L"Abrir pasta de gravações");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CommandAbout, L"Sobre");
    AppendMenuW(menu, MF_STRING, CommandExit, L"Sair");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(m_window);
    TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
        cursor.x,
        cursor.y,
        0,
        m_window,
        nullptr);
    PostMessageW(m_window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void AppController::handleCommand(UINT_PTR command) {
    switch (command) {
    case CommandShowWindow: showMainWindow(); break;
    case CommandToggle: toggleRecording(); break;
    case CommandOpenFolder: openRecordingsFolder(); break;
    case CommandAbout: showAboutDialog(); break;
    case CommandExit: PostQuitMessage(0); break;
    default: break;
    }
}

void AppController::handleWebMessage(const std::wstring& message) {
    if (message == L"ready") {
        m_webViewReady = true;
        syncInterface();
    } else if (message == L"record") {
        startRecording();
    } else if (message == L"pause") {
        togglePause();
    } else if (message == L"stop") {
        stopRecording();
    } else if (message == L"toggle-mic") {
        m_microphoneEnabled = !m_microphoneEnabled;
        persistSettings();
        syncInterface();
    } else if (message == L"toggle-webcam") {
        m_webcamEnabled = !m_webcamEnabled;
        persistSettings();
        syncInterface();
    } else if (message == L"open-folder") {
        openRecordingsFolder();
    } else if (message == L"pin") {
        m_alwaysOnTop = !m_alwaysOnTop;
        SetWindowPos(
            m_window,
            m_alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        persistSettings();
        syncInterface();
    } else if (message == L"minimize") {
        ShowWindow(m_window, SW_MINIMIZE);
    } else if (message == L"hide") {
        ShowWindow(m_window, SW_HIDE);
    } else if (message == L"exit") {
        PostQuitMessage(0);
    } else if (message == L"drag") {
        ReleaseCapture();
        SendMessageW(m_window, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    } else if (message.starts_with(L"engine:")) {
        const std::wstring value = message.substr(7);
        if (value == L"nvenc") {
            m_recorder.setEngine(recording::EncoderEngine::Nvenc);
        } else if (value == L"amf") {
            m_recorder.setEngine(recording::EncoderEngine::Amf);
        } else if (value == L"software") {
            m_recorder.setEngine(recording::EncoderEngine::Software);
        } else {
            m_recorder.setEngine(recording::EncoderEngine::Automatic);
        }
        persistSettings();
        syncInterface();
    } else if (message.starts_with(L"area:")) {
        const std::wstring area = message.substr(5);
        if (area == L"monitor" || area == L"window" || area == L"region") {
            m_area = area;
            persistSettings();
        }
        syncInterface();
    } else if (message.starts_with(L"bitrate:")) {
        const std::wstring value = message.substr(8);
        wchar_t* end = nullptr;
        const long parsed = std::wcstol(value.c_str(), &end, 10);
        if (end != value.c_str() && *end == L'\0') {
            m_bitrateMbps = static_cast<int>(std::clamp(parsed, 4L, 80L));
            persistSettings();
        }
        syncInterface();
    } else if (message.starts_with(L"resolution:")) {
        const std::wstring resolution = message.substr(11);
        if (resolution == L"1280x720" || resolution == L"1920x1080" ||
            resolution == L"2560x1440" || resolution == L"3840x2160") {
            m_resolution = resolution;
            persistSettings();
        }
        syncInterface();
    }
}

void AppController::syncInterface() {
    if (!m_webViewReady) {
        return;
    }

    const std::wstring script =
        L"window.fastRecord&&window.fastRecord.setState({"
        L"recording:" + std::wstring(jsonBoolean(m_recorder.isRecording())) +
        L",paused:" + std::wstring(jsonBoolean(m_recorder.isPaused())) +
        L",microphone:" + std::wstring(jsonBoolean(m_microphoneEnabled)) +
        L",webcam:" + std::wstring(jsonBoolean(m_webcamEnabled)) +
        L",pinned:" + std::wstring(jsonBoolean(m_alwaysOnTop)) +
        L",bitrate:" + std::to_wstring(m_bitrateMbps) +
        L",resolution:\"" + escapeJavaScriptString(m_resolution) +
        L"\",engine:\"" + escapeJavaScriptString(engineValue()) +
        L"\",area:\"" + escapeJavaScriptString(m_area) +
        L"\",status:\"" + escapeJavaScriptString(statusText()) +
        L"\",capability:\"" + escapeJavaScriptString(selectedEngineStatus()) +
        L"\"});";
    m_webView.executeScript(script);
    updateTrayTooltip();
}

void AppController::persistSettings() {
    if (!m_settingsLoaded) {
        return;
    }

    const AppSettings settings{
        m_recorder.engine(),
        m_area,
        m_resolution,
        m_bitrateMbps,
        m_microphoneEnabled,
        m_webcamEnabled,
        m_alwaysOnTop,
    };
    saveAppSettings(settings);
}

void AppController::startRecording() {
    if (m_area != L"monitor") {
        m_status = L"A seleção de janela e região entra na próxima etapa; escolha Monitor inteiro.";
        m_tray.showBalloon(L"Fast Record", m_status, NIIF_WARNING);
        syncInterface();
        return;
    }

    unsigned int width = 1920;
    unsigned int height = 1080;
    if (swscanf_s(m_resolution.c_str(), L"%ux%u", &width, &height) != 2) {
        width = 1920;
        height = 1080;
    }

    POINT cursor{};
    GetCursorPos(&cursor);

    recording::RecordingSettings settings;
    settings.target.kind = recording::CaptureTargetKind::CurrentMonitor;
    settings.target.monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    settings.outputDirectory = recordingsDirectory();
    settings.width = width;
    settings.height = height;
    settings.framesPerSecond = 30;
    settings.bitrateMbps = static_cast<std::uint32_t>(m_bitrateMbps);
    settings.captureCursor = true;
    settings.captureSystemAudio = false;
    settings.captureMicrophone = false;

    const auto result = m_recorder.start(settings);
    m_status = result.message;
    if (result.success && m_microphoneEnabled) {
        m_status = L"Gravando vídeo; o microfone será conectado na próxima etapa.";
    }
    if (!result.success) {
        m_tray.showBalloon(L"Fast Record", result.message, NIIF_WARNING);
    }
    syncInterface();
}

void AppController::togglePause() {
    const auto result = m_recorder.isPaused() ? m_recorder.resume() : m_recorder.pause();
    m_status = result.message;
    if (!result.success) {
        m_tray.showBalloon(L"Fast Record", result.message, NIIF_WARNING);
    }
    syncInterface();
}

void AppController::stopRecording() {
    const auto result = m_recorder.stop();
    const auto output = m_recorder.outputPath();
    m_status = result.success && !output.empty()
        ? L"Vídeo salvo em " + output.filename().wstring()
        : result.message;
    m_tray.showBalloon(
        L"Fast Record",
        result.message,
        result.success ? NIIF_INFO : NIIF_WARNING);
    syncInterface();
}

void AppController::toggleRecording() {
    if (m_recorder.isRecording()) {
        stopRecording();
    } else {
        startRecording();
    }
}

void AppController::updateTrayTooltip() {
    if (m_recorder.isPaused()) {
        m_tray.setTooltip(L"Fast Record - pausado");
    } else if (m_recorder.isRecording()) {
        m_tray.setTooltip(L"Fast Record - gravando");
    } else {
        m_tray.setTooltip(L"Fast Record - pronto");
    }
}

std::wstring AppController::engineValue() const {
    switch (m_recorder.engine()) {
    case recording::EncoderEngine::Nvenc: return L"nvenc";
    case recording::EncoderEngine::Amf: return L"amf";
    case recording::EncoderEngine::Software: return L"software";
    default: return L"auto";
    }
}

std::wstring AppController::selectedEngineStatus() const {
    switch (m_recorder.engine()) {
    case recording::EncoderEngine::Nvenc:
        return m_nvencProbe.description +
            L" A gravação atual usa a seleção de hardware do Media Foundation.";
    case recording::EncoderEngine::Amf:
        return m_amfProbe.description +
            L" A gravação atual usa a seleção de hardware do Media Foundation.";
    case recording::EncoderEngine::Software:
        return L"Compatível com qualquer GPU; utiliza a CPU.";
    default:
        if (m_nvencProbe.h264Supported) {
            return L"H.264 por hardware disponível; o Media Foundation escolherá o encoder.";
        }
        if (m_amfProbe.h264Supported) {
            return L"H.264 por hardware disponível; o Media Foundation escolherá o encoder.";
        }
        if (m_amfProbe.runtimeLibraryFound) {
            return L"Runtime AMD AMF detectado; validação completa requer o SDK.";
        }
        if (m_nvencProbe.runtimeLibraryFound) {
            return L"Runtime NVIDIA NVENC detectado; validação completa requer o SDK.";
        }
        return L"Nenhum encoder de GPU confirmado; será usado software.";
    }
}

std::wstring AppController::statusText() const {
    if (m_recorder.isPaused()) {
        return L"Gravação pausada";
    }
    if (m_recorder.isRecording()) {
        return L"Gravando agora";
    }
    return m_status;
}

std::filesystem::path AppController::recordingsDirectory() const {
    PWSTR rawPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_DEFAULT, nullptr, &rawPath)) ||
        rawPath == nullptr) {
        return {};
    }

    std::filesystem::path directory(rawPath);
    CoTaskMemFree(rawPath);
    directory /= L"Fast Record";
    return directory;
}

void AppController::openRecordingsFolder() {
    const auto directory = recordingsDirectory();
    if (directory.empty()) {
        m_status = L"Não foi possível localizar a pasta Vídeos";
        syncInterface();
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        m_status = L"Não foi possível criar a pasta de gravações";
        syncInterface();
        return;
    }

    ShellExecuteW(m_window, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void AppController::showAboutDialog() {
    const std::wstring message =
        std::wstring(L"Fast Record ") + FASTRECORD_VERSION +
        L"\n\nGravador rápido de tela para Windows.\n\n" +
        m_nvencProbe.description + L"\n" + m_amfProbe.description;
    MessageBoxW(m_window, message.c_str(), L"Sobre o Fast Record", MB_OK | MB_ICONINFORMATION);
}

LRESULT CALLBACK AppController::windowProcedure(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* app = static_cast<AppController*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }

    auto* app = reinterpret_cast<AppController*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return app == nullptr
        ? DefWindowProcW(hwnd, message, wParam, lParam)
        : app->handleMessage(hwnd, message, wParam, lParam);
}

LRESULT AppController::handleMessage(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {
    if (message == platform::TrayIcon::kCallbackMessage) {
        if (m_tray.isCreated()) {
            handleTrayEvent(lParam);
        }
        return 0;
    }

    if (message == kShowWindowMessage) {
        showMainWindow();
        return 0;
    }

    switch (message) {
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            ShowWindow(hwnd, SW_HIDE);
        } else {
            m_webView.resize();
        }
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        dockWindowToBottom();
        return 0;
    case WM_HOTKEY:
        if (wParam == kRecordHotkeyId) {
            toggleRecording();
        } else if (wParam == kPauseHotkeyId && m_recorder.isRecording()) {
            togglePause();
        } else if (wParam == kStopHotkeyId && m_recorder.isRecording()) {
            stopRecording();
        }
        return 0;
    case WM_COMMAND:
        handleCommand(LOWORD(wParam));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

} // namespace fastrecord
