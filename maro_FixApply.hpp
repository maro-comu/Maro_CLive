#pragma once

#include "maro_Models.hpp"
#include "maro_CodeDiagnostics.hpp"
#include "maro_Analyzer.hpp"
#include <algorithm>
#include <optional>
#include <string_view>

inline std::optional<Maro_SourcePosition> maro_ValidateSemicolonEdit(
    const Maro_SourceRequest& maro_expected, const Maro_Diagnostic& maro_diagnostic,
    std::uint64_t maro_version, std::wstring_view maro_path, std::wstring_view maro_current)
{
    if (!maro_version || maro_diagnostic.sourceVersion != maro_version ||
        maro_expected.sourceVersion != maro_version || maro_expected.sourcePath != maro_path ||
        maro_diagnostic.sourcePath != maro_path || maro_expected.sourceText != maro_current ||
        !maro_diagnostic.fix || maro_diagnostic.fix->edits.size() != 1 || maro_diagnostic.range.generated)
        return std::nullopt;
    const auto& maro_edit = maro_diagnostic.fix->edits.front();
    if (maro_edit.sourceVersion != maro_version || maro_edit.expectedText.empty() ||
        maro_edit.lengthUtf16 != maro_edit.expectedText.size() ||
        maro_edit.replacement != maro_edit.expectedText + L";" ||
        maro_edit.startOffsetUtf16 > maro_current.size() ||
        maro_edit.lengthUtf16 > maro_current.size() - maro_edit.startOffsetUtf16 ||
        maro_current.substr(maro_edit.startOffsetUtf16, maro_edit.lengthUtf16) != maro_edit.expectedText)
        return std::nullopt;
    const auto maro_offset = maro_edit.startOffsetUtf16 + maro_edit.lengthUtf16;
    if ((maro_offset < maro_current.size() && maro_current[maro_offset] == L';') ||
        (maro_offset && maro_current[maro_offset - 1] >= 0xd800 && maro_current[maro_offset - 1] <= 0xdbff))
        return std::nullopt;
    Maro_SourcePosition maro_position{1, 1};
    for (std::size_t maro_index = 0; maro_index < maro_offset; ++maro_index)
    {
        if (maro_current[maro_index] == L'\r' || maro_current[maro_index] == L'\n')
        {
            if (maro_current[maro_index] == L'\r' && maro_index + 1 < maro_offset && maro_current[maro_index + 1] == L'\n') ++maro_index;
            ++maro_position.line;
            maro_position.column = 1;
        }
        else ++maro_position.column;
    }
    return maro_position;
}

struct maro_ValidatedFix
{
    Maro_SourcePosition maro_start;
    Maro_SourcePosition maro_end;
    std::wstring maro_replacement;
};

inline Maro_SourcePosition maro_OffsetPosition(std::wstring_view maro_text, std::size_t maro_offset)
{
    Maro_SourcePosition maro_position{1, 1};
    for (std::size_t maro_index = 0; maro_index < maro_offset; ++maro_index)
    {
        if (maro_text[maro_index] == L'\r' || maro_text[maro_index] == L'\n')
        {
            if (maro_text[maro_index] == L'\r' && maro_index + 1 < maro_offset && maro_text[maro_index + 1] == L'\n') ++maro_index;
            ++maro_position.line;
            maro_position.column = 1;
        }
        else ++maro_position.column;
    }
    return maro_position;
}

inline std::optional<maro_ValidatedFix> maro_ValidateFix(
    const Maro_SourceRequest& maro_expected, const Maro_Diagnostic& maro_diagnostic,
    std::uint64_t maro_version, std::wstring_view maro_path, std::wstring_view maro_current)
{
    if (!maro_version || maro_diagnostic.sourceVersion != maro_version ||
        maro_expected.sourceVersion != maro_version || maro_expected.sourcePath != maro_path ||
        maro_diagnostic.sourcePath != maro_path || maro_expected.sourceText != maro_current ||
        !maro_diagnostic.fix || maro_diagnostic.fix->edits.empty() ||
        maro_diagnostic.fix->edits.size() > 32 || maro_diagnostic.range.generated) return std::nullopt;
    const auto maro_sameEdits = [](const std::vector<Maro_TextEdit>& maro_left, const std::vector<Maro_TextEdit>& maro_right) {
        return maro_left.size() == maro_right.size() && std::equal(maro_left.begin(), maro_left.end(), maro_right.begin(),
            [](const Maro_TextEdit& maro_a, const Maro_TextEdit& maro_b) {
                return maro_a.sourceVersion == maro_b.sourceVersion && maro_a.startOffsetUtf16 == maro_b.startOffsetUtf16 &&
                    maro_a.lengthUtf16 == maro_b.lengthUtf16 && maro_a.expectedText == maro_b.expectedText && maro_a.replacement == maro_b.replacement;
            });
    };
    std::vector<Maro_Diagnostic> maro_approved;
    maro_ImproveDiagnostics(maro_expected, maro_approved);
    bool maro_known = std::any_of(maro_approved.begin(), maro_approved.end(), [&](const Maro_Diagnostic& maro_item) {
        return maro_item.fix && maro_sameEdits(maro_item.fix->edits, maro_diagnostic.fix->edits);
    });
    if (!maro_known) maro_known = maro_ValidateSemicolonEdit(maro_expected, maro_diagnostic, maro_version, maro_path, maro_current).has_value() &&
        maro_VerifyCompilerSemicolonFix(maro_expected, maro_diagnostic.fix->edits.front());
    if (!maro_known) return std::nullopt;
    auto maro_edits = maro_diagnostic.fix->edits;
    std::sort(maro_edits.begin(), maro_edits.end(), [](const Maro_TextEdit& maro_a, const Maro_TextEdit& maro_b) {
        return maro_a.startOffsetUtf16 < maro_b.startOffsetUtf16;
    });
    std::size_t maro_previousEnd = 0;
    for (const auto& maro_edit : maro_edits)
    {
        if (maro_edit.sourceVersion != maro_version || maro_edit.startOffsetUtf16 < maro_previousEnd ||
            maro_edit.startOffsetUtf16 > maro_current.size() || maro_edit.lengthUtf16 > maro_current.size() - maro_edit.startOffsetUtf16 ||
            maro_edit.expectedText.size() != maro_edit.lengthUtf16 || maro_edit.replacement.size() > 65536 ||
            maro_current.substr(maro_edit.startOffsetUtf16, maro_edit.lengthUtf16) != maro_edit.expectedText) return std::nullopt;
        maro_previousEnd = maro_edit.startOffsetUtf16 + maro_edit.lengthUtf16;
    }
    auto maro_start = maro_edits.front().startOffsetUtf16;
    auto maro_end = maro_previousEnd;
    if (maro_end - maro_start > 65536) return std::nullopt;
    std::wstring maro_replacement(maro_current.substr(maro_start, maro_end - maro_start));
    for (auto maro_it = maro_edits.rbegin(); maro_it != maro_edits.rend(); ++maro_it)
        maro_replacement.replace(maro_it->startOffsetUtf16 - maro_start, maro_it->lengthUtf16, maro_it->replacement);
    std::size_t maro_prefix = 0;
    while (maro_prefix < maro_replacement.size() && maro_start + maro_prefix < maro_end &&
        maro_current[maro_start + maro_prefix] == maro_replacement[maro_prefix]) ++maro_prefix;
    maro_start += maro_prefix;
    maro_replacement.erase(0, maro_prefix);
    while (!maro_replacement.empty() && maro_end > maro_start && maro_current[maro_end - 1] == maro_replacement.back())
    {
        --maro_end;
        maro_replacement.pop_back();
    }
    const auto maro_boundary = [&](std::size_t maro_offset) {
        return !maro_offset || maro_offset == maro_current.size() ||
            !((maro_current[maro_offset - 1] >= 0xd800 && maro_current[maro_offset - 1] <= 0xdbff &&
                maro_current[maro_offset] >= 0xdc00 && maro_current[maro_offset] <= 0xdfff) ||
                (maro_current[maro_offset - 1] == L'\r' && maro_current[maro_offset] == L'\n'));
    };
    if ((!maro_replacement.size() && maro_start == maro_end) || !maro_boundary(maro_start) || !maro_boundary(maro_end)) return std::nullopt;
    return maro_ValidatedFix{maro_OffsetPosition(maro_current, maro_start), maro_OffsetPosition(maro_current, maro_end), std::move(maro_replacement)};
}
