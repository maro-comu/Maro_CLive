#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

std::string Maro_WideToUtf8(std::wstring_view text);
std::wstring Maro_Utf8ToWide(std::string_view text);
std::wstring Maro_NormalizeNewlines(std::wstring_view text);
std::uint64_t Maro_HashSource(std::wstring_view text);

std::size_t Maro_Utf8ByteColumnToUtf16Index(
    std::wstring_view line,
    std::size_t oneBasedByteColumn);

std::size_t Maro_LineColumnToUtf16Offset(
    std::wstring_view text,
    std::size_t oneBasedLine,
    std::size_t oneBasedUtf8ByteColumn);

std::wstring Maro_SanitizeOutput(std::wstring_view text);

class maro_OutputDecoder
{
public:
    explicit maro_OutputDecoder(unsigned maro_codePage = 0);
    std::wstring maro_Decode(std::string_view maro_text, bool maro_final = false);

private:
    std::string maro_pending_;
    unsigned maro_codePage_ = 0;
    bool maro_started_ = false;
};
