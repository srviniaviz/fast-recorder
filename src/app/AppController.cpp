#include "app/AppController.h"

#include "app/AppSettings.h"
#include "BuildVersion.h"
#include "platform/CaptureSelector.h"
#include "platform/WindowsStartup.h"
#include "resources/resource.h"

#include "ui/AppPage.h"

#include <Windows.h>
#include <propkey.h>
#include <propsys.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <fstream>
#include <iterator>
#include <new>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fastrecord {

namespace {

constexpr wchar_t kSingleInstanceName[] = L"Local\\FastRecord.SingleInstance.WebView";
constexpr wchar_t kWindowTitle[] = L"Fast Record";
constexpr wchar_t kTrayTooltip[] = L"Fast Record - painel de gravação";

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

bool isSupportedVideoPath(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return extension == L".mp4" || extension == L".mov" || extension == L".m4v" ||
        extension == L".wmv" || extension == L".avi" || extension == L".mkv" ||
        extension == L".webm" || extension == L".mpg" || extension == L".mpeg" ||
        extension == L".asf";
}

} // namespace

AppController::AppController(HINSTANCE instance)
    : m_instance(instance) {}

AppController::~AppController() {
    shutdown();
}

bool AppController::initialize(bool startHidden) {
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
    m_codec = savedSettings.codec;
    m_area = savedSettings.captureArea;
    m_resolution = savedSettings.resolution;
    m_framesPerSecond = savedSettings.framesPerSecond;
    m_bitrateMbps = savedSettings.bitrateMbps;
    m_microphoneEnabled = savedSettings.microphoneEnabled;
    m_systemAudioEnabled = savedSettings.systemAudioEnabled;
    m_alwaysOnTop = savedSettings.alwaysOnTop;
    m_startWithWindows = savedSettings.startWithWindows;
    m_transcription.model = savedSettings.transcriptionModel;
    m_settingsLoaded = true;

    if (m_startWithWindows && !platform::isStartWithWindowsEnabled()) {
        m_startWithWindows = false;
        persistSettings();
    } else if (m_startWithWindows) {
        std::wstring startupError;
        if (!platform::setStartWithWindows(true, startupError)) {
            m_startWithWindows = false;
            m_status = startupError;
            persistSettings();
        }
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_comInitialized = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        shutdown();
        return false;
    }

    m_nvencProbe = recording::probeNvenc();
    m_amfProbe = recording::probeAmf();
    m_qsvProbe = recording::probeQsv();
    refreshRecordings();
    m_transcription.modelReady = transcription::modelAvailable(m_transcription.model);
    m_transcription.status = m_transcription.modelReady
        ? L"Modelo Whisper " + std::wstring(transcription::modelPresentation(m_transcription.model).name) + L" pronto."
        : L"Instale o modelo Whisper escolhido para transcrever.";

    if (!createMainWindow() || !createTrayIcon()) {
        shutdown();
        return false;
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

    if (!startHidden) {
        showMainWindow();
    }
    SetTimer(m_window, 1, 250, nullptr);
    beginUpdateCheck();
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

    if (m_updateThread.joinable()) {
        m_updateThread.join();
    }
    if (m_transcriptionThread.joinable()) {
        m_transcriptionThread.request_stop();
        m_transcriptionThread.join();
    }

    if (m_recorder.isRecording()) {
        m_recorder.stop();
    }

    persistSettings();

    m_tray.destroy();
    for (HICON pulseIcon : m_recordingPulseIcons) {
        if (pulseIcon != nullptr) {
            DestroyIcon(pulseIcon);
        }
    }
    m_webView.close();

    if (m_window != nullptr) {
        DragAcceptFiles(m_window, FALSE);
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
    m_recordingPulseIcons[0] = platform::TrayIcon::createPulseFrame(m_icon, false);
    m_recordingPulseIcons[1] = platform::TrayIcon::createPulseFrame(m_icon, true);
    if (m_recordingPulseIcons[0] == nullptr || m_recordingPulseIcons[1] == nullptr) {
        for (HICON& pulseIcon : m_recordingPulseIcons) {
            if (pulseIcon != nullptr) {
                DestroyIcon(pulseIcon);
                pulseIcon = nullptr;
            }
        }
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
    DragAcceptFiles(m_window, TRUE);

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
        [this](LPARAM event) { handleTrayEvent(0, event); });
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

void AppController::handleTrayEvent(WPARAM eventCode, LPARAM eventData) {
    const UINT event = static_cast<UINT>(LOWORD(eventData));
    const UINT alternateEvent = static_cast<UINT>(LOWORD(eventCode));
    const auto isMouseOrSelectionEvent = [](UINT value) {
        return value == WM_LBUTTONDOWN || value == WM_LBUTTONUP ||
            value == WM_LBUTTONDBLCLK || value == NIN_SELECT ||
            value == NIN_KEYSELECT || value == WM_RBUTTONDOWN ||
            value == WM_RBUTTONUP || value == WM_CONTEXTMENU;
    };
    const UINT normalizedEvent = isMouseOrSelectionEvent(event) ? event : alternateEvent;

    switch (normalizedEvent) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case NIN_SELECT:
    case NIN_KEYSELECT:
        showMainWindow();
        break;
    case WM_RBUTTONDOWN:
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
    } else if (message == L"refresh-transcription") {
        syncInterface();
    } else if (message.rfind(transcription::kModelMessagePrefix, 0) == 0) {
        if (const auto selected = transcription::modelFromMessage(message)) {
            selectTranscriptionModel(*selected);
        }
    } else if (message == L"choose-transcription-video") {
        chooseTranscriptionVideo();
    } else if (message == L"download-whisper-model") {
        beginTranscriptionModelDownload();
    } else if (message == L"start-transcription") {
        beginVideoTranscription();
    } else if (message == L"cancel-transcription") {
        cancelTranscriptionTask();
    } else if (message == L"copy-transcript") {
        copyTranscriptToClipboard();
    } else if (message == L"save-transcript-srt") {
        saveTranscriptAsSrt();
    } else if (message == L"check-updates") {
        beginUpdateCheck();
    } else if (message == L"download-update") {
        beginUpdateDownload();
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
    } else if (message == L"toggle-system-audio") {
        m_systemAudioEnabled = !m_systemAudioEnabled;
        persistSettings();
        syncInterface();
    } else if (message == L"toggle-start-with-windows") {
        const bool enabled = !m_startWithWindows;
        std::wstring startupError;
        if (platform::setStartWithWindows(enabled, startupError)) {
            m_startWithWindows = enabled;
            persistSettings();
        } else {
            m_status = startupError;
            m_tray.showBalloon(L"Fast Record", startupError, NIIF_ERROR);
        }
        syncInterface();
    } else if (message == L"open-folder") {
        openRecordingsFolder();
    } else if (message == L"refresh-recordings") {
        refreshRecordings();
        syncInterface();
    } else if (message.starts_with(L"open-recording:") || message.starts_with(L"show-recording:")) {
        const bool reveal = message.starts_with(L"show-recording:");
        const std::filesystem::path recordingPath(message.substr(15));
        const auto directory = recordingsDirectory();
        std::error_code pathError;
        std::wstring extension = recordingPath.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
        const bool isMp4 = extension == L".mp4";
        const bool isInRecordingsDirectory =
            std::filesystem::equivalent(recordingPath.parent_path(), directory, pathError) &&
            !pathError;
        if (isMp4 && isInRecordingsDirectory &&
            std::filesystem::exists(recordingPath, pathError) && !pathError) {
            if (reveal) {
                const std::wstring parameters = L"/select,\"" + recordingPath.wstring() + L"\"";
                ShellExecuteW(m_window, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
            } else {
                ShellExecuteW(m_window, L"open", recordingPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
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
        } else if (value == L"qsv") {
            m_recorder.setEngine(recording::EncoderEngine::Qsv);
        } else if (value == L"software") {
            m_recorder.setEngine(recording::EncoderEngine::Software);
        } else {
            m_recorder.setEngine(recording::EncoderEngine::Automatic);
        }
        persistSettings();
        syncInterface();
    } else if (message.starts_with(L"codec:")) {
        const std::wstring value = message.substr(6);
        if (value == L"av1") {
            m_codec = recording::VideoCodec::Av1;
        } else {
            m_codec = recording::VideoCodec::H264;
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
    } else if (message.starts_with(L"fps:")) {
        const std::wstring value = message.substr(4);
        wchar_t* end = nullptr;
        const long parsed = std::wcstol(value.c_str(), &end, 10);
        if (end != value.c_str() && *end == L'\0' &&
            (parsed == 24 || parsed == 30 || parsed == 60 || parsed == 120)) {
            m_framesPerSecond = static_cast<int>(parsed);
            persistSettings();
        }
        syncInterface();
    }
}

void AppController::chooseTranscriptionVideo() {
    {
        std::scoped_lock lock(m_transcriptionMutex);
        if (m_transcription.busy) {
            return;
        }
    }

    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    HRESULT result = CoCreateInstance(
        CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(result) || !dialog) {
        {
            std::scoped_lock lock(m_transcriptionMutex);
            m_transcription.status = L"O seletor de arquivos do Windows não pôde ser aberto.";
        }
        syncInterface();
        return;
    }

    constexpr COMDLG_FILTERSPEC filters[]{
        {L"Vídeos", L"*.mp4;*.mov;*.m4v;*.wmv;*.avi;*.mkv;*.webm;*.mpg;*.mpeg;*.asf"},
        {L"Todos os arquivos", L"*.*"},
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"mp4");
    FILEOPENDIALOGOPTIONS options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    }

    result = dialog->Show(m_window);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return;
    }
    if (FAILED(result)) {
        {
            std::scoped_lock lock(m_transcriptionMutex);
            m_transcription.status = L"Não foi possível selecionar o vídeo.";
        }
        syncInterface();
        return;
    }

    Microsoft::WRL::ComPtr<IShellItem> item;
    PWSTR displayName = nullptr;
    if (FAILED(dialog->GetResult(&item)) || !item ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &displayName)) || displayName == nullptr) {
        {
            std::scoped_lock lock(m_transcriptionMutex);
            m_transcription.status = L"O caminho do vídeo não pôde ser lido.";
        }
        syncInterface();
        return;
    }
    const std::filesystem::path path(displayName);
    CoTaskMemFree(displayName);

    std::error_code pathError;
    if (!isSupportedVideoPath(path) || !std::filesystem::is_regular_file(path, pathError) || pathError) {
        {
            std::scoped_lock lock(m_transcriptionMutex);
            m_transcription.status = L"Escolha um arquivo de vídeo compatível.";
        }
        syncInterface();
        return;
    }
    {
        std::scoped_lock lock(m_transcriptionMutex);
        m_transcription.videoPath = path;
        m_transcription.videoName = path.filename().wstring();
        m_transcription.segments.clear();
        m_transcription.progress = 0;
        m_transcription.status = m_transcription.modelReady
            ? L"Vídeo escolhido. Você já pode iniciar a transcrição."
            : L"Vídeo escolhido. Baixe o Whisper para habilitar a transcrição.";
    }
    syncInterface();
}

void AppController::handleDroppedFiles(HDROP dropHandle) {
    if (dropHandle == nullptr) {
        return;
    }
    const UINT count = DragQueryFileW(dropHandle, 0xffffffff, nullptr, 0);
    if (count != 1) {
        DragFinish(dropHandle);
        {
            std::scoped_lock lock(m_transcriptionMutex);
            m_transcription.status = count == 0
                ? L"Nenhum arquivo foi recebido."
                : L"Solte um vídeo por vez para transcrevê-lo.";
        }
        syncInterface();
        return;
    }

    const UINT characterCount = DragQueryFileW(dropHandle, 0, nullptr, 0);
    std::wstring droppedPath(static_cast<std::size_t>(characterCount) + 1, L'\0');
    DragQueryFileW(dropHandle, 0, droppedPath.data(), characterCount + 1);
    droppedPath.resize(characterCount);
    DragFinish(dropHandle);

    const std::filesystem::path path(droppedPath);
    std::error_code pathError;
    if (!isSupportedVideoPath(path) || !std::filesystem::is_regular_file(path, pathError) || pathError) {
        {
            std::scoped_lock lock(m_transcriptionMutex);
            m_transcription.status = L"Esse arquivo não parece ser um vídeo compatível.";
        }
        syncInterface();
        return;
    }

    {
        std::scoped_lock lock(m_transcriptionMutex);
        if (m_transcription.busy) {
            return;
        }
        m_transcription.videoPath = path;
        m_transcription.videoName = path.filename().wstring();
        m_transcription.segments.clear();
        m_transcription.progress = 0;
        m_transcription.status = m_transcription.modelReady
            ? L"Vídeo escolhido. Você já pode iniciar a transcrição."
            : L"Vídeo escolhido. Baixe o Whisper para habilitar a transcrição.";
    }
    syncInterface();
}

void AppController::selectTranscriptionModel(transcription::Model model) {
    if (!transcription::isValidModel(model)) {
        return;
    }

    {
        std::scoped_lock lock(m_transcriptionMutex);
        if (m_transcription.busy || m_transcription.model == model) {
            return;
        }
        m_transcription.model = model;
        m_transcription.modelReady = transcription::modelAvailable(model);
        const auto& presentation = transcription::modelPresentation(model);
        m_transcription.status = m_transcription.modelReady
            ? L"Modelo " + std::wstring(presentation.name) + L" instalado neste PC."
            : L"Modelo " + std::wstring(presentation.name) + L" selecionado. Baixe-o para transcrever.";
    }
    persistSettings();
    syncInterface();
}

void AppController::beginTranscriptionModelDownload() {
    transcription::Model model = transcription::Model::Base;
    {
        std::scoped_lock lock(m_transcriptionMutex);
        if (m_transcription.busy || m_transcription.modelReady) {
            return;
        }
        model = m_transcription.model;
    }
    if (m_transcriptionThread.joinable()) {
        m_transcriptionThread.join();
    }

    {
        std::scoped_lock lock(m_transcriptionMutex);
        m_transcription.busy = true;
        m_transcription.progress = 0;
        m_transcription.status = L"Preparando o download do modelo Whisper…";
    }
    syncInterface();

    try {
        m_transcriptionThread = std::jthread([this, model](std::stop_token cancellation) {
            std::wstring error;
            const auto progress = [this](int percent, const std::wstring& status) {
                updateTranscriptionProgress(percent, status);
            };
            bool success = false;
            try {
                success = transcription::downloadModel(model, cancellation, progress, error);
            } catch (const std::bad_alloc&) {
                error = L"Não há memória suficiente para baixar o modelo.";
            } catch (...) {
                error = L"Ocorreu um erro inesperado ao baixar o modelo.";
            }
            {
                std::scoped_lock lock(m_transcriptionMutex);
                m_transcription.modelReady = success || transcription::modelAvailable(model);
                m_transcription.progress = success ? 100 : m_transcription.progress;
                if (success) {
                    m_transcription.status = L"Modelo " +
                        std::wstring(transcription::modelPresentation(model).name) +
                        L" pronto. A transcrição fica neste PC.";
                } else if (cancellation.stop_requested()) {
                    m_transcription.status = L"Download cancelado.";
                } else {
                    m_transcription.status = error.empty()
                        ? L"O modelo não pôde ser baixado. Tente novamente."
                        : error;
                }
            }
            PostMessageW(m_window, kTranscriptionTaskFinishedMessage, 0, 0);
        });
    } catch (...) {
        {
            std::scoped_lock lock(m_transcriptionMutex);
            m_transcription.busy = false;
            m_transcription.status = L"Não foi possível iniciar o download do modelo.";
        }
        syncInterface();
    }
}

void AppController::beginVideoTranscription() {
    std::filesystem::path videoPath;
    transcription::Model model = transcription::Model::Base;
    bool canStart = true;
    {
        std::scoped_lock lock(m_transcriptionMutex);
        if (m_transcription.busy) {
            return;
        }
        if (!m_transcription.modelReady) {
            m_transcription.status = L"Baixe o modelo Whisper antes de transcrever.";
            canStart = false;
        } else if (m_transcription.videoPath.empty()) {
            m_transcription.status = L"Escolha ou arraste um vídeo primeiro.";
            canStart = false;
        } else {
            videoPath = m_transcription.videoPath;
            model = m_transcription.model;
            m_transcription.busy = true;
            m_transcription.progress = 0;
            m_transcription.status = L"Preparando a transcrição…";
            m_transcription.segments.clear();
        }
    }
    if (!canStart) {
        syncInterface();
        return;
    }
    if (m_transcriptionThread.joinable()) {
        m_transcriptionThread.join();
    }
    syncInterface();

    try {
        m_transcriptionThread = std::jthread([this, model, videoPath](std::stop_token cancellation) {
            std::vector<transcription::Segment> segments;
            std::wstring error;
            const auto progress = [this](int percent, const std::wstring& status) {
                updateTranscriptionProgress(percent, status);
            };
            bool success = false;
            try {
                success = transcription::transcribeVideo(
                    model, videoPath, cancellation, progress, segments, error);
            } catch (const std::bad_alloc&) {
                error = L"Não há memória suficiente para processar este vídeo.";
            } catch (...) {
                error = L"Ocorreu um erro inesperado durante a transcrição.";
            }
            {
                std::scoped_lock lock(m_transcriptionMutex);
                m_transcription.progress = success ? 100 : m_transcription.progress;
                if (success) {
                    m_transcription.segments = std::move(segments);
                    m_transcription.status = L"Concluída · " +
                        std::to_wstring(m_transcription.segments.size()) + L" trechos.";
                } else if (cancellation.stop_requested()) {
                    m_transcription.status = L"Transcrição cancelada.";
                } else {
                    m_transcription.status = error.empty()
                        ? L"Não foi possível transcrever este vídeo."
                        : error;
                }
            }
            PostMessageW(m_window, kTranscriptionTaskFinishedMessage, 0, 0);
        });
    } catch (...) {
        {
            std::scoped_lock lock(m_transcriptionMutex);
            m_transcription.busy = false;
            m_transcription.status = L"Não foi possível iniciar a transcrição.";
        }
        syncInterface();
    }
}

void AppController::cancelTranscriptionTask() {
    if (!m_transcriptionThread.joinable()) {
        return;
    }
    m_transcriptionThread.request_stop();
    {
        std::scoped_lock lock(m_transcriptionMutex);
        m_transcription.status = L"Cancelando…";
    }
    syncInterface();
}

void AppController::updateTranscriptionProgress(int percent, const std::wstring& status) {
    bool changed = false;
    {
        std::scoped_lock lock(m_transcriptionMutex);
        if (!m_transcription.busy) {
            return;
        }
        const int boundedPercent = std::clamp(percent, 0, 100);
        changed = m_transcription.progress != boundedPercent || m_transcription.status != status;
        m_transcription.progress = boundedPercent;
        m_transcription.status = status;
    }
    if (changed && m_window != nullptr) {
        PostMessageW(m_window, kTranscriptionProgressMessage, 0, 0);
    }
}

void AppController::handleTranscriptionTaskFinished() {
    if (m_transcriptionThread.joinable()) {
        m_transcriptionThread.join();
    }
    {
        std::scoped_lock lock(m_transcriptionMutex);
        m_transcription.busy = false;
    }
    syncInterface();
}

void AppController::copyTranscriptToClipboard() {
    std::wstring text;
    {
        std::scoped_lock lock(m_transcriptionMutex);
        text = transcription::formatPlainText(m_transcription.segments);
    }
    if (text.empty()) {
        return;
    }
    if (!OpenClipboard(m_window)) {
        {
            std::scoped_lock lock(m_transcriptionMutex);
            m_transcription.status = L"Não foi possível acessar a área de transferência.";
        }
        syncInterface();
        return;
    }
    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        CloseClipboard();
        return;
    }
    void* destination = GlobalLock(memory);
    if (destination == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return;
    }
    std::memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return;
    }
    CloseClipboard();
    {
        std::scoped_lock lock(m_transcriptionMutex);
        m_transcription.status = L"Texto copiado para a área de transferência.";
    }
    syncInterface();
}

void AppController::saveTranscriptAsSrt() {
    std::vector<transcription::Segment> segments;
    std::wstring suggestedName = L"transcricao.srt";
    {
        std::scoped_lock lock(m_transcriptionMutex);
        segments = m_transcription.segments;
        if (!m_transcription.videoPath.empty()) {
            suggestedName = m_transcription.videoPath.stem().wstring() + L".srt";
        }
    }
    if (segments.empty()) {
        return;
    }

    Microsoft::WRL::ComPtr<IFileSaveDialog> dialog;
    HRESULT result = CoCreateInstance(
        CLSID_FileSaveDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(result) || !dialog) {
        {
            std::scoped_lock lock(m_transcriptionMutex);
            m_transcription.status = L"O seletor para salvar o arquivo não pôde ser aberto.";
        }
        syncInterface();
        return;
    }
    constexpr COMDLG_FILTERSPEC filter{L"Legenda SubRip (*.srt)", L"*.srt"};
    dialog->SetFileTypes(1, &filter);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"srt");
    dialog->SetFileName(suggestedName.c_str());
    FILEOPENDIALOGOPTIONS options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    }
    result = dialog->Show(m_window);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return;
    }
    Microsoft::WRL::ComPtr<IShellItem> item;
    PWSTR displayName = nullptr;
    if (FAILED(result) || FAILED(dialog->GetResult(&item)) || !item ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &displayName)) || displayName == nullptr) {
        {
            std::scoped_lock lock(m_transcriptionMutex);
            m_transcription.status = L"Não foi possível selecionar onde salvar o SRT.";
        }
        syncInterface();
        return;
    }
    const std::filesystem::path destination(displayName);
    CoTaskMemFree(displayName);
    const std::string output = transcription::encodeUtf8(transcription::formatSrt(segments));
    std::ofstream file(destination, std::ios::binary | std::ios::trunc);
    file.write(output.data(), static_cast<std::streamsize>(output.size()));
    {
        std::scoped_lock lock(m_transcriptionMutex);
        m_transcription.status = file
            ? L"Legenda SRT salva."
            : L"Não foi possível gravar o arquivo SRT.";
    }
    syncInterface();
}

void AppController::beginUpdateCheck() {
    if (m_updateBusy || m_shutdownStarted) {
        return;
    }
    if (m_updateThread.joinable()) {
        m_updateThread.join();
    }

    m_updateBusy = true;
    m_updateStatus = L"Verificando atualizações…";
    const HWND window = m_window;
    try {
        m_updateThread = std::thread([this, window] {
            UpdateTaskResult result;
            result.kind = UpdateTaskResult::Kind::Check;
            result.check = updates::checkForUpdate(FASTRECORD_VERSION);
            {
                std::lock_guard lock(m_updateResultMutex);
                m_pendingUpdateResult = std::move(result);
            }
            PostMessageW(window, kUpdateTaskFinishedMessage, 0, 0);
        });
    } catch (...) {
        m_updateBusy = false;
        m_updateStatus = L"Não foi possível iniciar a verificação de atualizações.";
    }
    syncInterface();
}

void AppController::beginUpdateDownload() {
    if (m_updateBusy || m_shutdownStarted || !m_availableUpdate.success ||
        !m_availableUpdate.updateAvailable) {
        return;
    }
    if (m_updateThread.joinable()) {
        m_updateThread.join();
    }

    m_updateBusy = true;
    m_updateStatus = L"Baixando e validando o instalador…";
    const HWND window = m_window;
    const updates::CheckResult update = m_availableUpdate;
    try {
        m_updateThread = std::thread([this, window, update] {
            UpdateTaskResult result;
            result.kind = UpdateTaskResult::Kind::Download;
            if (!updates::downloadAndVerifyInstaller(update, result.installerPath, result.error)) {
                result.installerPath.clear();
            }
            {
                std::lock_guard lock(m_updateResultMutex);
                m_pendingUpdateResult = std::move(result);
            }
            PostMessageW(window, kUpdateTaskFinishedMessage, 0, 0);
        });
    } catch (...) {
        m_updateBusy = false;
        m_updateStatus = L"Não foi possível iniciar o download do instalador.";
    }
    syncInterface();
}

void AppController::handleUpdateTaskFinished() {
    if (m_updateThread.joinable()) {
        m_updateThread.join();
    }

    std::optional<UpdateTaskResult> result;
    {
        std::lock_guard lock(m_updateResultMutex);
        result = std::move(m_pendingUpdateResult);
        m_pendingUpdateResult.reset();
    }
    m_updateBusy = false;
    if (!result.has_value()) {
        m_updateStatus = L"A tarefa de atualização terminou sem um resultado.";
        syncInterface();
        return;
    }

    if (result->kind == UpdateTaskResult::Kind::Check) {
        m_availableUpdate = std::move(result->check);
        if (!m_availableUpdate.success) {
            m_updateStatus = m_availableUpdate.error.empty()
                ? L"Não foi possível verificar atualizações."
                : m_availableUpdate.error;
        } else if (m_availableUpdate.updateAvailable) {
            m_updateStatus = L"Versão " + m_availableUpdate.version + L" disponível.";
            if (!IsWindowVisible(m_window)) {
                m_tray.showBalloon(
                    L"Fast Record",
                    L"A versão " + m_availableUpdate.version +
                        L" está disponível. Abra as configurações para atualizar.",
                    NIIF_INFO);
            }
        } else {
            m_updateStatus = L"Fast Record atualizado · v" +
                std::wstring(FASTRECORD_VERSION) + L".";
        }
        syncInterface();
        return;
    }

    if (result->installerPath.empty()) {
        m_updateStatus = result->error.empty()
            ? L"Não foi possível baixar e validar o instalador."
            : result->error;
        syncInterface();
        return;
    }

    const HINSTANCE launchResult = ShellExecuteW(
        m_window,
        L"open",
        result->installerPath.c_str(),
        nullptr,
        nullptr,
        SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(launchResult) > 32) {
        PostQuitMessage(0);
        return;
    }
    m_updateStatus = L"Não foi possível abrir o instalador baixado.";
    syncInterface();
}

void AppController::syncInterface() {
    if (!m_webViewReady) {
        return;
    }

    TranscriptionState transcriptionState;
    {
        std::scoped_lock lock(m_transcriptionMutex);
        transcriptionState = m_transcription;
    }
    std::wstring segmentsJson = L"[";
    for (std::size_t index = 0; index < transcriptionState.segments.size(); ++index) {
        if (index != 0) {
            segmentsJson += L",";
        }
        const auto& segment = transcriptionState.segments[index];
        segmentsJson += L"{\"startMs\":" + std::to_wstring(segment.startMilliseconds) +
            L",\"endMs\":" + std::to_wstring(segment.endMilliseconds) +
            L",\"text\":\"" + escapeJavaScriptString(segment.text) + L"\"}";
    }
    segmentsJson += L"]";
    const auto& modelInfo = transcription::modelPresentation(transcriptionState.model);
    const std::wstring transcriptionJson =
        L"{modelId:\"" + std::wstring(modelInfo.id) + L"\",modelName:\"" +
        std::wstring(modelInfo.name) + L"\",modelQuality:\"" +
        std::wstring(modelInfo.quality) + L"\",modelSize:\"" +
        std::wstring(modelInfo.diskSize) + L"\",modelReady:" +
        std::wstring(jsonBoolean(transcriptionState.modelReady)) +
        L",busy:" + jsonBoolean(transcriptionState.busy) +
        L",progress:" + std::to_wstring(transcriptionState.progress) +
        L",status:\"" + escapeJavaScriptString(transcriptionState.status) +
        L"\",videoPath:\"" + escapeJavaScriptString(transcriptionState.videoPath.wstring()) +
        L"\",videoName:\"" + escapeJavaScriptString(transcriptionState.videoName) +
        L"\",segments:" + segmentsJson + L"}";

    const std::wstring script =
        L"window.fastRecord&&window.fastRecord.setState({"
        L"recording:" + std::wstring(jsonBoolean(m_recorder.isRecording())) +
        L",paused:" + std::wstring(jsonBoolean(m_recorder.isPaused())) +
        L",microphone:" + std::wstring(jsonBoolean(m_microphoneEnabled)) +
        L",systemAudio:" + std::wstring(jsonBoolean(m_systemAudioEnabled)) +
        L",pinned:" + std::wstring(jsonBoolean(m_alwaysOnTop)) +
        L",startWithWindows:" + std::wstring(jsonBoolean(m_startWithWindows)) +
        L",updateAvailable:" + std::wstring(jsonBoolean(
            m_availableUpdate.success && m_availableUpdate.updateAvailable)) +
        L",updateBusy:" + std::wstring(jsonBoolean(m_updateBusy)) +
        L",updateStatus:\"" + escapeJavaScriptString(m_updateStatus) +
        L"\"" +
        L",elapsedSeconds:" + std::to_wstring(recordingElapsedSeconds()) +
        L",bitrate:" + std::to_wstring(m_bitrateMbps) +
        L",resolution:\"" + escapeJavaScriptString(m_resolution) +
        L"\",fps:\"" + std::to_wstring(m_framesPerSecond) +
        L"\",engine:\"" + escapeJavaScriptString(engineValue()) +
        L"\",codec:\"" + escapeJavaScriptString(codecValue()) +
        L"\",area:\"" + escapeJavaScriptString(m_area) +
        L"\",recordings:" + m_recordingsJson +
        L",transcription:" + transcriptionJson +
        L",status:\"" + escapeJavaScriptString(statusText()) +
        L"\",capability:\"" + escapeJavaScriptString(selectedEngineStatus()) +
        L"\"});";
    m_webView.executeScript(script);
    updateTrayTooltip();
    updateTrayIcon();
}

void AppController::persistSettings() {
    if (!m_settingsLoaded) {
        return;
    }

    const AppSettings settings{
        m_recorder.engine(),
        m_codec,
        m_area,
        m_resolution,
        m_framesPerSecond,
        m_bitrateMbps,
        m_microphoneEnabled,
        m_systemAudioEnabled,
        m_alwaysOnTop,
        m_startWithWindows,
        m_transcription.model,
    };
    saveAppSettings(settings);
}

void AppController::startRecording() {
    unsigned int width = 1920;
    unsigned int height = 1080;
    if (swscanf_s(m_resolution.c_str(), L"%ux%u", &width, &height) != 2) {
        width = 1920;
        height = 1080;
    }

    recording::RecordingSettings settings;
    POINT cursor{};
    GetCursorPos(&cursor);
    settings.target.monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);

    if (m_area == L"window") {
        const HWND selectedWindow = platform::chooseCaptureWindow(m_window, m_window);
        if (selectedWindow == nullptr) {
            m_status = L"Seleção de janela cancelada.";
            syncInterface();
            return;
        }
        settings.target.kind = recording::CaptureTargetKind::SelectedWindow;
        settings.target.window = selectedWindow;
        settings.target.monitor = MonitorFromWindow(selectedWindow, MONITOR_DEFAULTTONEAREST);
    } else if (m_area == L"region") {
        RECT selectedRegion{};
        if (!platform::chooseCaptureRegion(m_window, settings.target.monitor, selectedRegion)) {
            m_status = L"Seleção de região cancelada.";
            syncInterface();
            return;
        }
        settings.target.kind = recording::CaptureTargetKind::SelectedRegion;
        settings.captureRegion = selectedRegion;
    } else {
        settings.target.kind = recording::CaptureTargetKind::CurrentMonitor;
    }

    settings.outputDirectory = recordingsDirectory();
    settings.width = width;
    settings.height = height;
    settings.framesPerSecond = static_cast<std::uint32_t>(m_framesPerSecond);
    settings.bitrateMbps = static_cast<std::uint32_t>(m_bitrateMbps);
    settings.codec = m_codec;
    settings.captureCursor = true;
    settings.captureSystemAudio = m_systemAudioEnabled;
    settings.captureMicrophone = m_microphoneEnabled;

    const auto result = m_recorder.start(settings);
    m_status = result.message;
    if (result.success) {
        m_recordingElapsed = std::chrono::milliseconds{0};
        m_recordingResumedAt = std::chrono::steady_clock::now();
        m_recordingClockRunning = true;
        m_recordingPulsePhase = false;
    }
    if (result.success) {
        if (m_microphoneEnabled && m_systemAudioEnabled) {
            m_status = L"Gravando tela, microfone e áudio do PC.";
        } else if (m_microphoneEnabled) {
            m_status = L"Gravando tela e microfone.";
        } else if (m_systemAudioEnabled) {
            m_status = L"Gravando tela e áudio do PC.";
        } else {
            m_status = L"Gravando tela sem áudio.";
        }
    }
    if (!result.success) {
        m_tray.showBalloon(L"Fast Record", result.message, NIIF_WARNING);
    }
    syncInterface();
}

void AppController::togglePause() {
    const auto result = m_recorder.isPaused() ? m_recorder.resume() : m_recorder.pause();
    m_status = result.message;
    if (result.success) {
        m_recordingPulsePhase = false;
        if (m_recorder.isPaused()) {
            m_recordingElapsed += std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_recordingResumedAt);
            m_recordingClockRunning = false;
        } else {
            m_recordingResumedAt = std::chrono::steady_clock::now();
            m_recordingClockRunning = true;
        }
    }
    if (!result.success) {
        m_tray.showBalloon(L"Fast Record", result.message, NIIF_WARNING);
    }
    syncInterface();
}

void AppController::stopRecording() {
    if (m_recordingClockRunning) {
        m_recordingElapsed += std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_recordingResumedAt);
    }
    m_recordingClockRunning = false;
    m_recordingPulsePhase = false;
    const auto result = m_recorder.stop();
    const auto output = m_recorder.outputPath();
    m_status = result.success && !output.empty()
        ? L"Vídeo salvo em " + output.filename().wstring()
        : result.message;
    m_tray.showBalloon(
        L"Fast Record",
        result.message,
        result.success ? NIIF_INFO : NIIF_WARNING);
    refreshRecordings();
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

void AppController::updateTrayIcon() {
    if (m_recorder.isRecording() &&
        m_recordingPulseIcons[0] != nullptr &&
        m_recordingPulseIcons[1] != nullptr) {
        m_tray.setIcon(m_recordingPulseIcons[m_recordingPulsePhase ? 1 : 0]);
    } else {
        m_tray.setIcon(m_icon);
    }
}

std::wstring AppController::engineValue() const {
    switch (m_recorder.engine()) {
    case recording::EncoderEngine::Nvenc: return L"nvenc";
    case recording::EncoderEngine::Amf: return L"amf";
    case recording::EncoderEngine::Qsv: return L"qsv";
    case recording::EncoderEngine::Software: return L"software";
    default: return L"auto";
    }
}

std::wstring AppController::codecValue() const {
    return m_codec == recording::VideoCodec::Av1 ? L"av1" : L"h264";
}

std::wstring AppController::selectedEngineStatus() const {
    const bool av1 = m_codec == recording::VideoCodec::Av1;
    const wchar_t* codec = av1 ? L"AV1" : L"H.264";
    const auto engine = m_recorder.engine();
    if (av1 && (engine == recording::EncoderEngine::Qsv || engine == recording::EncoderEngine::Software)) {
        return L"AV1 nesta versão exige selecionar um encoder NVENC ou AMF compatível.";
    }

    switch (engine) {
    case recording::EncoderEngine::Nvenc:
        if (av1 && !m_nvencProbe.av1Supported) {
            return m_nvencProbe.description + L" AV1 não está disponível nesta GPU/driver.";
        }
        return m_nvencProbe.description +
            L" A gravação atual usa o backend direto NVENC com " + codec + L".";
    case recording::EncoderEngine::Amf:
        if (av1 && !m_amfProbe.av1Supported) {
            return m_amfProbe.description + L" Selecione H.264 para gravar pelo AMF.";
        }
        return m_amfProbe.description +
            L" A gravação atual usa o backend direto AMD AMF com " + codec + L".";
    case recording::EncoderEngine::Qsv:
        if (!m_qsvProbe.h264Supported) {
            return m_qsvProbe.description +
                L" Selecione Automático ou Software para continuar sem QSV.";
        }
        return m_qsvProbe.description +
            L" A gravação atual usa Intel Quick Sync via Media Foundation.";
    case recording::EncoderEngine::Software:
        return std::wstring(L"Compatível com qualquer GPU; utiliza a CPU para ") + codec + L".";
    default:
        if (av1) {
            if (m_nvencProbe.av1Supported) {
                return L"AV1 por NVENC disponível; selecione NVENC para gravar nesse formato.";
            }
            if (m_amfProbe.av1Supported) {
                return L"AV1 por AMD AMF disponível; selecione AMF para gravar nesse formato.";
            }
            return L"AV1 requer NVENC ou AMF compatível; selecione um deles após atualizar o driver, se necessário.";
        }
        if (m_nvencProbe.h264Supported) {
            return L"H.264 por hardware disponível; o Media Foundation escolherá o encoder.";
        }
        if (m_amfProbe.h264Supported) {
            return L"H.264 por hardware disponível; o Media Foundation escolherá o encoder.";
        }
        if (m_qsvProbe.h264Supported) {
            return L"H.264 por hardware disponível, incluindo Intel QSV; o Media Foundation escolherá o encoder.";
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

std::uint64_t AppController::recordingElapsedSeconds() const {
    auto elapsed = m_recordingElapsed;
    if (m_recordingClockRunning) {
        elapsed += std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_recordingResumedAt);
    }
    return elapsed.count() > 0
        ? static_cast<std::uint64_t>(elapsed.count() / 1000)
        : 0;
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

void AppController::refreshRecordings() {
    struct RecordingFile {
        std::filesystem::path path;
        std::filesystem::file_time_type modified;
        std::uintmax_t size{0};
        std::uint64_t duration{0};
    };

    std::vector<RecordingFile> files;
    const auto directory = recordingsDirectory();
    std::error_code error;
    if (directory.empty() || !std::filesystem::exists(directory, error) || error) {
        m_recordingsJson = L"[]";
        return;
    }

    for (std::filesystem::directory_iterator iterator(
             directory, std::filesystem::directory_options::skip_permission_denied, error),
         end;
         iterator != end && !error;
         iterator.increment(error)) {
        const auto& entry = *iterator;
        std::error_code entryError;
        if (!entry.is_regular_file(entryError) || entryError) {
            continue;
        }

        std::wstring extension = entry.path().extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
        if (extension != L".mp4") {
            continue;
        }

        const auto modified = entry.last_write_time(entryError);
        if (entryError) {
            continue;
        }
        const auto size = entry.file_size(entryError);
        std::uint64_t duration = 0;
        Microsoft::WRL::ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(SHGetPropertyStoreFromParsingName(
                entry.path().c_str(),
                nullptr,
                GPS_BESTEFFORT,
                IID_PPV_ARGS(&properties)))) {
            PROPVARIANT value{};
            PropVariantInit(&value);
            if (SUCCEEDED(properties->GetValue(PKEY_Media_Duration, &value)) && value.vt == VT_UI8) {
                duration = value.uhVal.QuadPart / 10'000'000ull;
            }
            PropVariantClear(&value);
        }
        files.push_back({entry.path(), modified, entryError ? 0 : size, duration});
    }

    std::sort(files.begin(), files.end(), [](const RecordingFile& left, const RecordingFile& right) {
        return left.modified > right.modified;
    });
    if (files.size() > 50) {
        files.resize(50);
    }

    const auto formatSize = [](std::uintmax_t bytes) {
        std::wostringstream output;
        if (bytes >= 1024ull * 1024ull * 1024ull) {
            output << std::fixed << std::setprecision(1)
                << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) << L" GB";
        } else if (bytes >= 1024ull * 1024ull) {
            output << std::fixed << std::setprecision(1)
                << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << L" MB";
        } else {
            output << std::max<std::uintmax_t>(1, bytes / 1024ull) << L" KB";
        }
        return output.str();
    };

    const auto formatTime = [](const std::filesystem::path& path) {
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
            return std::wstring(L"Data desconhecida");
        }

        FILETIME localTime{};
        SYSTEMTIME systemTime{};
        if (!FileTimeToLocalFileTime(&attributes.ftLastWriteTime, &localTime) ||
            !FileTimeToSystemTime(&localTime, &systemTime)) {
            return std::wstring(L"Data desconhecida");
        }

        wchar_t formatted[64]{};
        swprintf_s(
            formatted,
            _countof(formatted),
            L"%02u/%02u/%04u · %02u:%02u",
            static_cast<unsigned>(systemTime.wDay),
            static_cast<unsigned>(systemTime.wMonth),
            static_cast<unsigned>(systemTime.wYear),
            static_cast<unsigned>(systemTime.wHour),
            static_cast<unsigned>(systemTime.wMinute));
        return std::wstring(formatted);
    };

    const auto formatDuration = [](std::uint64_t seconds) {
        if (seconds == 0) {
            return std::wstring{};
        }
        const auto hours = seconds / 3600;
        const auto minutes = (seconds % 3600) / 60;
        const auto remainder = seconds % 60;
        wchar_t formatted[24]{};
        if (hours > 0) {
            swprintf_s(formatted, _countof(formatted), L"%02llu:%02llu:%02llu", hours, minutes, remainder);
        } else {
            swprintf_s(formatted, _countof(formatted), L"%02llu:%02llu", minutes, remainder);
        }
        return std::wstring(formatted);
    };

    std::wstring json = L"[";
    for (size_t index = 0; index < files.size(); ++index) {
        if (index != 0) {
            json += L",";
        }
        const auto& file = files[index];
        json += L"{\"name\":\"" + escapeJavaScriptString(file.path.filename().wstring()) +
            L"\",\"path\":\"" + escapeJavaScriptString(file.path.wstring()) +
            L"\",\"size\":\"" + escapeJavaScriptString(formatSize(file.size)) +
            L"\",\"duration\":\"" + escapeJavaScriptString(formatDuration(file.duration)) +
            L"\",\"time\":\"" + escapeJavaScriptString(formatTime(file.path)) + L"\"}";
    }
    json += L"]";
    m_recordingsJson = std::move(json);
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
        m_nvencProbe.description + L"\n" + m_amfProbe.description + L"\n" +
        m_qsvProbe.description;
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
            handleTrayEvent(wParam, lParam);
        }
        return 0;
    }

    if (message == kShowWindowMessage) {
        showMainWindow();
        return 0;
    }

    if (message == kUpdateTaskFinishedMessage) {
        handleUpdateTaskFinished();
        return 0;
    }
    if (message == kTranscriptionProgressMessage) {
        syncInterface();
        return 0;
    }
    if (message == kTranscriptionTaskFinishedMessage) {
        handleTranscriptionTaskFinished();
        return 0;
    }

    switch (message) {
    case WM_DROPFILES:
        handleDroppedFiles(reinterpret_cast<HDROP>(wParam));
        return 0;
    case WM_TIMER:
        if (wParam == 1) {
            if (m_recorder.hasFinished()) {
                stopRecording();
            } else if (m_recorder.isRecording()) {
                m_recordingPulsePhase = !m_recordingPulsePhase;
                syncInterface();
            }
        }
        return 0;
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
