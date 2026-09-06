#include "app/AppController.h"

#include <Windows.h>
#include <shellapi.h>

namespace {

bool launchedFromWindowsStartup() {
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) {
        return false;
    }

    bool startup = false;
    for (int index = 1; index < argumentCount; ++index) {
        if (lstrcmpiW(arguments[index], L"--startup") == 0) {
            startup = true;
            break;
        }
    }
    LocalFree(arguments);
    return startup;
}

} // namespace

int APIENTRY wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int) {
    fastrecord::AppController app(instance);

    if (!app.initialize(launchedFromWindowsStartup())) {
        return 1;
    }

    const int exitCode = app.run();
    app.shutdown();
    return exitCode;
}
