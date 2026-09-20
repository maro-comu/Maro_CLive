#include "maro_DiagnosticGuidance.hpp"

#include <array>

bool maro_TestDiagnosticGuidance()
{
    const std::array maro_codes = {
        L"C2143", L"C2146", L"C4430", L"C4431", L"C2065", L"C2064", L"C2059", L"C2057",
        L"C2106", L"C2440", L"C2660", L"C2664", L"C2665", L"C4130", L"C4172", L"C4477",
        L"C4706", L"C4700", L"C4715", L"C4244", L"C4267", L"C4018", L"C4996", L"C6385",
        L"C6386", L"C6011", L"C6001", L"C6054", L"C6262", L"C1083", L"C2124", L"C2061",
        L"C2144", L"C2198", L"C4701", L"C6031", L"C4473", L"C4474"
    };
    for (const auto* maro_code : maro_codes)
    {
        const auto maro_message = maro_CompilerGuidance(maro_code, L"raw compiler message");
        if (maro_message.empty() || maro_message == L"raw compiler message" || maro_message.size() > 240 ||
            maro_message.find(L'\0') != std::wstring::npos || maro_message.find(L"문법을 완성") != std::wstring::npos || maro_message.find(L"클릭") != std::wstring::npos)
            return false;
    }
    const auto maro_has = [](std::wstring_view maro_code, std::wstring_view maro_raw, std::wstring_view maro_expected) {
        return maro_CompilerGuidance(maro_code, maro_raw).find(maro_expected) != std::wstring::npos;
    };
    if (!maro_has(L"C2143", L"syntax error: missing ';' before 'return'", L"매크로") ||
        !maro_has(L"C2146", L"구문 오류: ';'이(가) 'value' 앞에 없습니다.", L"자료형") ||
        !maro_has(L"C2143", L"syntax error: missing ')' before '}'", L"닫는 기호 ')'") ||
        !maro_has(L"C2143", L"구문 오류: ']'이(가) ';' 앞에 없습니다.", L"닫는 기호 ']'") ||
        !maro_has(L"C2143", L"syntax error: missing '(' before 'value'", L"여는 기호 '('") ||
        !maro_has(L"C2146", L"syntax error: missing ',' before identifier 'value'", L"쉼표") ||
        !maro_has(L"C2143", L"unexpected message without quoted tokens", L"단정할 수는 없습니다") ||
        !maro_has(L"C2065", L"'maro_missing': undeclared identifier", L"maro_missing") ||
        !maro_has(L"C2065", L"'한글변수': 선언되지 않은 식별자입니다.", L"한글변수") ||
        !maro_has(L"C4130", L"", L"내용 비교가 목적이면") ||
        !maro_has(L"C2106", L"", L"배열 전체") ||
        !maro_has(L"C4172", L"", L"수명이 보장") ||
        !maro_has(L"C4477", L"", L"scanf") ||
        !maro_has(L"C4706", L"", L"의도한 대입이면") ||
        !maro_has(L"C2440", L"", L"강제 형변환") ||
        !maro_has(L"C4018", L"", L"음수 가능성") ||
        !maro_has(L"C4996", L"'strcpy': This function or variable may be unsafe.", L"버퍼 크기 인수") ||
        !maro_has(L"C4996", L"'strcpy': 이 함수 또는 변수는 안전하지 않을 수 있습니다.", L"버퍼 크기 인수") ||
        !maro_has(L"C4996", L"'oldApi': use newApi instead", L"무조건 _s") ||
        !maro_has(L"C4996", L"'oldApi': use newApi instead", L"newApi") ||
        !maro_has(L"C4996", L"'my_scanf_wrapper': deprecated; use parse_record instead", L"parse_record") ||
        !maro_has(L"C4996", L"'strcpy': custom deprecated declaration", L"custom deprecated declaration") ||
        !maro_has(L"C4477", L"'printf': format string '%d' requires an argument of type 'int'", L"unsigned int는 %u") ||
        !maro_has(L"C4477", L"'printf': format string '%d' requires an argument of type 'int'", L"void*") ||
        !maro_has(L"C4477", L"'scanf': format string '%f' requires an argument of type 'float *'", L"double*는 %lf") ||
        !maro_has(L"C4477", L"'std::scanf': wrong argument type", L"int*는 %d") ||
        !maro_has(L"C4477", L"'my_printf_wrapper': wrong argument type", L"함수 문서") ||
        !maro_has(L"C4701", L"potentially uninitialized local variable 'maro_value' used", L"maro_value") ||
        !maro_has(L"C6031", L"Return value ignored: 'maro_read' could return unexpected value", L"maro_read") ||
        !maro_has(L"C6031", L"", L"모든 API에서 0이 성공") ||
        !maro_has(L"C6385", L"", L"원소 개수 미만") ||
        !maro_has(L"C6386", L"", L"'\\0'을 넣을 공간도 남기세요") ||
        !maro_has(L"C6054", L"", L"'\\0'이 없을 수 있습니다") ||
        !maro_has(L"C6054", L"", L"문자열 함수에 전달하세요") ||
        !maro_has(L"C6011", L"", L"NULL이면 오류를 처리")) return false;
    if (maro_CompilerGuidance(L"-Wcustom", L" \tno matching overloaded function  \r\ncompiler context") !=
        L"no matching overloaded function") return false;
    if (maro_CompilerGuidance(L"C9999", L"\n\n useful detail\nrest") != L"useful detail") return false;
    if (maro_CompilerGuidance(L"C4700", L"'한글변수': uninitialized local variable used").find(L".\n") == std::wstring::npos) return false;
    if (maro_CompilerGuidance(L"", L" \t\r\n").empty()) return false;
    if (maro_CompilerGuidance(L"unknown", std::wstring(8192, L'x')).size() > 1024) return false;
    return true;
}
