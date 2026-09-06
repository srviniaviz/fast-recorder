#pragma once

#include <string>

namespace fastrecord::platform {

// Adds or removes Fast Record from the current user's Windows startup list.
// This uses HKCU, so enabling it never requires administrator privileges.
bool setStartWithWindows(bool enabled, std::wstring& error);

} // namespace fastrecord::platform
