#include "maro_Analyzer.hpp"
#include "maro_CodeDiagnostics.hpp"
#include "maro_Engine.hpp"
#include "maro_OutputQueue.hpp"
#include "maro_Process.hpp"
#include "maro_Text.hpp"
#include "maro_Update.hpp"
#include "maro_VisualStudio.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
struct Maro_TestState
{
    int checks = 0;
    int failures = 0;

    void Expect(bool condition, std::string_view message)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::cerr << "[FAIL] " << message << '\n';
        }
    }
};

class Maro_FinalResultCapture
{
public:
    void Publish(Maro_ResultEnvelope result)
    {
        if (result.phase != Maro_Phase::Completed)
        {
            return;
        }

        {
            std::lock_guard lock(mutex_);
            result_ = std::move(result);
        }
        condition_.notify_one();
    }

    std::optional<Maro_ResultEnvelope> Wait(std::chrono::seconds timeout)
    {
        std::unique_lock lock(mutex_);
        if (!condition_.wait_for(lock, timeout, [this] { return result_.has_value(); }))
        {
            return std::nullopt;
        }
        return result_;
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<Maro_ResultEnvelope> result_;
};

class Maro_ResultTimeline
{
public:
    using Clock = std::chrono::steady_clock;

    void Publish(Maro_ResultEnvelope result)
    {
        {
            std::lock_guard lock(mutex_);
            events_.emplace_back(Clock::now(), std::move(result));
        }
        condition_.notify_all();
    }

    std::optional<Clock::time_point> WaitForOutput(
        std::wstring_view token,
        std::chrono::seconds timeout)
    {
        std::unique_lock lock(mutex_);
        const auto ready = [this, token] {
            std::wstring output;
            for (const auto& entry : events_)
            {
                const Maro_ResultEnvelope& event = entry.second;
                if (event.phase != Maro_Phase::Completed)
                {
                    output += event.standardOutput;
                    if (output.find(token) != std::wstring::npos)
                    {
                        return true;
                    }
                }
                if (event.phase == Maro_Phase::Completed)
                {
                    return true;
                }
            }
            return false;
        };
        if (!condition_.wait_for(lock, timeout, ready))
        {
            return std::nullopt;
        }
        std::wstring output;
        for (const auto& entry : events_)
        {
            const Maro_ResultEnvelope& event = entry.second;
            if (event.phase != Maro_Phase::Completed)
            {
                output += event.standardOutput;
                if (output.find(token) != std::wstring::npos)
                {
                    return entry.first;
                }
            }
            if (event.phase == Maro_Phase::Completed)
            {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<std::pair<Clock::time_point, Maro_ResultEnvelope>> WaitForCompletion(
        std::chrono::seconds timeout)
    {
        std::unique_lock lock(mutex_);
        if (!condition_.wait_for(lock, timeout, [this] {
                return !events_.empty() && events_.back().second.phase == Maro_Phase::Completed;
            }))
        {
            return std::nullopt;
        }
        return events_.back();
    }

    std::wstring LiveOutput() const
    {
        std::lock_guard lock(mutex_);
        std::wstring result;
        for (const auto& entry : events_)
        {
            const Maro_ResultEnvelope& event = entry.second;
            if (event.phase != Maro_Phase::Completed)
            {
                result += event.standardOutput;
            }
        }
        return result;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<std::pair<Clock::time_point, Maro_ResultEnvelope>> events_;
};

std::optional<Maro_ResultEnvelope> Maro_RunToCompletion(Maro_SourceRequest request)
{
    Maro_FinalResultCapture capture;
    Maro_Engine engine([&capture](Maro_ResultEnvelope result) {
        capture.Publish(std::move(result));
    });

    const std::uint64_t requestId = engine.Submit(std::move(request));
    std::optional<Maro_ResultEnvelope> result;
    if (requestId != 0)
    {
        result = capture.Wait(std::chrono::seconds(30));
    }
    engine.Shutdown();
    return result;
}

void Maro_TestUtfConversions(Maro_TestState& state)
{
    constexpr std::wstring_view source = L"CLive_Maro 한글 \U0001F642";
    const std::string utf8 = Maro_WideToUtf8(source);

    state.Expect(!utf8.empty(), "UTF-8 conversion produces text");
    state.Expect(Maro_Utf8ToWide(utf8) == source, "UTF-8 conversion round-trips UTF-16 text");
    state.Expect(Maro_WideToUtf8(L"").empty(), "empty wide text remains empty");
    state.Expect(Maro_Utf8ToWide("").empty(), "empty UTF-8 text remains empty");
}

void Maro_TestTextCoordinates(Maro_TestState& state)
{
    state.Expect(
        Maro_NormalizeNewlines(L"one\r\ntwo\rthree\n") == L"one\ntwo\nthree\n",
        "newlines are normalized");

    constexpr std::wstring_view line = L"A한B";
    state.Expect(
        Maro_Utf8ByteColumnToUtf16Index(line, 1) == 0,
        "first compiler column maps to the first UTF-16 unit");
    state.Expect(
        Maro_Utf8ByteColumnToUtf16Index(line, 2) == 1,
        "multibyte character start maps correctly");
    state.Expect(
        Maro_Utf8ByteColumnToUtf16Index(line, 5) == 2,
        "column after a multibyte character maps correctly");
    state.Expect(
        Maro_LineColumnToUtf16Offset(L"x\r\nA한B", 2, 5) == 5,
        "line and byte column map to the editor offset");
}

void Maro_TestTextUtilities(Maro_TestState& state)
{
    const std::uint64_t first = Maro_HashSource(L"same source");
    state.Expect(first == Maro_HashSource(L"same source"), "source hashes are deterministic");
    state.Expect(first != Maro_HashSource(L"different source"), "different sources have different hashes");

    const std::wstring dirty = std::wstring(L"ok\n\t") + wchar_t{1} + L"end";
    state.Expect(
        Maro_SanitizeOutput(dirty) == L"ok\n\t\uFFFDend",
        "unsafe output control characters are replaced");
}

void Maro_TestCommandLineQuoting(Maro_TestState& state)
{
    state.Expect(Maro_QuoteWindowsArgument(L"") == L"\"\"", "empty arguments are quoted");
    state.Expect(Maro_QuoteWindowsArgument(L"alpha") == L"alpha", "simple arguments stay unquoted");
    state.Expect(
        Maro_QuoteWindowsArgument(L"alpha beta") == L"\"alpha beta\"",
        "arguments containing spaces are quoted");

    const std::wstring commandLine = maro_BuildWindowsCommandLine(
        L"C:\\Program Files\\CLive_Maro\\maro_CLive_Maro.exe",
        {L"--name", L"hello world"});
    state.Expect(
        commandLine ==
            L"\"C:\\Program Files\\CLive_Maro\\maro_CLive_Maro.exe\" --name \"hello world\"",
        "Windows command lines quote the executable and arguments");
}

void Maro_TestMainDetection(Maro_TestState& state)
{
    state.Expect(
        Maro_HasMain(L"#include <stdio.h>\nint main(void) { return 0; }") ,
        "a conventional main function is detected");
    state.Expect(
        !Maro_HasMain(L"int domain(void) { return 0; }"),
        "an identifier containing main is not treated as an entry point");
}

void Maro_TestSourceGenerationAndMapping(Maro_TestState& state)
{
    Maro_SourceRequest program;
    program.sourceVersion = 7;
    program.language = Maro_Language::Cpp20;
    program.mode = Maro_SourceMode::Program;
    program.sourceText = L"int main() { return 0; }";

    const Maro_GeneratedSource unchanged = maro_BuildGeneratedSource(program);
    state.Expect(!unchanged.wrapped, "a complete program is not wrapped");
    state.Expect(
        unchanged.text.find(program.sourceText) != std::wstring::npos,
        "a complete program remains in generated source");

    Maro_SourceRequest snippet;
    snippet.sourceVersion = 8;
    snippet.language = Maro_Language::Cpp20;
    snippet.mode = Maro_SourceMode::Snippet;
    snippet.sourceText = L"int value = 40;\nvalue += 2;";

    const Maro_GeneratedSource generated = maro_BuildGeneratedSource(snippet);
    state.Expect(generated.wrapped, "snippet mode wraps source in a program");
    state.Expect(!generated.notice.empty(), "snippet wrapping supplies a user notice");
    state.Expect(!generated.lineMap.empty(), "snippet wrapping supplies a source map");

    const Maro_SourceMapEntry* mappedEntry = nullptr;
    for (const Maro_SourceMapEntry& entry : generated.lineMap)
    {
        if (entry.userLine != 0)
        {
            mappedEntry = &entry;
            break;
        }
    }

    state.Expect(mappedEntry != nullptr, "source map contains a user source line");
    if (mappedEntry != nullptr)
    {
        bool generatedOnly = true;
        const Maro_SourcePosition mapped = Maro_MapGeneratedPosition(
            generated,
            Maro_SourcePosition{mappedEntry->generatedLine, 2},
            generatedOnly);
        state.Expect(!generatedOnly, "mapped user code is not marked as generated-only");
        state.Expect(mapped.line == mappedEntry->userLine, "generated line maps to the user line");
        state.Expect(mapped.column == 2, "source mapping preserves the compiler column");
    }

    Maro_SourceRequest leadingBlank;
    leadingBlank.language = Maro_Language::Cpp20;
    leadingBlank.mode = Maro_SourceMode::Snippet;
    leadingBlank.sourceText = L"\nint answer = 42;";
    const Maro_GeneratedSource blankGenerated = maro_BuildGeneratedSource(leadingBlank);

    bool mapsSecondUserLine = false;
    for (const Maro_SourceMapEntry& entry : blankGenerated.lineMap)
    {
        if (entry.userLine == 2)
        {
            mapsSecondUserLine = true;
            break;
        }
    }
    state.Expect(mapsSecondUserLine, "a leading blank line preserves the second user line mapping");

    Maro_SourceRequest emptySnippet;
    emptySnippet.language = Maro_Language::Cpp20;
    emptySnippet.mode = Maro_SourceMode::Snippet;
    const Maro_GeneratedSource emptyGenerated = maro_BuildGeneratedSource(emptySnippet);
    state.Expect(
        emptyGenerated.text.starts_with(L"\nint main()"),
        "an empty snippet preserves its blank user line before the wrapper");
    state.Expect(
        !emptyGenerated.lineMap.empty() &&
            emptyGenerated.lineMap.front().generatedLine == 1 &&
            emptyGenerated.lineMap.front().userLine == 1,
        "an empty snippet retains a source-map entry for its blank line");
}

void maro_TestCompilerDiagnostics(Maro_TestState& maro_state)
{
    Maro_SourceRequest maro_request;
    maro_request.sourceVersion = 19;
    maro_request.sourcePath = L"C:\\project\\maro_main.cpp";
    maro_request.sourceText = L"int maro_value = maro_missing;";
    maro_request.language = Maro_Language::Cpp20;
    maro_request.mode = Maro_SourceMode::Snippet;
    const auto maro_generated = maro_BuildGeneratedSource(maro_request);
    const std::wstring maro_snapshot = L"C:\\Temp\\maro_UserSource.cpp";
    const auto maro_msvc = Maro_ParseCompilerDiagnostics(
        L"C:/TEMP/maro_UserSource.cpp(3,18): error C2065: 'maro_missing': undeclared identifier\r\n"
        L"C:\\sdk\\vector(2361,7): error C2672: 'begin': no matching overloaded function found\r\n"
        L"C:\\headers\\maro_UserSource.cpp(3,1): error C2143: syntax error: missing ';'\r\n"
        L"LINK : fatal error LNK1120: 1 unresolved externals\r\n",
        maro_request, maro_generated, L"MSVC", L"test", maro_snapshot);
    maro_state.Expect(maro_msvc.size() == 4, "compiler parser retains located and linker diagnostics");
    if (maro_msvc.size() == 4)
    {
        maro_state.Expect(maro_msvc[0].code == L"C2065", "native MSVC error codes are preserved");
        maro_state.Expect(maro_msvc[0].range.start.line == 1 && !maro_msvc[0].range.generated,
            "snapshot diagnostic maps through snippet wrapper to the actual user line");
        maro_state.Expect(maro_msvc[0].sourcePath == maro_request.sourcePath,
            "mapped diagnostic identifies the user source instead of the temporary snapshot");
        maro_state.Expect(maro_msvc[0].friendlyMessage.find(L"maro_missing") != std::wstring::npos,
            "friendly compiler diagnostics preserve the actual failing identifier");
        maro_state.Expect(maro_msvc[1].sourcePath == L"C:\\sdk\\vector" &&
                maro_msvc[1].range.start.line == 2361 && maro_msvc[1].range.generated,
            "standard-library diagnostics retain their own path and line without claiming user location");
        maro_state.Expect(maro_msvc[1].friendlyMessage.find(L"no matching overloaded function") != std::wstring::npos,
            "unknown diagnostic families retain the specific compiler explanation");
        maro_state.Expect(maro_msvc[2].range.start.line == 3 && maro_msvc[2].range.generated,
            "a header with the snapshot basename is not remapped to user source");
        maro_state.Expect(maro_msvc[3].code == L"LNK1120" && maro_msvc[3].range.start.line == 0 &&
                maro_msvc[3].severity == Maro_Severity::Fatal,
            "unlocated linker diagnostics preserve their native code and severity");
    }
    const auto maro_clang = Maro_ParseCompilerDiagnostics(
        L"C:\\Temp\\maro_UserSource.cpp:3:5: warning: unused variable 'maro_value' [-Wunused-variable]\n"
        L"fix-it:\"C:\\sdk\\vector\":{3:5-3:15}:\"other\"\n"
        L"fix-it:\"C:\\Temp\\maro_UserSource.cpp\":{3:5-3:15}:\"maro_replaced\"\n"
        L"maro_UserSource.cpp(3,18): error: expected ';' after expression\n",
        maro_request, maro_generated, L"Clang", L"test", maro_snapshot);
    maro_state.Expect(maro_clang.size() == 2, "Clang and MSVC-style Clang diagnostics are parsed together");
    if (maro_clang.size() == 2)
    {
        maro_state.Expect(maro_clang[0].code == L"-Wunused-variable",
            "Clang warning-option codes are preserved");
        maro_state.Expect(!maro_clang[0].fix,
            "arbitrary compiler replacements are not exposed as one-click fixes");
        maro_state.Expect(maro_clang[1].code == L"CPP-SYN-1001" &&
                maro_clang[1].range.start.line == 1 && !maro_clang[1].range.generated,
            "code-less compiler errors retain a stable fallback code and valid user location");
        maro_state.Expect(maro_clang[1].originalDiagnostic.find(L"expected ';'") != std::wstring::npos,
            "compiler diagnostic original text remains available verbatim");
    }
    const auto maro_semicolon = [](std::wstring maro_source, std::size_t maro_line,
        std::wstring_view maro_message, std::wstring_view maro_code = L"C2143",
        Maro_Language maro_language = Maro_Language::C17) {
        Maro_SourceRequest maro_input;
        maro_input.sourceText = std::move(maro_source);
        maro_input.sourcePath = L"C:\\project\\maro_main.c";
        maro_input.language = maro_language;
        maro_input.mode = Maro_SourceMode::Program;
        const std::wstring maro_path = maro_language == Maro_Language::C17
            ? L"maro_UserSource.c" : L"maro_UserSource.cpp";
        return Maro_ParseCompilerDiagnostics(maro_path + L"(" + std::to_wstring(maro_line) +
            L",1): error " + std::wstring(maro_code) + L": " + std::wstring(maro_message),
            maro_input, maro_BuildGeneratedSource(maro_input), L"MSVC", L"test").at(0);
    };
    const std::wstring maro_missing = L"#include <stdio.h>\nint main() {\n    printf(\"나는야 몽몽이\")\n    return 0;\n}";
    const std::wstring maro_beforeReturn = L"syntax error: missing ';' before 'return'";
    const auto maro_english = maro_semicolon(maro_missing, 4, maro_beforeReturn);
    maro_state.Expect(maro_english.range.start.line == 3 && maro_english.range.end.line == 3 &&
        maro_english.code == L"C2143" && maro_english.originalDiagnostic.find(L"(4,1)") != std::wstring::npos,
        "missing semicolon points to the actual statement while preserving the compiler location and code");
    maro_state.Expect(maro_english.range.start.column ==
        std::wstring_view(L"    printf(\"나는야 몽몽이\")").size() + 1,
        "missing-semicolon insertion columns use editor UTF16 coordinates for Korean text");
    const auto maro_korean = maro_semicolon(maro_missing, 4, L"구문 오류: ';'이(가) 'return' 앞에 없습니다.");
    maro_state.Expect(maro_korean.range.start.line == 3 &&
        maro_korean.friendlyMessage.starts_with(L"문장 끝에 ';'가 필요합니다."),
        "localized MSVC semicolon diagnostics identify the previous statement");
    maro_state.Expect(maro_semicolon(L"int main() {\n    puts(\"x\") // trailing\n\n/* ignored\n ignored */\n    return 0;\n}",
        6, maro_beforeReturn).range.start.line == 2,
        "semicolon attribution skips blank lines and line or block comments");
    maro_state.Expect(maro_semicolon(L"int main() {\n    printf(\n        \"%d\",\n        42\n    )\n    return 0;\n}",
        6, maro_beforeReturn).range.start.line == 5,
        "multiline calls point to the closing expression line instead of its beginning");
    const std::wstring maro_inline = L"int main() { puts(\"x\") return 0; }";
    const auto maro_sameLine = maro_semicolon(maro_inline, 1, maro_beforeReturn);
    maro_state.Expect(maro_sameLine.range.start.line == 1 &&
        maro_sameLine.range.start.column == maro_inline.find(L" return") + 1,
        "same-line missing semicolons use the insertion column without changing the line");
    maro_state.Expect(maro_semicolon(L"int main() {\r\n    int maro_value = 42\r\n    return 0;\r\n}",
        3, maro_beforeReturn, L"C2143", Maro_Language::Cpp20).range.start.line == 2,
        "C++ declarations with initializers and CRLF line endings map to the missing terminator");
    const auto maro_crOnly = maro_semicolon(L"#include <stdio.h>\rint main() {\r    printf(\"한글\")\r    return 0;\r}",
        4, maro_beforeReturn);
    maro_state.Expect(maro_crOnly.range.start.line == 3 && maro_crOnly.range.start.column ==
        std::wstring_view(L"    printf(\"한글\")").size() + 1,
        "CR-only source preserves the missing semicolon line and editor column");
    maro_state.Expect(maro_semicolon(L"int main() {\r    puts(\"x\") // trailing\r\r/* ignored\r ignored */\r    return 0;\r}",
        6, maro_beforeReturn).range.start.line == 2,
        "CR-only comments and blank lines cannot displace a missing semicolon");
    maro_state.Expect(maro_semicolon(L"int main() {\r\n    puts(\"x\");\r    return 0;\n}",
        3, maro_beforeReturn).range.start.line == 3,
        "mixed newline complete statements retain the compiler location");
    maro_state.Expect(maro_semicolon(L"int main() {\n    std::cout << \"hello\"\n    return 0;\n}",
        3, maro_beforeReturn, L"C2143", Maro_Language::Cpp20).range.start.line == 2,
        "C++ stream expressions identify their missing semicolon");
    maro_state.Expect(maro_semicolon(L"int main() {\n    puts(\"x\")\n    maro_value = 1;\n}",
        3, L"syntax error: missing ';' before identifier 'maro_value'", L"C2146").range.start.line == 2,
        "C2146 identifier diagnostics can identify the preceding incomplete call");
    maro_state.Expect(maro_semicolon(maro_missing, 4, L"syntax error: missing ')' before 'return'").range.start.line == 4,
        "missing parentheses are never treated as missing semicolons");
    maro_state.Expect(maro_semicolon(maro_missing, 4, maro_beforeReturn, L"C2065").range.start.line == 4,
        "unrelated compiler error codes keep the compiler location");
    maro_state.Expect(maro_semicolon(L"int main() {\n    puts(\"x\");\n    return 0;\n}",
        3, maro_beforeReturn).range.start.line == 3,
        "a complete preceding statement is not incorrectly blamed");
    maro_state.Expect(maro_semicolon(L"int main() {\n    if (1)\n    return 0;\n}",
        3, maro_beforeReturn).range.start.line == 3,
        "control-flow headers are not mistaken for unterminated calls");
    maro_state.Expect(maro_semicolon(L"int main() {\n    for (int maro_i = 0\n        maro_i < 3; ++maro_i) {}\n}",
        3, L"syntax error: missing ';' before identifier 'maro_i'", L"C2146").range.start.line == 3,
        "errors inside for headers retain their compiler coordinates");
    maro_state.Expect(maro_semicolon(L"int main() {\n    puts(\"x\"\n    return 0;\n}",
        3, maro_beforeReturn).range.start.line == 3,
        "unbalanced expressions are not given speculative semicolon positions");
    maro_state.Expect(maro_semicolon(L"int main() {\n    int maro_function()\n    return 0;\n}",
        3, maro_beforeReturn, L"C2143", Maro_Language::Cpp20).range.start.line == 3,
        "function declarations are not confused with call expressions");
    maro_state.Expect(maro_semicolon(L"int main() {\n    puts(R\"tag(a\"; return)tag\")\n    return 0;\n}",
        3, maro_beforeReturn, L"C2143", Maro_Language::Cpp20).range.start.line == 3,
        "raw string literals do not cause speculative token-based remapping");
    maro_state.Expect(maro_semicolon(L"int main() {\n    puts(\"x\")\n    return 0;\n}",
        3, L"syntax error: missing ';' before 'not_in_source'").range.start.line == 3,
        "compiler token names must match the actual reported source line before remapping");
}

void maro_TestOutputQueue(Maro_TestState& maro_state)
{
    maro_OutputQueue maro_queue(8);
    maro_queue.Push(L"abc");
    maro_queue.Push(L"def");
    maro_state.Expect(maro_queue.Take(2) == L"ab", "output queue drains in FIFO order");
    maro_state.Expect(maro_queue.Take(0).empty(), "zero-sized drain preserves pending output");
    maro_state.Expect(maro_queue.Take(8) == L"cdef", "partial drain preserves the remaining output");
    maro_state.Expect(maro_queue.Take(8).empty(), "output is not repeated after draining");
    maro_queue.Push(L"abcde");
    maro_queue.Push(L"fghij");
    maro_state.Expect(maro_queue.Take(20) == L"cdefghij", "overflow retains bounded recent output");
    maro_queue.Push(L"123456789AB");
    maro_state.Expect(maro_queue.Take(20) == L"456789AB", "oversized output retains its bounded tail");
    maro_queue.Push(L"pending");
    maro_queue.Clear();
    maro_state.Expect(maro_queue.Take(8).empty(), "clearing a pane removes pending output");
    maro_OutputQueue maro_disabled(0);
    maro_disabled.Push(L"ignored");
    maro_disabled.Push(L"");
    maro_state.Expect(maro_disabled.Take(8).empty(), "zero-capacity queues remain empty");

    maro_OutputQueue maro_shared(8'192);
    std::atomic_bool maro_finished{false};
    std::wstring maro_expected;
    for (int maro_index = 0; maro_index < 512; ++maro_index)
    {
        maro_expected.append(L"한글\n");
    }
    std::thread maro_producer([&] {
        for (int maro_index = 0; maro_index < 512; ++maro_index)
        {
            maro_shared.Push(L"한글\n");
        }
        maro_finished.store(true, std::memory_order_release);
    });
    std::wstring maro_received;
    while (!maro_finished.load(std::memory_order_acquire))
    {
        maro_received.append(maro_shared.Take(7));
        std::this_thread::yield();
    }
    maro_producer.join();
    maro_received.append(maro_shared.Take(8'192));
    maro_state.Expect(maro_received == maro_expected, "worker output reaches the UI drain without loss or duplication");
    maro_state.Expect(maro_shared.Take(8'192).empty(), "the UI drain consumes all queued worker output");
    maro_queue.maro_Reset(10);
    maro_queue.maro_PushVersion(9, L"stale");
    maro_queue.maro_PushVersion(10, L"current");
    maro_state.Expect(maro_queue.Take(8) == L"current", "only current source output reaches the live pane");
    maro_queue.maro_Reset(11);
    maro_queue.maro_PushVersion(10, L"late");
    maro_state.Expect(maro_queue.Take(8).empty(), "late callbacks cannot repopulate a new source queue");
    maro_queue.maro_Reset(0);
    maro_queue.maro_PushVersion(0, L"invalid");
    maro_state.Expect(maro_queue.Take(8).empty(), "cancelled source cannot emit output");
    maro_shared.maro_Reset(20);
    std::thread maro_stale([&] {
        for (int maro_index = 0; maro_index < 10'000; ++maro_index)
        {
            maro_shared.maro_PushVersion(20, L"old");
        }
    });
    maro_shared.maro_Reset(21);
    maro_shared.maro_PushVersion(21, L"new");
    maro_stale.join();
    maro_state.Expect(maro_shared.Take(8'192) == L"new", "source reset and concurrent callback insertion are atomic");
}

void maro_TestEngineLifecycle(Maro_TestState& maro_state)
{
    bool maro_started = true;
    bool maro_cancelled = true;
    bool maro_closed = true;
    bool maro_callbacks = true;
    std::chrono::steady_clock::duration maro_longestShutdown{};
    for (std::uint64_t maro_iteration = 0; maro_iteration < 64; ++maro_iteration)
    {
        {
            Maro_Engine maro_idle({});
            maro_idle.Shutdown();
            maro_idle.Shutdown();
        }

        std::mutex maro_mutex;
        std::condition_variable maro_condition;
        bool maro_entered = false;
        bool maro_continue = false;
        std::size_t maro_callbackCount = 0;
        Maro_Engine maro_engine([&](Maro_ResultEnvelope maro_result) {
            std::unique_lock maro_lock(maro_mutex);
            ++maro_callbackCount;
            if (maro_result.phase == Maro_Phase::Generating)
            {
                maro_entered = true;
                maro_condition.notify_one();
                maro_condition.wait(maro_lock, [&] { return maro_continue; });
            }
        });
        Maro_SourceRequest maro_request;
        maro_request.sourceVersion = 5'000 + maro_iteration;
        maro_request.sourceText = L"int main(void) { return 0; }";
        maro_request.execute = false;
        const std::uint64_t maro_id = maro_engine.Submit(maro_request);
        maro_started = maro_started && maro_id != 0 &&
            maro_engine.IsCurrent(maro_id, maro_request.sourceVersion);
        {
            std::unique_lock maro_lock(maro_mutex);
            maro_started = maro_condition.wait_for(
                maro_lock, std::chrono::seconds(2), [&] { return maro_entered; }) && maro_started;
        }
        maro_engine.Cancel();
        maro_cancelled = maro_cancelled && maro_engine.CurrentRequestId() == 0 &&
            !maro_engine.IsCurrent(maro_id, maro_request.sourceVersion);
        {
            std::lock_guard maro_lock(maro_mutex);
            maro_continue = true;
        }
        maro_condition.notify_one();
        const auto maro_before = std::chrono::steady_clock::now();
        maro_engine.Shutdown();
        const auto maro_elapsed = std::chrono::steady_clock::now() - maro_before;
        if (maro_elapsed > maro_longestShutdown)
        {
            maro_longestShutdown = maro_elapsed;
        }
        maro_engine.Shutdown();
        maro_closed = maro_closed && maro_engine.Submit(maro_request) == 0 &&
            maro_engine.CurrentRequestId() == 0;
        maro_callbacks = maro_callbacks && maro_callbackCount == 1;
        if (!maro_started)
        {
            break;
        }
    }
    maro_state.Expect(maro_started, "repeated engine startup accepts and dispatches work");
    maro_state.Expect(maro_cancelled, "cancellation invalidates active work before shutdown");
    maro_state.Expect(maro_closed, "repeated shutdown is safe and rejects new work");
    maro_state.Expect(maro_callbacks, "cancelled engine work cannot publish after shutdown");
    maro_state.Expect(
        maro_longestShutdown < std::chrono::seconds(3),
        "engine shutdown does not stall on startup or cancelled work");
}

void maro_TestCompiledSolutions(Maro_TestState& maro_state)
{
    struct maro_Case { const wchar_t* maro_code; const wchar_t* maro_source; };
    const maro_Case maro_cases[] = {
        {L"MARO-MACRO-VALUE", L"#define MARO_CAP = 100;\nint main(void){int a[MARO_CAP]={7};return a[0];}"},
        {L"MARO-MACRO-VALUE", L"#define MARO_CAP = 100;\nint main(void){int a=MARO_CAP;return a;}"},
        {L"MARO-MACRO-VALUE", L"#define MARO_CAP = 100;\nint a=MARO_CAP;\nint main(void){return a;}"},
        {L"MARO-MACRO-PRECEDENCE", L"#define MARO_SQUARE(x) x * x\nint main(void){return 100/MARO_SQUARE(2+3);}"},
        {L"MARO-STRING-COMPARE", L"#include <string.h>\nint main(void){char* a=\"Hello\";if(a==\"Hello\"){return 1;}return 0;}"},
        {L"MARO-ARRAY-ASSIGNMENT", L"#include <string.h>\nint main(void){char* a=\"Hello\";char b[6];b=a;return b[0];}"},
        {L"MARO-PRINTF-POINTER", L"#include <stdio.h>\nint main(void){int a=7;int* p=&a;printf(\"%d\",p);return 0;}"},
        {L"MARO-CONDITION-ASSIGNMENT", L"int main(void){int b=0;if(b=0){return 1;}return 0;}"},
        {L"MARO-MISSING-SEMICOLON", L"int main(void){return 0\n}"}
    };
    std::uint64_t maro_version = 8000;
    for (const auto& maro_case : maro_cases)
    {
        Maro_SourceRequest maro_request;
        maro_request.sourceVersion = ++maro_version;
        maro_request.sourcePath = L"maro_SolutionCheck.c";
        maro_request.sourceText = maro_case.maro_source;
        maro_request.language = Maro_Language::C17;
        maro_request.mode = Maro_SourceMode::Program;
        maro_request.execute = false;
        if (std::wstring_view(maro_case.maro_code) == L"MARO-MACRO-VALUE")
        {
            const auto maro_original = Maro_RunToCompletion(maro_request);
            bool maro_root = false, maro_cascade = false;
            if (maro_original)
                for (const auto& maro_diagnostic : maro_original->diagnostics)
                {
                    maro_root = maro_root || maro_diagnostic.code == L"MARO-MACRO-VALUE";
                    maro_cascade = maro_cascade || maro_diagnostic.code == L"C4431" || maro_diagnostic.code == L"C2075";
                }
            maro_state.Expect(maro_original && maro_root && !maro_cascade,
                "real compiler's malformed numeric macro cascade reduces to the proven root definition");
        }
        std::vector<Maro_Diagnostic> maro_findings;
        maro_ImproveDiagnostics(maro_request, maro_findings);
        bool maro_applied = false;
        for (const auto& maro_finding : maro_findings)
        {
            if (maro_finding.code != maro_case.maro_code || !maro_finding.fix || maro_finding.fix->edits.size() != 1) continue;
            const auto& maro_edit = maro_finding.fix->edits.front();
            maro_request.sourceText.replace(maro_edit.startOffsetUtf16, maro_edit.lengthUtf16, maro_edit.replacement);
            maro_applied = true;
            break;
        }
        maro_state.Expect(maro_applied, "compiler verification has a supported solution to apply");
        if (!maro_applied) continue;
        const auto maro_result = Maro_RunToCompletion(std::move(maro_request));
        const bool maro_success = maro_result && maro_result->status == Maro_Status::Success;
        maro_state.Expect(maro_success, "each proposed non-semicolon and semicolon repair passes the real C compiler without executing user code");
        if (!maro_success)
            std::wcerr << maro_case.maro_code << L": " << (maro_result ? maro_result->compilerOutput : L"compiler timed out") << L'\n';
    }
}

void Maro_TestEngineSuccess(Maro_TestState& state)
{
    const Maro_ToolchainInfo toolchain = Maro_DetectToolchain();
    state.Expect(toolchain.kind != Maro_ToolchainKind::None, "a C/C++ toolchain is detected");

    Maro_SourceRequest request;
    request.sourceVersion = 1'001;
    request.language = Maro_Language::C17;
    request.mode = Maro_SourceMode::Program;
    request.execute = true;
    request.sourceText =
        L"#include <stdio.h>\n"
        L"int main(void)\n"
        L"{\n"
        L"    printf(\"Maro engine integration OK\\n\");\n"
        L"    return 0;\n"
        L"}\n";

    const std::optional<Maro_ResultEnvelope> result = Maro_RunToCompletion(std::move(request));
    state.Expect(result.has_value(), "engine success case completes within 30 seconds");
    if (!result)
    {
        return;
    }

    state.Expect(result->phase == Maro_Phase::Completed, "engine reports a completed phase");
    state.Expect(result->status == Maro_Status::Success, "valid C17 program succeeds");
    state.Expect(result->sourceVersion == 1'001, "engine preserves the submitted source version");
    state.Expect(
        result->standardOutput.find(L"Maro engine integration OK") != std::wstring::npos,
        "engine captures program stdout");
    state.Expect(result->hasExitCode && result->exitCode == 0, "engine reports exit code zero");
    state.Expect(!result->compilerName.empty(), "engine identifies the compiler used");
    if (toolchain.kind == Maro_ToolchainKind::Msvc)
    {
        state.Expect(result->usedFallbackCompiler, "MSVC is reported as the fallback compiler");
    }
}

void Maro_TestEngineStreamingOutput(Maro_TestState& state)
{
    Maro_SourceRequest request;
    request.sourceVersion = 1'004;
    request.language = Maro_Language::Cpp20;
    request.mode = Maro_SourceMode::Program;
    request.execute = true;
    request.sourceText =
        L"#include <iostream>\n"
        L"#include <windows.h>\n"
        L"int main()\n"
        L"{\n"
        L"    std::ios::sync_with_stdio(false);\n"
        L"    const char maro_first[] = {static_cast<char>(0xED), static_cast<char>(0x95)};\n"
        L"    const char maro_last[] = {static_cast<char>(0x9C)};\n"
        L"    std::cout.write(maro_first, sizeof(maro_first));\n"
        L"    Sleep(100);\n"
        L"    std::cout.write(maro_last, sizeof(maro_last));\n"
        L"    std::cout << \" MARO_LIVE_FIRST\\n\";\n"
        L"    Sleep(900);\n"
        L"    std::cout << \"MARO_LIVE_LAST\\n\";\n"
        L"    return 0;\n"
        L"}\n";

    Maro_ResultTimeline timeline;
    Maro_Engine engine([&timeline](Maro_ResultEnvelope result) {
        timeline.Publish(std::move(result));
    });
    const std::uint64_t requestId = engine.Submit(std::move(request));
    const auto firstOutput = timeline.WaitForOutput(L"MARO_LIVE_FIRST", std::chrono::seconds(30));
    const auto completed = timeline.WaitForCompletion(std::chrono::seconds(30));
    const std::wstring liveOutput = timeline.LiveOutput();
    engine.Shutdown();

    state.Expect(requestId != 0, "streaming execution request is accepted");
    state.Expect(firstOutput.has_value(), "stdout is published before execution completes");
    state.Expect(completed.has_value(), "streaming execution completes");
    if (!completed)
    {
        return;
    }
    const std::size_t firstToken = completed->second.standardOutput.find(L"한 MARO_LIVE_FIRST");
    const std::size_t lastToken = completed->second.standardOutput.find(L"MARO_LIVE_LAST");
    state.Expect(completed->second.status == Maro_Status::Success, "streaming execution succeeds");
    state.Expect(
        firstToken != std::wstring::npos && lastToken > firstToken,
        "final stdout preserves split UTF-8 and output order");
    state.Expect(
        liveOutput == completed->second.standardOutput,
        "live stdout is lossless and is not duplicated");
    state.Expect(liveOutput.find(L'\uFFFD') == std::wstring::npos, "live UTF-8 has no replacement characters");
    state.Expect(completed->second.standardError.empty(), "streaming stdout does not enter stderr");
    if (firstOutput)
    {
        state.Expect(
            completed->first - *firstOutput >= std::chrono::milliseconds(600),
            "the first stdout update arrives while the child process is still running");
    }
}

void Maro_TestEngineCompileFailure(Maro_TestState& state)
{
    Maro_SourceRequest request;
    request.sourceVersion = 1'002;
    request.language = Maro_Language::C17;
    request.mode = Maro_SourceMode::Program;
    request.execute = true;
    request.sourceText =
        L"#include <stdio.h>\n"
        L"int main(void)\n"
        L"{\n"
        L"    printf(\"missing semicolon\\n\")\n"
        L"    return 0;\n"
        L"}\n";

    const std::optional<Maro_ResultEnvelope> result = Maro_RunToCompletion(std::move(request));
    state.Expect(result.has_value(), "engine compile-failure case completes within 30 seconds");
    if (!result)
    {
        return;
    }

    state.Expect(result->phase == Maro_Phase::Completed, "compile failure reaches a completed phase");
    state.Expect(result->status == Maro_Status::CompileFailed, "missing semicolon fails compilation");
    state.Expect(!result->compilerOutput.empty(), "compile failure preserves compiler output");
    state.Expect(result->standardError.empty(), "compile diagnostics are not mislabeled as program stderr");
    state.Expect(!result->diagnostics.empty(), "compile failure produces diagnostics");

    bool hasOriginalDiagnostic = false;
    bool hasUserLocation = false;
    for (const Maro_Diagnostic& diagnostic : result->diagnostics)
    {
        hasOriginalDiagnostic = hasOriginalDiagnostic || !diagnostic.originalDiagnostic.empty();
        hasUserLocation = hasUserLocation ||
            (diagnostic.range.start.line != 0 && !diagnostic.range.generated);
    }
    state.Expect(hasOriginalDiagnostic, "diagnostic preserves the original compiler message");
    state.Expect(hasUserLocation, "diagnostic maps to a user source location");
}

void Maro_TestEngineCpp20Success(Maro_TestState& state)
{
    Maro_SourceRequest request;
    request.sourceVersion = 1'003;
    request.language = Maro_Language::Cpp20;
    request.mode = Maro_SourceMode::Program;
    request.execute = true;
    request.sourceText =
        L"#include <iostream>\n"
        L"consteval int Maro_Answer() { return 42; }\n"
        L"int main()\n"
        L"{\n"
        L"    std::cout << \"Maro C++20 integration \" << Maro_Answer() << '\\n';\n"
        L"    return 0;\n"
        L"}\n";

    const std::optional<Maro_ResultEnvelope> result = Maro_RunToCompletion(std::move(request));
    state.Expect(result.has_value(), "engine C++20 case completes within 30 seconds");
    if (!result)
    {
        return;
    }

    state.Expect(result->phase == Maro_Phase::Completed, "C++20 case reaches a completed phase");
    state.Expect(result->status == Maro_Status::Success, "valid C++20 program succeeds");
    state.Expect(
        result->standardOutput.find(L"Maro C++20 integration 42") != std::wstring::npos,
        "engine captures C++20 program stdout");
    state.Expect(result->hasExitCode && result->exitCode == 0, "C++20 program exits with code zero");
}

void Maro_TestVisualStudioLanguageDetection(Maro_TestState& state)
{
    const std::vector<std::wstring_view> supportedExtensions = {
        L".c", L".cc", L".cp", L".cpp", L".cxx", L".c++", L".h", L".hh",
        L".hpp", L".hxx", L".inl", L".ipp", L".tpp", L".ixx", L".cppm", L".mpp"};
    for (const std::wstring_view extension : supportedExtensions)
    {
        const std::wstring path = L"C:\\Maro_Project\\Maro_Source" + std::wstring(extension);
        state.Expect(Maro_IsVisualStudioCppPath(path), "Visual Studio C/C++ extension is supported");
    }

    state.Expect(
        Maro_IsVisualStudioCppPath(L"C:\\Maro_Project\\Maro_Source.CPP"),
        "Visual Studio extension matching ignores case");
    state.Expect(!Maro_IsVisualStudioCppPath(L"Maro_Source.txt"), "text documents are unsupported");
    state.Expect(!Maro_IsVisualStudioCppPath(L"Maro_Source.rs"), "Rust documents are unsupported");
    state.Expect(!Maro_IsVisualStudioCppPath(L"Maro_Source"), "extensionless paths are unsupported");
    state.Expect(!Maro_IsVisualStudioCppPath(L""), "empty paths are unsupported");

    state.Expect(
        Maro_InferVisualStudioLanguage(L"maro_Source.c", L"C++", L"namespace Maro {}") ==
            Maro_Language::C17,
        "the .c extension has priority and selects C17");
    state.Expect(
        Maro_InferVisualStudioLanguage(L"maro_Source.cpp", L"C", L"_Generic(value, int: 1)") ==
            Maro_Language::Cpp20,
        "a C++ extension has priority and selects C++20");
    state.Expect(
        Maro_InferVisualStudioLanguage(L"maro_Source.h", L"C++", L"_Static_assert(1, \"ok\");") ==
            Maro_Language::C17,
        "a C indicator in .h selects C17 before the document language");
    state.Expect(
        Maro_InferVisualStudioLanguage(L"maro_Source.h", L"C", L"namespace Maro { class Value {}; }") ==
            Maro_Language::Cpp20,
        "a C++ indicator in .h selects C++20 before the document language");
    state.Expect(
        Maro_InferVisualStudioLanguage(
            L"maro_Source.h",
            L"C++",
            L"_Generic(value, int: 1)\nnamespace Maro {}") == Maro_Language::C17,
        "C indicators take priority when a header contains mixed indicators");
    state.Expect(
        Maro_InferVisualStudioLanguage(L"maro_Source.h", L"C") == Maro_Language::C17,
        "the Visual Studio C document language resolves an otherwise ambiguous header");
    state.Expect(
        Maro_InferVisualStudioLanguage(L"maro_Source.h", L"C++") == Maro_Language::Cpp20,
        "the Visual Studio C++ document language resolves an ambiguous header");
    state.Expect(
        Maro_InferVisualStudioLanguage(L"maro_Source.h") == Maro_Language::Cpp20,
        "an ambiguous header defaults to C++20");
    state.Expect(
        Maro_InferVisualStudioLanguage(L"Maro_Untitled", L"C") == Maro_Language::C17,
        "an extensionless C document is inferred from its Visual Studio language");
    state.Expect(
        Maro_InferVisualStudioLanguage(L"Maro_Untitled", L"cpp") == Maro_Language::Cpp20,
        "an extensionless C++ document is inferred from its Visual Studio language");
    state.Expect(
        !Maro_InferVisualStudioLanguage(L"Maro_Source.txt", L"C++").has_value(),
        "an unsupported extension is not overridden by the document language");
    state.Expect(
        !Maro_InferVisualStudioLanguage(L"Maro_Untitled", L"Plain Text").has_value(),
        "an extensionless non-C/C++ document is not inferred");
}

void Maro_TestOptionalVisualStudioRead(Maro_TestState& state)
{
    wchar_t enabled[16]{};
    const DWORD length = GetEnvironmentVariableW(
        L"MARO_CLIVE_TEST_VISUAL_STUDIO",
        enabled,
        static_cast<DWORD>(std::size(enabled)));
    const std::wstring_view mode =
        length > 0 && length < std::size(enabled) ? std::wstring_view(enabled) : std::wstring_view{};
    if (mode != L"1" && mode != L"apply")
    {
        std::cout << "[SKIP] Visual Studio active-document smoke test\n";
        return;
    }

    constexpr std::uint64_t sourceVersion = 9'001;
    const Maro_VisualStudio visualStudio;
    const Maro_VisualStudioReadResult result = visualStudio.ReadActiveDocument(sourceVersion);
    state.Expect(static_cast<bool>(result), "Visual Studio active document can be read through COM");
    if (!result)
    {
        std::cerr << "[INFO] Visual Studio smoke status="
                  << static_cast<int>(result.status)
                  << ", instances=" << result.instancesInspected
                  << ", message=" << Maro_WideToUtf8(result.message) << '\n';
        return;
    }

    state.Expect(
        result.snapshot->sourceVersion == sourceVersion,
        "Visual Studio snapshot preserves the requested source version");
    state.Expect(
        Maro_IsVisualStudioCppPath(result.snapshot->path),
        "Visual Studio snapshot has a supported C/C++ path");
    state.Expect(
        Maro_InferVisualStudioLanguage(
            result.snapshot->path,
            result.snapshot->documentLanguage,
            result.snapshot->text) == result.snapshot->language,
        "Visual Studio snapshot language agrees with pure language inference");

    if (mode == L"apply")
    {
        const Maro_VisualStudioApplyResult applied = visualStudio.ApplyFullText(
            *result.snapshot,
            sourceVersion,
            result.snapshot->text);
        state.Expect(
            static_cast<bool>(applied),
            "Visual Studio accepts a same-text snapshot-validated replacement");
        if (applied)
        {
            state.Expect(applied.snapshotAfter.has_value(), "Visual Studio apply returns a verified snapshot");
            state.Expect(
                applied.snapshotAfter->path == result.snapshot->path,
                "Visual Studio apply preserves the active document path");
            state.Expect(
                applied.snapshotAfter->text == result.snapshot->text,
                "Visual Studio same-text apply preserves document contents");
        }
        else
        {
            std::cerr << "[INFO] Visual Studio apply status="
                      << static_cast<int>(applied.status)
                      << ", message=" << Maro_WideToUtf8(applied.message) << '\n';
        }
    }
}

void maro_TestUpdates(Maro_TestState& state)
{
    for (const std::string_view maro_text : {
             "v0.0.0", "v1.2.2", "v1.10.0", "v4294967295.4294967295.4294967295"})
    {
        const auto maro_version = maro_ParseSemanticVersion(maro_text);
        state.Expect(maro_version.has_value(), "valid numeric update version is accepted");
        state.Expect(
            maro_version && maro_FormatSemanticVersion(*maro_version) == maro_text,
            "numeric update version round-trips without truncation");
    }
    for (const std::string_view maro_text : {
             "", "v", "1.2.2", "V1.2.2", "v1.2", "v1.2.3.4", "v.2.3", "v1..3",
             "v1.2.", "v01.2.3", "v1.02.3", "v1.2.03", "v+1.2.3", "v1.-2.3",
             "v1.2.3-rc1", "v1.2.3+build", " v1.2.3", "v1.2.3\n", "v1.2.x",
             "v4294967296.0.0", "v0.4294967296.0", "v0.0.4294967296",
             "v999999999999999999999999999999.0.0"})
    {
        state.Expect(!maro_ParseSemanticVersion(maro_text), "malformed or overflowing version is rejected");
    }
    state.Expect(maro_CompareSemanticVersions({1, 9, 9}, {1, 10, 0}) < 0, "minor version comparison is numeric");
    state.Expect(maro_CompareSemanticVersions({1, 2, 9}, {1, 2, 10}) < 0, "patch version comparison is numeric");
    state.Expect(maro_CompareSemanticVersions({2, 0, 0}, {1, 99, 99}) > 0, "major version has priority");
    state.Expect(maro_CompareSemanticVersions({1, 3, 0}, {1, 2, 99}) > 0, "minor version has priority");
    state.Expect(maro_CompareSemanticVersions({1, 2, 2}, {1, 2, 2}) == 0, "identical versions compare equal");

    const std::string maro_asset =
        R"({"name":"maro_CLive_Maro_v1.2.2.exe","size":420352,)"
        R"("browser_download_url":"https://github.com/maro-comu/Maro_CLive/releases/download/v1.2.2/maro_CLive_Maro_v1.2.2.exe",)"
        R"("digest":"sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"})";
    const std::string maro_json =
        R"({"tag_name":"v1.2.2","draft":false,"prerelease":false,"assets":[)" + maro_asset +
        R"(,{"name":"maro_CLive_Maro_v1.2.2.zip","size":596075}],)"
        R"("body":"\uC5C5\uB370\uC774\uD2B8 \uD83D\uDE80","author":{"id":42,"flags":[true,null,-1.25e+2]}})";
    Maro_UpdateRelease maro_release;
    std::string maro_error = "previous error";
    state.Expect(maro_ParseUpdateReleaseJson(maro_json, maro_release, maro_error), "published GitHub release is accepted");
    state.Expect(maro_error.empty(), "successful release parse clears the previous error");
    state.Expect(
        maro_release.tag == "v1.2.2" && maro_release.version.major == 1 &&
            maro_release.version.minor == 2 && maro_release.version.patch == 2,
        "release contains the expected numeric version");
    state.Expect(
        maro_release.asset.fileName == L"maro_CLive_Maro_v1.2.2.exe" && maro_release.asset.size == 420352,
        "updater selects the versioned EXE instead of the ZIP");
    state.Expect(
        maro_release.asset.sha256[0] == 0x01 && maro_release.asset.sha256[7] == 0xef &&
            maro_release.asset.sha256[31] == 0xef,
        "release SHA-256 is decoded into bytes");
    const auto maro_replace = [&maro_json](std::string_view maro_from, std::string_view maro_to) {
        std::string maro_result = maro_json;
        maro_result.replace(maro_result.find(maro_from), maro_from.size(), maro_to);
        return maro_result;
    };
    const std::vector<std::pair<std::string, std::string_view>> maro_invalid = {
        {maro_replace(R"("draft":false)", R"("draft":true)"), "draft release is rejected"},
        {maro_replace(R"("prerelease":false)", R"("prerelease":true)"), "prerelease is rejected"},
        {maro_replace(R"("draft":false,)", ""), "missing draft flag is rejected"},
        {maro_replace(R"("prerelease":false)", R"("prerelease":"false")"), "non-boolean release flag is rejected"},
        {maro_replace(R"("tag_name":"v1.2.2")", R"("tag_name":"v1.2.2-rc1")"), "non-stable release tag is rejected"},
        {maro_replace("https://github.com/", "http://github.com/"), "HTTP asset URL is rejected"},
        {maro_replace("github.com/", "github.com.example/"), "lookalike download host is rejected"},
        {maro_replace("maro-comu/Maro_CLive/releases", "other/Maro_CLive/releases"), "another repository asset is rejected"},
        {maro_replace("download/v1.2.2/", "download/v1.2.1/"), "asset from another release is rejected"},
        {maro_replace(R"(.exe","digest")", R"(.exe?download=1","digest")"), "unexpected asset URL query is rejected"},
        {maro_replace(R"(,"digest":"sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")", ""), "missing SHA-256 is rejected"},
        {maro_replace(R"("sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")", "null"), "null SHA-256 is rejected"},
        {maro_replace("sha256:", "sha512:"), "wrong hash algorithm is rejected"},
        {maro_replace("sha256:0123", "sha256:012"), "short SHA-256 is rejected"},
        {maro_replace("sha256:0123", "sha256:g123"), "non-hex SHA-256 is rejected"},
        {maro_replace("420352", "0"), "zero-size installer is rejected"},
        {maro_replace("420352", "-1"), "negative installer size is rejected"},
        {maro_replace("420352", "18446744073709551616"), "overflowing installer size is rejected"},
        {maro_replace("420352", "420352.0"), "fractional installer size is rejected"},
        {maro_replace(maro_asset, maro_asset + "," + maro_asset), "duplicate matching installers are rejected"},
        {R"({"tag_name":"v1.2.2","draft":false,"prerelease":false,"assets":[]})", "empty assets are rejected"},
        {maro_json + "false", "trailing release data is rejected"},
        {maro_json.substr(0, maro_json.size() - 1), "truncated release JSON is rejected"},
        {maro_replace(R"(\uD83D\uDE80)", R"(\uD83D\u0041)"), "malformed Unicode escape is rejected"}};
    for (const auto& [maro_input, maro_message] : maro_invalid)
    {
        maro_error.clear();
        state.Expect(!maro_ParseUpdateReleaseJson(maro_input, maro_release, maro_error), maro_message);
        state.Expect(!maro_error.empty(), "rejected update metadata supplies an error");
    }
    state.Expect(
        maro_release.tag == "v1.2.2" && maro_release.asset.size == 420352,
        "failed parsing preserves the last valid release");

    const std::atomic_bool maro_cancelled{true};
    state.Expect(
        maro_CheckForUpdate({1, 2, 2}, &maro_cancelled).status == Maro_UpdateCheckStatus::Cancelled,
        "cancelled update check returns without a network request");
    state.Expect(
        maro_DownloadAndLaunchUpdate(maro_release, &maro_cancelled).status == Maro_UpdateInstallStatus::Cancelled,
        "cancelled update does not download or launch an installer");
}

void maro_TestOptionalUpdateCheck(Maro_TestState& state)
{
    wchar_t maro_enabled[2]{};
    if (GetEnvironmentVariableW(L"MARO_CLIVE_TEST_UPDATE", maro_enabled, 2) != 1 || maro_enabled[0] != L'1')
    {
        std::cout << "[SKIP] GitHub live update check\n";
        return;
    }
    const auto maro_result = maro_CheckForUpdate({0, 0, 0});
    state.Expect(maro_result.status == Maro_UpdateCheckStatus::Available, "GitHub reports a newer published version");
    if (maro_result.status != Maro_UpdateCheckStatus::Available)
    {
        std::cerr << "[INFO] GitHub update check: " << Maro_WideToUtf8(maro_result.error) << '\n';
        return;
    }
    state.Expect(
        maro_CheckForUpdate(maro_result.release.version).status == Maro_UpdateCheckStatus::Current,
        "the published version is reported as current");
    std::cout << "[INFO] GitHub latest release: " << maro_result.release.tag << '\n';
    std::uint64_t maro_downloaded = 0;
    const auto maro_download = maro_DownloadUpdate(maro_result.release, nullptr,
        [&maro_downloaded](std::uint64_t received, std::uint64_t) { maro_downloaded = received; });
    state.Expect(maro_download.status == Maro_UpdateInstallStatus::Downloaded,
        "published installer downloads and passes SHA-256 verification");
    if (maro_download.status != Maro_UpdateInstallStatus::Downloaded)
    {
        std::cerr << "[INFO] Download: " << Maro_WideToUtf8(maro_download.error) << '\n';
        return;
    }
    const std::filesystem::path maro_path(maro_download.installerPath);
    state.Expect(std::filesystem::file_size(maro_path) == maro_result.release.asset.size,
        "verified installer size matches release metadata");
    state.Expect(maro_downloaded == maro_result.release.asset.size,
        "download progress reaches the complete asset size");
    state.Expect(std::filesystem::remove(maro_path), "download test removes its installer");
    state.Expect(std::filesystem::remove(maro_path.parent_path()), "download test removes its empty directory");
    auto maro_invalid = maro_result.release;
    maro_invalid.asset.sha256[0] ^= 0xff;
    const auto maro_corrupt = maro_DownloadUpdate(maro_invalid);
    state.Expect(maro_corrupt.status == Maro_UpdateInstallStatus::Failed && maro_corrupt.installerPath.empty(),
        "hash mismatch rejects and removes the downloaded installer");
    std::atomic_bool maro_cancel{false};
    const auto maro_cancelled = maro_DownloadUpdate(maro_result.release, &maro_cancel,
        [&maro_cancel](std::uint64_t, std::uint64_t) { maro_cancel.store(true); });
    state.Expect(maro_cancelled.status == Maro_UpdateInstallStatus::Cancelled && maro_cancelled.installerPath.empty(),
        "in-progress cancellation removes the partial installer");
}
}

bool maro_TestDiagnosticWindow();
bool maro_TestDeferredCommands();
bool maro_TestSourceWindow();
bool maro_TestCodeDiagnostics();
bool maro_TestDiagnosticGuidance();

int maro_RunProcessInputChild(int, char**);
void maro_TestProcessInput(const std::function<void(bool, std::string_view)>&);
void maro_TestProjects(const std::function<void(bool, std::string_view)>&);
void maro_TestSourceInsight(const std::function<void(bool, std::string_view)>&);
void maro_TestEncoding(const std::function<void(bool, std::string_view)>&);
void maro_TestDiagnosticFilter(const std::function<void(bool, std::string_view)>&);
void maro_TestSafeFixes(const std::function<void(bool, std::string_view)>&);

int main(int maro_argc, char** maro_argv)
{
    const int maro_child = maro_RunProcessInputChild(maro_argc, maro_argv);
    if (maro_child >= 0) return maro_child;
    Maro_TestState state;

    try
    {
        maro_TestSourceInsight([&state](bool maro_ok, std::string_view maro_name) { state.Expect(maro_ok, maro_name); });
        maro_TestEncoding([&state](bool maro_ok, std::string_view maro_name) { state.Expect(maro_ok, maro_name); });
        maro_TestDiagnosticFilter([&state](bool maro_ok, std::string_view maro_name) { state.Expect(maro_ok, maro_name); });
        maro_TestSafeFixes([&state](bool maro_ok, std::string_view maro_name) { state.Expect(maro_ok, maro_name); });
        maro_TestProcessInput([&state](bool maro_ok, std::string_view maro_name) { state.Expect(maro_ok, std::string(maro_name)); });
        maro_TestProjects([&state](bool maro_ok, std::string_view maro_name) { state.Expect(maro_ok, maro_name); });
        state.Expect(maro_TestDeferredCommands(), "400 menu commands return without querying UI services or starting work");
        state.Expect(maro_TestSourceWindow(), "right source pane groups and navigation preserve exact source coordinates");
        state.Expect(maro_TestCodeDiagnostics(), "source-aware diagnostics and exact proposed replacements");
        state.Expect(maro_TestDiagnosticGuidance(), "compiler guidance describes specific safe remediation and intent limits");
        state.Expect(maro_TestDiagnosticWindow(), "diagnostic pane renders split read-only views, findings, resize and reopen");
        Maro_TestUtfConversions(state);
        Maro_TestTextCoordinates(state);
        Maro_TestTextUtilities(state);
        Maro_TestCommandLineQuoting(state);
        Maro_TestMainDetection(state);
        Maro_TestSourceGenerationAndMapping(state);
        maro_TestCompilerDiagnostics(state);
        maro_TestOutputQueue(state);
        maro_TestEngineLifecycle(state);
        maro_TestCompiledSolutions(state);
        Maro_TestEngineSuccess(state);
        Maro_TestEngineStreamingOutput(state);
        Maro_TestEngineCompileFailure(state);
        Maro_TestEngineCpp20Success(state);
        Maro_TestVisualStudioLanguageDetection(state);
        Maro_TestOptionalVisualStudioRead(state);
        maro_TestUpdates(state);
        maro_TestOptionalUpdateCheck(state);
    }
    catch (const std::exception& error)
    {
        ++state.failures;
        std::cerr << "[FAIL] unexpected exception: " << error.what() << '\n';
    }
    catch (...)
    {
        ++state.failures;
        std::cerr << "[FAIL] unexpected non-standard exception\n";
    }

    if (state.failures == 0)
    {
        std::cout << "[PASS] " << state.checks << " checks\n";
        return 0;
    }

    std::cerr << "[FAIL] " << state.failures << " of " << state.checks << " checks failed\n";
    return 1;
}
