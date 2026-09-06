#include "platform/WindowsStartup.h"

#include <Windows.h>

#include <array>
#include <string>

namespace fastrecord::platform {

namespace {

constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"Fast Record";
constexpr wchar_t kStartupApprovedKeyPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kStartupArgument[] = L" --startup";
constexpr std::array<BYTE, 12> kStartupApprovedEnabled{
    0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

void setRegistryError(
    std::wstring& error,
    const wchar_t* action,
    LSTATUS status) {
    error = std::wstring(action) + L" (código " + std::to_wstring(status) + L")";
}

std::wstring executablePath(std::wstring& error) {
    DWORD capacity = MAX_PATH;
    while (capacity <= 32768) {
        std::wstring buffer(capacity, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), capacity);
        if (length == 0) {
            setRegistryError(error, L"Não foi possível localizar o executável do Fast Record", GetLastError());
            return {};
        }

        if (length < capacity) {
            buffer.resize(length);
            return buffer;
        }

        capacity *= 2;
    }

    error = L"O caminho do executável do Fast Record é grande demais";
    return {};
}

bool hasRunValue() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            kRunKeyPath,
            0,
            KEY_READ,
            &key) != ERROR_SUCCESS) {
        return false;
    }

    DWORD size = 0;
    const LSTATUS status = RegGetValueW(
        key,
        nullptr,
        kRunValueName,
        RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
        nullptr,
        nullptr,
        &size);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && size >= sizeof(wchar_t);
}

bool startupApprovedValueExists(bool& enabled) {
    HKEY key = nullptr;
    const LSTATUS openStatus = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        kStartupApprovedKeyPath,
        0,
        KEY_READ,
        &key);
    if (openStatus == ERROR_FILE_NOT_FOUND) {
        return false;
    }
    if (openStatus != ERROR_SUCCESS) {
        enabled = false;
        return true;
    }

    std::array<BYTE, 12> value{};
    DWORD size = static_cast<DWORD>(value.size());
    const LSTATUS readStatus = RegGetValueW(
        key,
        nullptr,
        kRunValueName,
        RRF_RT_REG_BINARY,
        nullptr,
        value.data(),
        &size);
    RegCloseKey(key);
    if (readStatus == ERROR_FILE_NOT_FOUND) {
        return false;
    }
    if (readStatus != ERROR_SUCCESS || size == 0) {
        enabled = false;
        return true;
    }

    // Windows uses even first bytes for enabled states (02/06) and odd
    // first bytes for entries disabled by Task Manager or Settings.
    enabled = (value[0] % 2) == 0;
    return true;
}

bool removeValue(
    const wchar_t* keyPath,
    const wchar_t* valueName,
    const wchar_t* action,
    std::wstring& error) {
    HKEY key = nullptr;
    const LSTATUS openStatus = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        keyPath,
        0,
        KEY_SET_VALUE,
        &key);
    if (openStatus == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (openStatus != ERROR_SUCCESS) {
        setRegistryError(error, action, openStatus);
        return false;
    }

    const LSTATUS deleteStatus = RegDeleteValueW(key, valueName);
    RegCloseKey(key);
    if (deleteStatus == ERROR_SUCCESS || deleteStatus == ERROR_FILE_NOT_FOUND) {
        return true;
    }

    setRegistryError(error, action, deleteStatus);
    return false;
}

bool writeStartupApproved(std::wstring& error) {
    HKEY key = nullptr;
    const LSTATUS createStatus = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        kStartupApprovedKeyPath,
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE,
        nullptr,
        &key,
        nullptr);
    if (createStatus != ERROR_SUCCESS) {
        setRegistryError(
            error,
            L"Não foi possível atualizar o estado de inicialização do Windows",
            createStatus);
        return false;
    }

    const LSTATUS setStatus = RegSetValueExW(
        key,
        kRunValueName,
        0,
        REG_BINARY,
        kStartupApprovedEnabled.data(),
        static_cast<DWORD>(kStartupApprovedEnabled.size()));
    RegCloseKey(key);
    if (setStatus != ERROR_SUCCESS) {
        setRegistryError(
            error,
            L"Não foi possível atualizar o estado de inicialização do Windows",
            setStatus);
        return false;
    }

    return true;
}

} // namespace

bool isStartWithWindowsEnabled() {
    if (!hasRunValue()) {
        return false;
    }

    bool approved = true;
    if (!startupApprovedValueExists(approved)) {
        // Keep compatibility with the entry created by older Fast Record
        // builds, which only wrote the Run value.
        return true;
    }
    return approved;
}

bool setStartWithWindows(bool enabled, std::wstring& error) {
    error.clear();

    if (!enabled) {
        std::wstring runError;
        std::wstring approvedError;
        const bool runRemoved = removeValue(
            kRunKeyPath,
            kRunValueName,
            L"Não foi possível desativar a inicialização com o Windows",
            runError);
        const bool approvedRemoved = removeValue(
            kStartupApprovedKeyPath,
            kRunValueName,
            L"Não foi possível atualizar o estado de inicialização do Windows",
            approvedError);
        if (!runRemoved) {
            error = runError;
        } else if (!approvedRemoved) {
            error = approvedError;
        }
        return runRemoved && approvedRemoved;
    }

    const std::wstring path = executablePath(error);
    if (path.empty()) {
        return false;
    }

    HKEY key = nullptr;
    const LSTATUS createStatus = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        kRunKeyPath,
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE,
        nullptr,
        &key,
        nullptr);
    if (createStatus != ERROR_SUCCESS) {
        setRegistryError(error, L"Não foi possível acessar a inicialização do Windows", createStatus);
        return false;
    }

    const std::wstring command = L"\"" + path + L"\"" + kStartupArgument;
    const DWORD commandSize = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    const LSTATUS setStatus = RegSetValueExW(
        key,
        kRunValueName,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()),
        commandSize);
    RegCloseKey(key);
    if (setStatus != ERROR_SUCCESS) {
        setRegistryError(error, L"Não foi possível ativar a inicialização com o Windows", setStatus);
        return false;
    }

    if (!writeStartupApproved(error)) {
        std::wstring ignored;
        removeValue(kRunKeyPath, kRunValueName, L"", ignored);
        return false;
    }

    return true;
}

} // namespace fastrecord::platform
