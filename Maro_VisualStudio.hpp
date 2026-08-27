#pragma once

#include "Maro_Models.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class Maro_VisualStudioStatus
{
    Success,
    ComUnavailable,
    NotRunning,
    NoActiveDocument,
    UnsupportedDocument,
    Busy,
    AccessDenied,
    Disconnected,
    ReadOnly,
    SourceVersionMismatch,
    PathMismatch,
    ContentMismatch,
    ReplaceFailed,
    VerificationFailed,
    AutomationError
};

struct Maro_VisualStudioSnapshot
{
    std::uint64_t sourceVersion = 0;
    std::wstring instanceMoniker;
    std::wstring path;
    std::wstring name;
    std::wstring text;
    std::wstring documentLanguage;
    Maro_Language language = Maro_Language::Cpp20;
    std::uint32_t processId = 0;
    bool saved = false;
    bool readOnly = false;
};

struct Maro_VisualStudioReadResult
{
    Maro_VisualStudioStatus status = Maro_VisualStudioStatus::AutomationError;
    std::wstring message;
    std::optional<Maro_VisualStudioSnapshot> snapshot;
    std::size_t instancesInspected = 0;

    explicit operator bool() const noexcept
    {
        return status == Maro_VisualStudioStatus::Success && snapshot.has_value();
    }
};

struct Maro_VisualStudioApplyResult
{
    Maro_VisualStudioStatus status = Maro_VisualStudioStatus::AutomationError;
    std::wstring message;
    std::optional<Maro_VisualStudioSnapshot> snapshotAfter;

    explicit operator bool() const noexcept
    {
        return status == Maro_VisualStudioStatus::Success;
    }
};

bool Maro_IsVisualStudioCppPath(std::wstring_view path) noexcept;

std::optional<Maro_Language> Maro_InferVisualStudioLanguage(
    std::wstring_view path,
    std::wstring_view documentLanguage = {},
    std::wstring_view text = {}) noexcept;

class Maro_VisualStudio
{
public:
    // Reads the in-memory TextDocument buffer; the document does not need to be saved.
    Maro_VisualStudioReadResult ReadActiveDocument(
        std::uint64_t sourceVersion,
        std::wstring_view preferredInstanceMoniker = {}) const;

    // Re-opens the same ROT instance and validates sourceVersion, active path and
    // complete in-memory text before replacing the entire TextDocument buffer.
    Maro_VisualStudioApplyResult ApplyFullText(
        const Maro_VisualStudioSnapshot& expected,
        std::uint64_t currentSourceVersion,
        std::wstring_view replacementText) const;
};
