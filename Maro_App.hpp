#pragma once

#include <windows.h>

#include <memory>

class Maro_App final
{
public:
    explicit Maro_App(HINSTANCE instance);
    ~Maro_App();

    Maro_App(const Maro_App&) = delete;
    Maro_App& operator=(const Maro_App&) = delete;

    int Run(int showCommand);

private:
    struct Maro_Impl;
    std::unique_ptr<Maro_Impl> impl_;

    static LRESULT CALLBACK WindowProcedure(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam);
};
