#include "Maro_Analyzer.hpp"

#include "Maro_Text.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>

namespace
{
namespace fs = std::filesystem;

std::wstring Maro_GetEnvironment(std::wstring_view name)
{
    const DWORD required = GetEnvironmentVariableW(std::wstring(name).c_str(), nullptr, 0);
    if (required == 0)
    {
        return {};
    }
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        std::wstring(name).c_str(), value.data(), required);
    if (written == 0 || written >= required)
    {
        return {};
    }
    value.resize(written);
    return value;
}

fs::path Maro_SearchPath(std::wstring_view fileName)
{
    // Deliberately do not use SearchPathW: it searches the current working
    // directory before PATH, which would let a project-local fake compiler run.
    const std::wstring pathValue = Maro_GetEnvironment(L"PATH");
    std::size_t start = 0;
    while (start <= pathValue.size())
    {
        const std::size_t end = pathValue.find(L';', start);
        std::wstring directory = pathValue.substr(
            start,
            (end == std::wstring::npos ? pathValue.size() : end) - start);
        if (directory.size() >= 2 && directory.front() == L'"' && directory.back() == L'"')
        {
            directory = directory.substr(1, directory.size() - 2);
        }
        const fs::path directoryPath(directory);
        if (!directory.empty() && directoryPath.is_absolute())
        {
            const fs::path candidate = directoryPath / fileName;
            std::error_code error;
            if (fs::is_regular_file(candidate, error))
            {
                return candidate;
            }
        }
        if (end == std::wstring::npos)
        {
            break;
        }
        start = end + 1;
    }
    return {};
}

bool Maro_IsRegularFile(const fs::path& path)
{
    std::error_code error;
    return fs::is_regular_file(path, error);
}

std::vector<fs::path> Maro_ProgramFilesRoots()
{
    std::vector<fs::path> result;
    for (const std::wstring_view variable : {L"ProgramW6432", L"ProgramFiles", L"ProgramFiles(x86)"})
    {
        const std::wstring value = Maro_GetEnvironment(variable);
        if (!value.empty() && std::find(result.begin(), result.end(), fs::path(value)) == result.end())
        {
            result.emplace_back(value);
        }
    }
    return result;
}

struct Maro_MsvcDiscovery
{
    fs::path visualStudioRoot;
    fs::path msvcRoot;
    fs::path compiler;
};

Maro_MsvcDiscovery Maro_FindMsvc()
{
    const fs::path onPath = Maro_SearchPath(L"cl.exe");

    std::vector<fs::path> visualStudioRoots;
    const std::wstring configuredRoot = Maro_GetEnvironment(L"VSINSTALLDIR");
    if (!configuredRoot.empty())
    {
        visualStudioRoots.emplace_back(configuredRoot);
    }

    for (const fs::path& programFiles : Maro_ProgramFilesRoots())
    {
        const fs::path base = programFiles / L"Microsoft Visual Studio";
        std::error_code error;
        for (fs::directory_iterator version(base, fs::directory_options::skip_permission_denied, error), end;
             !error && version != end;
             version.increment(error))
        {
            if (!version->is_directory(error))
            {
                continue;
            }
            std::error_code editionError;
            for (fs::directory_iterator edition(
                     version->path(),
                     fs::directory_options::skip_permission_denied,
                     editionError), editionEnd;
                 !editionError && edition != editionEnd;
                 edition.increment(editionError))
            {
                if (edition->is_directory(editionError))
                {
                    visualStudioRoots.push_back(edition->path());
                }
            }
        }
    }

    Maro_MsvcDiscovery best;
    for (const fs::path& visualStudioRoot : visualStudioRoots)
    {
        const fs::path tools = visualStudioRoot / L"VC" / L"Tools" / L"MSVC";
        std::error_code error;
        for (fs::directory_iterator entry(tools, fs::directory_options::skip_permission_denied, error), end;
             !error && entry != end;
             entry.increment(error))
        {
            if (!entry->is_directory(error))
            {
                continue;
            }
            const fs::path compiler = entry->path() / L"bin" / L"Hostx64" / L"x64" / L"cl.exe";
            if (Maro_IsRegularFile(compiler) &&
                (best.msvcRoot.empty() || entry->path().filename().wstring() > best.msvcRoot.filename().wstring()))
            {
                best.visualStudioRoot = visualStudioRoot;
                best.msvcRoot = entry->path();
                best.compiler = compiler;
            }
        }
    }

    if (best.compiler.empty() && !onPath.empty())
    {
        best.compiler = onPath;
    }
    return best;
}

struct Maro_SdkDiscovery
{
    fs::path root;
    std::wstring version;
};

Maro_SdkDiscovery Maro_FindWindowsSdk()
{
    Maro_SdkDiscovery best;
    for (const fs::path& programFiles : Maro_ProgramFilesRoots())
    {
        const fs::path root = programFiles / L"Windows Kits" / L"10";
        const fs::path includes = root / L"Include";
        std::error_code error;
        for (fs::directory_iterator entry(includes, fs::directory_options::skip_permission_denied, error), end;
             !error && entry != end;
             entry.increment(error))
        {
            if (!entry->is_directory(error))
            {
                continue;
            }
            const std::wstring version = entry->path().filename().wstring();
            if (Maro_IsRegularFile(entry->path() / L"ucrt" / L"stdio.h") && version > best.version)
            {
                best.root = root;
                best.version = version;
            }
        }
    }
    return best;
}

std::wstring Maro_JoinPaths(const std::vector<fs::path>& paths, std::wstring_view existing)
{
    std::wstring result;
    for (const fs::path& path : paths)
    {
        if (path.empty())
        {
            continue;
        }
        if (!result.empty())
        {
            result.push_back(L';');
        }
        result += path.wstring();
    }
    if (!existing.empty())
    {
        if (!result.empty())
        {
            result.push_back(L';');
        }
        result += existing;
    }
    return result;
}

void Maro_AddBuildEnvironment(
    Maro_ToolchainInfo& info,
    const Maro_MsvcDiscovery& msvc,
    const Maro_SdkDiscovery& sdk)
{
    info.visualStudioRoot = msvc.visualStudioRoot;
    info.msvcRoot = msvc.msvcRoot;
    info.windowsSdkRoot = sdk.root;
    info.windowsSdkVersion = sdk.version;

    std::vector<fs::path> includePaths;
    std::vector<fs::path> libraryPaths;
    std::vector<fs::path> executablePaths;
    if (!msvc.msvcRoot.empty())
    {
        includePaths.push_back(msvc.msvcRoot / L"include");
        libraryPaths.push_back(msvc.msvcRoot / L"lib" / L"x64");
        executablePaths.push_back(msvc.msvcRoot / L"bin" / L"Hostx64" / L"x64");
    }
    if (!msvc.visualStudioRoot.empty())
    {
        executablePaths.push_back(msvc.visualStudioRoot / L"Common7" / L"IDE");
    }
    if (!sdk.root.empty() && !sdk.version.empty())
    {
        const fs::path includeRoot = sdk.root / L"Include" / sdk.version;
        includePaths.push_back(includeRoot / L"ucrt");
        includePaths.push_back(includeRoot / L"shared");
        includePaths.push_back(includeRoot / L"um");
        includePaths.push_back(includeRoot / L"winrt");
        libraryPaths.push_back(sdk.root / L"Lib" / sdk.version / L"ucrt" / L"x64");
        libraryPaths.push_back(sdk.root / L"Lib" / sdk.version / L"um" / L"x64");
        executablePaths.push_back(sdk.root / L"bin" / sdk.version / L"x64");
    }

    if (!includePaths.empty())
    {
        info.environment[L"INCLUDE"] = Maro_JoinPaths(includePaths, Maro_GetEnvironment(L"INCLUDE"));
    }
    if (!libraryPaths.empty())
    {
        info.environment[L"LIB"] = Maro_JoinPaths(libraryPaths, Maro_GetEnvironment(L"LIB"));
    }
    if (!executablePaths.empty())
    {
        info.environment[L"PATH"] = Maro_JoinPaths(executablePaths, Maro_GetEnvironment(L"PATH"));
    }
}

fs::path Maro_FindClang()
{
    fs::path compiler;
    for (const fs::path& root : Maro_ProgramFilesRoots())
    {
        compiler = root / L"LLVM" / L"bin" / L"clang.exe";
        if (Maro_IsRegularFile(compiler))
        {
            return compiler;
        }
    }
    return Maro_SearchPath(L"clang.exe");
}

std::wstring Maro_FirstLine(std::wstring_view text)
{
    const std::size_t end = text.find_first_of(L"\r\n");
    return std::wstring(text.substr(0, end));
}

std::wstring Maro_Lower(std::wstring_view text)
{
    std::wstring value(text);
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

Maro_Severity Maro_ParseSeverity(std::wstring_view value)
{
    const std::wstring lower = Maro_Lower(value);
    if (lower.find(L"fatal") != std::wstring::npos)
    {
        return Maro_Severity::Fatal;
    }
    if (lower.find(L"error") != std::wstring::npos)
    {
        return Maro_Severity::Error;
    }
    if (lower.find(L"warning") != std::wstring::npos)
    {
        return Maro_Severity::Warning;
    }
    return Maro_Severity::Info;
}

std::wstring Maro_DiagnosticCode(
    std::wstring_view message,
    Maro_Severity severity,
    Maro_Language language)
{
    const std::wstring lower = Maro_Lower(message);
    const std::wstring prefix = language == Maro_Language::C17 ? L"C-" : L"CPP-";
    if (lower.find(L"expected ';'") != std::wstring::npos || lower.find(L"c2143") != std::wstring::npos)
    {
        return prefix + L"SYN-1001";
    }
    if (lower.find(L"undeclared identifier") != std::wstring::npos || lower.find(L"c2065") != std::wstring::npos)
    {
        return prefix + L"NAME-1001";
    }
    if (lower.find(L"lnk") != std::wstring::npos ||
        lower.find(L"undefined reference") != std::wstring::npos ||
        lower.find(L"unresolved external") != std::wstring::npos)
    {
        return prefix + L"LINK-1001";
    }
    if (lower.find(L"incompatible") != std::wstring::npos || lower.find(L"cannot convert") != std::wstring::npos)
    {
        return prefix + L"TYPE-1001";
    }
    return prefix + (severity == Maro_Severity::Warning ? L"WARN-1001" : L"COMP-1001");
}

std::wstring Maro_FriendlyMessage(std::wstring_view original)
{
    const std::wstring lower = Maro_Lower(original);
    if (lower.find(L"expected ';'") != std::wstring::npos)
    {
        return L"문장 끝에 ';'가 필요합니다.";
    }
    if (lower.find(L"c2143") != std::wstring::npos)
    {
        return L"문법을 완성하는 기호가 필요합니다. 이 위치 앞뒤의 ';', 괄호 또는 중괄호를 확인하세요.";
    }
    if (lower.find(L"undeclared identifier") != std::wstring::npos || lower.find(L"c2065") != std::wstring::npos)
    {
        return L"사용한 이름의 선언을 찾을 수 없습니다.";
    }
    if (lower.find(L"main") != std::wstring::npos &&
        (lower.find(L"unresolved") != std::wstring::npos ||
         lower.find(L"undefined") != std::wstring::npos ||
         lower.find(L"lnk1561") != std::wstring::npos))
    {
        return L"Program 모드에는 main() 함수가 필요합니다.";
    }
    if (lower.find(L"unused") != std::wstring::npos || lower.find(L"c4101") != std::wstring::npos)
    {
        return L"선언하거나 계산한 값이 사용되지 않았습니다.";
    }
    if (lower.find(L"lnk") != std::wstring::npos || lower.find(L"linker") != std::wstring::npos)
    {
        return L"컴파일은 진행됐지만 실행 파일을 연결하지 못했습니다.";
    }
    return L"컴파일러가 이 위치에서 문제를 보고했습니다.";
}

std::wstring Maro_UnescapeFixIt(std::wstring_view escaped)
{
    std::string bytes;
    for (std::size_t index = 0; index < escaped.size(); ++index)
    {
        if (escaped[index] != L'\\' || index + 1 >= escaped.size())
        {
            std::size_t codeUnits = 1;
            if (escaped[index] >= 0xd800 && escaped[index] <= 0xdbff &&
                index + 1 < escaped.size() && escaped[index + 1] >= 0xdc00 && escaped[index + 1] <= 0xdfff)
            {
                codeUnits = 2;
            }
            bytes += Maro_WideToUtf8(escaped.substr(index, codeUnits));
            index += codeUnits - 1;
            continue;
        }
        const wchar_t next = escaped[++index];
        switch (next)
        {
        case L'n': bytes.push_back('\n'); break;
        case L'r': bytes.push_back('\r'); break;
        case L't': bytes.push_back('\t'); break;
        case L'\\': bytes.push_back('\\'); break;
        case L'"': bytes.push_back('"'); break;
        default:
            if (next >= L'0' && next <= L'7')
            {
                unsigned value = static_cast<unsigned>(next - L'0');
                for (int count = 0; count < 2 && index + 1 < escaped.size(); ++count)
                {
                    const wchar_t digit = escaped[index + 1];
                    if (digit < L'0' || digit > L'7')
                    {
                        break;
                    }
                    ++index;
                    value = value * 8 + static_cast<unsigned>(digit - L'0');
                }
                bytes.push_back(static_cast<char>(value & 0xffu));
            }
            else
            {
                bytes += Maro_WideToUtf8(std::wstring_view(&next, 1));
            }
            break;
        }
    }
    return Maro_Utf8ToWide(bytes);
}

std::vector<std::wstring> Maro_SplitLines(std::wstring_view text)
{
    std::vector<std::wstring> lines;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const std::size_t end = text.find(L'\n', start);
        if (end == std::wstring_view::npos)
        {
            lines.emplace_back(text.substr(start));
            break;
        }
        lines.emplace_back(text.substr(start, end - start));
        start = end + 1;
        if (start == text.size())
        {
            lines.emplace_back();
            break;
        }
    }
    if (lines.empty())
    {
        lines.emplace_back();
    }
    return lines;
}

std::wstring_view Maro_TrimLeft(std::wstring_view value)
{
    while (!value.empty() && std::iswspace(value.front()))
    {
        value.remove_prefix(1);
    }
    return value;
}

bool Maro_IsLeadingCommentOrBlank(std::wstring_view line, bool& inBlockComment)
{
    line = Maro_TrimLeft(line);
    for (;;)
    {
        if (inBlockComment)
        {
            const std::size_t end = line.find(L"*/");
            if (end == std::wstring_view::npos)
            {
                return true;
            }
            line.remove_prefix(end + 2);
            line = Maro_TrimLeft(line);
            inBlockComment = false;
            continue;
        }
        if (line.empty() || line.starts_with(L"//"))
        {
            return true;
        }
        if (line.starts_with(L"/*"))
        {
            inBlockComment = true;
            line.remove_prefix(2);
            continue;
        }
        return false;
    }
}

std::wstring Maro_CombineProcessOutput(const Maro_ProcessResult& process)
{
    std::wstring output = Maro_Utf8ToWide(process.standardOutputUtf8);
    const std::wstring error = Maro_Utf8ToWide(process.standardErrorUtf8);
    if (!output.empty() && !error.empty() && output.back() != L'\n')
    {
        output.push_back(L'\n');
    }
    output += error;
    return output;
}

bool Maro_WriteSourceFile(const fs::path& path, std::wstring_view source)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        return false;
    }
    const std::string utf8 = Maro_WideToUtf8(source);
    file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    return static_cast<bool>(file);
}

Maro_ProcessLimits Maro_CompilerProcessLimits(
    const Maro_ExecutionLimits& limits,
    bool syntaxOnly)
{
    Maro_ProcessLimits result;
    result.wallMilliseconds = syntaxOnly
        ? limits.analysisWallMilliseconds
        : limits.compileWallMilliseconds;
    result.cpuMilliseconds = result.wallMilliseconds;
    result.memoryBytes = limits.compileMemoryBytes;
    result.activeProcessLimit = 8;
    result.stdoutBytes = limits.compilerOutputBytes;
    result.stderrBytes = limits.compilerOutputBytes;
    return result;
}

std::vector<std::wstring> Maro_CompilerArguments(
    const Maro_ToolchainInfo& toolchain,
    const Maro_SourceRequest& request,
    const fs::path& sourcePath,
    const fs::path& executablePath,
    const fs::path& objectPath,
    bool syntaxOnly)
{
    std::vector<std::wstring> arguments;
    if (toolchain.kind == Maro_ToolchainKind::Clang)
    {
        arguments = {
            L"-x",
            request.language == Maro_Language::C17 ? L"c" : L"c++",
            request.language == Maro_Language::C17 ? L"-std=c17" : L"-std=c++20",
            L"-Wall",
            L"-Wextra",
            L"-Wpedantic",
            L"-fno-color-diagnostics",
            L"-fdiagnostics-format=msvc",
            L"-fdiagnostics-parseable-fixits",
            L"-ferror-limit=50"
        };
        if (!request.sourcePath.empty())
        {
            const fs::path includeDirectory = fs::path(request.sourcePath).parent_path();
            if (!includeDirectory.empty())
            {
                arguments.push_back(L"-I");
                arguments.push_back(includeDirectory.wstring());
            }
        }
        if (syntaxOnly)
        {
            arguments.push_back(L"-fsyntax-only");
        }
        else
        {
            arguments.push_back(L"-O0");
        }
        arguments.push_back(sourcePath.wstring());
        if (!syntaxOnly)
        {
            arguments.push_back(L"-o");
            arguments.push_back(executablePath.wstring());
        }
    }
    else
    {
        arguments = {
            L"/nologo",
            L"/utf-8",
            L"/diagnostics:column",
            L"/W4",
            request.language == Maro_Language::C17 ? L"/TC" : L"/TP",
            request.language == Maro_Language::C17 ? L"/std:c17" : L"/std:c++20"
        };
        if (request.language == Maro_Language::Cpp20)
        {
            arguments.push_back(L"/EHsc");
            arguments.push_back(L"/permissive-");
        }
        if (!request.sourcePath.empty())
        {
            const fs::path includeDirectory = fs::path(request.sourcePath).parent_path();
            if (!includeDirectory.empty())
            {
                arguments.push_back(L"/I");
                arguments.push_back(includeDirectory.wstring());
            }
        }
        if (syntaxOnly)
        {
            arguments.push_back(L"/Zs");
        }
        else
        {
            arguments.push_back(L"/Od");
            arguments.push_back(L"/Fe:" + executablePath.wstring());
            arguments.push_back(L"/Fo:" + objectPath.wstring());
        }
        arguments.push_back(sourcePath.wstring());
    }
    return arguments;
}

Maro_AnalysisResult Maro_RunCompiler(
    const Maro_ToolchainInfo& toolchain,
    const Maro_SourceRequest& request,
    const Maro_GeneratedSource& generated,
    const fs::path& workingDirectory,
    const Maro_ExecutionLimits& limits,
    const Maro_CancelCheck& cancelled,
    bool syntaxOnly,
    fs::path& executablePath)
{
    Maro_AnalysisResult result;
    const fs::path sourcePath = workingDirectory /
        (request.language == Maro_Language::C17 ? L"Maro_UserSource.c" : L"Maro_UserSource.cpp");
    executablePath = workingDirectory / L"Maro_UserProgram.exe";
    const fs::path objectPath = workingDirectory / L"Maro_UserProgram.obj";

    const std::string utf8 = Maro_WideToUtf8(generated.text);
    if (utf8.size() > limits.sourceBytes)
    {
        result.limitExceeded = true;
        Maro_Diagnostic diagnostic;
        diagnostic.sourceVersion = request.sourceVersion;
        diagnostic.findingId = L"Maro_source_limit";
        diagnostic.code = L"SAFE-1001";
        diagnostic.analyzer = L"CLive_Maro";
        diagnostic.severity = Maro_Severity::Error;
        diagnostic.evidence = Maro_Evidence::StaticAnalysis;
        diagnostic.friendlyMessage = L"소스 크기가 CLive_Maro의 분석 제한을 초과했습니다.";
        result.diagnostics.push_back(std::move(diagnostic));
        return result;
    }
    if (!Maro_WriteSourceFile(sourcePath, generated.text))
    {
        Maro_Diagnostic diagnostic;
        diagnostic.sourceVersion = request.sourceVersion;
        diagnostic.findingId = L"Maro_source_write";
        diagnostic.code = L"SAFE-1001";
        diagnostic.analyzer = L"CLive_Maro";
        diagnostic.severity = Maro_Severity::Error;
        diagnostic.evidence = Maro_Evidence::Unknown;
        diagnostic.friendlyMessage = L"격리 작업 폴더에 임시 소스를 만들지 못했습니다.";
        result.diagnostics.push_back(std::move(diagnostic));
        return result;
    }

    Maro_ProcessRequest processRequest;
    processRequest.executable = toolchain.compilerPath.wstring();
    processRequest.arguments = Maro_CompilerArguments(
        toolchain, request, sourcePath, executablePath, objectPath, syntaxOnly);
    processRequest.workingDirectory = workingDirectory.wstring();
    processRequest.environmentOverrides = toolchain.environment;
    processRequest.limits = Maro_CompilerProcessLimits(limits, syntaxOnly);

    const Maro_ProcessResult process = Maro_RunProcess(processRequest, cancelled);
    result.resourceLimitsApplied = process.jobObjectApplied;
    result.processStartFailed = process.termination == Maro_ProcessTermination::StartFailed ||
        process.termination == Maro_ProcessTermination::InternalError;
    result.compilerOutput = Maro_CombineProcessOutput(process);
    result.diagnostics = Maro_ParseCompilerDiagnostics(
        result.compilerOutput,
        request,
        generated,
        toolchain.name,
        toolchain.version);
    result.cancelled = process.termination == Maro_ProcessTermination::Cancelled;
    result.timedOut = process.termination == Maro_ProcessTermination::WallTimedOut ||
        process.termination == Maro_ProcessTermination::CpuTimedOut;
    result.limitExceeded = process.termination == Maro_ProcessTermination::OutputLimit ||
        process.termination == Maro_ProcessTermination::MemoryLimit ||
        process.termination == Maro_ProcessTermination::ProcessLimit;
    result.succeeded = process.termination == Maro_ProcessTermination::Exited &&
        process.hasExitCode && process.exitCode == 0;

    if (!result.succeeded && !result.cancelled && !result.timedOut &&
        !result.limitExceeded && result.diagnostics.empty())
    {
        Maro_Diagnostic diagnostic;
        diagnostic.sourceVersion = request.sourceVersion;
        diagnostic.findingId = L"Maro_compiler_failure";
        diagnostic.code = request.language == Maro_Language::C17 ? L"C-COMP-1001" : L"CPP-COMP-1001";
        diagnostic.analyzer = toolchain.name;
        diagnostic.analyzerVersion = toolchain.version;
        diagnostic.severity = Maro_Severity::Error;
        diagnostic.evidence = Maro_Evidence::StaticAnalysis;
        diagnostic.friendlyMessage = process.termination == Maro_ProcessTermination::StartFailed
            ? L"컴파일러 프로세스를 시작하지 못했습니다."
            : L"컴파일러가 실행 파일을 만들지 못했습니다.";
        diagnostic.originalDiagnostic = result.compilerOutput;
        result.diagnostics.push_back(std::move(diagnostic));
    }
    return result;
}
} // namespace

bool Maro_HasMain(std::wstring_view source)
{
    enum class State { Normal, LineComment, BlockComment, String, Character, Preprocessor };
    State state = State::Normal;
    bool beginningOfLine = true;
    for (std::size_t index = 0; index < source.size(); ++index)
    {
        const wchar_t character = source[index];
        const wchar_t next = index + 1 < source.size() ? source[index + 1] : L'\0';
        if (state == State::LineComment || state == State::Preprocessor)
        {
            if (character == L'\n')
            {
                beginningOfLine = true;
                std::size_t previous = index;
                while (previous > 0 && source[previous - 1] == L'\r')
                {
                    --previous;
                }
                const bool continuedDirective = state == State::Preprocessor &&
                    previous > 0 && source[previous - 1] == L'\\';
                if (!continuedDirective)
                {
                    state = State::Normal;
                }
            }
            continue;
        }
        if (state == State::BlockComment)
        {
            if (character == L'*' && next == L'/')
            {
                ++index;
                state = State::Normal;
            }
            if (character == L'\n')
            {
                beginningOfLine = true;
            }
            continue;
        }
        if (state == State::String || state == State::Character)
        {
            if (character == L'\\')
            {
                ++index;
            }
            else if ((state == State::String && character == L'"') ||
                     (state == State::Character && character == L'\''))
            {
                state = State::Normal;
            }
            continue;
        }

        if (character == L'\n')
        {
            beginningOfLine = true;
            continue;
        }
        if (beginningOfLine && std::iswspace(character))
        {
            continue;
        }
        if (beginningOfLine && character == L'#')
        {
            state = State::Preprocessor;
            continue;
        }
        beginningOfLine = false;
        if (character == L'/' && next == L'/')
        {
            ++index;
            state = State::LineComment;
            continue;
        }
        if (character == L'/' && next == L'*')
        {
            ++index;
            state = State::BlockComment;
            continue;
        }
        if (character == L'"')
        {
            state = State::String;
            continue;
        }
        if (character == L'\'')
        {
            state = State::Character;
            continue;
        }
        if (std::iswalpha(character) || character == L'_')
        {
            const std::size_t start = index;
            while (index + 1 < source.size() &&
                   (std::iswalnum(source[index + 1]) || source[index + 1] == L'_'))
            {
                ++index;
            }
            if (source.substr(start, index - start + 1) == L"main")
            {
                std::size_t cursor = index + 1;
                for (;;)
                {
                    while (cursor < source.size() && std::iswspace(source[cursor]))
                    {
                        ++cursor;
                    }
                    if (cursor + 1 < source.size() && source[cursor] == L'/' && source[cursor + 1] == L'*')
                    {
                        const std::size_t commentEnd = source.find(L"*/", cursor + 2);
                        if (commentEnd == std::wstring_view::npos)
                        {
                            break;
                        }
                        cursor = commentEnd + 2;
                        continue;
                    }
                    break;
                }
                if (cursor < source.size() && source[cursor] == L'(')
                {
                    return true;
                }
            }
        }
    }
    return false;
}

Maro_GeneratedSource Maro_BuildGeneratedSource(const Maro_SourceRequest& request)
{
    Maro_GeneratedSource result;
    const std::wstring normalized = Maro_NormalizeNewlines(request.sourceText);
    const std::vector<std::wstring> lines = Maro_SplitLines(normalized);

    std::size_t generatedLine = 0;
    auto appendLine = [&result, &generatedLine](std::wstring_view line, std::size_t userLine) {
        if (generatedLine != 0)
        {
            result.text.push_back(L'\n');
        }
        ++generatedLine;
        result.text.append(line);
        if (userLine != 0)
        {
            result.lineMap.push_back({generatedLine, userLine});
        }
    };

    if (request.mode == Maro_SourceMode::Program)
    {
        result.text = normalized;
        for (std::size_t line = 1; line <= lines.size(); ++line)
        {
            result.lineMap.push_back({line, line});
        }
        return result;
    }

    std::size_t preambleLines = 0;
    bool inBlockComment = false;
    bool directiveContinuation = false;
    while (preambleLines < lines.size())
    {
        const std::wstring_view trimmed = Maro_TrimLeft(lines[preambleLines]);
        const bool commentOrBlank = Maro_IsLeadingCommentOrBlank(lines[preambleLines], inBlockComment);
        const bool directive = directiveContinuation || (!trimmed.empty() && trimmed.front() == L'#');
        if (!commentOrBlank && !directive)
        {
            break;
        }
        directiveContinuation = directive && !lines[preambleLines].empty() && lines[preambleLines].back() == L'\\';
        ++preambleLines;
    }

    for (std::size_t line = 0; line < preambleLines; ++line)
    {
        appendLine(lines[line], line + 1);
    }
    appendLine(request.language == Maro_Language::C17 ? L"int main(void)" : L"int main()", 0);
    appendLine(L"{", 0);
    for (std::size_t line = preambleLines; line < lines.size(); ++line)
    {
        appendLine(lines[line], line + 1);
    }
    appendLine(L"return 0;", 0);
    appendLine(L"}", 0);
    result.wrapped = true;
    result.notice = L"학습용 실행: 코드 조각을 실행하기 위해 임시 main() 함수를 생성했습니다.";
    return result;
}

Maro_SourcePosition Maro_MapGeneratedPosition(
    const Maro_GeneratedSource& generated,
    Maro_SourcePosition position,
    bool& isGeneratedOnly)
{
    for (const Maro_SourceMapEntry& entry : generated.lineMap)
    {
        if (entry.generatedLine == position.line)
        {
            isGeneratedOnly = false;
            return {entry.userLine, position.column};
        }
    }
    isGeneratedOnly = true;
    return position;
}

std::vector<Maro_Diagnostic> Maro_ParseCompilerDiagnostics(
    std::wstring_view compilerOutput,
    const Maro_SourceRequest& request,
    const Maro_GeneratedSource& generated,
    std::wstring_view analyzerName,
    std::wstring_view analyzerVersion)
{
    const std::wregex locatedMsvc(
        LR"Maro(^(.*)\(([0-9]+)(?:,([0-9]+))?\)\s*:\s*(fatal error|error|warning|note|remark)(?:\s+([A-Za-z]+[0-9]+))?\s*:\s*(.*)$)Maro",
        std::regex::icase);
    const std::wregex locatedClang(
        LR"Maro(^(.*):([0-9]+):([0-9]+):\s*(fatal error|error|warning|note|remark)\s*:\s*(.*)$)Maro",
        std::regex::icase);
    const std::wregex unlocated(
        LR"Maro(^.*:\s*(fatal error|error|warning|note)(?:\s+([A-Za-z]+[0-9]+))?\s*:\s*(.*)$)Maro",
        std::regex::icase);
    const std::wregex fixIt(
        LR"Maro(^fix-it:".*":\{([0-9]+):([0-9]+)-([0-9]+):([0-9]+)\}:"(.*)"$)Maro");

    std::vector<Maro_Diagnostic> diagnostics;
    std::wistringstream stream{std::wstring(compilerOutput)};
    std::wstring line;
    std::size_t findingSequence = 0;
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == L'\r')
        {
            line.pop_back();
        }
        std::wsmatch match;
        if (std::regex_match(line, match, fixIt) && !diagnostics.empty())
        {
            Maro_SourcePosition generatedStart{
                static_cast<std::size_t>(std::stoull(match[1].str())),
                static_cast<std::size_t>(std::stoull(match[2].str()))};
            Maro_SourcePosition generatedEnd{
                static_cast<std::size_t>(std::stoull(match[3].str())),
                static_cast<std::size_t>(std::stoull(match[4].str()))};
            bool startGenerated = false;
            bool endGenerated = false;
            const Maro_SourcePosition start = Maro_MapGeneratedPosition(generated, generatedStart, startGenerated);
            const Maro_SourcePosition end = Maro_MapGeneratedPosition(generated, generatedEnd, endGenerated);
            if (!startGenerated && !endGenerated)
            {
                const std::size_t startOffset = Maro_LineColumnToUtf16Offset(
                    request.sourceText, start.line, start.column);
                const std::size_t endOffset = Maro_LineColumnToUtf16Offset(
                    request.sourceText, end.line, end.column);
                Maro_TextEdit edit;
                edit.sourceVersion = request.sourceVersion;
                edit.startOffsetUtf16 = startOffset;
                edit.lengthUtf16 = endOffset >= startOffset ? endOffset - startOffset : 0;
                edit.replacement = Maro_UnescapeFixIt(match[5].str());
                if (startOffset <= request.sourceText.size() &&
                    edit.lengthUtf16 <= request.sourceText.size() - startOffset)
                {
                    edit.expectedText = request.sourceText.substr(startOffset, edit.lengthUtf16);
                }
                if (!diagnostics.back().fix)
                {
                    diagnostics.back().fix = Maro_FixSuggestion{L"컴파일러 수정 제안", {}};
                }
                diagnostics.back().fix->edits.push_back(std::move(edit));
            }
            continue;
        }

        Maro_SourcePosition generatedPosition{};
        std::wstring severityText;
        std::wstring codeText;
        std::wstring message;
        bool matched = false;
        if (std::regex_match(line, match, locatedMsvc))
        {
            generatedPosition.line = static_cast<std::size_t>(std::stoull(match[2].str()));
            generatedPosition.column = match[3].matched
                ? static_cast<std::size_t>(std::stoull(match[3].str()))
                : 1;
            severityText = match[4].str();
            codeText = match[5].str();
            message = match[6].str();
            matched = true;
        }
        else if (std::regex_match(line, match, locatedClang))
        {
            generatedPosition.line = static_cast<std::size_t>(std::stoull(match[2].str()));
            generatedPosition.column = static_cast<std::size_t>(std::stoull(match[3].str()));
            severityText = match[4].str();
            message = match[5].str();
            matched = true;
        }
        else if (std::regex_match(line, match, unlocated))
        {
            severityText = match[1].str();
            codeText = match[2].str();
            message = match[3].str();
            matched = true;
        }
        if (!matched)
        {
            continue;
        }

        Maro_Diagnostic diagnostic;
        diagnostic.sourceVersion = request.sourceVersion;
        diagnostic.findingId = L"Maro_" + std::to_wstring(request.sourceVersion) + L"_" +
            std::to_wstring(++findingSequence);
        diagnostic.analyzer = std::wstring(analyzerName);
        diagnostic.analyzerVersion = std::wstring(analyzerVersion);
        diagnostic.severity = Maro_ParseSeverity(severityText);
        diagnostic.evidence = Maro_Evidence::StaticAnalysis;
        diagnostic.originalDiagnostic = line;
        diagnostic.code = codeText.empty()
            ? Maro_DiagnosticCode(message, diagnostic.severity, request.language)
            : Maro_DiagnosticCode(codeText + L" " + message, diagnostic.severity, request.language);
        diagnostic.friendlyMessage = Maro_FriendlyMessage(codeText + L" " + message);
        if (generatedPosition.line != 0)
        {
            bool generatedOnly = false;
            const Maro_SourcePosition mapped = Maro_MapGeneratedPosition(
                generated, generatedPosition, generatedOnly);
            diagnostic.range.start = mapped;
            diagnostic.range.end = mapped;
            diagnostic.range.generated = generatedOnly;
        }
        diagnostics.push_back(std::move(diagnostic));
    }
    return diagnostics;
}

Maro_ToolchainInfo Maro_DetectToolchain()
{
    const Maro_MsvcDiscovery msvc = Maro_FindMsvc();
    const Maro_SdkDiscovery sdk = Maro_FindWindowsSdk();
    const fs::path clang = Maro_FindClang();

    Maro_ToolchainInfo info;
    if (!clang.empty())
    {
        info.kind = Maro_ToolchainKind::Clang;
        info.compilerPath = clang;
        info.name = L"Clang";
        Maro_AddBuildEnvironment(info, msvc, sdk);

        Maro_ProcessRequest versionRequest;
        versionRequest.executable = clang.wstring();
        versionRequest.arguments = {L"--version"};
        versionRequest.workingDirectory = clang.parent_path().wstring();
        versionRequest.environmentOverrides = info.environment;
        versionRequest.limits.wallMilliseconds = 2'000;
        versionRequest.limits.cpuMilliseconds = 2'000;
        versionRequest.limits.memoryBytes = 512ull << 20;
        versionRequest.limits.activeProcessLimit = 2;
        versionRequest.limits.stdoutBytes = 64u << 10;
        versionRequest.limits.stderrBytes = 64u << 10;
        info.version = Maro_FirstLine(Maro_CombineProcessOutput(Maro_RunProcess(versionRequest)));
        return info;
    }

    if (!msvc.compiler.empty())
    {
        info.kind = Maro_ToolchainKind::Msvc;
        info.compilerPath = msvc.compiler;
        info.name = L"MSVC (fallback)";
        info.version = msvc.msvcRoot.empty() ? L"설치됨" : msvc.msvcRoot.filename().wstring();
        info.fallback = true;
        Maro_AddBuildEnvironment(info, msvc, sdk);
        info.environment[L"VSLANG"] = L"1033";
    }
    return info;
}

Maro_AnalysisResult Maro_AnalyzeSource(
    const Maro_ToolchainInfo& toolchain,
    const Maro_SourceRequest& request,
    const Maro_GeneratedSource& generated,
    const std::filesystem::path& workingDirectory,
    const Maro_ExecutionLimits& limits,
    const Maro_CancelCheck& cancelled)
{
    fs::path unusedExecutable;
    return Maro_RunCompiler(
        toolchain, request, generated, workingDirectory, limits, cancelled, true, unusedExecutable);
}

Maro_CompilationResult Maro_CompileSource(
    const Maro_ToolchainInfo& toolchain,
    const Maro_SourceRequest& request,
    const Maro_GeneratedSource& generated,
    const std::filesystem::path& workingDirectory,
    const Maro_ExecutionLimits& limits,
    const Maro_CancelCheck& cancelled)
{
    fs::path executablePath;
    Maro_AnalysisResult base = Maro_RunCompiler(
        toolchain, request, generated, workingDirectory, limits, cancelled, false, executablePath);
    Maro_CompilationResult result;
    static_cast<Maro_AnalysisResult&>(result) = std::move(base);
    if (result.succeeded)
    {
        result.executablePath = std::move(executablePath);
    }
    return result;
}
