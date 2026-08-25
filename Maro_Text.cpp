#include "Maro_Text.hpp"

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
} // namespace

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
    // FNV-1a is used only as a cache key. Freshness always uses sourceVersion.
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
