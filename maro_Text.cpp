#include "maro_Text.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <limits>

namespace
{
std::size_t Maro_Utf8Length(char32_t codePoint) noexcept
{
    if (codePoint <= 0x7f)
    {
        return 1;
    }
    if (codePoint <= 0x7ff)
    {
        return 2;
    }
    if (codePoint <= 0xffff)
    {
        return 3;
    }
    return 4;
}

char32_t Maro_ReadUtf16CodePoint(
    std::wstring_view text,
    std::size_t index,
    std::size_t& codeUnits) noexcept
{
    const char32_t first = static_cast<char32_t>(text[index]);
    codeUnits = 1;
    if (first >= 0xd800 && first <= 0xdbff && index + 1 < text.size())
    {
        const char32_t second = static_cast<char32_t>(text[index + 1]);
        if (second >= 0xdc00 && second <= 0xdfff)
        {
            codeUnits = 2;
            return 0x10000 + ((first - 0xd800) << 10) + (second - 0xdc00);
        }
    }
    if (first >= 0xd800 && first <= 0xdfff)
    {
        return 0xfffd;
    }
    return first;
}

unsigned maro_DetectKoreanCodePage(std::string_view maro_bytes, bool maro_final, unsigned maro_fallback)
{
    std::size_t maro_hangul = 0;
    std::size_t maro_index = 0;
    while (maro_index < maro_bytes.size() && maro_index < 8)
    {
        const auto maro_first = static_cast<unsigned char>(maro_bytes[maro_index]);
        if (maro_first < 0x80)
        {
            return maro_hangul != 0 ? 949 : maro_fallback;
        }
        if (!IsDBCSLeadByteEx(949, maro_first)) return maro_fallback;
        if (maro_index + 1 == maro_bytes.size())
        {
            return !maro_final ? 0 : maro_hangul != 0 ? 949 : maro_fallback;
        }
        wchar_t maro_unit = 0;
        if (MultiByteToWideChar(949, MB_ERR_INVALID_CHARS, maro_bytes.data() + maro_index,
                2, &maro_unit, 1) != 1 || maro_unit < 0xac00 || maro_unit > 0xd7a3)
        {
            return maro_fallback;
        }
        if (++maro_hangul == 2) return 949;
        maro_index += 2;
    }
    return !maro_final ? 0 : maro_hangul != 0 ? 949 : maro_fallback;
}
}

std::string Maro_WideToUtf8(std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }

    const int inputLength = text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
        ? std::numeric_limits<int>::max()
        : static_cast<int>(text.size());

    int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        inputLength,
        nullptr,
        0,
        nullptr,
        nullptr);
    DWORD flags = WC_ERR_INVALID_CHARS;
    if (required <= 0)
    {
        flags = 0;
        required = WideCharToMultiByte(
            CP_UTF8, flags, text.data(), inputLength, nullptr, 0, nullptr, nullptr);
    }
    if (required <= 0)
    {
        return {};
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            flags,
            text.data(),
            inputLength,
            result.data(),
            required,
            nullptr,
            nullptr) <= 0)
    {
        return {};
    }
    return result;
}

std::wstring Maro_Utf8ToWide(std::string_view text)
{
    if (text.empty())
    {
        return {};
    }

    const int inputLength = text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
        ? std::numeric_limits<int>::max()
        : static_cast<int>(text.size());

    int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        inputLength,
        nullptr,
        0);
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (required <= 0)
    {
        flags = 0;
        required = MultiByteToWideChar(CP_UTF8, flags, text.data(), inputLength, nullptr, 0);
    }
    if (required <= 0)
    {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            flags,
            text.data(),
            inputLength,
            result.data(),
            required) <= 0)
    {
        return {};
    }
    return result;
}

std::wstring Maro_NormalizeNewlines(std::wstring_view text)
{
    std::wstring result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        if (text[index] == L'\r')
        {
            if (index + 1 < text.size() && text[index + 1] == L'\n')
            {
                ++index;
            }
            result.push_back(L'\n');
        }
        else
        {
            result.push_back(text[index]);
        }
    }
    return result;
}

std::uint64_t Maro_HashSource(std::wstring_view text)
{
    constexpr std::uint64_t offsetBasis = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offsetBasis;
    const std::string utf8 = Maro_WideToUtf8(text);
    for (const unsigned char byte : utf8)
    {
        hash ^= byte;
        hash *= prime;
    }
    return hash;
}

std::size_t Maro_Utf8ByteColumnToUtf16Index(
    std::wstring_view line,
    std::size_t oneBasedByteColumn)
{
    const std::size_t targetByte = oneBasedByteColumn > 0 ? oneBasedByteColumn - 1 : 0;
    std::size_t utf8Bytes = 0;
    std::size_t index = 0;
    while (index < line.size())
    {
        if (utf8Bytes >= targetByte)
        {
            return index;
        }
        std::size_t codeUnits = 1;
        const char32_t codePoint = Maro_ReadUtf16CodePoint(line, index, codeUnits);
        const std::size_t nextBytes = utf8Bytes + Maro_Utf8Length(codePoint);
        if (targetByte < nextBytes)
        {
            return index;
        }
        utf8Bytes = nextBytes;
        index += codeUnits;
    }
    return line.size();
}

std::size_t Maro_LineColumnToUtf16Offset(
    std::wstring_view text,
    std::size_t oneBasedLine,
    std::size_t oneBasedUtf8ByteColumn)
{
    if (oneBasedLine == 0)
    {
        return 0;
    }

    std::size_t line = 1;
    std::size_t lineStart = 0;
    while (line < oneBasedLine && lineStart < text.size())
    {
        const std::size_t newline = text.find_first_of(L"\r\n", lineStart);
        if (newline == std::wstring_view::npos)
        {
            return text.size();
        }
        lineStart = newline + 1;
        if (text[newline] == L'\r' && lineStart < text.size() && text[lineStart] == L'\n')
        {
            ++lineStart;
        }
        ++line;
    }

    const std::size_t lineEnd = text.find_first_of(L"\r\n", lineStart);
    const std::size_t length = (lineEnd == std::wstring_view::npos ? text.size() : lineEnd) - lineStart;
    return lineStart + Maro_Utf8ByteColumnToUtf16Index(
        text.substr(lineStart, length), oneBasedUtf8ByteColumn);
}

std::wstring Maro_SanitizeOutput(std::wstring_view text)
{
    std::wstring result;
    result.reserve(text.size());
    for (const wchar_t character : text)
    {
        if (character == L'\n' || character == L'\r' || character == L'\t' || character >= 0x20)
        {
            result.push_back(character);
        }
        else
        {
            result.push_back(0xfffd);
        }
    }
    return result;
}

maro_OutputDecoder::maro_OutputDecoder(unsigned maro_codePage, unsigned maro_fallbackCodePage)
    : maro_codePage_(maro_codePage),
      maro_fallbackCodePage_(maro_fallbackCodePage == 0 ? GetACP() : maro_fallbackCodePage)
{
    CPINFO maro_info{};
    if (maro_codePage_ != 0 && maro_codePage_ != CP_UTF8 &&
        maro_codePage_ != 1200 && maro_codePage_ != 1201 &&
        (!GetCPInfo(maro_codePage_, &maro_info) || maro_info.MaxCharSize > 2))
    {
        maro_codePage_ = CP_UTF8;
    }
    if (maro_fallbackCodePage_ != CP_UTF8 &&
        (!GetCPInfo(maro_fallbackCodePage_, &maro_info) || maro_info.MaxCharSize > 2))
    {
        maro_fallbackCodePage_ = CP_UTF8;
    }
}

std::wstring maro_OutputDecoder::maro_Decode(std::string_view maro_text, bool maro_final)
{
    maro_pending_.append(maro_text);
    if (maro_pending_.empty())
    {
        return {};
    }
    if (!maro_started_)
    {
        struct maro_Bom
        {
            std::string_view maro_bytes;
            unsigned maro_page;
        };
        constexpr maro_Bom maro_boms[] = {{"\xef\xbb\xbf", CP_UTF8}, {"\xff\xfe", 1200}, {"\xfe\xff", 1201}};
        for (const auto& maro_bom : maro_boms)
        {
            if (maro_codePage_ != 0 && maro_codePage_ != maro_bom.maro_page)
            {
                continue;
            }
            if (maro_bom.maro_bytes.starts_with(maro_pending_) &&
                maro_pending_.size() < maro_bom.maro_bytes.size() && !maro_final)
            {
                return {};
            }
            if (maro_pending_.starts_with(maro_bom.maro_bytes))
            {
                maro_codePage_ = maro_bom.maro_page;
                maro_pending_.erase(0, maro_bom.maro_bytes.size());
                break;
            }
        }
        maro_started_ = true;
    }
    std::wstring maro_result;
    maro_result.reserve(maro_pending_.size());
    std::size_t maro_index = 0;
    if (maro_codePage_ == 0 || maro_codePage_ == CP_UTF8)
    {
        while (maro_index < maro_pending_.size())
        {
            const auto maro_first = static_cast<unsigned char>(maro_pending_[maro_index]);
            if (maro_first < 0x80)
            {
                maro_result.push_back(maro_first);
                ++maro_index;
                continue;
            }
            const std::size_t maro_length = maro_first >= 0xc2 && maro_first <= 0xdf ? 2 :
                maro_first >= 0xe0 && maro_first <= 0xef ? 3 : maro_first >= 0xf0 && maro_first <= 0xf4 ? 4 : 0;
            std::size_t maro_valid = 1;
            char32_t maro_scalar = maro_first & (maro_length == 2 ? 0x1f : maro_length == 3 ? 0x0f : 0x07);
            while (maro_valid < maro_length && maro_index + maro_valid < maro_pending_.size())
            {
                const auto maro_byte = static_cast<unsigned char>(maro_pending_[maro_index + maro_valid]);
                if (maro_byte < 0x80 || maro_byte > 0xbf ||
                    (maro_valid == 1 && ((maro_first == 0xe0 && maro_byte < 0xa0) ||
                    (maro_first == 0xed && maro_byte > 0x9f) || (maro_first == 0xf0 && maro_byte < 0x90) ||
                    (maro_first == 0xf4 && maro_byte > 0x8f))))
                {
                    break;
                }
                maro_scalar = (maro_scalar << 6) | (maro_byte & 0x3f);
                ++maro_valid;
            }
            if (maro_length != 0 && maro_valid == maro_length)
            {
                if (maro_scalar > 0xffff)
                {
                    maro_scalar -= 0x10000;
                    maro_result.push_back(static_cast<wchar_t>(0xd800 + (maro_scalar >> 10)));
                    maro_result.push_back(static_cast<wchar_t>(0xdc00 + (maro_scalar & 0x3ff)));
                }
                else
                {
                    maro_result.push_back(static_cast<wchar_t>(maro_scalar));
                }
                maro_index += maro_length;
                continue;
            }
            if (maro_length != 0 && maro_index + maro_valid == maro_pending_.size() && !maro_final)
            {
                break;
            }
            if (maro_codePage_ == 0)
            {
                maro_codePage_ = maro_DetectKoreanCodePage(
                    std::string_view(maro_pending_).substr(maro_index), maro_final, maro_fallbackCodePage_);
                if (maro_codePage_ == 0) break;
                if (maro_codePage_ != CP_UTF8)
                {
                    break;
                }
            }
            maro_result.push_back(0xfffd);
            maro_index += maro_valid;
        }
    }
    if (maro_codePage_ == 1200 || maro_codePage_ == 1201)
    {
        const auto maro_unit = [&](std::size_t maro_offset) {
            const auto maro_first = static_cast<unsigned char>(maro_pending_[maro_offset]);
            const auto maro_second = static_cast<unsigned char>(maro_pending_[maro_offset + 1]);
            return static_cast<wchar_t>(maro_codePage_ == 1200 ? maro_first | (maro_second << 8) :
                (maro_first << 8) | maro_second);
        };
        while (maro_index + 1 < maro_pending_.size())
        {
            const wchar_t maro_first = maro_unit(maro_index);
            if (maro_first >= 0xd800 && maro_first <= 0xdbff)
            {
                if (maro_index + 3 >= maro_pending_.size() && !maro_final)
                {
                    break;
                }
                if (maro_index + 3 < maro_pending_.size())
                {
                    const wchar_t maro_second = maro_unit(maro_index + 2);
                    if (maro_second >= 0xdc00 && maro_second <= 0xdfff)
                    {
                        maro_result.push_back(maro_first);
                        maro_result.push_back(maro_second);
                        maro_index += 4;
                        continue;
                    }
                }
                maro_result.push_back(0xfffd);
            }
            else
            {
                maro_result.push_back(maro_first >= 0xdc00 && maro_first <= 0xdfff ? 0xfffd : maro_first);
            }
            maro_index += 2;
        }
        if (maro_final && maro_index < maro_pending_.size())
        {
            maro_result.push_back(0xfffd);
            maro_index = maro_pending_.size();
        }
    }
    else if (maro_codePage_ != 0 && maro_codePage_ != CP_UTF8)
    {
        const std::size_t maro_start = maro_index;
        while (maro_index < maro_pending_.size())
        {
            const bool maro_lead = IsDBCSLeadByteEx(maro_codePage_,
                static_cast<BYTE>(maro_pending_[maro_index])) != FALSE;
            if (maro_lead && maro_index + 1 == maro_pending_.size() && !maro_final)
            {
                break;
            }
            maro_index += maro_lead && maro_index + 1 < maro_pending_.size() ? 2 : 1;
        }
        const auto maro_count = maro_index - maro_start;
        if (maro_count != 0 && maro_count <= static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            const char* maro_data = maro_pending_.data() + maro_start;
            const int maro_length = static_cast<int>(maro_count);
            const int maro_required = MultiByteToWideChar(maro_codePage_, 0, maro_data, maro_length, nullptr, 0);
            if (maro_required > 0)
            {
                const auto maro_offset = maro_result.size();
                maro_result.resize(maro_offset + static_cast<std::size_t>(maro_required));
                MultiByteToWideChar(maro_codePage_, 0, maro_data, maro_length,
                    maro_result.data() + maro_offset, maro_required);
            }
            else
            {
                maro_result.append(maro_count, 0xfffd);
            }
        }
    }
    maro_pending_.erase(0, maro_index);
    return maro_result;
}
