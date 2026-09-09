#pragma once

#include <memory>

class Maro_App final
{
public:
    Maro_App();
    ~Maro_App();

    Maro_App(const Maro_App&) = delete;
    Maro_App& operator=(const Maro_App&) = delete;

    int Run();

private:
    struct Maro_Impl;
    std::unique_ptr<Maro_Impl> impl_;

};
