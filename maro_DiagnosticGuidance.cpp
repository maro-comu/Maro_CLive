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

std::wstring maro_Readable(std::wstring_view maro_message)
{
    std::wstring maro_text(maro_message);
    if (maro_text.find(L'\n') == std::wstring::npos)
        if (const auto maro_break = maro_text.find(L". "); maro_break != std::wstring::npos)
            maro_text[maro_break + 1] = L'\n';
    return maro_text;
}

std::wstring maro_Named(std::wstring_view maro_name, std::wstring_view maro_message)
{
    if (maro_name.empty() || maro_name.find_first_of(L"\\/\r\n\t") != std::wstring_view::npos)
        return maro_Readable(maro_message);
    return L"'" + std::wstring(maro_name) + L"': " + maro_Readable(maro_message);
}

struct maro_GuidanceEntry
{
    std::wstring_view maro_code;
    std::wstring_view maro_message;
};

constexpr maro_GuidanceEntry maro_guidance[] = {
    {L"C4430", L"선언에 자료형이 없거나 자료형 이름을 찾지 못했습니다. 변수·함수 앞에 의도한 자료형을 적고, 사용자 정의 형식은 선언이나 헤더가 먼저 나오는지 확인하세요."},
    {L"C4431", L"C 선언에 자료형이 없거나 인식되지 않았습니다. 필요한 선언·헤더와 철자를 확인해 의도한 자료형을 명시하세요(매크로가 잘못 펼쳐진 경우에는 정의를 먼저 고치세요)."},
    {L"C2064", L"함수처럼 호출한 대상이 호출 가능한 함수가 아닙니다. 같은 이름의 변수에 가려졌는지 확인하고, 함수 포인터의 형식과 멤버 함수의 호출 대상을 맞추세요."},
    {L"C2059", L"이 위치에 올 수 없는 구문입니다. 앞 문장의 괄호·구분 기호와 사용한 매크로의 정의를 확인하고, 먼저 보고된 원인 오류부터 수정하세요."},
    {L"C2057", L"컴파일할 때 값이 정해지는 상수가 필요한 자리입니다. 배열 크기·case 값은 정수 상수식으로 바꾸고, 실행 중 정해지는 배열 크기라면 동적 저장 공간을 사용하세요."},
    {L"C2106", L"대입 연산자의 왼쪽이 수정 가능한 저장 공간이 아닙니다. 일반 변수에는 값을 대입하고, 배열 전체는 '=' 대신 크기를 확인해 원소나 문자열 내용을 복사하세요."},
    {L"C2440", L"값의 자료형을 대상 자료형으로 변환할 수 없습니다. 선언과 전달할 값의 형식·const 여부를 맞추고, 강제 형변환으로 오류만 숨기지 마세요."},
    {L"C2660", L"함수 선언과 호출의 인수 개수가 다릅니다. 실제로 호출되는 함수의 선언을 열어 인수의 개수와 순서를 맞추세요."},
    {L"C2664", L"함수에 전달한 인수의 자료형이 선언과 맞지 않습니다. 해당 인수의 값·포인터·참조·const 여부를 선언에 맞추고, 임의의 강제 형변환은 피하세요."},
    {L"C2665", L"전달한 인수들을 모두 받을 수 있는 함수가 없습니다. 후보 함수들의 인수 개수·형식·순서를 비교해 의도한 선언과 호출을 맞추세요."},
    {L"C4130", L"문자열 상수의 주소에 논리 연산을 하고 있습니다. 내용 비교가 목적이면 문자 종류에 맞는 strcmp/wcscmp의 결과를 0과 비교하세요(두 문자열은 유효하고 끝이 표시되어 있어야 합니다)."},
    {L"C4172", L"함수가 끝나면 사라지는 지역 변수나 임시 값의 주소·참조를 반환합니다. 호출자가 넘긴 저장 공간에 결과를 쓰거나 수명이 보장되는 값을 반환하도록 바꾸세요."},
    {L"C4706", L"조건식에서 '='로 값을 대입하고 있습니다. 비교가 목적이면 '=='로 바꾸고, 의도한 대입이면 대입 결과를 0 등과 명시적으로 비교하세요."},
    {L"C4700", L"값을 넣기 전에 지역 변수를 읽습니다. 선언할 때 의미 있는 초기값을 넣거나, 사용에 도달하는 모든 경로에서 먼저 값을 대입하세요."},
    {L"C4701", L"값을 넣지 않은 채 지역 변수를 읽는 경로가 있을 수 있습니다. 초기값을 정하거나, 모든 분기에서 값을 대입한 뒤에만 사용하도록 고치세요."},
    {L"C4715", L"값을 반환하지 않고 끝나는 실행 경로가 있습니다. 모든 정상 종료 경로에서 반환형에 맞는 값을 return하도록 분기를 완성하세요."},
    {L"C4244", L"자료형 변환 중 소수 부분이나 값의 일부를 잃을 수 있습니다. 원래 값을 담을 수 있는 형식을 사용하거나, 변환 전에 범위와 반올림·버림 정책을 확인하세요."},
    {L"C4267", L"size_t 크기 값을 더 작은 자료형에 담아 잘릴 수 있습니다. 길이·개수는 size_t로 받거나, 대상 형식의 최댓값 이내인지 검사한 뒤 변환하세요."},
    {L"C4018", L"부호가 있는 값과 없는 값을 비교해 음수가 큰 양수로 해석될 수 있습니다. 음수 가능성을 먼저 처리한 뒤 두 값의 범위를 보존하는 자료형으로 비교하세요."},
    {L"C6385", L"저장 공간의 범위를 벗어나 읽을 가능성이 있습니다. 인덱스를 0 이상 원소 개수 미만으로 제한하고, 전달한 읽기 길이가 실제 버퍼 크기를 넘지 않게 하세요."},
    {L"C6386", L"저장 공간의 범위를 벗어나 쓸 가능성이 있습니다. 쓰는 길이를 실제 버퍼 크기 이내로 제한하고, 문자열이라면 끝의 '\\0'을 넣을 공간도 남기세요."},
    {L"C6011", L"NULL일 수 있는 포인터가 가리키는 값을 사용합니다. 할당·함수 호출의 결과를 검사하고, NULL이면 오류를 처리한 뒤 접근하지 않도록 분기하세요."},
    {L"C6001", L"초기화되지 않은 메모리나 변수를 읽을 수 있습니다. 사용하는 모든 실행 경로에서 먼저 값을 채우고, 입력 함수의 성공 여부도 확인하세요."},
    {L"C6054", L"문자열 끝을 나타내는 '\\0'이 없을 수 있습니다. 버퍼 안에 종료 문자가 들어가도록 길이를 제한한 뒤 문자열 함수에 전달하세요."},
    {L"C6031", L"실패할 수 있는 함수의 반환값을 확인하지 않았습니다. API가 정한 성공·실패 값을 검사하고, 실패한 경우 결과를 사용하지 않도록 처리하세요(모든 API에서 0이 성공을 뜻하지는 않습니다)."},
    {L"C6262", L"이 함수의 지역 데이터가 스택을 많이 사용합니다. 큰 배열은 수명을 관리하는 동적 저장 공간으로 옮기고, 호출 깊이와 할당 실패 처리도 확인하세요."},
    {L"C1083", L"컴파일에 필요한 파일을 열 수 없습니다. 파일이 실제로 있는지 확인하고, 헤더라면 이름·추가 포함 디렉터리·접근 권한을 점검하세요."},
    {L"C2124", L"상수식을 계산하는 중 0으로 나누게 됩니다. 나누는 값이나 그 값을 만드는 매크로를 수정해 0이 되지 않도록 하세요."},
    {L"C2061", L"이름이 올 수 없는 위치에 식별자가 있습니다. 자료형으로 사용했다면 그 형식의 선언·헤더를 먼저 포함하고, 바로 앞의 선언과 구분 기호도 확인하세요."},
    {L"C2144", L"자료형 앞에서 필요한 기호를 찾지 못했습니다. 앞 선언의 끝과 괄호 짝을 확인하고, 매크로가 있다면 펼쳐지는 구문부터 고치세요."},
    {L"C2198", L"함수 호출에 필요한 인수가 빠졌습니다. 함수 선언을 확인해 각 인수의 의미와 순서에 맞춰 값을 전달하세요; 빈자리를 임의의 0이나 NULL로 채우지는 마세요."},
    {L"C4473", L"서식 문자열이 요구하는 인수보다 실제 전달한 인수가 적습니다. 서식과 인수를 순서대로 대조하고, printf의 '*' 폭·정밀도 인수나 scanf_s의 버퍼 크기 인수도 확인하세요."},
    {L"C4474", L"서식 문자열이 사용하지 않는 인수가 전달되었습니다. 출력·입력하려던 항목의 서식이 빠졌는지 확인하고, 불필요한 인수만 제거하세요."}
};
}

std::wstring maro_CompilerGuidance(std::wstring_view maro_code, std::wstring_view maro_raw)
{
    maro_raw = maro_FirstLine(maro_raw);
    if (maro_code == L"C2143" || maro_code == L"C2146")
    {
        const auto maro_token = maro_QuotedToken(maro_raw);
        if (maro_token == L";")
            return L"컴파일러가 이 구문 앞에서 ';'를 예상했습니다. 앞 문장의 끝을 확인하되, 매크로 정의나 자료형이 잘못된 경우에는 그 원인을 먼저 고치세요.";
        if (maro_token == L")" || maro_token == L"]" || maro_token == L"}")
            return L"닫는 기호 '" + std::wstring(maro_token) + L"'가 필요한 구문입니다. 대응하는 여는 기호와 내부 표현식을 확인해 올바른 위치에서 닫으세요(매크로를 사용했다면 정의도 확인하세요).";
        if (maro_token == L"(" || maro_token == L"[" || maro_token == L"{")
            return L"여는 기호 '" + std::wstring(maro_token) + L"'가 필요한 구문입니다. 조건식·배열·블록의 작성 형식에 맞게 여닫는 기호를 짝지어 고치세요.";
        if (maro_token == L",")
            return L"항목을 구분하는 ','가 필요한 구문입니다. 인수·초기값·선언 사이의 구분을 확인하고, 필요한 위치에 쉼표를 넣으세요.";
        return L"구문에 필요한 기호나 자료형을 찾지 못했습니다. 표시된 위치 앞의 선언·괄호와 매크로 정의를 확인하세요; 이 코드만으로 ';' 누락이라고 단정할 수는 없습니다.";
    }
    if (maro_code == L"C2065")
    {
        const auto maro_name = maro_QuotedToken(maro_raw);
        return (maro_name.empty() ? std::wstring(L"사용한 이름") : L"'" + std::wstring(maro_name) + L"'") +
            L"의 선언을 찾을 수 없습니다. 철자·대소문자·사용 범위를 확인하고, 필요한 선언이나 헤더를 사용 위치보다 앞에 두세요.";
    }
    if (maro_code == L"C4477")
    {
        const auto maro_name = maro_QuotedToken(maro_raw);
        if (maro_IsFunction(maro_name, {L"scanf", L"scanf_s", L"fscanf", L"fscanf_s", L"sscanf", L"sscanf_s", L"wscanf", L"wscanf_s", L"fwscanf", L"fwscanf_s", L"swscanf", L"swscanf_s"}))
            return maro_Named(maro_name, L"입력 서식과 저장할 변수의 주소 형식이 다릅니다.\nint*는 %d, float*는 %f, double*는 %lf에 맞추세요. 변수 값을 형변환하지 말고 올바른 주소를 전달하며, 문자열은 버퍼 크기와 입력 폭도 확인하세요.");
        if (maro_IsFunction(maro_name, {L"printf", L"printf_s", L"fprintf", L"fprintf_s", L"sprintf", L"sprintf_s", L"snprintf", L"snprintf_s", L"wprintf", L"wprintf_s", L"fwprintf", L"fwprintf_s", L"swprintf", L"swprintf_s"}))
            return maro_Named(maro_name, L"출력 서식과 인수의 자료형이 다릅니다.\n순서를 확인하고 int는 %d, unsigned int는 %u, double은 %f, 객체 주소는 %p와 void*에 맞추세요. long·size_t 등은 길이 지정자도 필요합니다. 값과 주소 중 무엇을 출력할지 먼저 정하세요.");
        return maro_Named(maro_name, L"서식 지정자와 인수의 자료형이 다릅니다. 각 항목의 순서·실제 자료형·길이 지정자를 함수 문서와 대조하세요. printf는 값을, scanf는 값을 저장할 주소를 받으므로 두 규칙을 구분해야 합니다.");
    }
    if (maro_code == L"C4996")
    {
        const auto maro_name = maro_QuotedToken(maro_raw);
        const bool maro_unsafe = maro_raw.find(L"unsafe") != std::wstring_view::npos || maro_raw.find(L"안전하지") != std::wstring_view::npos;
        if (maro_unsafe && maro_IsFunction(maro_name, {L"strcpy", L"strcat", L"sprintf", L"vsprintf", L"scanf", L"fscanf", L"sscanf", L"wcscpy", L"wcscat", L"swprintf", L"vswprintf", L"wscanf", L"fwscanf", L"swscanf"}))
            return maro_Named(maro_name, L"MSVC가 더 안전한 사용 방식이나 대체 API를 권장합니다. 실제 버퍼 크기에 맞춰 쓰기·입력 길이를 제한하세요. _s 계열로 바꿀 때는 필요한 버퍼 크기 인수와 반환값 처리도 함께 맞춰야 합니다.");
        return L"사용한 선언이 더 이상 권장되지 않습니다. 아래 API 제공자의 안내에 맞춰 대체 기능과 호출 방법을 확인하세요; 이름 뒤에 무조건 _s를 붙이거나 경고를 숨기는 것만으로 해결되지는 않습니다." +
            (maro_raw.empty() ? std::wstring{} : L"\n" + std::wstring(maro_raw));
    }
    for (const auto& maro_entry : maro_guidance)
        if (maro_entry.maro_code == maro_code)
        {
            if (maro_code == L"C2061" || maro_code == L"C2198" || maro_code == L"C4172" || maro_code == L"C4700" ||
                maro_code == L"C4701" || maro_code == L"C4715" || maro_code == L"C6011" || maro_code == L"C6001" || maro_code == L"C6031")
                return maro_Named(maro_QuotedToken(maro_raw), maro_entry.maro_message);
            return maro_Readable(maro_entry.maro_message);
        }
    if (!maro_raw.empty()) return std::wstring(maro_raw);
    return L"이 진단만으로 안전한 수정 방법을 판단할 수 없습니다. 오류코드의 공식 설명과 해당 코드를 함께 확인하세요.";
}
