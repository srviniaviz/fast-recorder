#include "app/AppController.h"

#include <Windows.h>

int APIENTRY wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int) {
    fastrecord::AppController app(instance);

    if (!app.initialize()) {
        return 1;
    }

    const int exitCode = app.run();
    app.shutdown();
    return exitCode;
}

