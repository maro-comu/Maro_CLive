#include "maro_SourceInsight.hpp"

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <unordered_set>
#include <utility>

namespace
{
constexpr std::size_t maro_sourceLimit = 1024 * 1024;
constexpr std::size_t maro_itemLimit = 2048;
constexpr std::size_t maro_lineLimit = 32768;
constexpr std::size_t maro_tokenLimit = 262144;
constexpr std::size_t maro_none = std::numeric_limits<std::size_t>::max();

enum class maro_TokenKind
{
    maro_Identifier,
    maro_Number,
    maro_Literal,
    maro_Punctuation,
    maro_Directive
};

struct maro_Token
{
    std::wstring_view maro_text;
    maro_TokenKind maro_kind;
    std::size_t maro_offset;
    std::size_t maro_end;
    std::size_t maro_line;
    std::size_t maro_column;
};

bool maro_IsIdentifierStart(wchar_t maro_character)
{
    return maro_character == L'_' || (maro_character >= L'a' && maro_character <= L'z') ||
        (maro_character >= L'A' && maro_character <= L'Z') || maro_character >= 0x80;
}

bool maro_IsIdentifierPart(wchar_t maro_character)
{
    return maro_IsIdentifierStart(maro_character) || (maro_character >= L'0' && maro_character <= L'9');
}

bool maro_IsSpace(wchar_t maro_character)
{
    return maro_character == L' ' || maro_character == L'\t' || maro_character == L'\r' ||
        maro_character == L'\n' || maro_character == L'\v' || maro_character == L'\f' || maro_character == 0xfeff;
}

std::wstring_view maro_Trim(std::wstring_view maro_text)
{
    while (!maro_text.empty() && maro_IsSpace(maro_text.front()))
        maro_text.remove_prefix(1);
    while (!maro_text.empty() && maro_IsSpace(maro_text.back()))
        maro_text.remove_suffix(1);
    return maro_text;
}

std::wstring maro_Compact(std::wstring_view maro_text, std::size_t maro_limit = 240)
{
    std::wstring maro_result;
    bool maro_pendingSpace = false;
    for (const wchar_t maro_character : maro_Trim(maro_text))
    {
        if (maro_IsSpace(maro_character))
        {
            maro_pendingSpace = !maro_result.empty();
            continue;
        }
        if (maro_pendingSpace)
            maro_result.push_back(L' ');
        maro_pendingSpace = false;
        maro_result.push_back(maro_character);
        if (maro_result.size() >= maro_limit)
        {
            maro_result += L"…";
            break;
        }
    }
    return maro_result;
}

bool maro_IsKeyword(std::wstring_view maro_text)
{
    static const std::unordered_set<std::wstring_view> maro_keywords = {
        L"alignas", L"alignof", L"asm", L"auto", L"bool", L"break", L"case", L"catch", L"char", L"char8_t",
        L"char16_t", L"char32_t", L"class", L"const", L"consteval", L"constexpr", L"constinit", L"const_cast",
        L"continue", L"decltype", L"default", L"delete", L"do", L"double", L"dynamic_cast", L"else", L"enum",
        L"explicit", L"export", L"extern", L"false", L"float", L"for", L"friend", L"goto", L"if", L"inline",
        L"int", L"long", L"mutable", L"namespace", L"new", L"noexcept", L"nullptr", L"operator", L"private",
        L"protected", L"public", L"register", L"reinterpret_cast", L"requires", L"return", L"short", L"signed",
        L"sizeof", L"static", L"static_assert", L"static_cast", L"struct", L"switch", L"template", L"this",
        L"thread_local", L"throw", L"true", L"try", L"typedef", L"typeid", L"typename", L"union", L"unsigned",
        L"using", L"virtual", L"void", L"volatile", L"wchar_t", L"while", L"_Atomic", L"_Bool", L"_Complex",
        L"_Generic", L"_Imaginary", L"_Noreturn", L"_Static_assert", L"_Thread_local", L"__declspec"
    };
    return maro_keywords.contains(maro_text);
}

bool maro_IsForbiddenPrefix(std::wstring_view maro_text)
{
    static const std::unordered_set<std::wstring_view> maro_forbidden = {
        L"return", L"co_return", L"co_await", L"co_yield", L"throw", L"delete", L"new", L"goto", L"break",
        L"continue", L"if", L"else", L"while", L"for", L"switch", L"case", L"default", L"do", L"catch",
        L"sizeof", L"alignof", L"static_assert", L"_Static_assert", L"using", L"typedef", L"namespace"
    };
    return maro_forbidden.contains(maro_text);
}

class maro_Lexer
{
public:
    explicit maro_Lexer(std::wstring_view maro_source) : maro_source_(maro_source) {}

    std::vector<maro_Token> maro_Read(bool& maro_truncated)
    {
        std::vector<maro_Token> maro_tokens;
        maro_tokens.reserve(std::min(maro_source_.size() / 3, maro_tokenLimit));
        while (maro_position_ < maro_source_.size())
        {
            if (maro_tokens.size() == maro_tokenLimit)
            {
                maro_truncated = true;
                break;
            }
            const wchar_t maro_character = maro_source_[maro_position_];
            if (maro_IsSpace(maro_character))
            {
                maro_Advance(maro_position_ + 1);
                continue;
            }
            if (maro_source_.substr(maro_position_, 2) == L"//")
            {
                auto maro_end = maro_source_.find(L'\n', maro_position_ + 2);
                while (maro_end != std::wstring_view::npos)
                {
                    auto maro_previous = maro_end;
                    if (maro_previous > 0 && maro_source_[maro_previous - 1] == L'\r')
                        --maro_previous;
                    if (maro_previous == 0 || maro_source_[maro_previous - 1] != L'\\')
                        break;
                    maro_end = maro_source_.find(L'\n', maro_end + 1);
                }
                maro_Advance(maro_end == std::wstring_view::npos ? maro_source_.size() : maro_end);
                continue;
            }
            if (maro_source_.substr(maro_position_, 2) == L"/*")
            {
                const auto maro_end = maro_source_.find(L"*/", maro_position_ + 2);
                maro_Advance(maro_end == std::wstring_view::npos ? maro_source_.size() : maro_end + 2);
                continue;
            }
            const std::size_t maro_start = maro_position_;
            const std::size_t maro_line = maro_line_;
            const std::size_t maro_column = maro_column_;
            maro_TokenKind maro_kind = maro_TokenKind::maro_Punctuation;
            std::size_t maro_end = maro_start + 1;
            if (maro_character == L'#' && maro_lineStart_)
            {
                maro_kind = maro_TokenKind::maro_Directive;
                for (maro_end = maro_start; maro_end < maro_source_.size(); ++maro_end)
                {
                    if (maro_source_[maro_end] != L'\n')
                        continue;
                    std::size_t maro_previous = maro_end;
                    if (maro_previous > maro_start && maro_source_[maro_previous - 1] == L'\r')
                        --maro_previous;
                    if (maro_previous == maro_start || maro_source_[maro_previous - 1] != L'\\')
                        break;
                }
            }
            else if (const auto maro_literalEnd = maro_ReadLiteral(maro_start); maro_literalEnd != maro_start)
            {
                maro_kind = maro_TokenKind::maro_Literal;
                maro_end = maro_literalEnd;
            }
            else if (maro_IsIdentifierStart(maro_character))
            {
                maro_kind = maro_TokenKind::maro_Identifier;
                while (maro_end < maro_source_.size() && maro_IsIdentifierPart(maro_source_[maro_end]))
                    ++maro_end;
            }
            else if (maro_character >= L'0' && maro_character <= L'9')
            {
                maro_kind = maro_TokenKind::maro_Number;
                while (maro_end < maro_source_.size() && (maro_IsIdentifierPart(maro_source_[maro_end]) ||
                    maro_source_[maro_end] == L'.' || maro_source_[maro_end] == L'\''))
                    ++maro_end;
            }
            else
            {
                const auto maro_pair = maro_source_.substr(maro_start, 2);
                constexpr std::array maro_pairs = {L"::", L"->", L"&&", L"||", L"==", L"!=", L"<=", L">=",
                    L"++", L"--", L"+=", L"-=", L"*=", L"/=", L"%=", L"&=", L"|=", L"^="};
                if (std::find(maro_pairs.begin(), maro_pairs.end(), maro_pair) != maro_pairs.end())
                    ++maro_end;
            }
            maro_tokens.push_back({maro_source_.substr(maro_start, maro_end - maro_start), maro_kind,
                maro_start, maro_end, maro_line, maro_column});
            maro_Advance(maro_end);
            maro_lineStart_ = false;
        }
        return maro_tokens;
    }

private:
    std::size_t maro_ReadLiteral(std::size_t maro_start) const
    {
        std::size_t maro_quote = maro_start;
        bool maro_raw = false;
        constexpr std::array maro_prefixes = {L"u8R\"", L"uR\"", L"UR\"", L"LR\"", L"R\"",
            L"u8\"", L"u\"", L"U\"", L"L\"", L"u8'", L"u'", L"U'", L"L'", L"\"", L"'"};
        bool maro_found = false;
        for (const std::wstring_view maro_prefix : maro_prefixes)
        {
            if (maro_source_.substr(maro_start, maro_prefix.size()) != maro_prefix)
                continue;
            maro_quote += maro_prefix.size() - 1;
            maro_raw = maro_prefix.find(L'R') != std::wstring_view::npos;
            maro_found = true;
            break;
        }
        if (!maro_found)
            return maro_start;
        if (maro_raw)
        {
            const auto maro_open = maro_source_.find(L'(', maro_quote + 1);
            if (maro_open == std::wstring_view::npos || maro_open - maro_quote > 17)
                return maro_source_.size();
            const auto maro_delimiter = maro_source_.substr(maro_quote + 1, maro_open - maro_quote - 1);
            if (maro_delimiter.find_first_of(L" \t\r\n\\)") != std::wstring_view::npos)
                return maro_source_.size();
            const std::wstring maro_closing = L")" + std::wstring(maro_delimiter) + L"\"";
            const auto maro_close = maro_source_.find(maro_closing, maro_open + 1);
            return maro_close == std::wstring_view::npos ? maro_source_.size() : maro_close + maro_closing.size();
        }
        const wchar_t maro_quoteCharacter = maro_source_[maro_quote];
        for (std::size_t maro_cursor = maro_quote + 1; maro_cursor < maro_source_.size(); ++maro_cursor)
        {
            if (maro_source_[maro_cursor] == L'\\')
            {
                if (maro_cursor + 1 < maro_source_.size() && maro_source_[maro_cursor + 1] == L'\r')
                    ++maro_cursor;
                ++maro_cursor;
            }
            else if (maro_source_[maro_cursor] == maro_quoteCharacter)
                return maro_cursor + 1;
            else if (maro_source_[maro_cursor] == L'\n')
                return maro_cursor;
        }
        return maro_source_.size();
    }

    void maro_Advance(std::size_t maro_end)
    {
        for (; maro_position_ < maro_end; ++maro_position_)
        {
            if (maro_source_[maro_position_] == L'\n')
            {
                ++maro_line_;
                maro_column_ = 1;
                maro_lineStart_ = true;
            }
            else
                ++maro_column_;
        }
    }

    std::wstring_view maro_source_;
    std::size_t maro_position_ = 0;
    std::size_t maro_line_ = 1;
    std::size_t maro_column_ = 1;
    bool maro_lineStart_ = true;
};

class maro_Inspector
{
public:
    maro_Inspector(std::wstring_view maro_source, std::wstring_view maro_path, maro_SourceInsight& maro_result)
        : maro_source_(maro_source), maro_path_(maro_path), maro_result_(maro_result),
          maro_tokens_(maro_Lexer(maro_source).maro_Read(maro_result.maro_truncated)),
          maro_matches_(maro_tokens_.size(), maro_none) {}

    void maro_Run()
    {
        maro_MatchDelimiters();
        for (std::size_t maro_index = 0; maro_index < maro_tokens_.size(); ++maro_index)
        {
            if (maro_tokens_[maro_index].maro_kind == maro_TokenKind::maro_Directive)
                maro_ReadDirective(maro_tokens_[maro_index]);
            else if (maro_tokens_[maro_index].maro_text == L"(")
                maro_ReadFunction(maro_index);
        }
        std::size_t maro_start = 0;
        for (std::size_t maro_index = 0; maro_index < maro_tokens_.size(); ++maro_index)
        {
            const auto& maro_token = maro_tokens_[maro_index];
            if (maro_token.maro_kind == maro_TokenKind::maro_Directive)
            {
                maro_start = maro_index + 1;
                continue;
            }
            if (maro_token.maro_text == L";" || maro_token.maro_text == L"{" || maro_token.maro_text == L"}")
            {
                maro_ReadVariables(maro_start, maro_index);
                maro_start = maro_index + 1;
            }
        }
        maro_ReadVariables(maro_start, maro_tokens_.size());
        maro_ExplainLines();
        std::stable_sort(maro_result_.maro_items.begin(), maro_result_.maro_items.end(),
            [](const maro_SourceItem& maro_left, const maro_SourceItem& maro_right)
            {
                if (maro_left.maro_kind != maro_right.maro_kind)
                    return maro_left.maro_kind < maro_right.maro_kind;
                return std::pair(maro_left.maro_line, maro_left.maro_column) <
                    std::pair(maro_right.maro_line, maro_right.maro_column);
            });
    }

private:
    void maro_Add(maro_SourceItemKind maro_kind, const maro_Token& maro_token, std::wstring maro_name,
        std::wstring maro_detail)
    {
        const std::size_t maro_key = maro_token.maro_offset * 4 + static_cast<std::size_t>(maro_kind);
        if (!maro_seen_.insert(maro_key).second)
            return;
        if (maro_result_.maro_items.size() == maro_itemLimit)
        {
            maro_result_.maro_truncated = true;
            return;
        }
        maro_result_.maro_items.push_back({maro_kind, std::move(maro_name), std::move(maro_detail),
            std::wstring(maro_path_), maro_token.maro_line, maro_token.maro_column});
    }

    void maro_MatchDelimiters()
    {
        std::vector<std::size_t> maro_stack;
        for (std::size_t maro_index = 0; maro_index < maro_tokens_.size(); ++maro_index)
        {
            const auto maro_text = maro_tokens_[maro_index].maro_text;
            if (maro_text == L"(" || maro_text == L"[" || maro_text == L"{")
                maro_stack.push_back(maro_index);
            else if (maro_text == L")" || maro_text == L"]" || maro_text == L"}")
            {
                if (maro_stack.empty())
                    continue;
                const auto maro_open = maro_stack.back();
                const auto maro_openText = maro_tokens_[maro_open].maro_text;
                if ((maro_text == L")" && maro_openText != L"(") ||
                    (maro_text == L"]" && maro_openText != L"[") ||
                    (maro_text == L"}" && maro_openText != L"{"))
                    continue;
                maro_stack.pop_back();
                maro_matches_[maro_open] = maro_index;
                maro_matches_[maro_index] = maro_open;
            }
        }
    }

    std::size_t maro_StartBefore(std::size_t maro_index) const
    {
        std::size_t maro_start = maro_index;
        while (maro_start > 0)
        {
            if (maro_index - maro_start > 128)
                break;
            const auto& maro_previous = maro_tokens_[maro_start - 1];
            if (maro_previous.maro_text == L";" || maro_previous.maro_text == L"{" ||
                maro_previous.maro_text == L"}" || maro_previous.maro_kind == maro_TokenKind::maro_Directive)
                break;
            --maro_start;
        }
        if (maro_start + 1 < maro_index && maro_tokens_[maro_start + 1].maro_text == L":" &&
            (maro_tokens_[maro_start].maro_text == L"public" || maro_tokens_[maro_start].maro_text == L"private" ||
             maro_tokens_[maro_start].maro_text == L"protected"))
            maro_start += 2;
        return maro_start;
    }

    bool maro_IsTypePrefix(std::size_t maro_start, std::size_t maro_end) const
    {
        if (maro_start >= maro_end || maro_end - maro_start > 128)
            return false;
        bool maro_hasType = false;
        int maro_angles = 0;
        for (std::size_t maro_index = maro_start; maro_index < maro_end; ++maro_index)
        {
            const auto& maro_token = maro_tokens_[maro_index];
            const auto maro_text = maro_token.maro_text;
            if (maro_token.maro_kind == maro_TokenKind::maro_Identifier)
            {
                if (maro_IsForbiddenPrefix(maro_text))
                    return false;
                maro_hasType = true;
            }
            else if (maro_text == L"<")
                ++maro_angles;
            else if (maro_text == L">")
            {
                if (--maro_angles < 0)
                    return false;
            }
            else if (maro_text == L"::" || maro_text == L"*" || maro_text == L"&" || maro_text == L"&&")
                continue;
            else if (maro_angles > 0 && (maro_text == L"," || maro_token.maro_kind == maro_TokenKind::maro_Number))
                continue;
            else
                return false;
        }
        return maro_hasType && maro_angles == 0;
    }

    std::wstring maro_Slice(std::size_t maro_start, std::size_t maro_end) const
    {
        if (maro_start >= maro_end || maro_end > maro_tokens_.size())
            return {};
        return maro_Compact(maro_source_.substr(maro_tokens_[maro_start].maro_offset,
            maro_tokens_[maro_end - 1].maro_end - maro_tokens_[maro_start].maro_offset));
    }

    void maro_ReadDirective(const maro_Token& maro_token)
    {
        auto maro_text = maro_Trim(maro_token.maro_text.substr(1));
        const auto maro_wordEnd = maro_text.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz_");
        const auto maro_word = maro_text.substr(0, maro_wordEnd);
        maro_text = maro_wordEnd == std::wstring_view::npos ? std::wstring_view{} : maro_Trim(maro_text.substr(maro_wordEnd));
        if (maro_word == L"include" || maro_word == L"include_next" || maro_word == L"import")
        {
            while (!maro_text.empty())
            {
                if (maro_text.starts_with(L"/*"))
                {
                    const auto maro_close = maro_text.find(L"*/", 2);
                    if (maro_close == std::wstring_view::npos)
                        return;
                    maro_text = maro_Trim(maro_text.substr(maro_close + 2));
                }
                else if (maro_text.starts_with(L"\\\r\n"))
                    maro_text = maro_Trim(maro_text.substr(3));
                else if (maro_text.starts_with(L"\\\n"))
                    maro_text = maro_Trim(maro_text.substr(2));
                else
                    break;
            }
            if (maro_text.empty())
                return;
            if (maro_text.front() == L'<' || maro_text.front() == L'\"')
            {
                const auto maro_end = maro_text.find(maro_text.front() == L'<' ? L'>' : L'\"', 1);
                const auto maro_name = maro_text.substr(1, maro_end == std::wstring_view::npos ? maro_text.size() - 1 : maro_end - 1);
                if (!maro_name.empty())
                    maro_Add(maro_SourceItemKind::maro_Header, maro_token, maro_Compact(maro_name),
                        maro_end == std::wstring_view::npos ? L"미완성 헤더 참조 · 정적 미리보기" : L"헤더 참조 · 포함 지시문 위치");
            }
            else
                maro_Add(maro_SourceItemKind::maro_Header, maro_token, maro_Compact(maro_text), L"매크로 헤더 참조 · 실제 경로는 빌드 설정에 따름");
            return;
        }
        if (maro_word != L"pragma")
            return;
        bool maro_unused = false;
        const auto maro_parts = maro_Lexer(maro_text).maro_Read(maro_unused);
        if (maro_parts.size() < 6 || maro_parts[0].maro_text != L"comment" || maro_parts[1].maro_text != L"(" ||
            maro_parts[2].maro_text != L"lib" || maro_parts[3].maro_text != L"," ||
            maro_parts[4].maro_kind != maro_TokenKind::maro_Literal || maro_parts[5].maro_text != L")")
            return;
        const auto maro_name = maro_parts[4].maro_text;
        if (maro_name.size() >= 2 && maro_name.front() == L'\"' && maro_name.back() == L'\"')
            maro_Add(maro_SourceItemKind::maro_Library, maro_token, maro_Compact(maro_name.substr(1, maro_name.size() - 2)),
                L"명시적 링크 라이브러리 · pragma 위치");
    }

    void maro_ReadFunction(std::size_t maro_open)
    {
        if (maro_open == 0)
            return;
        const std::size_t maro_nameIndex = maro_open - 1;
        const auto& maro_name = maro_tokens_[maro_nameIndex];
        if (maro_name.maro_kind != maro_TokenKind::maro_Identifier || maro_IsKeyword(maro_name.maro_text))
            return;
        const std::size_t maro_start = maro_StartBefore(maro_nameIndex);
        if (!maro_IsTypePrefix(maro_start, maro_nameIndex))
            return;
        const auto maro_close = maro_matches_[maro_open];
        if (maro_close == maro_none)
        {
            if (maro_open + 1 == maro_tokens_.size())
                maro_Add(maro_SourceItemKind::maro_Function, maro_name, std::wstring(maro_name.maro_text),
                    maro_Slice(maro_start, maro_open + 1) + L" · 미완성 선언, 정적 미리보기");
            return;
        }
        std::size_t maro_next = maro_close + 1;
        while (maro_next < maro_tokens_.size() && (maro_tokens_[maro_next].maro_text == L"const" ||
            maro_tokens_[maro_next].maro_text == L"volatile" || maro_tokens_[maro_next].maro_text == L"noexcept" ||
            maro_tokens_[maro_next].maro_text == L"override" || maro_tokens_[maro_next].maro_text == L"final" ||
            maro_tokens_[maro_next].maro_text == L"&" || maro_tokens_[maro_next].maro_text == L"&&"))
            ++maro_next;
        if (maro_next < maro_tokens_.size() && maro_tokens_[maro_next].maro_text != L"{" &&
            maro_tokens_[maro_next].maro_text != L";" && maro_tokens_[maro_next].maro_text != L"=")
            return;
        std::size_t maro_parameter = maro_open + 1;
        for (std::size_t maro_index = maro_parameter; maro_index < maro_close; ++maro_index)
        {
            if (maro_index == maro_parameter && (maro_tokens_[maro_index].maro_kind == maro_TokenKind::maro_Number ||
                maro_tokens_[maro_index].maro_kind == maro_TokenKind::maro_Literal))
                return;
            if (maro_tokens_[maro_index].maro_text == L",")
                maro_parameter = maro_index + 1;
        }
        const bool maro_definition = maro_next < maro_tokens_.size() && maro_tokens_[maro_next].maro_text == L"{";
        maro_Add(maro_SourceItemKind::maro_Function, maro_name, std::wstring(maro_name.maro_text),
            maro_Slice(maro_start, maro_close + 1) + (maro_definition ? L" · 함수 정의" : L" · 함수 선언"));
        maro_functions_.insert(maro_nameIndex);
        maro_parameter = maro_open + 1;
        for (std::size_t maro_index = maro_parameter; maro_index <= maro_close; ++maro_index)
        {
            if (maro_index == maro_close || maro_tokens_[maro_index].maro_text == L",")
            {
                maro_ReadVariables(maro_parameter, maro_index, true);
                maro_parameter = maro_index + 1;
            }
            else if ((maro_tokens_[maro_index].maro_text == L"(" || maro_tokens_[maro_index].maro_text == L"[") &&
                maro_matches_[maro_index] != maro_none)
                maro_index = maro_matches_[maro_index];
        }
    }

    void maro_ReadVariables(std::size_t maro_start, std::size_t maro_end, bool maro_parameter = false)
    {
        if (maro_start >= maro_end || maro_end - maro_start > 4096)
            return;
        if (maro_tokens_[maro_start].maro_text == L"for" && maro_start + 1 < maro_end && maro_tokens_[maro_start + 1].maro_text == L"(")
            maro_start += 2;
        if (maro_start + 1 < maro_end && maro_tokens_[maro_start + 1].maro_text == L":" &&
            (maro_tokens_[maro_start].maro_text == L"public" || maro_tokens_[maro_start].maro_text == L"private" ||
             maro_tokens_[maro_start].maro_text == L"protected"))
            maro_start += 2;
        if (maro_start >= maro_end || maro_IsForbiddenPrefix(maro_tokens_[maro_start].maro_text))
            return;
        std::size_t maro_nameIndex = maro_none;
        for (std::size_t maro_index = maro_start + 1; maro_index < maro_end && maro_index - maro_start <= 128; ++maro_index)
        {
            const auto& maro_token = maro_tokens_[maro_index];
            if (maro_functions_.contains(maro_index))
                return;
            if (maro_token.maro_kind != maro_TokenKind::maro_Identifier || maro_IsKeyword(maro_token.maro_text))
                continue;
            const auto maro_next = maro_index + 1 == maro_end ? std::wstring_view{} : maro_tokens_[maro_index + 1].maro_text;
            if (maro_next != L"=" && maro_next != L"," && maro_next != L"[" && !maro_next.empty())
                continue;
            if (!maro_IsTypePrefix(maro_start, maro_index))
                continue;
            if (maro_index == maro_start + 1 && (maro_tokens_[maro_start].maro_text == L"struct" ||
                maro_tokens_[maro_start].maro_text == L"class" || maro_tokens_[maro_start].maro_text == L"enum" ||
                maro_tokens_[maro_start].maro_text == L"union"))
                return;
            maro_nameIndex = maro_index;
            break;
        }
        if (maro_nameIndex == maro_none)
            return;
        const std::wstring maro_type = maro_Slice(maro_start, maro_nameIndex);
        while (maro_nameIndex < maro_end)
        {
            std::size_t maro_separator = maro_nameIndex + 1;
            int maro_angles = 0;
            for (; maro_separator < maro_end; ++maro_separator)
            {
                const auto maro_text = maro_tokens_[maro_separator].maro_text;
                if ((maro_text == L"(" || maro_text == L"[" || maro_text == L"{") && maro_matches_[maro_separator] != maro_none)
                    maro_separator = maro_matches_[maro_separator];
                else if (maro_text == L"," && maro_angles == 0)
                    break;
                else if (maro_text == L"<" && maro_separator > maro_nameIndex + 1 &&
                    maro_tokens_[maro_separator - 1].maro_kind == maro_TokenKind::maro_Identifier)
                    ++maro_angles;
                else if (maro_text == L">" && maro_angles > 0)
                    --maro_angles;
            }
            maro_separator = std::min(maro_separator, maro_end);
            const auto& maro_name = maro_tokens_[maro_nameIndex];
            std::wstring maro_detail = maro_type + (maro_parameter ? L" · 매개변수" : L" · 변수 선언");
            for (std::size_t maro_index = maro_nameIndex + 1; maro_index < maro_separator; ++maro_index)
            {
                if (maro_tokens_[maro_index].maro_text == L"=")
                {
                    const auto maro_initializer = maro_Slice(maro_index + 1, maro_separator);
                    maro_detail += maro_initializer.empty() ? L" · 초기식 작성 중" : L" · 초기식: " + maro_initializer + L" (실행값 아님)";
                    break;
                }
            }
            maro_Add(maro_SourceItemKind::maro_Variable, maro_name, std::wstring(maro_name.maro_text), std::move(maro_detail));
            maro_nameIndex = maro_separator + 1;
            while (maro_nameIndex < maro_end && (maro_tokens_[maro_nameIndex].maro_text == L"*" ||
                maro_tokens_[maro_nameIndex].maro_text == L"&" || maro_tokens_[maro_nameIndex].maro_text == L"&&" ||
                maro_tokens_[maro_nameIndex].maro_text == L"const"))
                ++maro_nameIndex;
            if (maro_nameIndex >= maro_end || maro_tokens_[maro_nameIndex].maro_kind != maro_TokenKind::maro_Identifier ||
                maro_IsKeyword(maro_tokens_[maro_nameIndex].maro_text))
                break;
        }
    }

    void maro_ExplainLines()
    {
        std::size_t maro_tokenIndex = 0;
        for (auto& maro_line : maro_result_.maro_lines)
        {
            while (maro_tokenIndex < maro_tokens_.size() && maro_tokens_[maro_tokenIndex].maro_line < maro_line.maro_line)
                ++maro_tokenIndex;
            if (maro_tokenIndex == maro_tokens_.size() || maro_tokens_[maro_tokenIndex].maro_line != maro_line.maro_line)
            {
                maro_line.maro_explanation = maro_Trim(maro_line.maro_text).empty() ? L"빈 줄" : L"주석 또는 리터럴의 일부 · 별도 실행 문장 없음";
                continue;
            }
            const auto& maro_token = maro_tokens_[maro_tokenIndex];
            const auto maro_text = maro_token.maro_text;
            if (maro_token.maro_kind == maro_TokenKind::maro_Directive)
                maro_line.maro_explanation = L"전처리 지시문 · 실제 포함·링크 결과는 빌드 설정에 따릅니다.";
            else if (maro_text == L"if" || maro_text == L"else" || maro_text == L"switch" || maro_text == L"case")
                maro_line.maro_explanation = L"조건에 따라 실행 경로를 선택합니다. 선택 결과는 실행 전에는 확정하지 않습니다.";
            else if (maro_text == L"for" || maro_text == L"while" || maro_text == L"do")
                maro_line.maro_explanation = L"반복문입니다. 반복 횟수와 변수 변화는 실제 실행에서 확인합니다.";
            else if (maro_text == L"return" || maro_text == L"co_return")
                maro_line.maro_explanation = L"함수를 마치고 반환식을 호출한 곳에 전달합니다. 표시된 식은 실행값이 아닙니다.";
            else if (maro_text == L"break" || maro_text == L"continue")
                maro_line.maro_explanation = L"현재 반복문 또는 분기의 제어 흐름을 변경합니다.";
            else if (maro_text == L"{" || maro_text == L"}")
                maro_line.maro_explanation = L"블록의 시작 또는 끝입니다. 블록은 선언과 문장의 범위를 정합니다.";
            else if (maro_token.maro_kind == maro_TokenKind::maro_Identifier && maro_tokenIndex + 1 < maro_tokens_.size() &&
                maro_tokens_[maro_tokenIndex + 1].maro_text == L"(")
                maro_line.maro_explanation = L"함수 " + std::wstring(maro_text) + L" 호출 문장입니다. 호출 결과와 부작용은 실제 실행에서 확인합니다.";
            else
                maro_line.maro_explanation = L"소스 문장 미리보기 · 실행되지 않은 코드의 값과 결과는 표시하지 않습니다.";
        }
        for (const auto& maro_item : maro_result_.maro_items)
        {
            if (maro_item.maro_line == 0 || maro_item.maro_line > maro_result_.maro_lines.size())
                continue;
            auto& maro_line = maro_result_.maro_lines[maro_item.maro_line - 1];
            switch (maro_item.maro_kind)
            {
            case maro_SourceItemKind::maro_Library:
                maro_line.maro_explanation = L"라이브러리 " + maro_item.maro_name + L"의 링크를 요청합니다.";
                break;
            case maro_SourceItemKind::maro_Header:
                maro_line.maro_explanation = L"헤더 " + maro_item.maro_name + L"를 참조합니다. 헤더 자체를 프로그램처럼 실행하지 않습니다.";
                break;
            case maro_SourceItemKind::maro_Function:
                maro_line.maro_explanation = L"함수 " + maro_item.maro_name + L"을 선언하거나 정의합니다. 호출 여부는 실제 실행에 따릅니다.";
                break;
            case maro_SourceItemKind::maro_Variable:
                if (maro_item.maro_detail.find(L"매개변수") == std::wstring::npos)
                    maro_line.maro_explanation = L"변수 " + maro_item.maro_name + L" · " + maro_item.maro_detail;
                break;
            }
        }
    }

    std::wstring_view maro_source_;
    std::wstring_view maro_path_;
    maro_SourceInsight& maro_result_;
    std::vector<maro_Token> maro_tokens_;
    std::vector<std::size_t> maro_matches_;
    std::unordered_set<std::size_t> maro_functions_;
    std::unordered_set<std::size_t> maro_seen_;
};
}

maro_SourceInsight maro_InspectSource(std::wstring_view maro_source, std::wstring_view maro_path)
{
    maro_SourceInsight maro_result;
    maro_result.maro_path = maro_path;
    std::wstring maro_normalized;
    if (maro_source.size() > maro_sourceLimit)
    {
        maro_source = maro_source.substr(0, maro_sourceLimit);
        if (!maro_source.empty() && maro_source.back() >= 0xd800 && maro_source.back() <= 0xdbff)
            maro_source.remove_suffix(1);
        maro_result.maro_truncated = true;
    }
    for (std::size_t maro_index = 0; maro_index < maro_source.size(); ++maro_index)
    {
        if (maro_source[maro_index] != L'\r' || (maro_index + 1 < maro_source.size() && maro_source[maro_index + 1] == L'\n')) continue;
        if (maro_normalized.empty()) maro_normalized.assign(maro_source);
        maro_normalized[maro_index] = L'\n';
    }
    if (!maro_normalized.empty()) maro_source = maro_normalized;
    std::size_t maro_start = 0;
    while (maro_start <= maro_source.size())
    {
        if (maro_result.maro_lines.size() == maro_lineLimit)
        {
            maro_source = maro_source.substr(0, maro_start);
            maro_result.maro_truncated = true;
            break;
        }
        const auto maro_end = maro_source.find(L'\n', maro_start);
        auto maro_text = maro_source.substr(maro_start, maro_end == std::wstring_view::npos ? maro_source.size() - maro_start : maro_end - maro_start);
        if (!maro_text.empty() && maro_text.back() == L'\r')
            maro_text.remove_suffix(1);
        maro_result.maro_lines.push_back({std::wstring(maro_text), {}, maro_result.maro_lines.size() + 1});
        if (maro_end == std::wstring_view::npos)
            break;
        maro_start = maro_end + 1;
    }
    maro_Inspector(maro_source, maro_path, maro_result).maro_Run();
    return maro_result;
}
