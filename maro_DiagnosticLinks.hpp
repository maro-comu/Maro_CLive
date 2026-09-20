#pragma once
#include <string>
#include <string_view>
#include <algorithm>
#include <iterator>

inline std::wstring maro_DiagnosticDocumentation(std::wstring_view maro_code)
{
    if (maro_code.size() == 5 && maro_code[0] == L'C' &&
        std::all_of(maro_code.begin() + 1, maro_code.end(), [](wchar_t maro_ch) { return maro_ch >= L'0' && maro_ch <= L'9'; }))
        return L"https://learn.microsoft.com/ko-kr/search/?terms=" + std::wstring(maro_code);
    if (maro_code.starts_with(L"-W") && maro_code.size() > 2 && maro_code.size() < 100 &&
        std::all_of(maro_code.begin() + 2, maro_code.end(), [](wchar_t maro_ch) {
            return (maro_ch >= L'a' && maro_ch <= L'z') || (maro_ch >= L'0' && maro_ch <= L'9') || maro_ch == L'-'; }))
        return L"https://clang.llvm.org/docs/DiagnosticsReference.html#w" + std::wstring(maro_code.substr(2));
    constexpr std::wstring_view maro_localCodes[] = {L"MARO-MACRO-VALUE", L"MARO-MACRO-PRECEDENCE",
        L"MARO-LOCAL-LIFETIME", L"MARO-STRING-COMPARE", L"MARO-ARRAY-ASSIGNMENT", L"MARO-PRINTF-POINTER",
        L"MARO-POINTER-TYPE", L"MARO-CONDITION-ASSIGNMENT", L"MARO-DIVIDE-ZERO", L"MARO-MISSING-SEMICOLON"};
    if (std::find(std::begin(maro_localCodes), std::end(maro_localCodes), maro_code) != std::end(maro_localCodes))
    {
        std::wstring maro_anchor(maro_code);
        for (auto& maro_ch : maro_anchor) if (maro_ch >= L'A' && maro_ch <= L'Z') maro_ch += L'a' - L'A';
        return L"https://github.com/maro-comu/Maro_CLive/blob/v2.3.4/maro_Diagnostics.md#" + maro_anchor;
    }
    return {};
}
