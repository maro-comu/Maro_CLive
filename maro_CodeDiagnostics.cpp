#include "maro_CodeDiagnostics.hpp"

#include <algorithm>
#include <cwctype>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace
{
constexpr std::size_t maro_none = std::numeric_limits<std::size_t>::max();

struct maro_Token
{
    std::wstring_view maro_text;
    std::size_t maro_start = 0;
    std::size_t maro_end = 0;
    std::size_t maro_line = 1;
    std::size_t maro_scope = 0;
    std::size_t maro_match = maro_none;
    bool maro_literal = false;
};

struct maro_Scope
{
    std::size_t maro_parent = 0;
    std::size_t maro_function = 0;
    std::wstring_view maro_nonzero;
};

struct maro_Declaration
{
    std::wstring_view maro_name;
    std::wstring_view maro_type;
    std::wstring_view maro_literal;
    std::size_t maro_token = 0;
    std::size_t maro_endToken = 0;
    std::size_t maro_scope = 0;
    std::size_t maro_arraySize = 0;
    bool maro_array = false;
    bool maro_pointer = false;
    bool maro_static = false;
    bool maro_const = false;
    bool maro_zero = false;
};

struct maro_MacroUse
{
    Maro_SourceRange maro_range;
    Maro_SourceRange maro_nameRange;
    bool maro_builtinDeclaration = false;
    bool maro_bracedInitializer = false;
};

struct maro_Context
{
    const Maro_SourceRequest& maro_request;
    std::vector<Maro_Diagnostic>& maro_diagnostics;
    std::vector<maro_Token> maro_tokens;
    std::vector<std::size_t> maro_lines{0};
    std::vector<maro_Scope> maro_scopes{{}};
    std::vector<maro_Declaration> maro_declarations;
    std::map<std::pair<std::size_t, std::wstring_view>, std::vector<std::size_t>> maro_bindings;
    std::map<std::size_t, std::vector<maro_MacroUse>> maro_macroIssues;
    std::map<std::size_t, Maro_SourceRange> maro_relatedRanges;
    std::set<std::wstring_view> maro_macros;
    std::set<std::wstring_view> maro_declaredFunctions;
    bool maro_stringHeader = false;
    bool maro_stdioHeader = false;
    bool maro_uncertain = false;
    std::size_t maro_initialCount = 0;
    mutable std::size_t maro_work = 0;

    Maro_SourcePosition maro_position(std::size_t maro_offset) const
    {
        const auto maro_line = static_cast<std::size_t>(std::upper_bound(maro_lines.begin(), maro_lines.end(), maro_offset) - maro_lines.begin());
        return {maro_line, maro_offset - maro_lines[maro_line - 1] + 1};
    }

    Maro_Diagnostic& maro_add(std::wstring_view maro_code, std::size_t maro_start, std::size_t maro_end,
        std::wstring maro_message, Maro_Severity maro_severity = Maro_Severity::Warning,
        Maro_Evidence maro_evidence = Maro_Evidence::StaticAnalysis)
    {
        Maro_Diagnostic maro_diagnostic;
        maro_diagnostic.code = maro_code;
        maro_diagnostic.analyzer = L"CLive_Maro source checks";
        maro_diagnostic.analyzerVersion = L"2.3.4";
        maro_diagnostic.sourceVersion = maro_request.sourceVersion;
        maro_diagnostic.sourcePath = maro_request.sourcePath;
        maro_diagnostic.range.start = maro_position(maro_start);
        maro_diagnostic.range.end = maro_position(maro_end);
        maro_diagnostic.severity = maro_severity;
        maro_diagnostic.evidence = maro_evidence;
        maro_diagnostic.findingId = std::wstring(maro_code) + L":" + std::to_wstring(maro_start);
        if (const auto maro_break = maro_message.find(L". "); maro_break != std::wstring::npos) maro_message[maro_break + 1] = L'\n';
        maro_diagnostic.friendlyMessage = std::move(maro_message);
        maro_diagnostics.push_back(std::move(maro_diagnostic));
        return maro_diagnostics.back();
    }

    void maro_fix(Maro_Diagnostic& maro_diagnostic, std::size_t maro_start, std::size_t maro_end,
        std::wstring maro_replacement, std::wstring maro_description) const
    {
        if (maro_end <= maro_start || maro_end > maro_request.sourceText.size() || maro_replacement.empty()) return;
        const auto maro_original = std::wstring_view(maro_request.sourceText).substr(maro_start, maro_end - maro_start);
        if (maro_original.find(L"//") != std::wstring_view::npos || maro_original.find(L"/*") != std::wstring_view::npos) return;
        Maro_TextEdit maro_edit;
        maro_edit.sourceVersion = maro_request.sourceVersion;
        maro_edit.startOffsetUtf16 = maro_start;
        maro_edit.lengthUtf16 = maro_end - maro_start;
        maro_edit.expectedText = maro_request.sourceText.substr(maro_start, maro_edit.lengthUtf16);
        maro_edit.replacement = std::move(maro_replacement);
        maro_diagnostic.fix = Maro_FixSuggestion{std::move(maro_description), {std::move(maro_edit)}};
    }

    bool maro_has(std::size_t maro_index, std::wstring_view maro_text) const
    {
        return maro_index < maro_tokens.size() && maro_tokens[maro_index].maro_text == maro_text;
    }

    const maro_Declaration* maro_resolve(std::size_t maro_index) const
    {
        if (maro_index >= maro_tokens.size() || !maro_standalone(maro_index)) return nullptr;
        auto maro_scope = maro_tokens[maro_index].maro_scope;
        const auto maro_function = maro_scopes[maro_scope].maro_function;
        if (!maro_function) return nullptr;
        for (;;)
        {
            if (maro_scopes[maro_scope].maro_function != maro_function) return nullptr;
            if (++maro_work > 262144) return nullptr;
            const auto maro_found = maro_bindings.find({maro_scope, maro_tokens[maro_index].maro_text});
            if (maro_found != maro_bindings.end())
            {
                const auto& maro_entries = maro_found->second;
                const auto maro_entry = std::lower_bound(maro_entries.begin(), maro_entries.end(), maro_index,
                    [&](std::size_t maro_declaration, std::size_t maro_token) { return maro_declarations[maro_declaration].maro_token < maro_token; });
                if (maro_entry != maro_entries.begin())
                {
                    const auto& maro_declaration = maro_declarations[*(maro_entry - 1)];
                    if (maro_index - maro_declaration.maro_token > 4096) return nullptr;
                    for (auto maro_probe = maro_declaration.maro_endToken + 1; maro_probe < maro_index; ++maro_probe)
                    {
                        if (++maro_work > 262144) return nullptr;
                        if (maro_tokens[maro_probe].maro_text != maro_declaration.maro_name || !maro_probe) continue;
                        const auto maro_previous = maro_tokens[maro_probe - 1].maro_text;
                        if (maro_previous == L"*" || maro_previous == L"&" || maro_previous == L"&&" || maro_previous == L")" ||
                            (std::all_of(maro_previous.begin(), maro_previous.end(), [](wchar_t maro_ch) { return std::iswalnum(maro_ch) || maro_ch == L'_'; }) &&
                                maro_previous != L"return")) return nullptr;
                    }
                    return &maro_declaration;
                }
            }
            if (!maro_scope) return nullptr;
            maro_scope = maro_scopes[maro_scope].maro_parent;
        }
    }

    bool maro_standalone(std::size_t maro_index) const
    {
        return (maro_index == 0 || (!maro_has(maro_index - 1, L".") && !maro_has(maro_index - 1, L"->") && !maro_has(maro_index - 1, L"::"))) &&
            !maro_has(maro_index + 1, L".") && !maro_has(maro_index + 1, L"->") && !maro_has(maro_index + 1, L"::");
    }

    bool maro_libraryFunction(std::wstring_view maro_name) const
    {
        return maro_plain(maro_name) && !maro_declaredFunctions.contains(maro_name) &&
            std::none_of(maro_declarations.begin(), maro_declarations.end(),
                [&](const maro_Declaration& maro_declaration) { return maro_declaration.maro_name == maro_name; });
    }

    bool maro_plain(std::wstring_view maro_name) const
    {
        return !maro_macros.contains(maro_name);
    }
};

bool maro_identifier(std::wstring_view maro_text)
{
    if (maro_text.empty() || !(std::iswalpha(maro_text.front()) || maro_text.front() == L'_')) return false;
    return std::all_of(maro_text.begin() + 1, maro_text.end(), [](wchar_t maro_ch) { return std::iswalnum(maro_ch) || maro_ch == L'_'; });
}

std::size_t maro_integer(std::wstring_view maro_text)
{
    if (maro_text.empty() || maro_text.size() > 8) return maro_none;
    const std::size_t maro_base = maro_text.size() > 1 && maro_text.front() == L'0' ? 8 : 10;
    std::size_t maro_value = 0;
    for (const auto maro_ch : maro_text)
    {
        if (maro_ch < L'0' || maro_ch >= L'0' + maro_base) return maro_none;
        maro_value = maro_value * maro_base + static_cast<std::size_t>(maro_ch - L'0');
    }
    return maro_value;
}

bool maro_asciiString(std::wstring_view maro_text)
{
    return maro_text.size() >= 2 && maro_text.front() == L'"' && maro_text.back() == L'"' &&
        std::all_of(maro_text.begin() + 1, maro_text.end() - 1,
            [](wchar_t maro_ch) { return maro_ch >= 32 && maro_ch < 127 && maro_ch != L'\\' && maro_ch != L'"'; });
}

bool maro_lex(maro_Context& maro_context, std::vector<maro_Token>& maro_all)
{
    const auto& maro_text = maro_context.maro_request.sourceText;
    std::size_t maro_line = 1;
    for (std::size_t maro_index = 0; maro_index < maro_text.size();)
    {
        const auto maro_start = maro_index;
        const auto maro_ch = maro_text[maro_index];
        if (maro_ch == L'\r' || maro_ch == L'\n')
        {
            if (maro_ch == L'\r' && maro_index + 1 < maro_text.size() && maro_text[maro_index + 1] == L'\n') ++maro_index;
            maro_context.maro_lines.push_back(++maro_index);
            ++maro_line;
            continue;
        }
        if (std::iswspace(maro_ch)) { ++maro_index; continue; }
        if (maro_ch == L'\\') return false;
        if (maro_ch == L'/' && maro_index + 1 < maro_text.size() && (maro_text[maro_index + 1] == L'/' || maro_text[maro_index + 1] == L'*'))
        {
            const bool maro_block = maro_text[maro_index + 1] == L'*';
            maro_index += 2;
            bool maro_closed = !maro_block;
            while (maro_index < maro_text.size())
            {
                if (maro_block && maro_text[maro_index] == L'*' && maro_index + 1 < maro_text.size() && maro_text[maro_index + 1] == L'/')
                {
                    maro_index += 2;
                    maro_closed = true;
                    break;
                }
                if (maro_text[maro_index] == L'\r' || maro_text[maro_index] == L'\n')
                {
                    if (!maro_block) break;
                    if (maro_text[maro_index] == L'\r' && maro_index + 1 < maro_text.size() && maro_text[maro_index + 1] == L'\n') ++maro_index;
                    maro_context.maro_lines.push_back(++maro_index);
                    ++maro_line;
                }
                else if (maro_text[maro_index] == L'\\' && maro_index + 1 < maro_text.size() &&
                    (maro_text[maro_index + 1] == L'\r' || maro_text[maro_index + 1] == L'\n')) return false;
                else ++maro_index;
            }
            if (!maro_closed) return false;
            continue;
        }
        bool maro_literal = false;
        if (maro_ch == L'"' || maro_ch == L'\'')
        {
            maro_literal = true;
            ++maro_index;
            bool maro_closed = false;
            while (maro_index < maro_text.size())
            {
                if (maro_text[maro_index] == L'\r' || maro_text[maro_index] == L'\n') return false;
                if (maro_text[maro_index] == L'\\')
                {
                    if (maro_index + 1 >= maro_text.size() || maro_text[maro_index + 1] == L'\r' || maro_text[maro_index + 1] == L'\n') return false;
                    maro_index += 2;
                }
                else if (maro_text[maro_index++] == maro_ch) { maro_closed = true; break; }
            }
            if (!maro_closed) return false;
        }
        else if (std::iswalnum(maro_ch) || maro_ch == L'_')
        {
            while (maro_index < maro_text.size() && (std::iswalnum(maro_text[maro_index]) || maro_text[maro_index] == L'_')) ++maro_index;
            if (maro_index < maro_text.size() && maro_text[maro_index] == L'"') return false;
        }
        else
        {
            ++maro_index;
            if (maro_index < maro_text.size())
            {
                const std::wstring_view maro_pair(maro_text.data() + maro_start, 2);
                for (const auto maro_operator : {L"==", L"!=", L"<=", L">=", L"++", L"--", L"+=", L"-=", L"*=", L"/=", L"&&", L"||", L"->", L"<<", L">>", L"::", L"%=", L"&=", L"|=", L"^="})
                    if (maro_pair == maro_operator) { ++maro_index; break; }
            }
        }
        maro_all.push_back({std::wstring_view(maro_text).substr(maro_start, maro_index - maro_start), maro_start, maro_index, maro_line, 0, maro_none, maro_literal});
        if (maro_all.size() > 32768) return false;
    }
    return true;
}

void maro_directives(maro_Context& maro_context, const std::vector<maro_Token>& maro_all)
{
    std::set<std::wstring_view> maro_seen;
    for (std::size_t maro_index = 0; maro_index < maro_all.size();)
    {
        if (maro_all[maro_index].maro_text != L"#")
        {
            maro_context.maro_tokens.push_back(maro_all[maro_index++]);
            continue;
        }
        auto maro_end = maro_index + 1;
        while (maro_end < maro_all.size() && maro_all[maro_end].maro_line == maro_all[maro_index].maro_line) ++maro_end;
        const auto maro_count = maro_end - maro_index;
        const auto maro_is = [&](std::size_t maro_at, std::wstring_view maro_text) { return maro_at < maro_count && maro_all[maro_index + maro_at].maro_text == maro_text; };
        if (maro_is(1, L"include") && maro_count == 7 && maro_is(2, L"<") && maro_is(4, L".") && maro_is(5, L"h") && maro_is(6, L">"))
        {
            const auto maro_name = maro_all[maro_index + 3].maro_text;
            if (maro_name == L"string") maro_context.maro_stringHeader = true;
            else if (maro_name == L"stdio") maro_context.maro_stdioHeader = true;
            else if (maro_name != L"stdlib" && maro_name != L"stddef" && maro_name != L"stdint" && maro_name != L"stdbool" && maro_name != L"limits" && maro_name != L"math") maro_context.maro_uncertain = true;
        }
        else if (maro_is(1, L"define") && maro_count >= 3 && maro_identifier(maro_all[maro_index + 2].maro_text))
        {
            const auto maro_name = maro_all[maro_index + 2].maro_text;
            maro_context.maro_macros.insert(maro_name);
            if (!maro_seen.insert(maro_name).second) maro_context.maro_uncertain = true;
        }
        else maro_context.maro_uncertain = true;
        maro_index = maro_end;
    }
    if (maro_context.maro_uncertain) return;
    std::map<std::wstring_view, std::vector<maro_MacroUse>> maro_valueUses;
    for (std::size_t maro_index = 1; maro_index + 1 < maro_context.maro_tokens.size(); ++maro_index)
    {
        const auto& maro_token = maro_context.maro_tokens[maro_index];
        if (!maro_context.maro_has(maro_index - 1, L"[") || !maro_context.maro_has(maro_index + 1, L"]") || !maro_context.maro_macros.contains(maro_token.maro_text)) continue;
        maro_MacroUse maro_use;
        maro_use.maro_range = {maro_context.maro_position(maro_token.maro_start), maro_context.maro_position(maro_token.maro_end), false};
        if (maro_index >= 3 && (maro_index == 3 || maro_context.maro_has(maro_index - 4, L";") ||
            maro_context.maro_has(maro_index - 4, L"{") || maro_context.maro_has(maro_index - 4, L"}")))
        {
            const auto& maro_type = maro_context.maro_tokens[maro_index - 3];
            const auto& maro_name = maro_context.maro_tokens[maro_index - 2];
            const bool maro_builtin = maro_type.maro_text == L"int" || maro_type.maro_text == L"char" || maro_type.maro_text == L"short" ||
                maro_type.maro_text == L"long" || maro_type.maro_text == L"float" || maro_type.maro_text == L"double" ||
                maro_type.maro_text == L"signed" || maro_type.maro_text == L"unsigned";
            maro_use.maro_builtinDeclaration = maro_builtin && maro_context.maro_plain(maro_type.maro_text) &&
                maro_identifier(maro_name.maro_text) && maro_context.maro_plain(maro_name.maro_text) &&
                (maro_context.maro_has(maro_index + 2, L";") || maro_context.maro_has(maro_index + 2, L"="));
            maro_use.maro_nameRange = {maro_context.maro_position(maro_name.maro_start), maro_context.maro_position(maro_name.maro_end), false};
            if (maro_use.maro_builtinDeclaration && maro_context.maro_has(maro_index + 2, L"=") && maro_context.maro_has(maro_index + 3, L"{"))
            {
                std::size_t maro_depth = 1;
                for (auto maro_probe = maro_index + 4; maro_probe < maro_context.maro_tokens.size() && maro_probe - maro_index < 4096; ++maro_probe)
                {
                    if (maro_context.maro_has(maro_probe, L"{")) ++maro_depth;
                    else if (maro_context.maro_has(maro_probe, L"}") && !--maro_depth)
                    {
                        maro_use.maro_bracedInitializer = maro_context.maro_has(maro_probe + 1, L";");
                        break;
                    }
                }
            }
        }
        maro_valueUses[maro_token.maro_text].push_back(maro_use);
    }
    for (std::size_t maro_index = 0; maro_index < maro_all.size(); ++maro_index)
    {
        if (maro_context.maro_diagnostics.size() - maro_context.maro_initialCount >= 128) break;
        if (maro_all[maro_index].maro_text != L"#") continue;
        auto maro_end = maro_index + 1;
        while (maro_end < maro_all.size() && maro_all[maro_end].maro_line == maro_all[maro_index].maro_line) ++maro_end;
        const auto maro_count = maro_end - maro_index;
        const auto maro_is = [&](std::size_t maro_at, std::wstring_view maro_text) { return maro_at < maro_count && maro_all[maro_index + maro_at].maro_text == maro_text; };
        if (!maro_is(1, L"define")) continue;
        const auto& maro_name = maro_all[maro_index + 2];
        if (maro_count == 6 && maro_is(3, L"=") && maro_integer(maro_all[maro_index + 4].maro_text) != maro_none && maro_is(5, L";") &&
            maro_valueUses.contains(maro_name.maro_text))
        {
            maro_context.maro_macroIssues.emplace(maro_context.maro_diagnostics.size(), maro_valueUses[maro_name.maro_text]);
            auto& maro_diagnostic = maro_context.maro_add(L"MARO-MACRO-VALUE", maro_all[maro_index + 3].maro_start, maro_all[maro_index + 5].maro_end,
                L"'" + std::wstring(maro_name.maro_text) + L"'를 대괄호 안에서 값으로 사용했지만, 매크로에 '='와 ';'가 포함되어 문법 오류가 납니다. '#define " +
                std::wstring(maro_name.maro_text) + L" " + std::wstring(maro_all[maro_index + 4].maro_text) + L"'로 바꿔 상수 값만 남기세요.", Maro_Severity::Error);
            maro_context.maro_fix(maro_diagnostic, maro_all[maro_index + 3].maro_start, maro_all[maro_index + 5].maro_end,
                std::wstring(maro_all[maro_index + 4].maro_text), L"매크로를 '#define " + std::wstring(maro_name.maro_text) + L" " + std::wstring(maro_all[maro_index + 4].maro_text) + L"'로 수정합니다.");
        }
        else if (maro_count == 9 && maro_is(3, L"(") && maro_is(5, L")") && maro_is(7, L"*") &&
            maro_name.maro_end == maro_all[maro_index + 3].maro_start && maro_identifier(maro_all[maro_index + 4].maro_text) &&
            maro_all[maro_index + 4].maro_text == maro_all[maro_index + 6].maro_text && maro_all[maro_index + 4].maro_text == maro_all[maro_index + 8].maro_text)
        {
            const auto maro_parameter = std::wstring(maro_all[maro_index + 4].maro_text);
            const auto maro_replacement = L"((" + maro_parameter + L") * (" + maro_parameter + L"))";
            auto& maro_diagnostic = maro_context.maro_add(L"MARO-MACRO-PRECEDENCE", maro_all[maro_index + 6].maro_start, maro_all[maro_index + 8].maro_end,
                L"괄호가 없어 인수와 주변 식의 계산 순서가 달라질 수 있습니다. 본문을 '" + maro_replacement +
                L"'로 바꿔 각 인수와 전체 곱셈식을 괄호로 감싸세요. 인수는 여전히 두 번 계산되므로 ++나 부작용이 있는 함수 호출을 넣지 마세요.");
            maro_context.maro_fix(maro_diagnostic, maro_all[maro_index + 6].maro_start, maro_all[maro_index + 8].maro_end,
                maro_replacement, L"매크로 본문을 '" + maro_replacement + L"'로 수정합니다.");
        }
        maro_index = maro_end - 1;
    }
}

bool maro_structure(maro_Context& maro_context)
{
    auto& maro_tokens = maro_context.maro_tokens;
    std::vector<std::size_t> maro_stack;
    for (std::size_t maro_index = 0; maro_index < maro_tokens.size(); ++maro_index)
    {
        const auto maro_text = maro_tokens[maro_index].maro_text;
        if (maro_text == L"(" || maro_text == L"[" || maro_text == L"{")
        {
            if (maro_stack.size() >= 128) return false;
            maro_stack.push_back(maro_index);
        }
        else if (maro_text == L")" || maro_text == L"]" || maro_text == L"}")
        {
            if (maro_stack.empty()) return false;
            const auto maro_open = maro_stack.back();
            if ((maro_text == L")" && maro_tokens[maro_open].maro_text != L"(") ||
                (maro_text == L"]" && maro_tokens[maro_open].maro_text != L"[") ||
                (maro_text == L"}" && maro_tokens[maro_open].maro_text != L"{")) return false;
            maro_tokens[maro_index].maro_match = maro_open;
            maro_tokens[maro_open].maro_match = maro_index;
            maro_stack.pop_back();
        }
    }
    if (!maro_stack.empty()) return false;
    std::size_t maro_scope = 0;
    for (std::size_t maro_index = 0; maro_index < maro_tokens.size(); ++maro_index)
    {
        auto& maro_token = maro_tokens[maro_index];
        if ((maro_token.maro_text == L"strcmp" || maro_token.maro_text == L"memcpy" || maro_token.maro_text == L"printf") &&
            (!maro_context.maro_scopes[maro_scope].maro_function || !maro_context.maro_has(maro_index + 1, L"(")))
            maro_context.maro_declaredFunctions.insert(maro_token.maro_text);
        if (maro_token.maro_text == L"{")
        {
            auto maro_function = maro_context.maro_scopes[maro_scope].maro_function;
            std::wstring_view maro_nonzero;
            if (maro_index && maro_context.maro_has(maro_index - 1, L")"))
            {
                const auto maro_open = maro_tokens[maro_index - 1].maro_match;
                if (maro_open && maro_open != maro_none && maro_identifier(maro_tokens[maro_open - 1].maro_text))
                {
                    const auto maro_name = maro_tokens[maro_open - 1].maro_text;
                    if (maro_name != L"if" && maro_name != L"while" && maro_name != L"for" && maro_name != L"switch" && maro_name != L"catch")
                    {
                        maro_function = maro_context.maro_scopes.size();
                        maro_context.maro_declaredFunctions.insert(maro_name);
                    }
                    else if (maro_name == L"if" && maro_open + 4 == maro_index - 1 && maro_context.maro_has(maro_open + 2, L"!=") && maro_context.maro_has(maro_open + 3, L"0")) maro_nonzero = maro_tokens[maro_open + 1].maro_text;
                }
            }
            else if (maro_index >= 2 && maro_context.maro_has(maro_index - 1, L"else") && maro_context.maro_has(maro_index - 2, L"}"))
            {
                const auto maro_then = maro_tokens[maro_index - 2].maro_match;
                if (maro_then && maro_then != maro_none && maro_context.maro_has(maro_then - 1, L")"))
                {
                    const auto maro_open = maro_tokens[maro_then - 1].maro_match;
                    if (maro_open && maro_open != maro_none && maro_context.maro_has(maro_open - 1, L"if") && maro_open + 4 == maro_then - 1 &&
                        maro_context.maro_has(maro_open + 2, L"==") && maro_context.maro_has(maro_open + 3, L"0")) maro_nonzero = maro_tokens[maro_open + 1].maro_text;
                }
            }
            maro_context.maro_scopes.push_back({maro_scope, maro_function, maro_nonzero});
            maro_scope = maro_context.maro_scopes.size() - 1;
        }
        maro_token.maro_scope = maro_scope;
        if (maro_token.maro_text == L"}") maro_scope = maro_context.maro_scopes[maro_scope].maro_parent;
    }
    return true;
}

void maro_declarations(maro_Context& maro_context)
{
    const auto& maro_tokens = maro_context.maro_tokens;
    for (std::size_t maro_index = 0; maro_index + 2 < maro_tokens.size(); ++maro_index)
    {
        if (maro_index && !maro_context.maro_has(maro_index - 1, L";") && !maro_context.maro_has(maro_index - 1, L"{") && !maro_context.maro_has(maro_index - 1, L"}")) continue;
        auto maro_type = maro_index;
        bool maro_static = false;
        bool maro_const = false;
        while (maro_context.maro_has(maro_type, L"static") || maro_context.maro_has(maro_type, L"const"))
        {
            maro_static = maro_static || maro_context.maro_has(maro_type, L"static");
            maro_const = maro_const || maro_context.maro_has(maro_type, L"const");
            ++maro_type;
        }
        if (maro_type >= maro_tokens.size() || !maro_identifier(maro_tokens[maro_type].maro_text)) continue;
        auto maro_name = maro_type + 1;
        if (maro_context.maro_has(maro_name, L"const")) { maro_const = true; ++maro_name; }
        const bool maro_pointer = maro_context.maro_has(maro_name, L"*");
        if (maro_pointer) ++maro_name;
        if (maro_context.maro_has(maro_name, L"const")) ++maro_name;
        const bool maro_reference = maro_context.maro_has(maro_name, L"&") || maro_context.maro_has(maro_name, L"&&");
        if (maro_reference) ++maro_name;
        if (maro_name >= maro_tokens.size() || !maro_identifier(maro_tokens[maro_name].maro_text)) continue;
        if (maro_context.maro_has(maro_name + 1, L"(")) { maro_context.maro_declaredFunctions.insert(maro_tokens[maro_name].maro_text); continue; }
        if (!maro_context.maro_plain(maro_tokens[maro_name].maro_text) || !maro_context.maro_plain(maro_tokens[maro_type].maro_text)) continue;
        auto maro_end = maro_name + 1;
        while (maro_end < maro_tokens.size() && maro_end - maro_name < 128 && !maro_context.maro_has(maro_end, L";") && !maro_context.maro_has(maro_end, L"}"))
        {
            if (maro_context.maro_has(maro_end, L"{") && maro_tokens[maro_end].maro_match != maro_none) maro_end = maro_tokens[maro_end].maro_match;
            ++maro_end;
        }
        if (!maro_context.maro_has(maro_end, L";")) continue;
        maro_Declaration maro_declaration;
        maro_declaration.maro_name = maro_tokens[maro_name].maro_text;
        maro_declaration.maro_type = maro_tokens[maro_type].maro_text;
        maro_declaration.maro_token = maro_name;
        maro_declaration.maro_endToken = maro_end;
        maro_declaration.maro_scope = maro_tokens[maro_name].maro_scope;
        maro_declaration.maro_pointer = maro_pointer;
        maro_declaration.maro_static = maro_static;
        maro_declaration.maro_const = maro_const;
        if (maro_context.maro_has(maro_name + 1, L"[") && maro_context.maro_has(maro_name + 3, L"]"))
        {
            const auto maro_size = maro_integer(maro_tokens[maro_name + 2].maro_text);
            if (maro_size == maro_none || maro_size == 0 || maro_size > 32768) continue;
            maro_declaration.maro_array = true;
            maro_declaration.maro_arraySize = maro_size;
        }
        else if (maro_end == maro_name + 3 && maro_context.maro_has(maro_name + 1, L"="))
        {
            maro_declaration.maro_zero = !maro_pointer && !maro_reference && maro_context.maro_has(maro_name + 2, L"0");
            if (maro_pointer && !maro_reference && maro_declaration.maro_type == L"char" && maro_asciiString(maro_tokens[maro_name + 2].maro_text)) maro_declaration.maro_literal = maro_tokens[maro_name + 2].maro_text;
        }
        else if (maro_end != maro_name + 1 && !maro_context.maro_has(maro_name + 1, L"=")) continue;
        bool maro_multiple = false;
        for (auto maro_part = maro_name + 1; maro_part < maro_end; ++maro_part)
        {
            if (maro_context.maro_has(maro_part, L"{") || maro_context.maro_has(maro_part, L"(")) maro_part = maro_tokens[maro_part].maro_match;
            else if (maro_context.maro_has(maro_part, L",")) maro_multiple = true;
        }
        if (maro_multiple) { maro_context.maro_uncertain = true; return; }
        maro_context.maro_bindings[{maro_declaration.maro_scope, maro_declaration.maro_name}].push_back(maro_context.maro_declarations.size());
        maro_context.maro_declarations.push_back(maro_declaration);
    }
}

bool maro_unchanged(const maro_Context& maro_context, const maro_Declaration& maro_declaration, std::size_t maro_until, bool maro_zero = false)
{
    const auto& maro_tokens = maro_context.maro_tokens;
    if (!maro_context.maro_scopes[maro_declaration.maro_scope].maro_function) return false;
    if (maro_until - maro_declaration.maro_token > 4096) return false;
    for (auto maro_index = maro_declaration.maro_endToken + 1; maro_index < maro_until; ++maro_index)
    {
        if (++maro_context.maro_work > 262144) return false;
        if (!maro_context.maro_plain(maro_tokens[maro_index].maro_text)) return false;
        if (maro_context.maro_has(maro_index, L"&")) return false;
        if (maro_context.maro_request.language == Maro_Language::Cpp20 && maro_identifier(maro_tokens[maro_index].maro_text) &&
            maro_context.maro_has(maro_index + 1, L"(") && maro_tokens[maro_index].maro_text != L"if" &&
            maro_tokens[maro_index].maro_text != L"while" && maro_tokens[maro_index].maro_text != L"for" &&
            maro_tokens[maro_index].maro_text != L"switch" && maro_tokens[maro_index].maro_text != L"sizeof") return false;
        if (maro_tokens[maro_index].maro_text != maro_declaration.maro_name) continue;
        if (maro_index && maro_identifier(maro_tokens[maro_index - 1].maro_text) && !maro_context.maro_has(maro_index - 1, L"return")) return false;
        if (maro_context.maro_resolve(maro_index) != &maro_declaration) continue;
        if (maro_index && (maro_context.maro_has(maro_index - 1, L"&") || maro_context.maro_has(maro_index - 1, L"++") || maro_context.maro_has(maro_index - 1, L"--"))) return false;
        for (const auto maro_operator : {L"=", L"+=", L"-=", L"*=", L"/=", L"%=", L"&=", L"|=", L"^=", L"++", L"--"})
            if (maro_context.maro_has(maro_index + 1, maro_operator))
            {
                if (maro_zero && std::wstring_view(maro_operator) == L"=" && maro_context.maro_has(maro_index + 2, L"0") &&
                    (maro_context.maro_has(maro_index + 3, L";") || maro_context.maro_has(maro_index + 3, L")"))) break;
                return false;
            }
        if (maro_context.maro_has(maro_index + 1, L"[") || maro_context.maro_has(maro_index + 1, L"->")) return false;
    }
    return true;
}

void maro_codeChecks(maro_Context& maro_context)
{
    const auto& maro_tokens = maro_context.maro_tokens;
    for (std::size_t maro_index = 0; maro_index < maro_tokens.size(); ++maro_index)
    {
        if (maro_context.maro_diagnostics.size() - maro_context.maro_initialCount >= 128 || maro_context.maro_work >= 262144) break;
        const auto& maro_token = maro_tokens[maro_index];
        if (!maro_context.maro_plain(maro_token.maro_text)) continue;
        if (maro_token.maro_text == L"return" && maro_index + 2 < maro_tokens.size())
        {
            const auto* maro_declaration = maro_context.maro_resolve(maro_index + 1);
            if (maro_declaration && maro_declaration->maro_array && !maro_declaration->maro_static && maro_context.maro_has(maro_index + 2, L";") &&
                maro_context.maro_scopes[maro_declaration->maro_scope].maro_function &&
                maro_context.maro_scopes[maro_declaration->maro_scope].maro_function == maro_context.maro_scopes[maro_token.maro_scope].maro_function)
            {
                maro_context.maro_relatedRanges.emplace(maro_context.maro_diagnostics.size(), Maro_SourceRange{
                    maro_context.maro_position(maro_token.maro_start), maro_context.maro_position(maro_tokens[maro_index + 1].maro_end), false});
                maro_context.maro_add(L"MARO-LOCAL-LIFETIME", maro_tokens[maro_index + 1].maro_start, maro_tokens[maro_index + 1].maro_end,
                    L"지역 배열은 함수가 끝나면 사라집니다. 호출자가 배열을 만들고 함수에 전달하도록 바꾸세요. static이나 동적 할당으로 임의 변환하면 수명·공유 방식이 달라져 자동 수정하지 않습니다.");
            }
            if (maro_context.maro_has(maro_index + 1, L"0") && maro_context.maro_has(maro_index + 2, L"}") &&
                maro_context.maro_scopes[maro_token.maro_scope].maro_function && maro_context.maro_plain(L"return"))
            {
                auto& maro_diagnostic = maro_context.maro_add(L"MARO-MISSING-SEMICOLON", maro_tokens[maro_index + 1].maro_end, maro_tokens[maro_index + 1].maro_end,
                    L"return 0 뒤에 ';'를 붙여 문장을 끝내세요.", Maro_Severity::Error);
                maro_context.maro_fix(maro_diagnostic, maro_token.maro_start, maro_tokens[maro_index + 1].maro_end,
                    maro_context.maro_request.sourceText.substr(maro_token.maro_start, maro_tokens[maro_index + 1].maro_end - maro_token.maro_start) + L";",
                    L"return 0 끝에 ';'를 추가합니다.");
            }
        }
        if (maro_token.maro_text == L"if" && maro_context.maro_has(maro_index + 1, L"(") && maro_context.maro_has(maro_index + 3, L"=") &&
            maro_context.maro_has(maro_index + 4, L"0") && maro_context.maro_has(maro_index + 5, L")"))
        {
            const auto* maro_declaration = maro_context.maro_resolve(maro_index + 2);
            if (maro_declaration && !maro_declaration->maro_pointer && !maro_declaration->maro_array && maro_declaration->maro_type == L"int")
            {
                auto& maro_diagnostic = maro_context.maro_add(L"MARO-CONDITION-ASSIGNMENT", maro_tokens[maro_index + 3].maro_start, maro_tokens[maro_index + 3].maro_end,
                    L"'='는 0을 대입하여 이 조건을 항상 거짓으로 만듭니다. 대입이 목적이면 그대로 두고, 0인지 비교하려는 의도라면 '=='로 바꾸세요.", Maro_Severity::Warning, Maro_Evidence::Conditional);
                maro_context.maro_fix(maro_diagnostic, maro_tokens[maro_index + 3].maro_start, maro_tokens[maro_index + 3].maro_end,
                    L"==", L"0과 비교하려는 경우: '='를 '=='로 수정합니다.");
            }
        }
        const auto maro_comparisonBoundary = [&](std::size_t maro_at, bool maro_before) {
            if (maro_at >= maro_tokens.size()) return true;
            const auto maro_text = maro_tokens[maro_at].maro_text;
            if (maro_text == L"&&" || maro_text == L"||" || maro_text == L"," || maro_text == L";" || maro_text == L"?" || maro_text == L":" || maro_text == L"}") return true;
            return maro_before ? maro_text == L"(" || maro_text == L"=" || maro_text == L"return" || maro_text == L"{" : maro_text == L")";
        };
        if (maro_identifier(maro_token.maro_text) && maro_context.maro_has(maro_index + 1, L"==") && maro_index + 2 < maro_tokens.size() &&
            maro_asciiString(maro_tokens[maro_index + 2].maro_text) && (maro_index == 0 || maro_comparisonBoundary(maro_index - 1, true)) &&
            maro_comparisonBoundary(maro_index + 3, false))
        {
            const auto* maro_declaration = maro_context.maro_resolve(maro_index);
            if (maro_declaration && maro_declaration->maro_type == L"char" && !maro_declaration->maro_literal.empty() && maro_unchanged(maro_context, *maro_declaration, maro_index))
            {
                auto& maro_diagnostic = maro_context.maro_add(L"MARO-STRING-COMPARE", maro_tokens[maro_index + 1].maro_start, maro_tokens[maro_index + 2].maro_end,
                    L"'=='는 문자열 내용이 아닌 주소를 비교합니다. 내용이 같은지 확인하려면 strcmp(" + std::wstring(maro_token.maro_text) + L", " + std::wstring(maro_tokens[maro_index + 2].maro_text) + L") == 0으로 바꾸세요.", Maro_Severity::Warning, Maro_Evidence::Conditional);
                if (maro_context.maro_stringHeader && maro_context.maro_libraryFunction(L"strcmp") &&
                    (maro_index == 0 || (!maro_context.maro_has(maro_index - 1, L"!") && !maro_context.maro_has(maro_index - 1, L"*") && !maro_context.maro_has(maro_index - 1, L"&") && !maro_context.maro_has(maro_index - 1, L"."))))
                    maro_context.maro_fix(maro_diagnostic, maro_token.maro_start, maro_tokens[maro_index + 2].maro_end,
                        L"(strcmp(" + std::wstring(maro_token.maro_text) + L", " + std::wstring(maro_tokens[maro_index + 2].maro_text) + L") == 0)",
                        L"내용을 비교하려는 경우: 'strcmp(...) == 0'으로 수정합니다.");
                else maro_diagnostic.friendlyMessage += L" <string.h>와 함수 선언을 확인한 뒤 직접 수정하세요.";
            }
        }
        if (maro_identifier(maro_token.maro_text) && maro_context.maro_has(maro_index + 1, L"=") && maro_context.maro_has(maro_index + 3, L";") &&
            (maro_index == 0 || maro_context.maro_has(maro_index - 1, L"{") || maro_context.maro_has(maro_index - 1, L";") || maro_context.maro_has(maro_index - 1, L"}")))
        {
            const auto* maro_target = maro_context.maro_resolve(maro_index);
            const auto* maro_source = maro_context.maro_resolve(maro_index + 2);
            if (maro_target && maro_target->maro_array && maro_target->maro_type == L"char" && maro_source && maro_source->maro_type == L"char" && !maro_source->maro_literal.empty())
            {
                auto& maro_diagnostic = maro_context.maro_add(L"MARO-ARRAY-ASSIGNMENT", maro_token.maro_start, maro_tokens[maro_index + 2].maro_end,
                    L"배열에는 '='로 문자열을 대입할 수 없습니다. 널 문자까지 들어갈 크기를 확인한 뒤 문자열 내용을 배열에 복사하세요.", Maro_Severity::Error);
                if (!maro_target->maro_const && maro_context.maro_stringHeader && maro_context.maro_libraryFunction(L"memcpy") &&
                    maro_source->maro_literal.size() - 1 <= maro_target->maro_arraySize && maro_unchanged(maro_context, *maro_source, maro_index))
                {
                    const auto maro_count = maro_source->maro_literal.size() - 1;
                    maro_diagnostic.friendlyMessage = L"배열에는 '=' 대신 복사가 필요합니다. 확인된 문자열과 널 문자를 합한 " + std::to_wstring(maro_count) + L"바이트를 memcpy(" + std::wstring(maro_token.maro_text) + L", " + std::wstring(maro_tokens[maro_index + 2].maro_text) + L", " + std::to_wstring(maro_count) + L")로 복사하세요.";
                    maro_context.maro_fix(maro_diagnostic, maro_token.maro_start, maro_tokens[maro_index + 2].maro_end,
                        L"memcpy(" + std::wstring(maro_token.maro_text) + L", " + std::wstring(maro_tokens[maro_index + 2].maro_text) + L", " + std::to_wstring(maro_count) + L")",
                        L"널 문자를 포함한 " + std::to_wstring(maro_count) + L"바이트를 memcpy로 복사하도록 수정합니다.");
                }
                else if (maro_target->maro_const) maro_diagnostic.friendlyMessage += L" 대상이 const 배열이므로 나중에 쓸 수 없습니다. 선언할 때 초기화하거나 변경 가능한 별도 배열을 사용하세요.";
                else if (maro_source->maro_literal.size() - 1 > maro_target->maro_arraySize) maro_diagnostic.friendlyMessage += L" 현재 배열 크기가 문자열과 널 문자를 담기에 부족합니다. 배열 크기부터 늘리세요.";
            }
        }
        if (maro_token.maro_text == L"printf" && maro_context.maro_standalone(maro_index) && maro_context.maro_stdioHeader && maro_context.maro_libraryFunction(L"printf") &&
            maro_context.maro_has(maro_index + 1, L"(") && maro_context.maro_has(maro_index + 3, L",") && maro_context.maro_has(maro_index + 5, L")"))
        {
            const auto maro_format = maro_tokens[maro_index + 2].maro_text;
            const auto maro_percent = maro_format.find(L'%');
            const auto* maro_declaration = maro_context.maro_resolve(maro_index + 4);
            if (maro_declaration && maro_declaration->maro_pointer && maro_format.size() > 2 && maro_format.front() == L'"' && maro_format.back() == L'"' &&
                maro_percent != std::wstring_view::npos && maro_percent + 1 < maro_format.size() && maro_format[maro_percent + 1] == L'd' &&
                maro_format.find(L'%', maro_percent + 1) == std::wstring_view::npos)
            {
                maro_context.maro_relatedRanges.emplace(maro_context.maro_diagnostics.size(), Maro_SourceRange{
                    maro_context.maro_position(maro_token.maro_start), maro_context.maro_position(maro_tokens[maro_index + 5].maro_end), false});
                auto& maro_diagnostic = maro_context.maro_add(L"MARO-PRINTF-POINTER", maro_tokens[maro_index + 4].maro_start, maro_tokens[maro_index + 4].maro_end,
                    L"%d는 정수용인데 포인터를 넘겼습니다. 주소를 출력하려면 %p와 (void*) 변환을 함께 사용하세요. 가리키는 값 출력은 포인터의 형식과 수명을 먼저 확인해야 합니다.", Maro_Severity::Warning, Maro_Evidence::Conditional);
                auto maro_newFormat = std::wstring(maro_format);
                maro_newFormat[maro_percent + 1] = L'p';
                maro_context.maro_fix(maro_diagnostic, maro_tokens[maro_index + 2].maro_start, maro_tokens[maro_index + 4].maro_end,
                    maro_newFormat + L", (void*)" + std::wstring(maro_tokens[maro_index + 4].maro_text),
                    L"주소를 출력하려는 경우: %p와 (void*)로 수정합니다.");
            }
        }
        if (maro_token.maro_text == L"(" && maro_context.maro_has(maro_index + 1, L"int") && maro_context.maro_has(maro_index + 2, L"*") &&
            maro_context.maro_has(maro_index + 3, L")") && maro_context.maro_has(maro_index + 4, L"&") && maro_index + 5 < maro_tokens.size())
        {
            const auto* maro_declaration = maro_context.maro_resolve(maro_index + 5);
            if (maro_declaration && maro_declaration->maro_type == L"double" && !maro_declaration->maro_pointer && !maro_declaration->maro_array)
                maro_context.maro_add(L"MARO-POINTER-TYPE", maro_token.maro_start, maro_tokens[maro_index + 5].maro_end,
                    L"double의 주소를 int*로 바꿔 읽으면 형식 별칭 규칙을 위반할 수 있습니다. 숫자 변환이면 값의 범위와 소수 처리 방법을 정해 값 자체를 변환하세요. 바이트 확인이면 충분한 unsigned char 배열에 원래 값의 sizeof 크기만큼 memcpy로 복사하세요. 의도를 결정해야 하므로 자동 수정하지 않습니다.", Maro_Severity::Warning, Maro_Evidence::Conditional);
        }
        if (maro_token.maro_text == L"/" && maro_index + 1 < maro_tokens.size())
        {
            const auto* maro_declaration = maro_context.maro_resolve(maro_index + 1);
            bool maro_zero = maro_context.maro_has(maro_index + 1, L"0") && !maro_context.maro_has(maro_index + 2, L".");
            if (maro_declaration && maro_declaration->maro_zero && maro_declaration->maro_type == L"int" && maro_unchanged(maro_context, *maro_declaration, maro_index, true))
            {
                maro_zero = true;
                auto maro_scope = maro_token.maro_scope;
                while (maro_scope)
                {
                    if (maro_context.maro_scopes[maro_scope].maro_nonzero == maro_declaration->maro_name) maro_zero = false;
                    maro_scope = maro_context.maro_scopes[maro_scope].maro_parent;
                }
            }
            if (maro_zero)
                maro_context.maro_add(L"MARO-DIVIDE-ZERO", maro_tokens[maro_index + 1].maro_start, maro_tokens[maro_index + 1].maro_end,
                    L"분모가 0인 상태로 이 나눗셈을 실행할 수 있습니다. 나누기 전에 분모 != 0을 확인하고, 0이면 계산을 건너뛰거나 다시 입력받으세요. 올바른 분모를 추측해 자동 변경하지 않습니다.", Maro_Severity::Warning, Maro_Evidence::Conditional);
        }
    }
}

void maro_deduplicate(maro_Context& maro_context)
{
    auto& maro_diagnostics = maro_context.maro_diagnostics;
    std::vector<bool> maro_removed(maro_diagnostics.size(), false);
    const auto maro_contains = [](const Maro_SourceRange& maro_range, const Maro_SourcePosition& maro_position) {
        const auto maro_value = std::pair{maro_position.line, maro_position.column};
        return maro_value >= std::pair{maro_range.start.line, maro_range.start.column} &&
            maro_value <= std::pair{maro_range.end.line, maro_range.end.column};
    };
    for (std::size_t maro_index = 0; maro_index < maro_context.maro_initialCount; ++maro_index)
    {
        const auto& maro_compiler = maro_diagnostics[maro_index];
        if (maro_compiler.sourceVersion != maro_context.maro_request.sourceVersion ||
            maro_compiler.sourcePath != maro_context.maro_request.sourcePath || maro_compiler.range.generated) continue;
        for (auto maro_root = maro_context.maro_initialCount; maro_root < maro_diagnostics.size(); ++maro_root)
        {
            auto& maro_diagnostic = maro_diagnostics[maro_root];
            const auto maro_origin = maro_context.maro_relatedRanges.find(maro_root);
            const auto& maro_range = maro_origin != maro_context.maro_relatedRanges.end() ? maro_origin->second : maro_diagnostic.range;
            bool maro_sameIssue = maro_contains(maro_range, maro_compiler.range.start) &&
                ((maro_diagnostic.code == L"MARO-LOCAL-LIFETIME" && maro_compiler.code == L"C4172") ||
                (maro_diagnostic.code == L"MARO-STRING-COMPARE" && maro_compiler.code == L"C4130") ||
                (maro_diagnostic.code == L"MARO-ARRAY-ASSIGNMENT" && maro_compiler.code == L"C2106") ||
                (maro_diagnostic.code == L"MARO-PRINTF-POINTER" && maro_compiler.code == L"C4477") ||
                (maro_diagnostic.code == L"MARO-CONDITION-ASSIGNMENT" && maro_compiler.code == L"C4706"));
            if (maro_diagnostic.code == L"MARO-MISSING-SEMICOLON" && (maro_compiler.code == L"C2143" || maro_compiler.code == L"C2146") &&
                maro_compiler.fix && maro_diagnostic.fix && maro_compiler.fix->edits.size() == 1 &&
                maro_compiler.fix->edits[0].startOffsetUtf16 + maro_compiler.fix->edits[0].lengthUtf16 ==
                maro_diagnostic.fix->edits[0].startOffsetUtf16 + maro_diagnostic.fix->edits[0].lengthUtf16) maro_sameIssue = true;
            if (maro_diagnostic.code == L"MARO-MACRO-VALUE")
            {
                const auto maro_uses = maro_context.maro_macroIssues.find(maro_root);
                if (maro_uses != maro_context.maro_macroIssues.end())
                    for (const auto& maro_use : maro_uses->second)
                    {
                        const bool maro_atMacro = maro_contains(maro_use.maro_range, maro_compiler.range.start);
                        const bool maro_atName = maro_use.maro_builtinDeclaration && maro_contains(maro_use.maro_nameRange, maro_compiler.range.start);
                        if ((maro_atMacro && (maro_compiler.code == L"C2143" || maro_compiler.code == L"C2059" || maro_compiler.code == L"C2146")) ||
                            (maro_use.maro_builtinDeclaration && (maro_atMacro || maro_atName) && (maro_compiler.code == L"C4430" || maro_compiler.code == L"C4431")) ||
                            (maro_atMacro && maro_use.maro_bracedInitializer && maro_compiler.code == L"C2075"))
                        { maro_sameIssue = true; break; }
                    }
            }
            if (!maro_sameIssue) continue;
            maro_removed[maro_index] = true;
            maro_diagnostic.maro_relatedCount += 1 + maro_compiler.maro_relatedCount;
            if (maro_diagnostic.originalDiagnostic.size() < 16384)
            {
                if (!maro_diagnostic.originalDiagnostic.empty()) maro_diagnostic.originalDiagnostic += L'\n';
                const auto& maro_original = maro_compiler.originalDiagnostic.empty() ? maro_compiler.code : maro_compiler.originalDiagnostic;
                maro_diagnostic.originalDiagnostic.append(maro_original, 0, 16384 - maro_diagnostic.originalDiagnostic.size());
            }
            break;
        }
    }
    std::vector<Maro_Diagnostic> maro_kept;
    maro_kept.reserve(maro_diagnostics.size());
    for (std::size_t maro_index = 0; maro_index < maro_diagnostics.size(); ++maro_index)
        if (!maro_removed[maro_index]) maro_kept.push_back(std::move(maro_diagnostics[maro_index]));
    maro_diagnostics = std::move(maro_kept);
}
}

void maro_ImproveDiagnostics(const Maro_SourceRequest& maro_request, std::vector<Maro_Diagnostic>& maro_diagnostics)
{
    if (maro_request.sourceText.empty() || maro_request.sourceText.size() > 262144 || maro_diagnostics.size() > 1024) return;
    if (std::any_of(maro_diagnostics.begin(), maro_diagnostics.end(), [](const Maro_Diagnostic& maro_item) { return maro_item.analyzer == L"CLive_Maro source checks"; })) return;
    maro_Context maro_context{maro_request, maro_diagnostics};
    maro_context.maro_initialCount = maro_diagnostics.size();
    std::vector<maro_Token> maro_all;
    if (!maro_lex(maro_context, maro_all)) return;
    maro_directives(maro_context, maro_all);
    if (maro_context.maro_uncertain || !maro_structure(maro_context))
    {
        maro_diagnostics.resize(maro_context.maro_initialCount);
        return;
    }
    maro_declarations(maro_context);
    if (maro_context.maro_uncertain)
    {
        maro_diagnostics.resize(maro_context.maro_initialCount);
        return;
    }
    maro_codeChecks(maro_context);
    maro_deduplicate(maro_context);
}
