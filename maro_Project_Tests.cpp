#include "maro_Project.hpp"
#include "maro_Engine.hpp"
#include "maro_Analyzer.hpp"
#include "maro_Text.hpp"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>

void maro_TestProjects(const std::function<void(bool, std::string_view)>& maro_expect)
{
    namespace maro_fs = std::filesystem;
    maro_ProjectResult maro_properties;
    maro_expect(maro_ParseProjectProperties(LR"({"Properties":{"ConfigurationType":"Application","TargetPath":"C:\\space dir\\maro.exe","LocalDebuggerCommandArguments":"\"two words\" 42","LocalDebuggerWorkingDirectory":"C:\\work","LocalDebuggerEnvironment":"MARO_ONE=ok\nMARO_TWO=2","LocalDebuggerMergeEnvironment":"false"}})", maro_properties) &&
        maro_properties.maro_executablePath == L"C:\\space dir\\maro.exe" &&
        maro_properties.maro_arguments == std::vector<std::wstring>{L"two words", L"42"} &&
        maro_properties.maro_environment[L"MARO_TWO"] == L"2" && !maro_properties.maro_inheritEnvironment,
        "project properties preserve quoted arguments, environment and working directory");
    maro_expect(maro_EscapeMsbuildProperty(L"a;b%,c") == L"a%3Bb%25%2Cc", "MSBuild property separators escaped");
    const auto maro_root = maro_fs::temp_directory_path() /
        (L"maro_ProjectTests_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    maro_fs::create_directory(maro_root);
    const auto maro_write = [&](const wchar_t* maro_name, std::string_view maro_text) {
        std::ofstream maro_file(maro_root / maro_name, std::ios::binary);
        maro_file.write(maro_text.data(), static_cast<std::streamsize>(maro_text.size()));
        if (!maro_file) throw std::runtime_error("test source write failed");
    };
    try
    {
        maro_write(L"maro_value.h", "#pragma once\nint maro_value(void);\n");
        maro_write(L"maro_value.c", "#include \"maro_value.h\"\nint maro_value(void){return 42;}\n");
        maro_write(L"maro_main.c", "#include <stdio.h>\n#include \"maro_value.h\"\nint main(void){printf(\"maro_project=%d\\n\",maro_value());return 0;}\n");
        maro_write(L"maro_test.vcxproj", R"(<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
<ItemGroup Label="ProjectConfigurations"><ProjectConfiguration Include="Release|x64"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration></ItemGroup>
<PropertyGroup Label="Globals"><ProjectGuid>{E95DCA94-134A-4D14-B523-69B158D4C4EA}</ProjectGuid><WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion></PropertyGroup>
<Import Project="$(VCTargetsPath)\Microsoft.Cpp.Default.props"/>
<PropertyGroup Label="Configuration"><ConfigurationType>Application</ConfigurationType><PlatformToolset>v145</PlatformToolset></PropertyGroup>
<Import Project="$(VCTargetsPath)\Microsoft.Cpp.props"/>
<PropertyGroup><OutDir>$(ProjectDir)maro_out\</OutDir><IntDir>$(ProjectDir)maro_obj\</IntDir><TargetName>maro_program</TargetName></PropertyGroup>
<ItemDefinitionGroup><ClCompile><AdditionalOptions>/utf-8 %(AdditionalOptions)</AdditionalOptions></ClCompile></ItemDefinitionGroup>
<ItemGroup><ClCompile Include="maro_main.c"/><ClCompile Include="maro_value.c"/></ItemGroup>
<ItemGroup><ClInclude Include="maro_value.h"/></ItemGroup>
<Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets"/></Project>)");
        maro_ProjectRequest maro_request;
        maro_request.maro_projectPath = (maro_root / L"maro_test.vcxproj").wstring();
        maro_request.maro_configuration = L"Release";
        maro_request.maro_platform = L"x64";
        const auto maro_project = maro_BuildProject(maro_request);
        maro_expect(maro_project.maro_success && !maro_project.maro_executablePath.empty(), "real MSBuild multi-file C project with local header builds");
        if (maro_project.maro_success)
        {
            Maro_ProcessRequest maro_run;
            maro_run.executable = maro_project.maro_executablePath;
            maro_run.workingDirectory = maro_project.maro_workingDirectory;
            const auto maro_result = Maro_RunProcess(maro_run);
            maro_expect(maro_result.hasExitCode && maro_result.exitCode == 0 &&
                maro_result.standardOutputUtf8.find("maro_project=42") != std::string::npos,
                "project runs linked translation units");
            {
                const std::string maro_warningSource = "#include <stdio.h>\n#pragma warning(1:4101)\n"
                    "int main(void){int maro_unused;puts(\"maro_project_input\");fflush(stdout);return getchar()=='\\n'?0:1;}\n";
                maro_write(L"maro_main.c", maro_warningSource);
                std::mutex maro_liveMutex;
                std::condition_variable maro_liveChanged;
                std::optional<Maro_ResultEnvelope> maro_initialRunning;
                std::optional<Maro_ResultEnvelope> maro_completed;
                std::wstring maro_liveOutput;
                auto maro_liveInput = std::make_shared<maro_ProcessInput>();
                Maro_Engine maro_liveEngine([&](Maro_ResultEnvelope maro_update) {
                    std::lock_guard maro_lock(maro_liveMutex);
                    if (maro_update.phase == Maro_Phase::Completed) maro_completed = std::move(maro_update);
                    else if (maro_update.phase == Maro_Phase::Running)
                    {
                        maro_liveOutput += maro_update.standardOutput;
                        if (!maro_initialRunning) maro_initialRunning = std::move(maro_update);
                    }
                    maro_liveChanged.notify_all();
                });
                Maro_SourceRequest maro_liveSource;
                maro_liveSource.sourceVersion = 1;
                maro_liveSource.sourcePath = (maro_root / L"maro_main.c").wstring();
                maro_liveSource.sourceText = Maro_Utf8ToWide(maro_warningSource);
                maro_liveSource.maro_projectPath = maro_request.maro_projectPath;
                maro_liveSource.maro_configuration = maro_request.maro_configuration;
                maro_liveSource.maro_platform = maro_request.maro_platform;
                maro_liveSource.maro_input = maro_liveInput;
                maro_liveSource.execute = true;
                maro_liveEngine.Submit(std::move(maro_liveSource));
                {
                    std::unique_lock maro_lock(maro_liveMutex);
                    maro_liveChanged.wait_for(maro_lock, std::chrono::seconds(45), [&] {
                        return maro_completed.has_value() ||
                            (maro_initialRunning && maro_liveOutput.find(L"maro_project_input") != std::wstring::npos);
                    });
                    maro_expect(maro_initialRunning && !maro_completed &&
                        maro_liveOutput.find(L"maro_project_input") != std::wstring::npos &&
                        maro_initialRunning->compilerOutput.find(L"C4101") != std::wstring::npos &&
                        std::any_of(maro_initialRunning->diagnostics.begin(), maro_initialRunning->diagnostics.end(),
                            [](const Maro_Diagnostic& maro_diagnostic) {
                                return maro_diagnostic.code == L"C4101" && maro_diagnostic.severity == Maro_Severity::Warning;
                            }), "project warnings remain in the initial Running snapshot while the program awaits input");
                }
                maro_liveInput->maro_Submit("\n");
                maro_liveInput->maro_End();
                {
                    std::unique_lock maro_lock(maro_liveMutex);
                    maro_liveChanged.wait_for(maro_lock, std::chrono::seconds(10), [&] { return maro_completed.has_value(); });
                    maro_expect(maro_completed && maro_completed->status == Maro_Status::Success,
                        "project warning regression program completes after interactive input");
                }
                maro_liveEngine.Shutdown();
            }
            maro_write(L"maro_value.h", "#error maro_header_failure\nint maro_value(void);\n");
            const auto maro_failed = maro_BuildProject(maro_request);
            maro_expect(!maro_failed.maro_success && maro_failed.maro_executablePath.empty() &&
                maro_failed.maro_buildOutput.find(L"maro_header_failure") != std::wstring::npos,
                "header build failure rejects old existing executable");
        }
        maro_write(L"maro_value.h", "#define maro_number 10\n");
        std::mutex maro_mutex;
        std::condition_variable maro_condition;
        std::optional<Maro_ResultEnvelope> maro_final;
        std::wstring maro_stream;
        bool maro_prompt = false;
        auto maro_input = std::make_shared<maro_ProcessInput>();
        Maro_Engine maro_engine([&](Maro_ResultEnvelope maro_update) {
            std::lock_guard maro_lock(maro_mutex);
            if (maro_update.phase == Maro_Phase::Completed) maro_final = std::move(maro_update);
            else
            {
                maro_stream += maro_update.standardOutput;
                if (!maro_prompt && maro_stream.find(L"number?") != std::wstring::npos)
                {
                    maro_prompt = true;
                    maro_input->maro_Submit("7\n");
                    maro_input->maro_End();
                }
            }
            maro_condition.notify_all();
        });
        Maro_SourceRequest maro_source;
        maro_source.sourceVersion = 1;
        maro_source.sourcePath = (maro_root / L"maro_main.c").wstring();
        maro_source.sourceText = L"#include <stdio.h>\n#include <windows.h>\n#include \"maro_value.h\"\nint main(void){int n=0;printf(\"number?\");if(scanf(\"%d\",&n)!=1)return 2;Sleep(3500);printf(\"answer=%d\",n+maro_number);return 0;}";
        maro_source.maro_input = maro_input;
        maro_engine.Submit(maro_source);
        {
            std::unique_lock maro_lock(maro_mutex);
            maro_condition.wait_for(maro_lock, std::chrono::seconds(30), [&] { return maro_final.has_value(); });
        }
        maro_engine.Shutdown();
        maro_expect(maro_prompt && maro_final && maro_final->status == Maro_Status::Success &&
            maro_stream.find(L"answer=17") != std::wstring::npos,
            "interactive engine streams prompt, reads stdin, includes local header and runs beyond three seconds");
        maro_expect(!maro_input->maro_IsOpen(), "completed interactive engine closes input");
    }
    catch (...)
    {
        maro_fs::remove_all(maro_root);
        throw;
    }
    if (maro_root.parent_path() == maro_fs::temp_directory_path() && maro_root.filename().wstring().starts_with(L"maro_ProjectTests_"))
        maro_fs::remove_all(maro_root);
}
