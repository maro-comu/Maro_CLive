#include "Maro_CLive_Maro_Package.hpp"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <new>
#include <sstream>
#include <utility>

const CLSID Maro_CLive_Maro_PackageClsid =
    {0xc766c863, 0x83a1, 0x4fcf, {0xbb, 0xf5, 0xf2, 0x6f, 0xfc, 0x83, 0x79, 0x6b}};
const GUID Maro_CLive_Maro_CommandSet =
    {0x24748981, 0x75eb, 0x4e44, {0xbc, 0xbe, 0x9f, 0x92, 0x0d, 0x16, 0xd3, 0xc3}};
const GUID Maro_CLive_Maro_OutputPane =
    {0x315cbbd5, 0xcc25, 0x4567, {0xbc, 0x11, 0x70, 0x21, 0xb3, 0xf8, 0xd8, 0x40}};

OBJECT_ENTRY_AUTO(Maro_CLive_Maro_PackageClsid, Maro_CLive_Maro_Package)

namespace
{
std::wstring Maro_LowerExtension(const std::wstring& path)
{
    std::wstring extension = std::filesystem::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    return extension;
}

const wchar_t* Maro_SeverityText(Maro_Severity severity) noexcept
{
    switch (severity)
    {
    case Maro_Severity::Warning: return L"warning";
    case Maro_Severity::Error: return L"error";
    case Maro_Severity::Fatal: return L"fatal";
    default: return L"info";
    }
}

bool Maro_IsCompleted(const Maro_ResultEnvelope& result) noexcept
{
    return result.phase == Maro_Phase::Completed;
}
} // namespace

STDMETHODIMP Maro_CLive_Maro_Package::SetSite(IServiceProvider* serviceProvider)
{
    try
    {
        if (serviceProvider == nullptr)
        {
            Shutdown();
            return S_OK;
        }

        serviceProvider_ = serviceProvider;
        const HRESULT paneResult = EnsureOutputPane();
        if (FAILED(paneResult))
        {
            serviceProvider_.Release();
            return paneResult;
        }

        engine_ = std::make_unique<Maro_Engine>([this](Maro_ResultEnvelope result) {
            PublishResult(result);
        });
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        Shutdown();
        return E_OUTOFMEMORY;
    }
    catch (...)
    {
        Shutdown();
        return E_FAIL;
    }
}

STDMETHODIMP Maro_CLive_Maro_Package::QueryClose(BOOL* canClose)
{
    if (canClose == nullptr)
    {
        return E_POINTER;
    }
    *canClose = TRUE;
    return S_OK;
}

STDMETHODIMP Maro_CLive_Maro_Package::Close()
{
    Shutdown();
    return S_OK;
}

STDMETHODIMP Maro_CLive_Maro_Package::GetAutomationObject(LPCOLESTR, IDispatch** dispatch)
{
    if (dispatch == nullptr)
    {
        return E_POINTER;
    }
    *dispatch = nullptr;
    return E_NOTIMPL;
}

STDMETHODIMP Maro_CLive_Maro_Package::CreateTool(REFGUID)
{
    return E_NOTIMPL;
}

STDMETHODIMP Maro_CLive_Maro_Package::ResetDefaults(PKGRESETFLAGS)
{
    return S_OK;
}

STDMETHODIMP Maro_CLive_Maro_Package::GetPropertyPage(REFGUID, VSPROPSHEETPAGE* propertyPage)
{
    if (propertyPage == nullptr)
    {
        return E_POINTER;
    }
    return E_NOTIMPL;
}

STDMETHODIMP Maro_CLive_Maro_Package::QueryStatus(
    const GUID* commandGroup,
    ULONG commandCount,
    OLECMD commands[],
    OLECMDTEXT*)
{
    if (commandGroup == nullptr || commands == nullptr)
    {
        return E_POINTER;
    }
    if (*commandGroup != Maro_CLive_Maro_CommandSet)
    {
        return OLECMDERR_E_UNKNOWNGROUP;
    }

    bool handled = false;
    for (ULONG index = 0; index < commandCount; ++index)
    {
        if (commands[index].cmdID == Maro_CLive_Maro_CommandAnalyze ||
            commands[index].cmdID == Maro_CLive_Maro_CommandRun)
        {
            commands[index].cmdf = OLECMDF_SUPPORTED | OLECMDF_ENABLED;
            handled = true;
        }
    }
    return handled ? S_OK : OLECMDERR_E_NOTSUPPORTED;
}

STDMETHODIMP Maro_CLive_Maro_Package::Exec(
    const GUID* commandGroup,
    DWORD commandId,
    DWORD,
    VARIANT*,
    VARIANT*)
{
    if (commandGroup == nullptr)
    {
        return E_POINTER;
    }
    if (*commandGroup != Maro_CLive_Maro_CommandSet)
    {
        return OLECMDERR_E_UNKNOWNGROUP;
    }

    if (commandId == Maro_CLive_Maro_CommandAnalyze)
    {
        return StartAnalysis(false);
    }
    if (commandId == Maro_CLive_Maro_CommandRun)
    {
        return StartAnalysis(true);
    }
    return OLECMDERR_E_NOTSUPPORTED;
}

HRESULT Maro_CLive_Maro_Package::EnsureOutputPane()
{
    if (outputPane_ != nullptr)
    {
        return S_OK;
    }
    if (serviceProvider_ == nullptr)
    {
        return E_UNEXPECTED;
    }

    ATL::CComPtr<IVsOutputWindow> outputWindow;
    HRESULT result = serviceProvider_->QueryService(
        SID_SVsOutputWindow,
        IID_IVsOutputWindow,
        reinterpret_cast<void**>(&outputWindow));
    if (FAILED(result) || outputWindow == nullptr)
    {
        return FAILED(result) ? result : E_NOINTERFACE;
    }

    result = outputWindow->GetPane(Maro_CLive_Maro_OutputPane, &outputPane_);
    if (FAILED(result) || outputPane_ == nullptr)
    {
        result = outputWindow->CreatePane(Maro_CLive_Maro_OutputPane, L"CLive_Maro", TRUE, FALSE);
        if (FAILED(result))
        {
            return result;
        }
        result = outputWindow->GetPane(Maro_CLive_Maro_OutputPane, &outputPane_);
    }
    return outputPane_ != nullptr ? result : E_NOINTERFACE;
}

HRESULT Maro_CLive_Maro_Package::ReadActiveSource(
    Maro_SourceRequest& request,
    std::wstring& displayPath)
{
    if (serviceProvider_ == nullptr)
    {
        return E_UNEXPECTED;
    }

    ATL::CComPtr<IVsTextManager> textManager;
    HRESULT result = serviceProvider_->QueryService(
        SID_SVsTextManager,
        IID_IVsTextManager,
        reinterpret_cast<void**>(&textManager));
    if (FAILED(result) || textManager == nullptr)
    {
        return FAILED(result) ? result : E_NOINTERFACE;
    }

    ATL::CComPtr<IVsTextView> view;
    result = textManager->GetActiveView(FALSE, nullptr, &view);
    if (FAILED(result) || view == nullptr)
    {
        return FAILED(result) ? result : E_FAIL;
    }

    ATL::CComPtr<IVsTextLines> lines;
    result = view->GetBuffer(&lines);
    if (FAILED(result) || lines == nullptr)
    {
        return FAILED(result) ? result : E_FAIL;
    }

    ATL::CComQIPtr<IPersistFileFormat> file(lines);
    if (file != nullptr)
    {
        LPOLESTR rawPath = nullptr;
        DWORD formatIndex = 0;
        if (SUCCEEDED(file->GetCurFile(&rawPath, &formatIndex)) && rawPath != nullptr)
        {
            request.sourcePath = rawPath;
            CoTaskMemFree(rawPath);
        }
    }

    if (request.sourcePath.empty())
    {
        request.sourcePath = L"Maro_Active.cpp";
        displayPath = L"저장되지 않은 C++ 문서";
        request.language = Maro_Language::Cpp20;
    }
    else
    {
        displayPath = request.sourcePath;
        const std::wstring extension = Maro_LowerExtension(request.sourcePath);
        if (extension == L".c")
        {
            request.language = Maro_Language::C17;
        }
        else if (extension == L".cc" || extension == L".cpp" || extension == L".cxx" ||
                 extension == L".c++" || extension == L".h" || extension == L".hh" ||
                 extension == L".hpp" || extension == L".hxx")
        {
            request.language = Maro_Language::Cpp20;
        }
        else
        {
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
    }

    long lastLine = 0;
    long lastIndex = 0;
    result = lines->GetLastLineIndex(&lastLine, &lastIndex);
    if (FAILED(result))
    {
        return result;
    }

    ATL::CComBSTR source;
    result = lines->GetLineText(0, 0, lastLine, lastIndex, &source);
    if (FAILED(result))
    {
        return result;
    }

    request.sourceText.assign(source.m_str != nullptr ? source.m_str : L"", source.Length());
    request.sourceVersion = ++sourceVersion_;
    request.mode = Maro_SourceMode::Program;
    return S_OK;
}

HRESULT Maro_CLive_Maro_Package::StartAnalysis(bool execute)
{
    try
    {
        if (engine_ == nullptr)
        {
            return E_UNEXPECTED;
        }

        Maro_SourceRequest request;
        std::wstring displayPath;
        const HRESULT readResult = ReadActiveSource(request, displayPath);
        if (FAILED(readResult))
        {
            if (outputPane_ != nullptr)
            {
                outputPane_->Activate();
                outputPane_->Clear();
            }
            WriteOutput(readResult == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)
                ? L"C/C++ 문서를 열어 주세요.\r\n"
                : L"활성 편집기 내용을 읽지 못했습니다.\r\n");
            return S_OK;
        }

        request.execute = execute;
        if (outputPane_ != nullptr)
        {
            outputPane_->Activate();
            outputPane_->Clear();
        }

        std::wstring heading = L"CLive_Maro | ";
        heading += execute ? L"분석 및 실행" : L"분석";
        heading += L"\r\n";
        heading += displayPath;
        heading += L"\r\n\r\n";
        WriteOutput(heading);

        if (engine_->Submit(std::move(request)) == 0)
        {
            WriteOutput(L"작업을 시작하지 못했습니다.\r\n");
        }
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        WriteOutput(L"메모리가 부족합니다.\r\n");
        return E_OUTOFMEMORY;
    }
    catch (...)
    {
        WriteOutput(L"작업을 시작하지 못했습니다.\r\n");
        return E_FAIL;
    }
}

void Maro_CLive_Maro_Package::PublishResult(const Maro_ResultEnvelope& result) noexcept
{
    try
    {
        if (!Maro_IsCompleted(result))
        {
            WriteOutput(L"> " + result.statusText + L"\r\n");
            return;
        }

        std::wostringstream text;
        text << L"\r\n" << result.statusText << L"\r\n";
        for (const Maro_Diagnostic& diagnostic : result.diagnostics)
        {
            text << (diagnostic.range.start.line == 0 ? 1 : diagnostic.range.start.line);
            if (diagnostic.range.start.column != 0)
            {
                text << L":" << diagnostic.range.start.column;
            }
            text << L" " << Maro_SeverityText(diagnostic.severity);
            if (!diagnostic.code.empty())
            {
                text << L" " << diagnostic.code;
            }
            text << L" | " << diagnostic.friendlyMessage << L"\r\n";
        }

        if (!result.standardOutput.empty())
        {
            text << L"\r\nstdout\r\n" << result.standardOutput;
            if (result.standardOutput.back() != L'\n')
            {
                text << L"\r\n";
            }
        }
        if (!result.standardError.empty())
        {
            text << L"\r\nstderr\r\n" << result.standardError;
            if (result.standardError.back() != L'\n')
            {
                text << L"\r\n";
            }
        }
        if (result.diagnostics.empty() && result.standardError.empty() &&
            result.standardOutput.empty() && !result.compilerOutput.empty() &&
            result.status != Maro_Status::Success)
        {
            text << L"\r\n" << result.compilerOutput;
        }
        WriteOutput(text.str());
    }
    catch (...)
    {
        WriteOutput(L"결과 표시 중 오류가 발생했습니다.\r\n");
    }
}

void Maro_CLive_Maro_Package::WriteOutput(std::wstring_view text) noexcept
{
    ATL::CComPtr<IVsOutputWindowPane> pane = outputPane_;
    if (pane == nullptr || text.empty())
    {
        return;
    }
    std::wstring owned(text);
    pane->OutputStringThreadSafe(owned.c_str());
}

void Maro_CLive_Maro_Package::Shutdown() noexcept
{
    if (engine_ != nullptr)
    {
        engine_->Shutdown();
        engine_.reset();
    }
    outputPane_.Release();
    serviceProvider_.Release();
}
