#include "maro_Analyzer.hpp"

#include "maro_Text.hpp"

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
    return {};
}

std::wstring maro_NormalizeDiagnosticPath(std::wstring_view maro_path)
{
    while (!maro_path.empty() && std::iswspace(maro_path.front()))
    {
        maro_path.remove_prefix(1);
    }
    while (!maro_path.empty() && std::iswspace(maro_path.back()))
    {
        maro_path.remove_suffix(1);
    }
    if (maro_path.size() >= 2 && maro_path.front() == L'"' && maro_path.back() == L'"')
    {
        maro_path.remove_prefix(1);
        maro_path.remove_suffix(1);
    }
    std::wstring maro_normalized(maro_path);
    std::replace(maro_normalized.begin(), maro_normalized.end(), L'/', L'\\');
    return Maro_Lower(fs::path(maro_normalized).lexically_normal().wstring());
}

bool maro_IsSnapshotDiagnostic(std::wstring_view maro_path, std::wstring_view maro_generatedSourcePath,
    Maro_Language maro_language)
{
    const std::wstring maro_normalized = maro_NormalizeDiagnosticPath(maro_path);
    const std::wstring maro_snapshot = maro_NormalizeDiagnosticPath(maro_generatedSourcePath);
    if (!maro_snapshot.empty() && maro_normalized == maro_snapshot)
    {
        return true;
    }
    const fs::path maro_reported(maro_normalized);
    const std::wstring maro_name = maro_snapshot.empty()
        ? (maro_language == Maro_Language::C17 ? L"maro_usersource.c" : L"maro_usersource.cpp")
        : fs::path(maro_snapshot).filename().wstring();
    return !maro_reported.has_parent_path() && maro_reported.filename() == maro_name;
}

struct maro_SyntaxToken
{
    std::wstring_view maro_text;
    std::size_t maro_start;
    std::size_t maro_end;
};

std::optional<Maro_SourcePosition> maro_FindMissingSemicolon(
    std::wstring_view maro_source, Maro_SourcePosition maro_reported,
    std::wstring_view maro_code, const std::wstring& maro_message)
{
    if (maro_code != L"C2143" && maro_code != L"C2146")
    {
        return std::nullopt;
    }
    static const std::wregex maro_english(
        LR"(missing\s+';'\s+before(?:\s+identifier)?\s+'([^']+)')", std::regex::icase);
    static const std::wregex maro_korean(LR"(';'.*'([^']+)'\s*앞에.*없)");
    std::wsmatch maro_match;
    if (!std::regex_search(maro_message, maro_match, maro_english) &&
        !std::regex_search(maro_message, maro_match, maro_korean))
    {
        return std::nullopt;
    }
    const std::wstring maro_normalized = Maro_NormalizeNewlines(maro_source);
    maro_source = maro_normalized;
    const std::wstring maro_target = maro_match[1].str();
    const std::size_t maro_lineStart = Maro_LineColumnToUtf16Offset(maro_source, maro_reported.line, 1);
    const std::size_t maro_columnOffset = Maro_LineColumnToUtf16Offset(
        maro_source, maro_reported.line, maro_reported.column);
    const std::size_t maro_lineEnd = maro_source.find(L'\n', maro_lineStart);
    const std::size_t maro_limit = maro_lineEnd == std::wstring_view::npos ? maro_source.size() : maro_lineEnd;
    std::vector<maro_SyntaxToken> maro_tokens;
    std::size_t maro_targetIndex = 0;
    bool maro_found = false;
    for (std::size_t maro_index = 0; maro_index < maro_limit;)
    {
        if (std::iswspace(maro_source[maro_index]))
        {
            ++maro_index;
            continue;
        }
        const std::size_t maro_start = maro_index;
        const wchar_t maro_character = maro_source[maro_index];
        const wchar_t maro_next = maro_index + 1 < maro_source.size() ? maro_source[maro_index + 1] : 0;
        if (maro_character == L'/' && maro_next == L'/')
        {
            maro_index = maro_source.find(L'\n', maro_index + 2);
            if (maro_index == std::wstring_view::npos)
            {
                break;
            }
            continue;
        }
        if (maro_character == L'/' && maro_next == L'*')
        {
            maro_index = maro_source.find(L"*/", maro_index + 2);
            if (maro_index == std::wstring_view::npos)
            {
                return std::nullopt;
            }
            maro_index += 2;
            continue;
        }
        if (maro_character == L'"' || maro_character == L'\'')
        {
            bool maro_closed = false;
            for (++maro_index; maro_index < maro_source.size(); ++maro_index)
            {
                if (maro_source[maro_index] == L'\\')
                {
                    ++maro_index;
                }
                else if (maro_source[maro_index] == maro_character)
                {
                    ++maro_index;
                    maro_closed = true;
                    break;
                }
            }
            if (!maro_closed)
            {
                return std::nullopt;
            }
        }
        else if (std::iswalnum(maro_character) || maro_character == L'_')
        {
            while (++maro_index < maro_source.size() &&
                (std::iswalnum(maro_source[maro_index]) || maro_source[maro_index] == L'_'))
            {
            }
            if (maro_index < maro_source.size() && maro_source[maro_index] == L'"' &&
                maro_source[maro_index - 1] == L'R')
            {
                return std::nullopt;
            }
        }
        else
        {
            ++maro_index;
            const std::wstring_view maro_pair = maro_source.substr(maro_start, 2);
            if (maro_pair == L"::" || maro_pair == L"->" || maro_pair == L"++" || maro_pair == L"--" ||
                maro_pair == L"==" || maro_pair == L"!=" || maro_pair == L"<=" || maro_pair == L">=" ||
                maro_pair == L"<<" || maro_pair == L">>")
            {
                ++maro_index;
            }
        }
        const std::wstring_view maro_text = maro_source.substr(maro_start, maro_index - maro_start);
        if (maro_start >= maro_columnOffset && maro_start >= maro_lineStart && maro_text == maro_target)
        {
            maro_targetIndex = maro_tokens.size();
            maro_found = true;
            break;
        }
        maro_tokens.push_back({maro_text, maro_start, maro_index});
    }
    if (!maro_found || maro_targetIndex == 0)
    {
        return std::nullopt;
    }
    std::size_t maro_statement = 0;
    int maro_parentheses = 0;
    int maro_brackets = 0;
    for (std::size_t maro_index = 0; maro_index < maro_targetIndex; ++maro_index)
    {
        const auto maro_token = maro_tokens[maro_index].maro_text;
        if (maro_token == L"(") ++maro_parentheses;
        if (maro_token == L")") --maro_parentheses;
        if (maro_token == L"[") ++maro_brackets;
        if (maro_token == L"]") --maro_brackets;
        if (maro_parentheses < 0 || maro_brackets < 0)
        {
            return std::nullopt;
        }
        if (maro_parentheses == 0 && maro_brackets == 0 &&
            (maro_token == L";" || maro_token == L"{" || maro_token == L"}"))
        {
            maro_statement = maro_index + 1;
        }
    }
    if (maro_parentheses != 0 || maro_brackets != 0 || maro_statement >= maro_targetIndex)
    {
        return std::nullopt;
    }
    const auto maro_first = maro_tokens[maro_statement].maro_text;
    for (const auto maro_control : {L"if", L"else", L"for", L"while", L"do", L"switch", L"catch",
        L"try", L"struct", L"class", L"enum", L"namespace", L"typedef", L"template", L"case", L"default"})
    {
        if (maro_first == maro_control)
        {
            return std::nullopt;
        }
    }
    bool maro_expression = maro_first == L"return" || maro_first == L"co_return" || maro_first == L"throw" ||
        maro_first == L"break" || maro_first == L"continue" || maro_first == L"goto";
    bool maro_callPrefix = std::iswalpha(maro_first.front()) || maro_first.front() == L'_';
    for (std::size_t maro_index = maro_statement; maro_index < maro_targetIndex; ++maro_index)
    {
        const auto maro_token = maro_tokens[maro_index].maro_text;
        if (maro_token == L"#" || maro_token == L"\\")
        {
            return std::nullopt;
        }
        maro_expression = maro_expression || maro_token == L"=" || maro_token == L"<<";
        if (maro_token == L"(")
        {
            maro_expression = maro_expression || maro_callPrefix;
            maro_callPrefix = false;
        }
        else if (maro_index > maro_statement)
        {
            const auto maro_previous = maro_tokens[maro_index - 1].maro_text;
            maro_callPrefix = maro_callPrefix && (maro_token == L"::" || maro_token == L"." ||
                maro_token == L"->" || maro_previous == L"::" || maro_previous == L"." || maro_previous == L"->");
        }
    }
    const auto& maro_last = maro_tokens[maro_targetIndex - 1];
    const wchar_t maro_lastCharacter = maro_last.maro_text.back();
    if (!maro_expression || !(std::iswalnum(maro_lastCharacter) || maro_lastCharacter == L'_' ||
        maro_lastCharacter == L')' || maro_lastCharacter == L']' || maro_lastCharacter == L'"' ||
        maro_lastCharacter == L'\'' || maro_last.maro_text == L"++" || maro_last.maro_text == L"--"))
    {
        return std::nullopt;
    }
    const std::size_t maro_previousNewline = maro_source.rfind(L'\n', maro_last.maro_end - 1);
    const std::size_t maro_insertLineStart = maro_previousNewline == std::wstring_view::npos ? 0 : maro_previousNewline + 1;
    return Maro_SourcePosition{
        1 + static_cast<std::size_t>(std::count(maro_source.begin(), maro_source.begin() + maro_last.maro_end, L'\n')),
        1 + Maro_WideToUtf8(maro_source.substr(maro_insertLineStart, maro_last.maro_end - maro_insertLineStart)).size()};
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
    maro_OutputDecoder maro_stdout;
    maro_OutputDecoder maro_stderr;
    std::wstring output = maro_stdout.maro_Decode(process.standardOutputUtf8, true);
    const std::wstring error = maro_stderr.maro_Decode(process.standardErrorUtf8, true);
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
    const fs::path& liveOutputHeader,
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
            if (request.maro_trace)
            {
                arguments.push_back(L"-g");
                arguments.push_back(L"-gcodeview");
                arguments.push_back(L"-Xlinker");
                arguments.push_back(L"/DEBUG");
                arguments.push_back(L"-Xlinker");
                arguments.push_back(L"/PDB:" + (executablePath.parent_path() / L"maro_UserProgram.pdb").wstring());
            }
            arguments.push_back(L"-include");
            arguments.push_back(liveOutputHeader.wstring());
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
            if (request.maro_trace) arguments.push_back(L"/Z7");
            arguments.push_back(L"/FI" + liveOutputHeader.wstring());
            arguments.push_back(L"/Fe:" + executablePath.wstring());
            arguments.push_back(L"/Fo:" + objectPath.wstring());
        }
        arguments.push_back(sourcePath.wstring());
        if (!syntaxOnly && request.maro_trace)
        {
            arguments.push_back(L"/link");
            arguments.push_back(L"/DEBUG");
            arguments.push_back(L"/INCREMENTAL:NO");
            arguments.push_back(L"/PDB:" + (executablePath.parent_path() / L"maro_UserProgram.pdb").wstring());
        }
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
        (request.language == Maro_Language::C17 ? L"maro_UserSource.c" : L"maro_UserSource.cpp");
    executablePath = workingDirectory / L"maro_UserProgram.exe";
    const fs::path objectPath = workingDirectory / L"maro_UserProgram.obj";
    const fs::path liveOutputHeader = workingDirectory / L"maro_LiveOutput.hpp";

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
    if (!syntaxOnly && !Maro_WriteSourceFile(
            liveOutputHeader,
            L"#include <stdio.h>\n"
            L"#ifdef __cplusplus\n"
            L"#include <iostream>\n"
            L"namespace { struct maro_live_output_mode { maro_live_output_mode() { setvbuf(stdout, 0, _IONBF, 0); setvbuf(stderr, 0, _IONBF, 0); std::cout << std::unitbuf; std::cerr << std::unitbuf; std::clog << std::unitbuf; } }; maro_live_output_mode maro_live_output_mode_instance; }\n"
            L"#elif defined(_MSC_VER)\n"
            L"static void __cdecl maro_set_output_mode(void) { setvbuf(stdout, 0, _IONBF, 0); setvbuf(stderr, 0, _IONBF, 0); }\n"
            L"#pragma section(\".CRT$XCU\", read)\n"
            L"__declspec(allocate(\".CRT$XCU\")) void (__cdecl* maro_live_output_mode_instance)(void) = maro_set_output_mode;\n"
            L"#else\n"
            L"__attribute__((constructor)) static void maro_set_output_mode(void) { setvbuf(stdout, 0, _IONBF, 0); setvbuf(stderr, 0, _IONBF, 0); }\n"
            L"#endif\n"))
    {
        return result;
    }

    Maro_ProcessRequest processRequest;
    processRequest.executable = toolchain.compilerPath.wstring();
    processRequest.arguments = Maro_CompilerArguments(
        toolchain,
        request,
        sourcePath,
        executablePath,
        objectPath,
        liveOutputHeader,
        syntaxOnly);
    processRequest.workingDirectory = workingDirectory.wstring();
    processRequest.environmentOverrides = toolchain.environment;
    processRequest.limits = Maro_CompilerProcessLimits(limits, syntaxOnly);
    processRequest.maro_background = request.maro_background && !request.maro_trace;

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
        toolchain.version,
        sourcePath.wstring());
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
}

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

Maro_GeneratedSource maro_BuildGeneratedSource(const Maro_SourceRequest& request)
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
    std::wstring_view analyzerVersion,
    std::wstring_view maro_generatedSourcePath)
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
        LR"Maro(^fix-it:"(.*)":\{([0-9]+):([0-9]+)-([0-9]+):([0-9]+)\}:"(.*)"$)Maro");
    const std::wregex maro_warningCode(LR"Maro(\[(-W[^\]]+)\]\s*$)Maro");

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
            if (!maro_IsSnapshotDiagnostic(match[1].str(), maro_generatedSourcePath, request.language))
            {
                continue;
            }
            Maro_SourcePosition generatedStart{
                static_cast<std::size_t>(std::stoull(match[2].str())),
                static_cast<std::size_t>(std::stoull(match[3].str()))};
            Maro_SourcePosition generatedEnd{
                static_cast<std::size_t>(std::stoull(match[4].str())),
                static_cast<std::size_t>(std::stoull(match[5].str()))};
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
                edit.replacement = Maro_UnescapeFixIt(match[6].str());
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
        std::wstring maro_reportedPath;
        bool matched = false;
        if (std::regex_match(line, match, locatedMsvc))
        {
            maro_reportedPath = match[1].str();
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
            maro_reportedPath = match[1].str();
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
        if (codeText.empty() && std::regex_search(message, match, maro_warningCode))
        {
            codeText = match[1].str();
        }
        diagnostic.code = codeText.empty()
            ? Maro_DiagnosticCode(message, diagnostic.severity, request.language)
            : codeText;
        diagnostic.friendlyMessage = Maro_FriendlyMessage(codeText + L" " + message);
        if (!diagnostic.friendlyMessage.empty())
        {
            diagnostic.friendlyMessage += L"\r\n";
        }
        diagnostic.friendlyMessage += message;
        diagnostic.sourcePath = maro_reportedPath;
        if (generatedPosition.line != 0)
        {
            bool generatedOnly = true;
            const bool maro_snapshot = maro_IsSnapshotDiagnostic(
                maro_reportedPath, maro_generatedSourcePath, request.language);
            const Maro_SourcePosition mapped = maro_snapshot
                ? Maro_MapGeneratedPosition(generated, generatedPosition, generatedOnly)
                : generatedPosition;
            diagnostic.range.start = mapped;
            diagnostic.range.end = mapped;
            diagnostic.range.generated = generatedOnly;
            if (!generatedOnly)
            {
                diagnostic.sourcePath = request.sourcePath;
                if (const auto maro_insertion = maro_FindMissingSemicolon(
                    request.sourceText, mapped, codeText, message))
                {
                    diagnostic.range.start = *maro_insertion;
                    diagnostic.range.end = *maro_insertion;
                    diagnostic.friendlyMessage = L"문장 끝에 ';'가 필요합니다.\r\n" + message;
                }
            }
        }
        diagnostics.push_back(std::move(diagnostic));
    }
    return diagnostics;
}

Maro_ToolchainInfo Maro_DetectToolchain(bool maro_msvcOnly)
{
    const Maro_MsvcDiscovery msvc = Maro_FindMsvc();
    const Maro_SdkDiscovery sdk = Maro_FindWindowsSdk();
    const fs::path clang = maro_msvcOnly ? fs::path{} : Maro_FindClang();

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
        info.name = maro_msvcOnly ? L"MSVC" : L"MSVC (fallback)";
        info.version = msvc.msvcRoot.empty() ? L"설치됨" : msvc.msvcRoot.filename().wstring();
        info.fallback = !maro_msvcOnly;
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
    std::error_code error;
    if (result.succeeded && (!fs::is_regular_file(executablePath, error) || error))
    {
        result.succeeded = false;
        if (!result.compilerOutput.empty())
        {
            result.compilerOutput += L"\r\n";
        }
        result.compilerOutput += L"CLive_Maro: 실행 파일이 생성되지 않았습니다.";
    }
    if (result.succeeded)
    {
        result.executablePath = std::move(executablePath);
    }
    return result;
}
