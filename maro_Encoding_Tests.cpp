#include "maro_Text.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <string>
#include <string_view>

namespace
{
bool maro_CheckEncodingSplits(std::string_view maro_bytes, std::wstring_view maro_expected, unsigned maro_page,
    unsigned maro_fallback = 0)
{
    for (std::size_t maro_split = 0; maro_split <= maro_bytes.size(); ++maro_split)
    {
        maro_OutputDecoder maro_decoder(maro_page, maro_fallback);
        std::wstring maro_result = maro_decoder.maro_Decode(maro_bytes.substr(0, maro_split));
        maro_result += maro_decoder.maro_Decode(maro_bytes.substr(maro_split));
        maro_result += maro_decoder.maro_Decode({}, true);
        if (maro_result != maro_expected)
        {
            return false;
        }
    }
    maro_OutputDecoder maro_decoder(maro_page, maro_fallback);
    std::wstring maro_result;
    for (std::size_t maro_index = 0; maro_index < maro_bytes.size(); ++maro_index)
    {
        maro_result += maro_decoder.maro_Decode(maro_bytes.substr(maro_index, 1));
    }
    maro_result += maro_decoder.maro_Decode({}, true);
    return maro_result == maro_expected && maro_decoder.maro_Decode({}, true).empty();
}

std::string maro_EncodePage(std::wstring_view maro_text, unsigned maro_page)
{
    const int maro_count = WideCharToMultiByte(maro_page, 0, maro_text.data(),
        static_cast<int>(maro_text.size()), nullptr, 0, nullptr, nullptr);
    if (maro_count <= 0)
    {
        return {};
    }
    std::string maro_result(static_cast<std::size_t>(maro_count), '\0');
    WideCharToMultiByte(maro_page, 0, maro_text.data(), static_cast<int>(maro_text.size()),
        maro_result.data(), maro_count, nullptr, nullptr);
    return maro_result;
}
}

void maro_TestEncoding(const std::function<void(bool, std::string_view)>& maro_expect)
{
    const std::wstring maro_hangul = L"시작: 한글 입력과 출력\n끝";
    const std::wstring maro_unicode = maro_hangul + L" \U0001f642";
    const auto maro_utf8 = Maro_WideToUtf8(maro_unicode);
    maro_expect(maro_CheckEncodingSplits(maro_utf8, maro_unicode, 0),
        "auto output decoder preserves Korean and emoji at every UTF8 chunk boundary");
    maro_expect(maro_CheckEncodingSplits(maro_utf8, maro_unicode, CP_UTF8),
        "explicit UTF8 output decoder preserves every scalar boundary");
    maro_expect(maro_CheckEncodingSplits("\xef\xbb\xbf" + maro_utf8, maro_unicode, 0),
        "auto output decoder strips a split UTF8 BOM");
    maro_expect(maro_CheckEncodingSplits("\xef\xbb\xbf" + maro_utf8, maro_unicode, CP_UTF8),
        "explicit UTF8 decoder strips a split BOM");
    maro_expect(maro_CheckEncodingSplits("ready> \r\n42", L"ready> \r\n42", 0),
        "ASCII prompts appear immediately without newline buffering");
    const auto maro_cp949 = maro_EncodePage(maro_hangul, 949);
    maro_expect(!maro_cp949.empty() && maro_CheckEncodingSplits(maro_cp949, maro_hangul, 949),
        "CP949 Hangul survives every DBCS lead-byte boundary");
    maro_expect(!maro_cp949.empty() && maro_CheckEncodingSplits(maro_cp949, maro_hangul, 0, 1252),
        "automatic Korean CP949 detection does not depend on a Korean Windows locale");
    maro_expect(!maro_cp949.empty() && maro_CheckEncodingSplits(maro_cp949, maro_hangul, 0, CP_UTF8),
        "automatic Korean CP949 detection works with Windows UTF8 system locale");
    maro_expect(maro_CheckEncodingSplits(maro_EncodePage(L"한", 949), L"한", 0, 1252),
        "a final single Korean syllable is decoded even on Western Windows");
    maro_expect(maro_CheckEncodingSplits(maro_EncodePage(L"값: ", 949), L"값: ", 0, 1252),
        "a Korean CP949 input prompt is recognized before its ASCII boundary");
    maro_expect(maro_CheckEncodingSplits(maro_EncodePage(L"똠방각하", 949), L"똠방각하", 0, 1252),
        "extended CP949 syllables are detected beyond the EUC-KR subset");
    maro_expect(maro_CheckEncodingSplits(maro_utf8, maro_unicode, 0, 1252),
        "UTF8 Korean and emoji remain preferred over the Western fallback page");
    maro_expect(maro_CheckEncodingSplits("\xef\xbb\xbf" + maro_utf8, maro_unicode, 0, 949),
        "an explicit UTF8 BOM remains authoritative over Korean legacy fallback");
    maro_expect(maro_CheckEncodingSplits(std::string("caf\xe9 \x80"), L"caf\u00e9 \u20ac", 1252),
        "explicit Windows1252 output retains accented letters and euro");
    maro_expect(maro_CheckEncodingSplits(std::string("caf\xe9 \x80"), L"caf\u00e9 \u20ac", 0, 1252),
        "automatic legacy detection retains Western text when it is not CP949 Hangul");
    maro_expect(maro_CheckEncodingSplits(std::string("\xc7\xd1"), L"\u00c7\u00d1", 1252, 949),
        "an explicit Western encoding is never overridden by the Korean heuristic");
    maro_expect(maro_CheckEncodingSplits(std::string("A\x80"), L"A\ufffd", 0, 999999),
        "an invalid fallback page replaces malformed input safely");
    const unsigned maro_acp = GetACP();
    const std::wstring maro_ansiText = maro_acp == 949 ? L"가나다" : maro_acp == CP_UTF8 ? maro_hangul : L"caf\u00e9";
    const auto maro_ansiBytes = maro_EncodePage(maro_ansiText, maro_acp);
    const int maro_ansiLength = MultiByteToWideChar(maro_acp, 0, maro_ansiBytes.data(),
        static_cast<int>(maro_ansiBytes.size()), nullptr, 0);
    std::wstring maro_ansiExpected(static_cast<std::size_t>(maro_ansiLength), L'\0');
    MultiByteToWideChar(maro_acp, 0, maro_ansiBytes.data(), static_cast<int>(maro_ansiBytes.size()),
        maro_ansiExpected.data(), maro_ansiLength);
    maro_expect(!maro_ansiBytes.empty() && maro_CheckEncodingSplits(maro_ansiBytes, maro_ansiExpected, 0),
        "auto output decoder falls back to actual Windows ANSI code page");
    std::string maro_utf16le("\xff\xfe", 2);
    std::string maro_utf16be("\xfe\xff", 2);
    for (const auto maro_unit : maro_unicode)
    {
        maro_utf16le.push_back(static_cast<char>(maro_unit & 0xff));
        maro_utf16le.push_back(static_cast<char>(maro_unit >> 8));
        maro_utf16be.push_back(static_cast<char>(maro_unit >> 8));
        maro_utf16be.push_back(static_cast<char>(maro_unit & 0xff));
    }
    maro_expect(maro_CheckEncodingSplits(maro_utf16le, maro_unicode, 0),
        "UTF16LE BOM, code units and surrogate pairs survive every chunk boundary");
    maro_expect(maro_CheckEncodingSplits(maro_utf16be, maro_unicode, 0),
        "UTF16BE BOM and surrogate pairs decode without byte reversal corruption");
    maro_expect(maro_CheckEncodingSplits(maro_utf16le, maro_unicode, 1200),
        "explicit UTF16LE decoder preserves split BOM and code units");
    maro_expect(maro_CheckEncodingSplits(std::string("A\xe1\x80") + "B\xff", L"A\ufffdB\ufffd", CP_UTF8),
        "invalid UTF8 safely replaces malformed subsequences without dropping following text");
    maro_expect(maro_CheckEncodingSplits("\xed\xa0\x80", L"\ufffd\ufffd\ufffd", CP_UTF8),
        "UTF8 encoded surrogate values never reach native output controls");
    maro_expect(maro_CheckEncodingSplits("\xf4\x90\x80\x80", L"\ufffd\ufffd\ufffd\ufffd", CP_UTF8),
        "out of range UTF8 scalars are replaced safely");
    maro_expect(maro_CheckEncodingSplits("\xe1\x80", L"\ufffd", CP_UTF8),
        "final UTF8 flush replaces a truncated scalar exactly once");
    maro_expect(maro_CheckEncodingSplits(std::string("\xff\xfe\x00\xd8", 4), L"\ufffd", 0),
        "final UTF16 flush replaces an unmatched high surrogate");
    maro_expect(maro_CheckEncodingSplits(std::string("\xff\xfe\x41", 3), L"\ufffd", 0),
        "final UTF16 flush replaces a dangling byte");
    maro_expect(maro_CheckEncodingSplits(maro_utf8, maro_unicode, 999999),
        "unsupported output code page safely defaults to UTF8");
    maro_OutputDecoder maro_prompt;
    maro_expect(maro_prompt.maro_Decode("input> ") == L"input> " && maro_prompt.maro_Decode({}).empty(),
        "interactive ASCII prompts stream before process exit");
    maro_OutputDecoder maro_split;
    maro_expect(maro_split.maro_Decode("\xed").empty() && maro_split.maro_Decode("\x95").empty() &&
        maro_split.maro_Decode("\x9c") == L"한", "incomplete Hangul is withheld until its final byte");
}
