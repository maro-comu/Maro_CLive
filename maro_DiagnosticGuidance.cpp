#include "maro_DiagnosticGuidance.hpp"

#include <algorithm>
#include <initializer_list>

namespace
{
std::wstring_view maro_FirstLine(std::wstring_view maro_text)
{
    maro_text = maro_text.substr(0, (std::min)(maro_text.size(), std::size_t{1024}));
    const auto maro_first = maro_text.find_first_not_of(L" \t\r\n");
    if (maro_first == std::wstring_view::npos) return {};
    maro_text.remove_prefix(maro_first);
    maro_text = maro_text.substr(0, maro_text.find_first_of(L"\r\n"));
    const auto maro_last = maro_text.find_last_not_of(L" \t");
    return maro_text.substr(0, maro_last + 1);
}

std::wstring_view maro_QuotedToken(std::wstring_view maro_text)
{
    const auto maro_first = maro_text.find(L'\'');
    if (maro_first == std::wstring_view::npos) return {};
    const auto maro_last = maro_text.find(L'\'', maro_first + 1);
    if (maro_last == std::wstring_view::npos || maro_last - maro_first > 96) return {};
    return maro_text.substr(maro_first + 1, maro_last - maro_first - 1);
}

bool maro_IsFunction(std::wstring_view maro_name, std::initializer_list<std::wstring_view> maro_names)
{
    if (maro_name.starts_with(L"std::")) maro_name.remove_prefix(5);
    return std::find(maro_names.begin(), maro_names.end(), maro_name) != maro_names.end();
}

std::wstring maro_Named(std::wstring_view maro_name, std::wstring_view maro_message)
{
    if (maro_name.empty() || maro_name.size() > 40 || maro_name.find_first_of(L"\\/\r\n\t") != std::wstring_view::npos)
        return std::wstring(maro_message);
    return L"'" + std::wstring(maro_name) + L"': " + std::wstring(maro_message);
}

std::wstring maro_FormatGuidance(std::wstring_view maro_raw, bool maro_input)
{
    const auto maro_name = maro_QuotedToken(maro_raw);
    const auto maro_offset = maro_raw.find(L'\'', maro_raw.find(L'\'') + 1);
    const auto maro_format = maro_offset == std::wstring_view::npos ? std::wstring_view{} : maro_QuotedToken(maro_raw.substr(maro_offset + 1));
    std::wstring_view maro_type;
    if (maro_format == L"%d" || maro_format == L"%i") maro_type = maro_input ? L"int* 주소" : L"int 값";
    if (maro_format == L"%u") maro_type = maro_input ? L"unsigned int* 주소" : L"unsigned int 값";
    if (maro_format == L"%f") maro_type = maro_input ? L"float* 주소" : L"double 값";
    if (maro_format == L"%lf") maro_type = maro_input ? L"double* 주소" : L"double 값";
    if (!maro_input && maro_format == L"%p") maro_type = L"void* 객체 주소";
    if (!maro_type.empty())
        return maro_Named(maro_name, L"'" + std::wstring(maro_format) + L"'에는 " + std::wstring(maro_type) +
            (maro_input ? L"를 전달하거나, 저장할 변수에 맞게 서식을 바꾸세요." :
                maro_format == L"%p" ? L"를 전달하거나, 출력할 값에 맞게 서식을 바꾸세요." : L"을 전달하거나, 출력할 값에 맞게 서식을 바꾸세요."));
    return maro_Named(maro_name, maro_input ?
        L"저장할 변수의 주소 형식에 입력 서식을 맞추세요(문자열은 버퍼 크기와 입력 폭도 확인)." :
        L"출력할 값의 자료형에 서식·길이 지정자를 맞추세요(객체 주소는 %p와 void* 사용).");
}

struct maro_GuidanceEntry
{
    std::wstring_view maro_code;
    std::wstring_view maro_message;
};

constexpr maro_GuidanceEntry maro_guidance[] = {
    {L"C4430", L"변수·함수 앞에 자료형을 적고, 사용자 정의 형식의 선언을 먼저 두세요."},
    {L"C4431", L"올바른 자료형을 명시하되, 잘못된 매크로가 원인이면 정의부터 고치세요."},
    {L"C2064", L"함수 이름을 가린 변수를 고치거나, 호출 가능한 함수·함수 포인터를 사용하세요."},
    {L"C2059", L"표시된 구문과 바로 앞 문장의 괄호·구분 기호를 고치세요."},
    {L"C2057", L"정수 상수식을 쓰고, 실행 중 정해지는 배열 크기는 동적 저장 공간을 사용하세요."},
    {L"C2106", L"수정 가능한 변수에 대입하고, 배열 전체는 '=' 대신 크기에 맞게 내용을 복사하세요."},
    {L"C2440", L"강제 형변환 대신, 대상과 값의 자료형·const 조건을 맞추세요."},
    {L"C2660", L"함수 선언에 맞춰 인수의 개수와 순서를 고치세요."},
    {L"C2664", L"함수 선언에 맞춰 인수의 자료형과 포인터·참조·const 조건을 고치세요."},
    {L"C2665", L"호출하려는 함수 선언에 맞춰 인수의 개수·형식·순서를 고치세요."},
    {L"C4130", L"내용 비교가 목적이면 유효한 문자열에 strcmp/wcscmp를 사용해 결과를 0과 비교하세요."},
    {L"C4172", L"지역 변수의 주소 대신 수명이 보장되는 값을 반환하거나 호출자의 저장 공간에 쓰세요."},
    {L"C4706", L"비교라면 '=='로 바꾸고, 의도한 대입이면 그 결과를 0 등과 명시적으로 비교하세요."},
    {L"C4700", L"사용 전에 의미 있는 초기값을 넣도록 모든 실행 경로를 고치세요."},
    {L"C4701", L"값을 읽기 전에 모든 분기에서 초기화되도록 고치세요."},
    {L"C4715", L"모든 정상 종료 경로에서 반환형에 맞는 값을 return하세요."},
    {L"C4244", L"값을 온전히 담는 자료형을 쓰거나, 범위와 반올림·버림 방식을 정한 뒤 변환하세요."},
    {L"C4267", L"길이·개수는 size_t로 받거나, 대상 자료형의 범위 안인지 검사한 뒤 변환하세요."},
    {L"C4018", L"음수 가능성을 먼저 처리한 뒤, 두 값의 범위를 보존하는 자료형으로 비교하세요."},
    {L"C6385", L"인덱스를 0 이상 원소 개수 미만으로 제한하고, 읽기 길이를 버퍼 크기에 맞추세요."},
    {L"C6386", L"쓰기 길이를 버퍼 크기로 제한하고, 문자열 끝의 '\\0'을 넣을 공간도 남기세요."},
    {L"C6011", L"포인터를 검사해 NULL이면 오류를 처리하고, NULL이 아닐 때만 접근하세요."},
    {L"C6001", L"모든 실행 경로에서 값을 먼저 채우고, 입력 함수가 성공했을 때만 사용하세요."},
    {L"C6054", L"버퍼 안에 문자열 끝의 '\\0'을 넣은 뒤 문자열 함수에 전달하세요."},
    {L"C6031", L"API 문서의 성공·실패 기준으로 반환값을 검사하고, 실패하면 결과를 사용하지 마세요."},
    {L"C6262", L"큰 지역 배열을 동적 저장 공간으로 옮기고, 할당 실패와 해제를 처리하세요."},
    {L"C1083", L"파일 이름과 실제 위치를 확인해 포함 경로를 맞추고, 읽기 권한을 확인하세요."},
    {L"C2124", L"나누는 값 또는 그 값을 만드는 매크로를 고쳐 0으로 나누지 않게 하세요."},
    {L"C2061", L"자료형의 선언·헤더를 사용 위치보다 앞에 두고, 앞 선언의 구분 기호를 확인하세요."},
    {L"C2144", L"바로 앞 선언의 끝과 괄호 짝을 고치고, 매크로가 원인이면 정의부터 수정하세요."},
    {L"C2198", L"함수 선언의 의미와 순서에 맞춰 빠진 인수를 전달하세요."},
    {L"C4473", L"서식에 필요한 인수를 채우세요(printf의 '*' 인수와 scanf_s의 버퍼 크기도 포함)."},
    {L"C4474", L"의도한 입출력 항목의 서식을 추가하거나, 사용하지 않는 인수만 제거하세요."}
};
}

std::wstring maro_CompilerGuidance(std::wstring_view maro_code, std::wstring_view maro_raw)
{
    maro_raw = maro_FirstLine(maro_raw);
    if (maro_code == L"C2143" || maro_code == L"C2146")
    {
        const auto maro_token = maro_QuotedToken(maro_raw);
        if (maro_token == L";")
            return L"앞 문장의 ';' 누락을 확인하되, 매크로·자료형 오류가 원인이면 먼저 고치세요.";
        if (maro_token == L")" || maro_token == L"]" || maro_token == L"}")
            return L"여는 기호와 내부 표현식을 확인해 닫는 기호 '" + std::wstring(maro_token) + L"'를 올바른 위치에 넣으세요.";
        if (maro_token == L"(" || maro_token == L"[" || maro_token == L"{")
            return L"구문의 작성 형식에 맞춰 여는 기호 '" + std::wstring(maro_token) + L"'와 닫는 기호를 짝지어 넣으세요.";
        if (maro_token == L",")
            return L"인수·초기값·선언 항목 사이를 확인해 빠진 쉼표 ','를 넣으세요.";
        return L"표시된 위치 앞의 선언과 괄호 짝을 확인하고, 매크로가 원인이면 정의부터 고치세요.";
    }
    if (maro_code == L"C2065")
    {
        const auto maro_name = maro_QuotedToken(maro_raw);
        return maro_Named(maro_name, L"철자와 사용 범위를 확인하고, 필요한 선언·헤더를 사용 위치보다 앞에 두세요.");
    }
    if (maro_code == L"C4477")
    {
        const auto maro_name = maro_QuotedToken(maro_raw);
        if (maro_IsFunction(maro_name, {L"scanf", L"scanf_s", L"fscanf", L"fscanf_s", L"sscanf", L"sscanf_s", L"wscanf", L"wscanf_s", L"fwscanf", L"fwscanf_s", L"swscanf", L"swscanf_s"}))
            return maro_FormatGuidance(maro_raw, true);
        if (maro_IsFunction(maro_name, {L"printf", L"printf_s", L"fprintf", L"fprintf_s", L"sprintf", L"sprintf_s", L"snprintf", L"snprintf_s", L"wprintf", L"wprintf_s", L"fwprintf", L"fwprintf_s", L"swprintf", L"swprintf_s"}))
            return maro_FormatGuidance(maro_raw, false);
        return maro_Named(maro_name, L"함수 문서에 맞춰 서식과 인수형을 고치세요(printf는 값, scanf는 저장할 주소).");
    }
    if (maro_code == L"C4996")
    {
        const auto maro_name = maro_QuotedToken(maro_raw);
        const bool maro_unsafe = maro_raw.find(L"unsafe") != std::wstring_view::npos || maro_raw.find(L"안전하지") != std::wstring_view::npos;
        if (maro_unsafe && maro_IsFunction(maro_name, {L"strcpy", L"strcat", L"sprintf", L"vsprintf", L"scanf", L"fscanf", L"sscanf", L"wcscpy", L"wcscat", L"swprintf", L"vswprintf", L"wscanf", L"fwscanf", L"swscanf"}))
            return maro_Named(maro_name, L"_s 계열로 바꿀 때는 필요한 버퍼 크기 인수와 반환값 처리도 맞추세요.");
        const auto maro_detail = maro_raw.substr(0, 100);
        return L"API 제공자의 대체 기능·호출 방법을 확인하세요" + (maro_detail.empty() ? std::wstring(L".") :
            L": " + std::wstring(maro_detail) + (maro_detail.size() < maro_raw.size() ? L"…" : L""));
    }
    for (const auto& maro_entry : maro_guidance)
        if (maro_entry.maro_code == maro_code)
        {
            if (maro_code == L"C2061" || maro_code == L"C2198" || maro_code == L"C4172" || maro_code == L"C4700" ||
                maro_code == L"C4701" || maro_code == L"C4715" || maro_code == L"C6011" || maro_code == L"C6001" || maro_code == L"C6031")
                return maro_Named(maro_QuotedToken(maro_raw), maro_entry.maro_message);
            return std::wstring(maro_entry.maro_message);
        }
    if (!maro_raw.empty()) return std::wstring(maro_raw);
    return L"오류코드의 공식 설명을 열어 해당 코드의 수정 방법을 확인하세요.";
}
