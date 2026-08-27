#include "Maro_VisualStudio.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
template <typename T>
class Maro_ComPtr
{
public:
    Maro_ComPtr() = default;
    explicit Maro_ComPtr(T* value, bool addReference = false) noexcept : value_(value)
    {
        if (value_ != nullptr && addReference)
        {
            value_->AddRef();
        }
    }
    ~Maro_ComPtr()
    {
        reset();
    }

    Maro_ComPtr(const Maro_ComPtr&) = delete;
    Maro_ComPtr& operator=(const Maro_ComPtr&) = delete;

    Maro_ComPtr(Maro_ComPtr&& other) noexcept : value_(other.detach()) {}
    Maro_ComPtr& operator=(Maro_ComPtr&& other) noexcept
    {
        if (this != &other)
        {
            reset(other.detach());
        }
        return *this;
    }

    T* get() const noexcept { return value_; }
    T** put() noexcept
    {
        reset();
        return &value_;
    }
    T* operator->() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

    void reset(T* value = nullptr, bool addReference = false) noexcept
    {
        if (value_ != nullptr)
        {
            value_->Release();
        }
        value_ = value;
        if (value_ != nullptr && addReference)
        {
            value_->AddRef();
        }
    }
    T* detach() noexcept
    {
        T* value = value_;
        value_ = nullptr;
        return value;
    }

private:
    T* value_ = nullptr;
};

class Maro_Variant
{
public:
    Maro_Variant() noexcept
    {
        VariantInit(&value_);
    }
    ~Maro_Variant()
    {
        VariantClear(&value_);
    }
    Maro_Variant(const Maro_Variant&) = delete;
    Maro_Variant& operator=(const Maro_Variant&) = delete;

    VARIANT& value() noexcept { return value_; }
    const VARIANT& value() const noexcept { return value_; }
    VARIANT* put() noexcept
    {
        VariantClear(&value_);
        VariantInit(&value_);
        return &value_;
    }

private:
    VARIANT value_{};
};

class Maro_ComApartment
{
public:
    Maro_ComApartment() noexcept
        : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)),
          ownsInitialization_(result_ == S_OK || result_ == S_FALSE)
    {
    }
    ~Maro_ComApartment()
    {
        if (ownsInitialization_)
        {
            CoUninitialize();
        }
    }

    bool usable() const noexcept
    {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }
    HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_ = E_FAIL;
    bool ownsInitialization_ = false;
};

bool Maro_IsBusyResult(HRESULT result) noexcept
{
    return result == RPC_E_CALL_REJECTED ||
        result == RPC_E_SERVERCALL_RETRYLATER ||
        result == RPC_E_RETRY;
}

bool Maro_IsDisconnectedResult(HRESULT result) noexcept
{
    return result == RPC_E_DISCONNECTED ||
        result == RPC_E_SERVER_DIED ||
        result == RPC_E_SERVER_DIED_DNE ||
        result == CO_E_OBJNOTCONNECTED ||
        result == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE);
}

bool Maro_IsAccessDeniedResult(HRESULT result) noexcept
{
    return result == E_ACCESSDENIED || result == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
}

void Maro_ClearExceptionInfo(EXCEPINFO& exceptionInfo) noexcept
{
    if (exceptionInfo.bstrSource != nullptr)
    {
        SysFreeString(exceptionInfo.bstrSource);
    }
    if (exceptionInfo.bstrDescription != nullptr)
    {
        SysFreeString(exceptionInfo.bstrDescription);
    }
    if (exceptionInfo.bstrHelpFile != nullptr)
    {
        SysFreeString(exceptionInfo.bstrHelpFile);
    }
    exceptionInfo = {};
}

HRESULT Maro_Invoke(
    IDispatch* object,
    const wchar_t* member,
    WORD flags,
    std::initializer_list<const VARIANT*> arguments,
    VARIANT* result)
{
    if (object == nullptr || member == nullptr)
    {
        return E_POINTER;
    }

    HRESULT lastResult = E_FAIL;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        LPOLESTR mutableMember = const_cast<LPOLESTR>(member);
        DISPID memberId = DISPID_UNKNOWN;
        HRESULT invokeResult = object->GetIDsOfNames(
            IID_NULL, &mutableMember, 1, LOCALE_USER_DEFAULT, &memberId);
        if (SUCCEEDED(invokeResult))
        {
            std::vector<VARIANTARG> reversedArguments;
            reversedArguments.reserve(arguments.size());
            for (auto iterator = arguments.end(); iterator != arguments.begin();)
            {
                --iterator;
                reversedArguments.push_back(**iterator);
            }

            DISPPARAMS parameters{};
            parameters.rgvarg = reversedArguments.empty() ? nullptr : reversedArguments.data();
            parameters.cArgs = static_cast<UINT>(reversedArguments.size());
            EXCEPINFO exceptionInfo{};
            UINT argumentError = 0;
            if (result != nullptr)
            {
                VariantClear(result);
                VariantInit(result);
            }
            invokeResult = object->Invoke(
                memberId,
                IID_NULL,
                LOCALE_USER_DEFAULT,
                flags,
                &parameters,
                result,
                &exceptionInfo,
                &argumentError);
            if (invokeResult == DISP_E_EXCEPTION)
            {
                if (exceptionInfo.pfnDeferredFillIn != nullptr)
                {
                    exceptionInfo.pfnDeferredFillIn(&exceptionInfo);
                }
                if (FAILED(exceptionInfo.scode))
                {
                    invokeResult = exceptionInfo.scode;
                }
            }
            Maro_ClearExceptionInfo(exceptionInfo);
        }

        lastResult = invokeResult;
        if (!Maro_IsBusyResult(invokeResult) || attempt == 2)
        {
            return invokeResult;
        }
        Sleep(static_cast<DWORD>(40 * (attempt + 1)));
    }
    return lastResult;
}

HRESULT Maro_GetDispatchProperty(
    IDispatch* object,
    const wchar_t* property,
    Maro_ComPtr<IDispatch>& value)
{
    Maro_Variant result;
    const HRESULT callResult = Maro_Invoke(
        object, property, DISPATCH_PROPERTYGET, {}, result.put());
    if (FAILED(callResult))
    {
        return callResult;
    }
    if (result.value().vt == VT_DISPATCH && result.value().pdispVal != nullptr)
    {
        value.reset(result.value().pdispVal, true);
        return S_OK;
    }
    if (result.value().vt == VT_UNKNOWN && result.value().punkVal != nullptr)
    {
        return result.value().punkVal->QueryInterface(
            __uuidof(IDispatch), reinterpret_cast<void**>(value.put()));
    }
    return S_FALSE;
}

HRESULT Maro_GetStringProperty(
    IDispatch* object,
    const wchar_t* property,
    std::wstring& value)
{
    Maro_Variant result;
    const HRESULT callResult = Maro_Invoke(
        object, property, DISPATCH_PROPERTYGET, {}, result.put());
    if (FAILED(callResult))
    {
        return callResult;
    }
    if (result.value().vt == VT_BSTR)
    {
        value.assign(
            result.value().bstrVal == nullptr ? L"" : result.value().bstrVal,
            result.value().bstrVal == nullptr ? 0 : SysStringLen(result.value().bstrVal));
        return S_OK;
    }
    if (result.value().vt == VT_EMPTY || result.value().vt == VT_NULL)
    {
        value.clear();
        return S_OK;
    }
    return DISP_E_TYPEMISMATCH;
}

HRESULT Maro_GetBoolProperty(
    IDispatch* object,
    const wchar_t* property,
    bool& value)
{
    Maro_Variant result;
    const HRESULT callResult = Maro_Invoke(
        object, property, DISPATCH_PROPERTYGET, {}, result.put());
    if (FAILED(callResult))
    {
        return callResult;
    }
    if (result.value().vt == VT_BOOL)
    {
        value = result.value().boolVal != VARIANT_FALSE;
        return S_OK;
    }
    if (result.value().vt == VT_I4 || result.value().vt == VT_INT)
    {
        value = result.value().lVal != 0;
        return S_OK;
    }
    return DISP_E_TYPEMISMATCH;
}

HRESULT Maro_GetUnsignedProperty(
    IDispatch* object,
    const wchar_t* property,
    std::uint32_t& value)
{
    Maro_Variant result;
    const HRESULT callResult = Maro_Invoke(
        object, property, DISPATCH_PROPERTYGET, {}, result.put());
    if (FAILED(callResult))
    {
        return callResult;
    }
    if (result.value().vt == VT_I4 || result.value().vt == VT_INT)
    {
        value = static_cast<std::uint32_t>(result.value().lVal);
        return S_OK;
    }
    if (result.value().vt == VT_UI4 || result.value().vt == VT_UINT)
    {
        value = result.value().ulVal;
        return S_OK;
    }
    return DISP_E_TYPEMISMATCH;
}

HRESULT Maro_GetTextDocument(
    IDispatch* document,
    Maro_ComPtr<IDispatch>& textDocument)
{
    Maro_Variant modelKind;
    modelKind.value().vt = VT_BSTR;
    modelKind.value().bstrVal = SysAllocString(L"TextDocument");
    if (modelKind.value().bstrVal == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    Maro_Variant result;
    const HRESULT callResult = Maro_Invoke(
        document,
        L"Object",
        DISPATCH_METHOD | DISPATCH_PROPERTYGET,
        {&modelKind.value()},
        result.put());
    if (FAILED(callResult))
    {
        return callResult;
    }
    if (result.value().vt == VT_DISPATCH && result.value().pdispVal != nullptr)
    {
        textDocument.reset(result.value().pdispVal, true);
        return S_OK;
    }
    if (result.value().vt == VT_UNKNOWN && result.value().punkVal != nullptr)
    {
        return result.value().punkVal->QueryInterface(
            __uuidof(IDispatch), reinterpret_cast<void**>(textDocument.put()));
    }
    return E_NOINTERFACE;
}

HRESULT Maro_CreateEditPoint(
    IDispatch* textPoint,
    Maro_ComPtr<IDispatch>& editPoint)
{
    Maro_Variant result;
    const HRESULT callResult = Maro_Invoke(
        textPoint, L"CreateEditPoint", DISPATCH_METHOD, {}, result.put());
    if (FAILED(callResult))
    {
        return callResult;
    }
    if (result.value().vt == VT_DISPATCH && result.value().pdispVal != nullptr)
    {
        editPoint.reset(result.value().pdispVal, true);
        return S_OK;
    }
    return E_NOINTERFACE;
}

HRESULT Maro_ReadTextDocument(IDispatch* textDocument, std::wstring& text)
{
    Maro_ComPtr<IDispatch> startPoint;
    Maro_ComPtr<IDispatch> endPoint;
    HRESULT result = Maro_GetDispatchProperty(textDocument, L"StartPoint", startPoint);
    if (FAILED(result) || !startPoint)
    {
        return FAILED(result) ? result : E_NOINTERFACE;
    }
    result = Maro_GetDispatchProperty(textDocument, L"EndPoint", endPoint);
    if (FAILED(result) || !endPoint)
    {
        return FAILED(result) ? result : E_NOINTERFACE;
    }

    Maro_ComPtr<IDispatch> editPoint;
    result = Maro_CreateEditPoint(startPoint.get(), editPoint);
    if (FAILED(result) || !editPoint)
    {
        return FAILED(result) ? result : E_NOINTERFACE;
    }

    Maro_Variant endArgument;
    endArgument.value().vt = VT_DISPATCH;
    endArgument.value().pdispVal = endPoint.get();
    // VARIANT owns a COM reference and VariantClear releases it. Keep the
    // borrowed smart-pointer reference balanced while the invocation runs.
    endPoint->AddRef();
    Maro_Variant textResult;
    result = Maro_Invoke(
        editPoint.get(),
        L"GetText",
        DISPATCH_METHOD,
        {&endArgument.value()},
        textResult.put());
    if (FAILED(result))
    {
        return result;
    }
    if (textResult.value().vt != VT_BSTR)
    {
        return DISP_E_TYPEMISMATCH;
    }
    text.assign(
        textResult.value().bstrVal == nullptr ? L"" : textResult.value().bstrVal,
        textResult.value().bstrVal == nullptr ? 0 : SysStringLen(textResult.value().bstrVal));
    return S_OK;
}

HRESULT Maro_ReplaceTextDocument(IDispatch* textDocument, std::wstring_view replacement)
{
    Maro_ComPtr<IDispatch> startPoint;
    Maro_ComPtr<IDispatch> endPoint;
    HRESULT result = Maro_GetDispatchProperty(textDocument, L"StartPoint", startPoint);
    if (FAILED(result) || !startPoint)
    {
        return FAILED(result) ? result : E_NOINTERFACE;
    }
    result = Maro_GetDispatchProperty(textDocument, L"EndPoint", endPoint);
    if (FAILED(result) || !endPoint)
    {
        return FAILED(result) ? result : E_NOINTERFACE;
    }

    Maro_ComPtr<IDispatch> editPoint;
    result = Maro_CreateEditPoint(startPoint.get(), editPoint);
    if (FAILED(result) || !editPoint)
    {
        return FAILED(result) ? result : E_NOINTERFACE;
    }

    Maro_Variant endArgument;
    endArgument.value().vt = VT_DISPATCH;
    endArgument.value().pdispVal = endPoint.get();
    // VARIANT owns a COM reference and VariantClear releases it. Keep the
    // borrowed smart-pointer reference balanced while the invocation runs.
    endPoint->AddRef();
    Maro_Variant textArgument;
    textArgument.value().vt = VT_BSTR;
    textArgument.value().bstrVal = SysAllocStringLen(
        replacement.data(), static_cast<UINT>(replacement.size()));
    if (textArgument.value().bstrVal == nullptr && !replacement.empty())
    {
        return E_OUTOFMEMORY;
    }
    Maro_Variant flagsArgument;
    flagsArgument.value().vt = VT_I4;
    flagsArgument.value().lVal = 1; // vsEPReplaceTextKeepMarkers
    return Maro_Invoke(
        editPoint.get(),
        L"ReplaceText",
        DISPATCH_METHOD,
        {&endArgument.value(), &textArgument.value(), &flagsArgument.value()},
        nullptr);
}

std::wstring Maro_Lower(std::wstring_view value)
{
    std::wstring result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return result;
}

std::wstring Maro_FileExtension(std::wstring_view path)
{
    const std::size_t slash = path.find_last_of(L"/\\");
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring_view::npos ||
        (slash != std::wstring_view::npos && dot < slash) || dot + 1 >= path.size())
    {
        return {};
    }
    return Maro_Lower(path.substr(dot));
}

bool Maro_ContainsAny(
    std::wstring_view text,
    std::initializer_list<std::wstring_view> needles) noexcept
{
    for (const std::wstring_view needle : needles)
    {
        if (text.find(needle) != std::wstring_view::npos)
        {
            return true;
        }
    }
    return false;
}

bool Maro_IsVisualStudioMoniker(std::wstring_view displayName)
{
    try
    {
        return Maro_Lower(displayName).find(L"visualstudio.dte.") != std::wstring::npos;
    }
    catch (...)
    {
        return false;
    }
}

std::uint32_t Maro_ProcessIdFromMoniker(std::wstring_view displayName)
{
    const std::size_t colon = displayName.find_last_of(L':');
    if (colon == std::wstring_view::npos || colon + 1 >= displayName.size())
    {
        return 0;
    }
    const std::wstring processText(displayName.substr(colon + 1));
    wchar_t* end = nullptr;
    const unsigned long processId = std::wcstoul(processText.c_str(), &end, 10);
    if (end == processText.c_str() || *end != L'\0')
    {
        return 0;
    }
    return static_cast<std::uint32_t>(processId);
}

std::wstring Maro_NormalizePathForComparison(std::wstring_view path)
{
    std::wstring result(path);
    std::replace(result.begin(), result.end(), L'/', L'\\');
    while (result.size() > 3 && result.back() == L'\\')
    {
        result.pop_back();
    }
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return result;
}

bool Maro_SamePath(std::wstring_view left, std::wstring_view right)
{
    if (left.empty() || right.empty())
    {
        return false;
    }
    return Maro_NormalizePathForComparison(left) == Maro_NormalizePathForComparison(right);
}

struct Maro_TokenIdentity
{
    BYTE sid[SECURITY_MAX_SID_SIZE]{};
    DWORD sidLength = 0;
    DWORD integrityLevel = 0;
};

bool Maro_QueryTokenIdentity(HANDLE process, Maro_TokenIdentity& identity) noexcept
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token))
    {
        return false;
    }

    alignas(TOKEN_USER) BYTE userBuffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
    DWORD required = 0;
    bool success = GetTokenInformation(
        token,
        TokenUser,
        userBuffer,
        static_cast<DWORD>(sizeof(userBuffer)),
        &required) != FALSE;
    if (success)
    {
        const auto* user = reinterpret_cast<const TOKEN_USER*>(userBuffer);
        success = IsValidSid(user->User.Sid) != FALSE;
        if (success)
        {
            identity.sidLength = GetLengthSid(user->User.Sid);
            success = identity.sidLength <= sizeof(identity.sid) &&
                CopySid(
                    static_cast<DWORD>(sizeof(identity.sid)),
                    identity.sid,
                    user->User.Sid) != FALSE;
        }
    }

    alignas(TOKEN_MANDATORY_LABEL)
        BYTE integrityBuffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE]{};
    if (success)
    {
        success = GetTokenInformation(
            token,
            TokenIntegrityLevel,
            integrityBuffer,
            static_cast<DWORD>(sizeof(integrityBuffer)),
            &required) != FALSE;
    }
    if (success)
    {
        const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(integrityBuffer);
        success = IsValidSid(label->Label.Sid) != FALSE;
        if (success)
        {
            const UCHAR count = *GetSidSubAuthorityCount(label->Label.Sid);
            success = count != 0;
            if (success)
            {
                identity.integrityLevel =
                    *GetSidSubAuthority(label->Label.Sid, static_cast<DWORD>(count - 1));
            }
        }
    }
    CloseHandle(token);
    return success;
}

bool Maro_HasSameTokenIdentity(
    const Maro_TokenIdentity& left,
    const Maro_TokenIdentity& right) noexcept
{
    return left.sidLength != 0 && right.sidLength != 0 &&
        EqualSid(
            const_cast<BYTE*>(left.sid),
            const_cast<BYTE*>(right.sid)) != FALSE &&
        left.integrityLevel == right.integrityLevel;
}

enum class Maro_DevenvProcessState
{
    NotRunning,
    SameSecurityContext,
    DifferentSecurityContext
};

Maro_DevenvProcessState Maro_GetDevenvProcessState() noexcept
{
    Maro_TokenIdentity currentIdentity;
    const bool hasCurrentIdentity =
        Maro_QueryTokenIdentity(GetCurrentProcess(), currentIdentity);
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return Maro_DevenvProcessState::NotRunning;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool sawRunning = false;
    bool sawSameSecurityContext = false;
    bool sawDifferentSecurityContext = false;
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, L"devenv.exe") != 0)
            {
                continue;
            }
            sawRunning = true;
            const HANDLE process = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (process == nullptr)
            {
                sawDifferentSecurityContext = sawDifferentSecurityContext ||
                    GetLastError() == ERROR_ACCESS_DENIED;
                continue;
            }
            Maro_TokenIdentity processIdentity;
            const bool hasProcessIdentity =
                Maro_QueryTokenIdentity(process, processIdentity);
            CloseHandle(process);
            if (!hasCurrentIdentity || !hasProcessIdentity)
            {
                sawDifferentSecurityContext = sawDifferentSecurityContext ||
                    hasCurrentIdentity;
                sawSameSecurityContext = sawSameSecurityContext ||
                    !hasCurrentIdentity;
                continue;
            }
            const bool same = Maro_HasSameTokenIdentity(
                currentIdentity, processIdentity);
            sawSameSecurityContext = sawSameSecurityContext || same;
            sawDifferentSecurityContext = sawDifferentSecurityContext || !same;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (sawSameSecurityContext || (sawRunning && !sawDifferentSecurityContext))
    {
        return Maro_DevenvProcessState::SameSecurityContext;
    }
    if (sawDifferentSecurityContext)
    {
        return Maro_DevenvProcessState::DifferentSecurityContext;
    }
    return Maro_DevenvProcessState::NotRunning;
}

Maro_VisualStudioStatus Maro_StatusFromResult(HRESULT result) noexcept
{
    if (Maro_IsBusyResult(result))
    {
        return Maro_VisualStudioStatus::Busy;
    }
    if (Maro_IsAccessDeniedResult(result))
    {
        return Maro_VisualStudioStatus::AccessDenied;
    }
    if (Maro_IsDisconnectedResult(result))
    {
        return Maro_VisualStudioStatus::Disconnected;
    }
    return Maro_VisualStudioStatus::AutomationError;
}

std::wstring Maro_MessageForStatus(Maro_VisualStudioStatus status)
{
    switch (status)
    {
    case Maro_VisualStudioStatus::Success:
        return L"Visual Studio의 활성 C/C++ 편집 버퍼를 연결했습니다.";
    case Maro_VisualStudioStatus::ComUnavailable:
        return L"이 스레드에서 Visual Studio COM 자동화를 초기화하지 못했습니다.";
    case Maro_VisualStudioStatus::NotRunning:
        return L"실행 중인 Visual Studio 인스턴스를 찾지 못했습니다.";
    case Maro_VisualStudioStatus::NoActiveDocument:
        return L"Visual Studio에 활성 문서가 없습니다. C 또는 C++ 파일 탭을 선택해 주세요.";
    case Maro_VisualStudioStatus::UnsupportedDocument:
        return L"활성 문서가 지원되는 C/C++ 소스 파일이 아닙니다.";
    case Maro_VisualStudioStatus::Busy:
        return L"Visual Studio가 다른 작업을 처리 중입니다. 잠시 후 다시 시도해 주세요.";
    case Maro_VisualStudioStatus::AccessDenied:
        return L"Visual Studio와 CLive_Maro의 권한 수준이 달라 연결할 수 없습니다. 두 프로그램을 같은 권한으로 실행해 주세요.";
    case Maro_VisualStudioStatus::Disconnected:
        return L"연결했던 Visual Studio 인스턴스가 종료되었거나 다시 시작되었습니다.";
    case Maro_VisualStudioStatus::ReadOnly:
        return L"Visual Studio의 활성 문서가 읽기 전용이라 수정할 수 없습니다.";
    case Maro_VisualStudioStatus::SourceVersionMismatch:
        return L"CLive_Maro 코드가 바뀌어 이전 수정 제안을 적용하지 않았습니다.";
    case Maro_VisualStudioStatus::PathMismatch:
        return L"Visual Studio의 활성 문서가 바뀌어 수정 제안을 적용하지 않았습니다.";
    case Maro_VisualStudioStatus::ContentMismatch:
        return L"Visual Studio 편집 버퍼가 바뀌어 이전 수정 제안을 적용하지 않았습니다.";
    case Maro_VisualStudioStatus::ReplaceFailed:
        return L"Visual Studio 편집 버퍼에 수정 내용을 적용하지 못했습니다.";
    case Maro_VisualStudioStatus::VerificationFailed:
        return L"수정 후 편집 버퍼 검증에 실패했습니다. Visual Studio에서 내용을 확인하고 필요하면 Undo를 사용하세요.";
    case Maro_VisualStudioStatus::AutomationError:
        return L"Visual Studio 자동화 인터페이스에서 예기치 않은 오류가 발생했습니다.";
    }
    return L"Visual Studio 연결 상태를 확인할 수 없습니다.";
}

struct Maro_DocumentContext
{
    Maro_VisualStudioSnapshot snapshot;
    Maro_ComPtr<IDispatch> dte;
    Maro_ComPtr<IDispatch> document;
    Maro_ComPtr<IDispatch> textDocument;
};

struct Maro_ContextResult
{
    Maro_VisualStudioStatus status = Maro_VisualStudioStatus::AutomationError;
    HRESULT comResult = E_FAIL;
    std::optional<Maro_DocumentContext> context;
};

Maro_ContextResult Maro_ReadContext(
    IDispatch* dte,
    std::wstring_view moniker,
    std::uint64_t sourceVersion)
{
    Maro_ContextResult result;
    Maro_DocumentContext context;
    context.dte.reset(dte, true);
    context.snapshot.sourceVersion = sourceVersion;
    context.snapshot.instanceMoniker = std::wstring(moniker);
    context.snapshot.processId = Maro_ProcessIdFromMoniker(moniker);

    HRESULT callResult = Maro_GetDispatchProperty(dte, L"ActiveDocument", context.document);
    if (callResult == S_FALSE || !context.document)
    {
        result.status = Maro_VisualStudioStatus::NoActiveDocument;
        result.comResult = S_FALSE;
        return result;
    }
    if (FAILED(callResult))
    {
        result.status = Maro_StatusFromResult(callResult);
        result.comResult = callResult;
        return result;
    }

    callResult = Maro_GetStringProperty(context.document.get(), L"Name", context.snapshot.name);
    if (FAILED(callResult) && Maro_IsBusyResult(callResult))
    {
        result.status = Maro_VisualStudioStatus::Busy;
        result.comResult = callResult;
        return result;
    }
    const HRESULT pathResult = Maro_GetStringProperty(
        context.document.get(), L"FullName", context.snapshot.path);
    if (FAILED(pathResult))
    {
        if (Maro_IsBusyResult(pathResult) || Maro_IsAccessDeniedResult(pathResult) ||
            Maro_IsDisconnectedResult(pathResult))
        {
            result.status = Maro_StatusFromResult(pathResult);
            result.comResult = pathResult;
            return result;
        }
        context.snapshot.path = context.snapshot.name;
    }
    const HRESULT languageResult = Maro_GetStringProperty(
        context.document.get(), L"Language", context.snapshot.documentLanguage);
    if (FAILED(languageResult) && Maro_IsBusyResult(languageResult))
    {
        result.status = Maro_VisualStudioStatus::Busy;
        result.comResult = languageResult;
        return result;
    }

    callResult = Maro_GetTextDocument(context.document.get(), context.textDocument);
    if (FAILED(callResult) || !context.textDocument)
    {
        result.status = (callResult == DISP_E_UNKNOWNNAME ||
                         callResult == DISP_E_MEMBERNOTFOUND ||
                         callResult == E_NOINTERFACE)
            ? Maro_VisualStudioStatus::UnsupportedDocument
            : Maro_StatusFromResult(callResult);
        result.comResult = callResult;
        return result;
    }

    callResult = Maro_ReadTextDocument(context.textDocument.get(), context.snapshot.text);
    if (FAILED(callResult))
    {
        result.status = Maro_StatusFromResult(callResult);
        result.comResult = callResult;
        return result;
    }

    const std::optional<Maro_Language> language = Maro_InferVisualStudioLanguage(
        context.snapshot.path,
        context.snapshot.documentLanguage,
        context.snapshot.text);
    if (!language)
    {
        result.status = Maro_VisualStudioStatus::UnsupportedDocument;
        result.comResult = S_FALSE;
        return result;
    }
    context.snapshot.language = *language;

    bool saved = false;
    if (SUCCEEDED(Maro_GetBoolProperty(context.document.get(), L"Saved", saved)))
    {
        context.snapshot.saved = saved;
    }
    bool readOnly = false;
    if (SUCCEEDED(Maro_GetBoolProperty(context.document.get(), L"ReadOnly", readOnly)))
    {
        context.snapshot.readOnly = readOnly;
    }
    if (context.snapshot.processId == 0)
    {
        Maro_GetUnsignedProperty(dte, L"ProcessID", context.snapshot.processId);
    }

    result.status = Maro_VisualStudioStatus::Success;
    result.comResult = S_OK;
    result.context = std::move(context);
    return result;
}

HRESULT Maro_GetRotDisplayName(
    IMoniker* moniker,
    IBindCtx* bindContext,
    std::wstring& displayName)
{
    LPOLESTR rawName = nullptr;
    const HRESULT result = moniker->GetDisplayName(bindContext, nullptr, &rawName);
    if (SUCCEEDED(result) && rawName != nullptr)
    {
        displayName = rawName;
    }
    if (rawName != nullptr)
    {
        CoTaskMemFree(rawName);
    }
    return result;
}

HRESULT Maro_GetRotDispatch(
    IRunningObjectTable* runningObjects,
    IMoniker* moniker,
    Maro_ComPtr<IDispatch>& dispatch)
{
    Maro_ComPtr<IUnknown> unknown;
    HRESULT result = runningObjects->GetObject(moniker, unknown.put());
    if (FAILED(result) || !unknown)
    {
        return result;
    }
    return unknown->QueryInterface(
        __uuidof(IDispatch), reinterpret_cast<void**>(dispatch.put()));
}

struct Maro_RotSearchResult
{
    Maro_VisualStudioStatus status = Maro_VisualStudioStatus::NotRunning;
    std::size_t instancesInspected = 0;
    std::optional<Maro_DocumentContext> context;
};

Maro_RotSearchResult Maro_FindActiveContext(
    std::uint64_t sourceVersion,
    std::optional<std::wstring_view> exactMoniker,
    std::wstring_view preferredMoniker = {})
{
    Maro_RotSearchResult result;
    Maro_ComPtr<IRunningObjectTable> runningObjects;
    HRESULT comResult = GetRunningObjectTable(0, runningObjects.put());
    if (FAILED(comResult) || !runningObjects)
    {
        result.status = Maro_StatusFromResult(comResult);
        return result;
    }
    Maro_ComPtr<IBindCtx> bindContext;
    comResult = CreateBindCtx(0, bindContext.put());
    if (FAILED(comResult) || !bindContext)
    {
        result.status = Maro_VisualStudioStatus::ComUnavailable;
        return result;
    }
    Maro_ComPtr<IEnumMoniker> enumerator;
    comResult = runningObjects->EnumRunning(enumerator.put());
    if (FAILED(comResult) || !enumerator)
    {
        result.status = Maro_StatusFromResult(comResult);
        return result;
    }

    DWORD foregroundProcessId = 0;
    if (const HWND foregroundWindow = GetForegroundWindow(); foregroundWindow != nullptr)
    {
        GetWindowThreadProcessId(foregroundWindow, &foregroundProcessId);
    }

    bool sawAccessDenied = false;
    bool sawBusy = false;
    bool sawDisconnected = false;
    bool sawNoDocument = false;
    bool sawUnsupported = false;
    bool sawAutomationError = false;
    std::optional<Maro_DocumentContext> preferred;
    std::optional<Maro_DocumentContext> foreground;
    std::optional<Maro_DocumentContext> fallback;

    for (;;)
    {
        Maro_ComPtr<IMoniker> moniker;
        ULONG fetched = 0;
        const HRESULT nextResult = enumerator->Next(1, moniker.put(), &fetched);
        if (nextResult != S_OK || fetched != 1)
        {
            break;
        }

        std::wstring displayName;
        const HRESULT nameResult = Maro_GetRotDisplayName(
            moniker.get(), bindContext.get(), displayName);
        if (FAILED(nameResult))
        {
            continue;
        }
        if (!Maro_IsVisualStudioMoniker(displayName))
        {
            continue;
        }
        if (exactMoniker && _wcsicmp(displayName.c_str(), std::wstring(*exactMoniker).c_str()) != 0)
        {
            continue;
        }
        ++result.instancesInspected;

        Maro_ComPtr<IDispatch> dte;
        const HRESULT objectResult = Maro_GetRotDispatch(
            runningObjects.get(), moniker.get(), dte);
        if (FAILED(objectResult) || !dte)
        {
            sawAccessDenied = sawAccessDenied || Maro_IsAccessDeniedResult(objectResult);
            sawBusy = sawBusy || Maro_IsBusyResult(objectResult);
            sawDisconnected = sawDisconnected || Maro_IsDisconnectedResult(objectResult);
            sawAutomationError = sawAutomationError ||
                (!Maro_IsAccessDeniedResult(objectResult) &&
                 !Maro_IsBusyResult(objectResult) &&
                 !Maro_IsDisconnectedResult(objectResult));
            continue;
        }

        Maro_ContextResult contextResult = Maro_ReadContext(
            dte.get(), displayName, sourceVersion);
        if (contextResult.status == Maro_VisualStudioStatus::Success && contextResult.context)
        {
            if (exactMoniker)
            {
                result.status = Maro_VisualStudioStatus::Success;
                result.context = std::move(contextResult.context);
                return result;
            }
            if (!preferred && !preferredMoniker.empty() &&
                contextResult.context->snapshot.instanceMoniker == preferredMoniker)
            {
                preferred = std::move(contextResult.context);
            }
            else if (!foreground && foregroundProcessId != 0 &&
                     contextResult.context->snapshot.processId == foregroundProcessId)
            {
                foreground = std::move(contextResult.context);
            }
            else if (!fallback)
            {
                fallback = std::move(contextResult.context);
            }
            continue;
        }
        sawAccessDenied = sawAccessDenied || contextResult.status == Maro_VisualStudioStatus::AccessDenied;
        sawBusy = sawBusy || contextResult.status == Maro_VisualStudioStatus::Busy;
        sawDisconnected = sawDisconnected || contextResult.status == Maro_VisualStudioStatus::Disconnected;
        sawNoDocument = sawNoDocument || contextResult.status == Maro_VisualStudioStatus::NoActiveDocument;
        sawUnsupported = sawUnsupported || contextResult.status == Maro_VisualStudioStatus::UnsupportedDocument;
        sawAutomationError = sawAutomationError || contextResult.status == Maro_VisualStudioStatus::AutomationError;
    }

    const Maro_DevenvProcessState processState = Maro_GetDevenvProcessState();
    if (foreground)
    {
        result.status = Maro_VisualStudioStatus::Success;
        result.context = std::move(foreground);
    }
    else if (preferred)
    {
        result.status = Maro_VisualStudioStatus::Success;
        result.context = std::move(preferred);
    }
    else if (fallback)
    {
        result.status = Maro_VisualStudioStatus::Success;
        result.context = std::move(fallback);
    }
    else if (sawAccessDenied)
    {
        result.status = Maro_VisualStudioStatus::AccessDenied;
    }
    else if (sawBusy)
    {
        result.status = Maro_VisualStudioStatus::Busy;
    }
    else if (sawNoDocument)
    {
        result.status = Maro_VisualStudioStatus::NoActiveDocument;
    }
    else if (sawUnsupported)
    {
        result.status = Maro_VisualStudioStatus::UnsupportedDocument;
    }
    else if (sawDisconnected || exactMoniker)
    {
        result.status = Maro_VisualStudioStatus::Disconnected;
    }
    else if (sawAutomationError)
    {
        result.status = Maro_VisualStudioStatus::AutomationError;
    }
    else
    {
        result.status = processState == Maro_DevenvProcessState::DifferentSecurityContext
            ? Maro_VisualStudioStatus::AccessDenied
            : processState == Maro_DevenvProcessState::SameSecurityContext
                ? Maro_VisualStudioStatus::Busy
                : Maro_VisualStudioStatus::NotRunning;
    }
    return result;
}
} // namespace

bool Maro_IsVisualStudioCppPath(std::wstring_view path) noexcept
{
    try
    {
        const std::wstring extension = Maro_FileExtension(path);
        static constexpr std::wstring_view supported[] = {
            L".c", L".cc", L".cp", L".cpp", L".cxx", L".c++",
            L".h", L".hh", L".hpp", L".hxx", L".inl", L".ipp",
            L".tpp", L".ixx", L".cppm", L".mpp"};
        return std::find(std::begin(supported), std::end(supported), extension) != std::end(supported);
    }
    catch (...)
    {
        return false;
    }
}

std::optional<Maro_Language> Maro_InferVisualStudioLanguage(
    std::wstring_view path,
    std::wstring_view documentLanguage,
    std::wstring_view text) noexcept
{
    try
    {
        const std::wstring extension = Maro_FileExtension(path);
        if (extension == L".c")
        {
            return Maro_Language::C17;
        }
        static constexpr std::wstring_view cppExtensions[] = {
            L".cc", L".cp", L".cpp", L".cxx", L".c++", L".hh",
            L".hpp", L".hxx", L".inl", L".ipp", L".tpp", L".ixx",
            L".cppm", L".mpp"};
        if (std::find(std::begin(cppExtensions), std::end(cppExtensions), extension) !=
            std::end(cppExtensions))
        {
            return Maro_Language::Cpp20;
        }
        if (extension == L".h")
        {
            if (Maro_ContainsAny(text, {
                    L"_Generic", L"_Static_assert", L"<stdbool.h>", L"<stdatomic.h>"}))
            {
                return Maro_Language::C17;
            }
            if (Maro_ContainsAny(text, {
                    L"namespace ", L"template<", L"template <", L"class ",
                    L"constexpr", L"std::", L"<iostream>"}))
            {
                return Maro_Language::Cpp20;
            }
        }
        else if (!extension.empty())
        {
            return std::nullopt;
        }

        const std::wstring language = Maro_Lower(documentLanguage);
        if (language.find(L"c++") != std::wstring::npos ||
            language.find(L"cpp") != std::wstring::npos)
        {
            return Maro_Language::Cpp20;
        }
        if (language == L"c" || language == L"c language")
        {
            return Maro_Language::C17;
        }
        if (extension == L".h")
        {
            return Maro_Language::Cpp20;
        }
        return std::nullopt;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

Maro_VisualStudioReadResult Maro_VisualStudio::ReadActiveDocument(
    std::uint64_t sourceVersion,
    std::wstring_view preferredInstanceMoniker) const
try
{
    Maro_VisualStudioReadResult result;
    Maro_ComApartment apartment;
    if (!apartment.usable())
    {
        result.status = Maro_VisualStudioStatus::ComUnavailable;
        result.message = Maro_MessageForStatus(result.status);
        return result;
    }

    Maro_RotSearchResult search = Maro_FindActiveContext(
        sourceVersion, std::nullopt, preferredInstanceMoniker);
    result.status = search.status;
    result.instancesInspected = search.instancesInspected;
    result.message = Maro_MessageForStatus(result.status);
    if (search.context)
    {
        result.snapshot = std::move(search.context->snapshot);
    }
    return result;
}
catch (...)
{
    Maro_VisualStudioReadResult result;
    result.status = Maro_VisualStudioStatus::AutomationError;
    result.message = Maro_MessageForStatus(result.status);
    return result;
}

Maro_VisualStudioApplyResult Maro_VisualStudio::ApplyFullText(
    const Maro_VisualStudioSnapshot& expected,
    std::uint64_t currentSourceVersion,
    std::wstring_view replacementText) const
try
{
    Maro_VisualStudioApplyResult result;
    if (currentSourceVersion != expected.sourceVersion)
    {
        result.status = Maro_VisualStudioStatus::SourceVersionMismatch;
        result.message = Maro_MessageForStatus(result.status);
        return result;
    }
    if (expected.instanceMoniker.empty())
    {
        result.status = Maro_VisualStudioStatus::Disconnected;
        result.message = Maro_MessageForStatus(result.status);
        return result;
    }

    Maro_ComApartment apartment;
    if (!apartment.usable())
    {
        result.status = Maro_VisualStudioStatus::ComUnavailable;
        result.message = Maro_MessageForStatus(result.status);
        return result;
    }

    Maro_RotSearchResult search = Maro_FindActiveContext(
        currentSourceVersion, expected.instanceMoniker);
    if (search.status != Maro_VisualStudioStatus::Success || !search.context)
    {
        result.status = search.status;
        result.message = Maro_MessageForStatus(result.status);
        return result;
    }
    Maro_DocumentContext context = std::move(*search.context);
    if (!Maro_SamePath(context.snapshot.path, expected.path))
    {
        result.status = Maro_VisualStudioStatus::PathMismatch;
        result.message = Maro_MessageForStatus(result.status);
        return result;
    }
    if (context.snapshot.text != expected.text)
    {
        result.status = Maro_VisualStudioStatus::ContentMismatch;
        result.message = Maro_MessageForStatus(result.status);
        return result;
    }
    if (context.snapshot.readOnly)
    {
        result.status = Maro_VisualStudioStatus::ReadOnly;
        result.message = Maro_MessageForStatus(result.status);
        return result;
    }

    const HRESULT replaceResult = Maro_ReplaceTextDocument(
        context.textDocument.get(), replacementText);
    if (FAILED(replaceResult))
    {
        result.status = Maro_IsAccessDeniedResult(replaceResult)
            ? Maro_VisualStudioStatus::ReadOnly
            : Maro_StatusFromResult(replaceResult);
        if (result.status == Maro_VisualStudioStatus::AutomationError)
        {
            result.status = Maro_VisualStudioStatus::ReplaceFailed;
        }
        result.message = Maro_MessageForStatus(result.status);
        return result;
    }

    Maro_ContextResult verification = Maro_ReadContext(
        context.dte.get(), expected.instanceMoniker, currentSourceVersion);
    if (verification.status != Maro_VisualStudioStatus::Success || !verification.context ||
        !Maro_SamePath(verification.context->snapshot.path, expected.path) ||
        verification.context->snapshot.text != replacementText)
    {
        result.status = Maro_VisualStudioStatus::VerificationFailed;
        result.message = Maro_MessageForStatus(result.status);
        return result;
    }

    result.status = Maro_VisualStudioStatus::Success;
    result.message = L"현재 Visual Studio 편집 버퍼에 수정 내용을 적용했습니다.";
    result.snapshotAfter = std::move(verification.context->snapshot);
    return result;
}
catch (...)
{
    Maro_VisualStudioApplyResult result;
    result.status = Maro_VisualStudioStatus::AutomationError;
    result.message = Maro_MessageForStatus(result.status);
    return result;
}
