#pragma once
#include <string>
#include <string_view>
#include <algorithm>
#include <iterator>

inline std::wstring maro_DiagnosticDocumentation(std::wstring_view maro_code)
{
    constexpr std::wstring_view maro_runtimeCodes[] = {L"0xC0000005", L"0xC0000094", L"0xC00000FD",
        L"0xC0000017", L"0xC0000135", L"0xC000007B", L"0xC000001D", L"0xC0000409"};
    if (std::find(std::begin(maro_runtimeCodes), std::end(maro_runtimeCodes), maro_code) != std::end(maro_runtimeCodes))
        return L"https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-erref/596a1078-e883-4972-9bbc-49e60bebca55";
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
        return L"https://github.com/maro-comu/Maro_CLive/blob/v2.4.0/maro_Diagnostics.md#" + maro_anchor;
    }
    return {};
}
