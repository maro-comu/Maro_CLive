#include "Maro_App.hpp"

#include <windows.h>

int APIENTRY wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int showCommand)
{
    // DPI awareness must be selected before any HWND is created.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    Maro_App application(instance);
    return application.Run(showCommand);
}
