#include "maro_DiagnosticFilter.hpp"

#include <algorithm>
#include <cwctype>
#include <limits>
#include <map>
#include <string_view>
#include <tuple>

namespace
{
using maro_Context = std::tuple<std::uint64_t, std::wstring, std::wstring, std::wstring,
    Maro_Severity, Maro_Evidence, bool>;
using maro_Location = std::tuple<std::wstring, std::size_t, std::size_t>;
using maro_CascadeKey = std::tuple<maro_Context, maro_Location, std::wstring>;
using maro_DuplicateKey = std::tuple<maro_Context, std::wstring, std::size_t, std::size_t,
    std::size_t, std::size_t, std::wstring>;

std::wstring maro_NormalizeDiagnosticPath(std::wstring_view maro_path)
{
    std::wstring maro_result(maro_path);
    for (auto& maro_character : maro_result)
    {
        maro_character = maro_character == L'/' ? L'\\' : std::towlower(maro_character);
    }
    return maro_result;
}

maro_Context maro_DiagnosticContext(const Maro_Diagnostic& maro_diagnostic)
{
    return {maro_diagnostic.sourceVersion, maro_NormalizeDiagnosticPath(maro_diagnostic.sourcePath),
        maro_diagnostic.analyzer, maro_diagnostic.analyzerVersion, maro_diagnostic.severity,
        maro_diagnostic.evidence, maro_diagnostic.range.generated};
}

bool maro_ReadPosition(std::wstring_view maro_text, std::size_t& maro_value)
{
    if (maro_text.empty())
    {
        return false;
    }
    maro_value = 0;
    for (const wchar_t maro_character : maro_text)
    {
        if (maro_character < L'0' || maro_character > L'9' ||
            maro_value > ((std::numeric_limits<std::size_t>::max)() - (maro_character - L'0')) / 10)
        {
            return false;
        }
        maro_value = maro_value * 10 + (maro_character - L'0');
    }
    return maro_value != 0;
}

struct maro_SyntaxDiagnostic
{
    maro_Location maro_location;
    std::wstring maro_missing;
    std::wstring maro_target;
    bool maro_valid = false;
};

maro_SyntaxDiagnostic maro_ReadSyntaxDiagnostic(const Maro_Diagnostic& maro_diagnostic)
{
    maro_SyntaxDiagnostic maro_result;
    const auto& maro_code = maro_diagnostic.code;
    if (maro_diagnostic.severity != Maro_Severity::Error ||
        maro_diagnostic.evidence != Maro_Evidence::StaticAnalysis ||
        (maro_code != L"C2143" && maro_code != L"C2146" && maro_code != L"C2059" &&
            maro_code != L"C2061" && maro_code != L"C2447"))
    {
        return maro_result;
    }
    const std::wstring_view maro_original(maro_diagnostic.originalDiagnostic);
    const auto maro_codePosition = maro_original.rfind(L" " + maro_code + L":");
    if (maro_codePosition == std::wstring_view::npos)
    {
        return maro_result;
    }
    const auto maro_close = maro_original.rfind(L')', maro_codePosition);
    const auto maro_open = maro_original.rfind(L'(', maro_close);
    if (maro_close == std::wstring_view::npos || maro_open == std::wstring_view::npos || maro_open == 0)
    {
        return maro_result;
    }
    const auto maro_position = maro_original.substr(maro_open + 1, maro_close - maro_open - 1);
    const auto maro_comma = maro_position.find(L',');
    std::size_t maro_line = 0;
    std::size_t maro_column = 0;
    if (!maro_ReadPosition(maro_position.substr(0, maro_comma), maro_line) ||
        (maro_comma != std::wstring_view::npos && !maro_ReadPosition(maro_position.substr(maro_comma + 1), maro_column)))
    {
        return maro_result;
    }
    const auto maro_message = maro_original.substr(maro_codePosition + maro_code.size() + 2);
    std::vector<std::wstring> maro_tokens;
    for (std::size_t maro_offset = 0; maro_offset < maro_message.size();)
    {
        const auto maro_start = maro_message.find(L'\'', maro_offset);
        if (maro_start == std::wstring_view::npos)
        {
            break;
        }
        const auto maro_end = maro_message.find(L'\'', maro_start + 1);
        if (maro_end == std::wstring_view::npos || maro_end - maro_start > 513 || maro_tokens.size() == 2)
        {
            return maro_result;
        }
        maro_tokens.emplace_back(maro_message.substr(maro_start + 1, maro_end - maro_start - 1));
        maro_offset = maro_end + 1;
    }
    if (maro_code == L"C2143" || maro_code == L"C2146")
    {
        if (maro_tokens.size() != 2 || maro_tokens[0].size() != 1 ||
            std::wstring_view(L";)]}").find(maro_tokens[0][0]) == std::wstring_view::npos)
        {
            return maro_result;
        }
        maro_result.maro_missing = std::move(maro_tokens[0]);
    }
    else if (maro_tokens.size() != 1)
    {
        return maro_result;
    }
    if (maro_tokens.back().empty())
    {
        return maro_result;
    }
    maro_result.maro_location = {maro_NormalizeDiagnosticPath(maro_original.substr(0, maro_open)), maro_line, maro_column};
    maro_result.maro_target = std::move(maro_tokens.back());
    maro_result.maro_valid = true;
    return maro_result;
}

void maro_FoldDiagnostic(Maro_Diagnostic& maro_root, const Maro_Diagnostic& maro_related)
{
    const auto maro_available = (std::numeric_limits<std::size_t>::max)() - maro_root.maro_relatedCount;
    maro_root.maro_relatedCount += maro_related.maro_relatedCount >= maro_available
        ? maro_available : maro_related.maro_relatedCount + 1;
}

struct maro_CascadeRoot
{
    std::size_t maro_index = 0;
    std::wstring maro_missing;
    bool maro_ambiguous = false;
};
}

std::vector<Maro_Diagnostic> maro_GroupDiagnostics(const std::vector<Maro_Diagnostic>& maro_diagnostics)
{
    std::vector<Maro_Diagnostic> maro_unique;
    maro_unique.reserve(maro_diagnostics.size());
    std::map<maro_DuplicateKey, std::size_t> maro_seen;
    for (const auto& maro_diagnostic : maro_diagnostics)
    {
        const auto& maro_payload = maro_diagnostic.originalDiagnostic.empty()
            ? maro_diagnostic.friendlyMessage : maro_diagnostic.originalDiagnostic;
        if ((maro_diagnostic.severity == Maro_Severity::Error || maro_diagnostic.severity == Maro_Severity::Fatal) &&
            !maro_payload.empty())
        {
            maro_DuplicateKey maro_key{maro_DiagnosticContext(maro_diagnostic), maro_diagnostic.code,
                maro_diagnostic.range.start.line, maro_diagnostic.range.start.column,
                maro_diagnostic.range.end.line, maro_diagnostic.range.end.column, maro_payload};
            const auto [maro_found, maro_inserted] = maro_seen.try_emplace(std::move(maro_key), maro_unique.size());
            if (!maro_inserted)
            {
                maro_FoldDiagnostic(maro_unique[maro_found->second], maro_diagnostic);
                continue;
            }
        }
        maro_unique.push_back(maro_diagnostic);
    }

    std::vector<maro_SyntaxDiagnostic> maro_syntax;
    maro_syntax.reserve(maro_unique.size());
    std::vector<bool> maro_folded(maro_unique.size(), false);
    std::map<maro_CascadeKey, maro_CascadeRoot> maro_roots;
    for (std::size_t maro_index = 0; maro_index < maro_unique.size(); ++maro_index)
    {
        maro_syntax.push_back(maro_ReadSyntaxDiagnostic(maro_unique[maro_index]));
        const auto& maro_item = maro_syntax.back();
        if (!maro_item.maro_valid || maro_item.maro_missing.empty())
        {
            continue;
        }
        maro_CascadeKey maro_key{maro_DiagnosticContext(maro_unique[maro_index]), maro_item.maro_location, maro_item.maro_target};
        const auto [maro_found, maro_inserted] = maro_roots.try_emplace(std::move(maro_key),
            maro_CascadeRoot{maro_index, maro_item.maro_missing});
        if (!maro_inserted && maro_found->second.maro_missing != maro_item.maro_missing)
        {
            maro_found->second.maro_ambiguous = true;
        }
    }
    for (std::size_t maro_index = 0; maro_index < maro_unique.size(); ++maro_index)
    {
        const auto& maro_item = maro_syntax[maro_index];
        if (!maro_item.maro_valid)
        {
            continue;
        }
        const auto maro_found = maro_roots.find({maro_DiagnosticContext(maro_unique[maro_index]),
            maro_item.maro_location, maro_item.maro_target});
        if (maro_found == maro_roots.end() || maro_found->second.maro_ambiguous || maro_found->second.maro_index == maro_index)
        {
            continue;
        }
        const auto& maro_rootInfo = maro_found->second;
        auto& maro_root = maro_unique[maro_rootInfo.maro_index];
        const auto& maro_diagnostic = maro_unique[maro_index];
        if (maro_diagnostic.code == L"C2061" &&
            (maro_rootInfo.maro_missing != L";" || !maro_root.friendlyMessage.starts_with(L"문장 끝에 ';'가 필요합니다.")))
        {
            continue;
        }
        if (maro_diagnostic.code == L"C2447" && (maro_rootInfo.maro_missing != L";" || maro_item.maro_target != L"{"))
        {
            continue;
        }
        maro_FoldDiagnostic(maro_root, maro_diagnostic);
        maro_folded[maro_index] = true;
    }
    std::vector<Maro_Diagnostic> maro_result;
    maro_result.reserve(maro_unique.size());
    for (std::size_t maro_index = 0; maro_index < maro_unique.size(); ++maro_index)
    {
        if (!maro_folded[maro_index])
        {
            maro_result.push_back(std::move(maro_unique[maro_index]));
        }
    }
    return maro_result;
}
