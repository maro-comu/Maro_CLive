#include "Maro_Analyzer.hpp"
#include "Maro_Engine.hpp"
#include "Maro_Process.hpp"
#include "Maro_Text.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <exception>
#include <iostream>
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

    const std::wstring commandLine = Maro_BuildWindowsCommandLine(
        L"C:\\Program Files\\CLive_Maro\\Maro_CLive_Maro.exe",
        {L"--name", L"hello world"});
    state.Expect(
        commandLine ==
            L"\"C:\\Program Files\\CLive_Maro\\Maro_CLive_Maro.exe\" --name \"hello world\"",
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

    const Maro_GeneratedSource unchanged = Maro_BuildGeneratedSource(program);
    state.Expect(!unchanged.wrapped, "a complete program is not wrapped");
    state.Expect(
        unchanged.text.find(program.sourceText) != std::wstring::npos,
        "a complete program remains in generated source");

    Maro_SourceRequest snippet;
    snippet.sourceVersion = 8;
    snippet.language = Maro_Language::Cpp20;
    snippet.mode = Maro_SourceMode::Snippet;
    snippet.sourceText = L"int value = 40;\nvalue += 2;";

    const Maro_GeneratedSource generated = Maro_BuildGeneratedSource(snippet);
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
    const Maro_GeneratedSource blankGenerated = Maro_BuildGeneratedSource(leadingBlank);

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
    const Maro_GeneratedSource emptyGenerated = Maro_BuildGeneratedSource(emptySnippet);
    state.Expect(
        emptyGenerated.text.starts_with(L"\nint main()"),
        "an empty snippet preserves its blank user line before the wrapper");
    state.Expect(
        !emptyGenerated.lineMap.empty() &&
            emptyGenerated.lineMap.front().generatedLine == 1 &&
            emptyGenerated.lineMap.front().userLine == 1,
        "an empty snippet retains a source-map entry for its blank line");
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
} // namespace

int main()
{
    Maro_TestState state;

    try
    {
        Maro_TestUtfConversions(state);
        Maro_TestTextCoordinates(state);
        Maro_TestTextUtilities(state);
        Maro_TestCommandLineQuoting(state);
        Maro_TestMainDetection(state);
        Maro_TestSourceGenerationAndMapping(state);
        Maro_TestEngineSuccess(state);
        Maro_TestEngineCompileFailure(state);
        Maro_TestEngineCpp20Success(state);
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
