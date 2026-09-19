#include "maro_Project.hpp"
#include "maro_Analyzer.hpp"
#include "maro_Text.hpp"

#include <windows.h>
#include <shellapi.h>
#include <cwctype>
#include <filesystem>
#include <sstream>

namespace
{
bool maro_JsonString(std::wstring_view maro_json, std::size_t& maro_index, std::wstring& maro_value)
{
    if (maro_index >= maro_json.size() || maro_json[maro_index++] != L'"') return false;
    maro_value.clear();
    while (maro_index < maro_json.size())
    {
        wchar_t maro_char = maro_json[maro_index++];
        if (maro_char == L'"') return true;
        if (maro_char < 32) return false;
        if (maro_char == L'\\')
        {
            if (maro_index >= maro_json.size()) return false;
            maro_char = maro_json[maro_index++];
            switch (maro_char)
            {
            case L'"': case L'\\': case L'/': break;
            case L'n': maro_char = L'\n'; break;
            case L'r': maro_char = L'\r'; break;
            case L't': maro_char = L'\t'; break;
            case L'b': maro_char = L'\b'; break;
            case L'f': maro_char = L'\f'; break;
            case L'u':
                {
                    unsigned maro_valueCode = 0;
                    for (int maro_digit = 0; maro_digit < 4; ++maro_digit)
                    {
                        if (maro_index >= maro_json.size()) return false;
                        const auto maro_hex = std::towlower(maro_json[maro_index++]);
                        if (!((maro_hex >= L'0' && maro_hex <= L'9') || (maro_hex >= L'a' && maro_hex <= L'f'))) return false;
                        maro_valueCode = maro_valueCode * 16 + (maro_hex <= L'9' ? maro_hex - L'0' : maro_hex - L'a' + 10);
                    }
                    maro_char = static_cast<wchar_t>(maro_valueCode);
                }
                break;
            default: return false;
            }
        }
        if (maro_char == 0) return false;
        maro_value += maro_char;
    }
    return false;
}

std::wstring maro_Property(std::wstring_view maro_json, std::wstring_view maro_key)
{
    for (std::size_t maro_index = 0; maro_index < maro_json.size();)
    {
        if (maro_json[maro_index] != L'"') { ++maro_index; continue; }
        std::wstring maro_name;
        if (!maro_JsonString(maro_json, maro_index, maro_name)) return {};
        while (maro_index < maro_json.size() && std::iswspace(maro_json[maro_index])) ++maro_index;
        if (maro_index >= maro_json.size() || maro_json[maro_index] != L':') continue;
        ++maro_index;
        while (maro_index < maro_json.size() && std::iswspace(maro_json[maro_index])) ++maro_index;
        if (maro_index >= maro_json.size() || maro_json[maro_index] != L'"') continue;
        std::wstring maro_value;
        if (!maro_JsonString(maro_json, maro_index, maro_value)) return {};
        if (maro_name == maro_key) return maro_value;
    }
    return {};
}
}

std::wstring maro_EscapeMsbuildProperty(std::wstring_view maro_value)
{
    std::wstring maro_result;
    constexpr wchar_t maro_digits[] = L"0123456789ABCDEF";
    for (const auto maro_char : maro_value)
    {
        if (std::wstring_view(L"%;,$@'()*?\"").find(maro_char) != std::wstring_view::npos)
        {
            maro_result += L'%';
            maro_result += maro_digits[(maro_char >> 4) & 15];
            maro_result += maro_digits[maro_char & 15];
        }
        else maro_result += maro_char;
    }
    return maro_result;
}

bool maro_ParseProjectProperties(std::wstring_view maro_json, maro_ProjectResult& maro_result)
{
    maro_result.maro_configurationType = maro_Property(maro_json, L"ConfigurationType");
    maro_result.maro_executablePath = maro_Property(maro_json, L"TargetPath");
    maro_result.maro_workingDirectory = maro_Property(maro_json, L"LocalDebuggerWorkingDirectory");
    maro_result.maro_arguments.clear();
    maro_result.maro_environment.clear();
    const auto maro_args = maro_Property(maro_json, L"LocalDebuggerCommandArguments");
    if (!maro_args.empty())
    {
        int maro_count = 0;
        const auto maro_argv = CommandLineToArgvW((L"maro.exe " + maro_args).c_str(), &maro_count);
        if (!maro_argv) return false;
        for (int maro_index = 1; maro_index < maro_count; ++maro_index) maro_result.maro_arguments.emplace_back(maro_argv[maro_index]);
        LocalFree(maro_argv);
    }
    std::wistringstream maro_environment(maro_Property(maro_json, L"LocalDebuggerEnvironment"));
    std::wstring maro_line;
    while (std::getline(maro_environment, maro_line))
    {
        if (!maro_line.empty() && maro_line.back() == L'\r') maro_line.pop_back();
        const auto maro_equal = maro_line.find(L'=');
        if (maro_equal != std::wstring::npos && maro_equal > 0)
            maro_result.maro_environment[maro_line.substr(0, maro_equal)] = maro_line.substr(maro_equal + 1);
    }
    maro_result.maro_inheritEnvironment = maro_Property(maro_json, L"LocalDebuggerMergeEnvironment") != L"false";
    return !maro_result.maro_configurationType.empty() && !maro_result.maro_executablePath.empty();
}

maro_ProjectResult maro_BuildProject(const maro_ProjectRequest& maro_request,
    const Maro_CancelCheck& maro_cancelled, const Maro_ProcessOutputCallback& maro_output)
{
    namespace maro_fs = std::filesystem;
    maro_ProjectResult maro_result;
    const maro_fs::path maro_path(maro_request.maro_projectPath);
    std::error_code maro_error;
    if (!maro_path.is_absolute() || _wcsicmp(maro_path.extension().c_str(), L".vcxproj") != 0 ||
        !maro_fs::is_regular_file(maro_path, maro_error) || maro_request.maro_configuration.empty() || maro_request.maro_platform.empty())
    {
        maro_result.maro_message = L"유효한 .vcxproj와 구성·플랫폼이 필요합니다.";
        return maro_result;
    }
    maro_fs::path maro_msbuild(maro_request.maro_msbuildPath);
    if (maro_msbuild.empty()) maro_msbuild = Maro_DetectToolchain().visualStudioRoot / L"MSBuild/Current/Bin/MSBuild.exe";
    if (!maro_fs::is_regular_file(maro_msbuild, maro_error))
    {
        maro_result.maro_message = L"Visual Studio MSBuild를 찾지 못했습니다.";
        return maro_result;
    }
    Maro_ProcessRequest maro_build;
    maro_build.maro_background = maro_request.maro_background;
    maro_build.executable = maro_msbuild.wstring();
    maro_build.workingDirectory = maro_path.parent_path().wstring();
    maro_build.arguments = {maro_path.wstring(), L"/nologo", L"/nr:false", L"/m:2", L"/v:minimal",
        L"/p:Configuration=" + maro_EscapeMsbuildProperty(maro_request.maro_configuration),
        L"/p:Platform=" + maro_EscapeMsbuildProperty(maro_request.maro_platform)};
    if (!maro_request.maro_solutionPath.empty())
    {
        const maro_fs::path maro_solution(maro_request.maro_solutionPath);
        maro_build.arguments.push_back(L"/p:SolutionDir=" + maro_EscapeMsbuildProperty(maro_solution.parent_path().wstring() + L"\\"));
        maro_build.arguments.push_back(L"/p:SolutionPath=" + maro_EscapeMsbuildProperty(maro_solution.wstring()));
    }
    maro_build.environmentOverrides[L"DOTNET_CLI_UI_LANGUAGE"] = L"en-US";
    maro_build.limits = maro_request.maro_limits;
    maro_build.maro_rollingOutput = true;
    maro_build.arguments.push_back(L"/t:Build");
    maro_result.maro_process = Maro_RunProcess(maro_build, maro_cancelled, maro_output);
    maro_OutputDecoder maro_stdoutDecoder;
    maro_OutputDecoder maro_stderrDecoder;
    maro_result.maro_buildOutput = maro_stdoutDecoder.maro_Decode(maro_result.maro_process.standardOutputUtf8, true) +
        maro_stderrDecoder.maro_Decode(maro_result.maro_process.standardErrorUtf8, true);
    if (maro_result.maro_process.termination != Maro_ProcessTermination::Exited ||
        !maro_result.maro_process.hasExitCode || maro_result.maro_process.exitCode != 0 || (maro_cancelled && maro_cancelled()))
    {
        maro_result.maro_message = L"프로젝트 빌드를 완료하지 못했습니다. 진단을 확인해 주세요.";
        return maro_result;
    }
    maro_build.arguments.back() = L"/getProperty:TargetPath,ConfigurationType,LocalDebuggerWorkingDirectory,LocalDebuggerCommandArguments,LocalDebuggerEnvironment,LocalDebuggerMergeEnvironment";
    maro_build.limits.wallMilliseconds = 60'000;
    const auto maro_properties = Maro_RunProcess(maro_build, maro_cancelled);
    maro_OutputDecoder maro_propertiesDecoder;
    if (maro_properties.termination != Maro_ProcessTermination::Exited || !maro_properties.hasExitCode ||
        maro_properties.exitCode != 0 || !maro_ParseProjectProperties(
            maro_propertiesDecoder.maro_Decode(maro_properties.standardOutputUtf8, true), maro_result))
    {
        maro_result.maro_message = L"빌드는 끝났지만 실행 경로를 확인하지 못했습니다. MSBuild 17.8 이상이 필요합니다.";
        maro_result.maro_executablePath.clear();
        return maro_result;
    }
    if (maro_result.maro_workingDirectory.empty()) maro_result.maro_workingDirectory = maro_path.parent_path().wstring();
    else if (maro_fs::path(maro_result.maro_workingDirectory).is_relative())
        maro_result.maro_workingDirectory = (maro_path.parent_path() / maro_result.maro_workingDirectory).wstring();
    if (maro_result.maro_configurationType != L"Application")
    {
        maro_result.maro_executablePath.clear();
        maro_result.maro_success = true;
        maro_result.maro_message = L"라이브러리 빌드 완료. 실행하려면 실행 프로젝트를 선택하세요.";
        return maro_result;
    }
    const maro_fs::path maro_executable(maro_result.maro_executablePath);
    if (_wcsicmp(maro_executable.extension().c_str(), L".exe") != 0 || !maro_executable.is_absolute() ||
        !maro_fs::is_regular_file(maro_executable, maro_error) || !maro_fs::is_directory(maro_result.maro_workingDirectory, maro_error))
    {
        maro_result.maro_message = L"프로젝트 실행 파일 또는 작업 폴더가 없습니다.";
        maro_result.maro_executablePath.clear();
        return maro_result;
    }
    maro_result.maro_success = true;
    maro_result.maro_message = L"프로젝트 빌드 완료.";
    return maro_result;
}
