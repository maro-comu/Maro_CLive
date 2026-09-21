#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <vector>

class maro_ProcessInput;

enum class Maro_Language
{
    C17,
    Cpp20
};

enum class Maro_SourceMode
{
    Program,
    Snippet
};

enum class Maro_Phase
{
    Idle,
    Debouncing,
    Analyzing,
    Generating,
    Compiling,
    Running,
    Completed
};

enum class Maro_Status
{
    Pending,
    Success,
    Cancelled,
    Stale,
    CompileFailed,
    RuntimeFailed,
    TimedOut,
    LimitExceeded,
    ToolchainMissing,
    SandboxUnavailable,
    InternalError
};

enum class Maro_Severity
{
    Info,
    Warning,
    Error,
    Fatal
};

enum class Maro_Evidence
{
    RuntimeObservation,
    StaticAnalysis,
    Conditional,
    NotRun,
    Unknown
};

struct Maro_SourcePosition
{
    std::size_t line = 0;
    std::size_t column = 0;
};

struct Maro_SourceRange
{
    Maro_SourcePosition start;
    Maro_SourcePosition end;
    bool generated = false;
};

struct Maro_TextEdit
{
    std::uint64_t sourceVersion = 0;
    std::size_t startOffsetUtf16 = 0;
    std::size_t lengthUtf16 = 0;
    std::wstring replacement;
    std::wstring expectedText;
};

struct Maro_FixSuggestion
{
    std::wstring description;
    std::vector<Maro_TextEdit> edits;
};

struct Maro_Diagnostic
{
    std::uint64_t sourceVersion = 0;
    std::wstring findingId;
    std::wstring code;
    std::wstring analyzer;
    std::wstring analyzerVersion;
    Maro_Severity severity = Maro_Severity::Info;
    Maro_Evidence evidence = Maro_Evidence::StaticAnalysis;
    Maro_SourceRange range;
    std::wstring sourcePath;
    std::wstring friendlyMessage;
    std::wstring originalDiagnostic;
    std::optional<Maro_FixSuggestion> fix;
    std::size_t maro_relatedCount = 0;
};

struct Maro_SourceRequest
{
    std::uint64_t sourceVersion = 0;
    std::wstring sourceText;
    std::wstring sourcePath;
    std::wstring standardInput;
    Maro_Language language = Maro_Language::C17;
    Maro_SourceMode mode = Maro_SourceMode::Program;
    bool execute = true;
    std::shared_ptr<maro_ProcessInput> maro_input;
    std::wstring maro_projectPath;
    std::wstring maro_configuration;
    std::wstring maro_platform;
    std::wstring maro_msbuildPath;
    std::wstring maro_solutionPath;
    unsigned maro_outputCodePage = 0;
    bool maro_background = false;
};

struct Maro_SourceMapEntry
{
    std::size_t generatedLine = 0;
    std::size_t userLine = 0;
};

struct Maro_GeneratedSource
{
    std::wstring text;
    std::vector<Maro_SourceMapEntry> lineMap;
    bool wrapped = false;
    std::wstring notice;
};

struct Maro_ExecutionLimits
{
    std::uint32_t analysisWallMilliseconds = 5'000;
    std::uint32_t compileWallMilliseconds = 12'000;
    std::uint32_t runWallMilliseconds = 3'000;
    std::uint64_t compileMemoryBytes = 1ull << 30;
    std::uint64_t runMemoryBytes = 256ull << 20;
    std::size_t compilerOutputBytes = 2u << 20;
    std::size_t standardOutputBytes = 1u << 20;
    std::size_t standardErrorBytes = 1u << 20;
    std::size_t standardInputBytes = 64u << 10;
    std::size_t sourceBytes = 1u << 20;
};

struct Maro_ResultEnvelope
{
    std::uint64_t requestId = 0;
    std::uint64_t executionId = 0;
    std::uint64_t sourceVersion = 0;
    std::uint64_t sourceHash = 0;
    Maro_Phase phase = Maro_Phase::Idle;
    Maro_Status status = Maro_Status::Pending;
    std::wstring statusText;
    std::wstring generatedSource;
    std::wstring standardOutput;
    std::wstring standardError;
    std::wstring compilerOutput;
    std::vector<Maro_Diagnostic> diagnostics;
    std::wstring compilerName;
    std::wstring compilerVersion;
    std::uint32_t exitCode = 0;
    bool hasExitCode = false;
    bool usedFallbackCompiler = false;
    bool snippetWrapped = false;
    bool resourceLimitsApplied = false;
    std::uint64_t maro_compileMilliseconds = 0;
    std::uint64_t maro_runMilliseconds = 0;
};
