#pragma once

#include "maro_Process.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

struct maro_ProjectRequest
{
    std::wstring maro_projectPath;
    std::wstring maro_configuration;
    std::wstring maro_platform;
    std::wstring maro_msbuildPath;
    std::wstring maro_solutionPath;
    bool maro_background = false;
    Maro_ProcessLimits maro_limits{1'800'000, 0, 8ull << 30, 128, 16u << 20, 16u << 20};
};

struct maro_ProjectResult
{
    bool maro_success = false;
    std::wstring maro_executablePath;
    std::wstring maro_workingDirectory;
    std::wstring maro_configurationType;
    std::vector<std::wstring> maro_arguments;
    std::map<std::wstring, std::wstring, std::less<>> maro_environment;
    bool maro_inheritEnvironment = true;
    std::wstring maro_message;
    std::wstring maro_buildOutput;
    Maro_ProcessResult maro_process;
};

std::wstring maro_EscapeMsbuildProperty(std::wstring_view maro_value);
bool maro_ParseProjectProperties(std::wstring_view maro_json, maro_ProjectResult& maro_result);

maro_ProjectResult maro_BuildProject(
    const maro_ProjectRequest& maro_request,
    const Maro_CancelCheck& maro_cancelled = {},
    const Maro_ProcessOutputCallback& maro_output = {});
