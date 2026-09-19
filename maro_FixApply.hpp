#pragma once

#include "maro_Models.hpp"
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
