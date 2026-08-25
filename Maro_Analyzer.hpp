#pragma once

#include "Maro_Models.hpp"
#include "Maro_Process.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

enum class Maro_ToolchainKind
{
    None,
    Clang,
    Msvc
};

struct Maro_ToolchainInfo
{
    Maro_ToolchainKind kind = Maro_ToolchainKind::None;
    std::filesystem::path compilerPath;
    std::filesystem::path visualStudioRoot;
    std::filesystem::path msvcRoot;
    std::filesystem::path windowsSdkRoot;
    std::wstring windowsSdkVersion;
    std::map<std::wstring, std::wstring, std::less<>> environment;
    std::wstring name;
    std::wstring version;
    bool fallback = false;
};

struct Maro_AnalysisResult
{
    bool succeeded = false;
    bool cancelled = false;
    bool timedOut = false;
    bool limitExceeded = false;
    bool processStartFailed = false;
    bool resourceLimitsApplied = false;
    std::wstring compilerOutput;
    std::vector<Maro_Diagnostic> diagnostics;
};

struct Maro_CompilationResult : Maro_AnalysisResult
{
    std::filesystem::path executablePath;
};

bool Maro_HasMain(std::wstring_view source);
Maro_GeneratedSource Maro_BuildGeneratedSource(const Maro_SourceRequest& request);

Maro_SourcePosition Maro_MapGeneratedPosition(
    const Maro_GeneratedSource& generated,
    Maro_SourcePosition position,
    bool& isGeneratedOnly);

std::vector<Maro_Diagnostic> Maro_ParseCompilerDiagnostics(
    std::wstring_view compilerOutput,
    const Maro_SourceRequest& request,
    const Maro_GeneratedSource& generated,
    std::wstring_view analyzerName,
    std::wstring_view analyzerVersion);

Maro_ToolchainInfo Maro_DetectToolchain();

Maro_AnalysisResult Maro_AnalyzeSource(
    const Maro_ToolchainInfo& toolchain,
    const Maro_SourceRequest& request,
    const Maro_GeneratedSource& generated,
    const std::filesystem::path& workingDirectory,
    const Maro_ExecutionLimits& limits,
    const Maro_CancelCheck& cancelled);

Maro_CompilationResult Maro_CompileSource(
    const Maro_ToolchainInfo& toolchain,
    const Maro_SourceRequest& request,
    const Maro_GeneratedSource& generated,
    const std::filesystem::path& workingDirectory,
    const Maro_ExecutionLimits& limits,
    const Maro_CancelCheck& cancelled);
