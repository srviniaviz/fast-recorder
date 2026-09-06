#include "platform/WindowsStartup.h"

#include <Windows.h>

#include <string>

namespace fastrecord::platform {

namespace {

constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"Fast Record";
constexpr wchar_t kStartupArgument[] = L" --startup";

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

} // namespace

bool setStartWithWindows(bool enabled, std::wstring& error) {
    error.clear();

    if (!enabled) {
        HKEY key = nullptr;
        const LSTATUS openStatus = RegOpenKeyExW(
            HKEY_CURRENT_USER,
            kRunKeyPath,
            0,
            KEY_SET_VALUE,
            &key);
        if (openStatus == ERROR_FILE_NOT_FOUND) {
            return true;
        }
        if (openStatus != ERROR_SUCCESS) {
            setRegistryError(error, L"Não foi possível acessar a inicialização do Windows", openStatus);
            return false;
        }

        const LSTATUS deleteStatus = RegDeleteValueW(key, kRunValueName);
        RegCloseKey(key);
        if (deleteStatus == ERROR_SUCCESS || deleteStatus == ERROR_FILE_NOT_FOUND) {
            return true;
        }

        setRegistryError(error, L"Não foi possível desativar a inicialização com o Windows", deleteStatus);
        return false;
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

    return true;
}

} // namespace fastrecord::platform
