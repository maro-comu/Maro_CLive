#include "Maro_App.hpp"

#include "Maro_Engine.hpp"
#include "Maro_Models.hpp"
#include "Maro_VisualStudio.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <richedit.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace
{
constexpr wchar_t Maro_MainWindowClass[] = L"Maro_CLive_Maro_MainWindow";
constexpr wchar_t Maro_TextDialogClass[] = L"Maro_CLive_Maro_TextDialog";
constexpr UINT Maro_ResultReadyMessage = WM_APP + 41;
constexpr UINT_PTR Maro_VisualStudioPollTimer = 0xC117;
constexpr UINT Maro_VisualStudioPollMilliseconds = 400;

constexpr COLORREF Maro_ColorWindow = RGB(13, 17, 23);
constexpr COLORREF Maro_ColorSurface = RGB(22, 27, 34);
constexpr COLORREF Maro_ColorPanel = RGB(15, 20, 26);
constexpr COLORREF Maro_ColorBorder = RGB(48, 54, 61);
constexpr COLORREF Maro_ColorText = RGB(230, 237, 243);
constexpr COLORREF Maro_ColorMuted = RGB(139, 148, 158);
constexpr COLORREF Maro_ColorAccent = RGB(47, 129, 247);
constexpr COLORREF Maro_ColorAccentPressed = RGB(31, 111, 235);
constexpr COLORREF Maro_ColorDisabled = RGB(68, 76, 86);

enum Maro_ControlId : int
{
    Maro_AnalyzeButtonId = 1001,
    Maro_CancelButtonId,
    Maro_InputButtonId,
    Maro_GeneratedButtonId,
    Maro_FixButtonId,
    Maro_ConnectionLabelId,
    Maro_ActiveFileLabelId,
    Maro_LanguageLabelId,
    Maro_StatusLabelId,
    Maro_OutputId,
    Maro_AnalysisId,
    Maro_OutputHeaderId,
    Maro_AnalysisHeaderId
};

struct Maro_LayoutRects
{
    RECT toolbar{};
    RECT outputHeader{};
    RECT outputBody{};
    RECT splitter{};
    RECT analysisHeader{};
    RECT analysisBody{};
};

int Maro_DipToPixel(int dip, UINT dpi)
{
    return MulDiv(dip, static_cast<int>(dpi), 96);
}

std::wstring Maro_GetWindowTextString(HWND window)
{
    const int length = GetWindowTextLengthW(window);
    if (length <= 0)
    {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, text.data(), length + 1);
    text.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0U);
    return text;
}

void Maro_SetWindowTextIfChanged(HWND window, const std::wstring& text)
{
    if (Maro_GetWindowTextString(window) != text)
    {
        SetWindowTextW(window, text.c_str());
    }
}

std::wstring Maro_SanitizeDisplayText(std::wstring_view text)
{
    constexpr std::size_t Maro_MaxVisibleCharacters = 1U << 20;
    const std::size_t count = (std::min)(text.size(), Maro_MaxVisibleCharacters);
    std::wstring result;
    result.reserve(count + 64);
    for (std::size_t index = 0; index < count; ++index)
    {
        const wchar_t character = text[index];
        if (character == L'\n' || character == L'\r' || character == L'\t')
        {
            result.push_back(character);
        }
        else if (character >= L' ' && character != 0x7f && character != 0x1b)
        {
            result.push_back(character);
        }
        else
        {
            result.push_back(L'?');
        }
    }
    if (text.size() > count)
    {
        result.append(L"\r\n[표시 한도를 넘어 나머지는 생략되었습니다.]\r\n");
    }
    return result;
}

const wchar_t* Maro_PhaseName(Maro_Phase phase)
{
    switch (phase)
    {
    case Maro_Phase::Idle:
        return L"대기";
    case Maro_Phase::Debouncing:
        return L"Visual Studio 확인 중";
    case Maro_Phase::Analyzing:
        return L"분석 중";
    case Maro_Phase::Generating:
        return L"코드 생성 중";
    case Maro_Phase::Compiling:
        return L"컴파일 중";
    case Maro_Phase::Running:
        return L"실행 중";
    case Maro_Phase::Completed:
        return L"완료";
    }
    return L"상태 불명";
}

const wchar_t* Maro_StatusName(Maro_Status status)
{
    switch (status)
    {
    case Maro_Status::Pending:
        return L"진행 중";
    case Maro_Status::Success:
        return L"성공";
    case Maro_Status::Cancelled:
        return L"취소됨";
    case Maro_Status::Stale:
        return L"오래된 결과";
    case Maro_Status::CompileFailed:
        return L"컴파일 실패";
    case Maro_Status::RuntimeFailed:
        return L"실행 실패";
    case Maro_Status::TimedOut:
        return L"시간 초과";
    case Maro_Status::LimitExceeded:
        return L"리소스 제한 초과";
    case Maro_Status::ToolchainMissing:
        return L"컴파일러 없음";
    case Maro_Status::SandboxUnavailable:
        return L"제한 실행 사용 불가";
    case Maro_Status::InternalError:
        return L"내부 오류";
    }
    return L"상태 불명";
}

const wchar_t* Maro_SeverityName(Maro_Severity severity)
{
    switch (severity)
    {
    case Maro_Severity::Info:
        return L"INFO";
    case Maro_Severity::Warning:
        return L"WARNING";
    case Maro_Severity::Error:
        return L"ERROR";
    case Maro_Severity::Fatal:
        return L"FATAL";
    }
    return L"INFO";
}

const wchar_t* Maro_EvidenceName(Maro_Evidence evidence)
{
    switch (evidence)
    {
    case Maro_Evidence::RuntimeObservation:
        return L"실행 확인";
    case Maro_Evidence::StaticAnalysis:
        return L"정적 분석";
    case Maro_Evidence::Conditional:
        return L"가능성";
    case Maro_Evidence::NotRun:
        return L"실행 안 됨";
    case Maro_Evidence::Unknown:
        return L"판단 불가";
    }
    return L"판단 불가";
}

const wchar_t* Maro_EvidenceSymbol(Maro_Evidence evidence)
{
    switch (evidence)
    {
    case Maro_Evidence::RuntimeObservation:
        return L"✓";
    case Maro_Evidence::StaticAnalysis:
        return L"◆";
    case Maro_Evidence::Conditional:
        return L"◇";
    case Maro_Evidence::NotRun:
        return L"○";
    case Maro_Evidence::Unknown:
        return L"?";
    }
    return L"?";
}

const wchar_t* Maro_LanguageName(Maro_Language language)
{
    return language == Maro_Language::C17 ? L"C17" : L"C++20";
}

bool Maro_IsBusy(Maro_Phase phase, Maro_Status status)
{
    return status == Maro_Status::Pending && phase != Maro_Phase::Idle &&
           phase != Maro_Phase::Debouncing && phase != Maro_Phase::Completed;
}

const wchar_t* Maro_VisualStudioStatusName(Maro_VisualStudioStatus status)
{
    switch (status)
    {
    case Maro_VisualStudioStatus::Success:
        return L"연결됨";
    case Maro_VisualStudioStatus::ComUnavailable:
        return L"COM 자동화 사용 불가";
    case Maro_VisualStudioStatus::NotRunning:
        return L"실행 중인 Visual Studio 없음";
    case Maro_VisualStudioStatus::NoActiveDocument:
        return L"활성 문서 없음";
    case Maro_VisualStudioStatus::UnsupportedDocument:
        return L"지원하지 않는 문서";
    case Maro_VisualStudioStatus::Busy:
        return L"Visual Studio 사용 중";
    case Maro_VisualStudioStatus::AccessDenied:
        return L"접근 거부";
    case Maro_VisualStudioStatus::Disconnected:
        return L"연결 끊김";
    case Maro_VisualStudioStatus::ReadOnly:
        return L"읽기 전용";
    case Maro_VisualStudioStatus::SourceVersionMismatch:
        return L"소스 버전 불일치";
    case Maro_VisualStudioStatus::PathMismatch:
        return L"활성 파일 불일치";
    case Maro_VisualStudioStatus::ContentMismatch:
        return L"편집 버퍼 변경됨";
    case Maro_VisualStudioStatus::ReplaceFailed:
        return L"편집 버퍼 수정 실패";
    case Maro_VisualStudioStatus::VerificationFailed:
        return L"수정 검증 실패";
    case Maro_VisualStudioStatus::AutomationError:
        return L"Visual Studio 자동화 오류";
    }
    return L"상태 불명";
}

std::wstring Maro_VisualStudioGuidance(const Maro_VisualStudioReadResult& read)
{
    if (!read.message.empty())
    {
        return read.message;
    }
    switch (read.status)
    {
    case Maro_VisualStudioStatus::NotRunning:
        return L"Visual Studio를 실행하고 C/C++ 문서를 활성화해 주세요.";
    case Maro_VisualStudioStatus::NoActiveDocument:
        return L"Visual Studio에서 분석할 C/C++ 문서를 열어 활성화해 주세요.";
    case Maro_VisualStudioStatus::UnsupportedDocument:
        return L"현재 활성 문서는 C/C++ 분석 대상으로 확인되지 않았습니다.";
    case Maro_VisualStudioStatus::Busy:
        return L"현재 편집 버퍼를 확인하지 못했습니다. 다음 연결 확인을 기다립니다.";
    case Maro_VisualStudioStatus::AccessDenied:
        return L"Visual Studio 자동화 개체에 접근하지 못했습니다. 권한 수준을 확인해 주세요.";
    case Maro_VisualStudioStatus::ComUnavailable:
        return L"현재 환경에서 Visual Studio COM 자동화를 사용할 수 없습니다.";
    default:
        return L"현재 Visual Studio 활성 편집 버퍼를 확인할 수 없습니다.";
    }
}

bool Maro_SameSnapshot(const Maro_VisualStudioSnapshot& left,
                       const Maro_VisualStudioSnapshot& right)
{
    return left.instanceMoniker == right.instanceMoniker &&
           left.processId == right.processId && left.path == right.path &&
           left.text == right.text && left.language == right.language;
}

bool Maro_SameDocument(const Maro_VisualStudioSnapshot& left,
                       const Maro_VisualStudioSnapshot& right)
{
    return left.instanceMoniker == right.instanceMoniker &&
           left.processId == right.processId && left.path == right.path;
}

bool Maro_IsHeaderDocument(std::wstring_view path)
{
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring_view::npos)
    {
        return false;
    }
    std::wstring extension(path.substr(dot));
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t character) {
                       return static_cast<wchar_t>(std::towlower(character));
                   });
    return extension == L".h" || extension == L".hh" || extension == L".hpp" ||
           extension == L".hxx" || extension == L".inl" || extension == L".ipp" ||
           extension == L".tpp";
}

bool Maro_IsHeaderDocument(const Maro_VisualStudioSnapshot& document)
{
    return Maro_IsHeaderDocument(document.path.empty() ? document.name
                                                        : document.path);
}

struct Maro_TextDialogState
{
    bool inputMode = false;
    bool accepted = false;
    bool finished = false;
    std::wstring initialText;
    std::wstring result;
    HWND edit = nullptr;
    HFONT font = nullptr;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH panelBrush = nullptr;
};

void Maro_LayoutTextDialog(HWND window, Maro_TextDialogState& state)
{
    RECT client{};
    GetClientRect(window, &client);
    const UINT dpi = GetDpiForWindow(window);
    const int padding = Maro_DipToPixel(12, dpi);
    const int buttonWidth = Maro_DipToPixel(84, dpi);
    const int buttonHeight = Maro_DipToPixel(30, dpi);
    const int gap = Maro_DipToPixel(8, dpi);
    const int buttonTop = static_cast<int>(client.bottom) - padding - buttonHeight;
    MoveWindow(state.edit, padding, padding,
               (std::max)(0, static_cast<int>(client.right) - padding * 2),
               (std::max)(0, buttonTop - gap - padding), TRUE);
    const HWND ok = GetDlgItem(window, IDOK);
    const HWND cancel = GetDlgItem(window, IDCANCEL);
    if (state.inputMode)
    {
        MoveWindow(ok,
                   static_cast<int>(client.right) - padding * 2 - buttonWidth * 2 - gap,
                   buttonTop, buttonWidth, buttonHeight, TRUE);
        MoveWindow(cancel, static_cast<int>(client.right) - padding - buttonWidth,
                   buttonTop, buttonWidth, buttonHeight, TRUE);
    }
    else
    {
        MoveWindow(ok, static_cast<int>(client.right) - padding - buttonWidth,
                   buttonTop, buttonWidth, buttonHeight, TRUE);
    }
}

LRESULT CALLBACK Maro_TextDialogProcedure(HWND window, UINT message,
                                          WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<Maro_TextDialogState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<Maro_TextDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr)
    {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message)
    {
    case WM_CREATE:
    {
        const DWORD editStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
                                (state->inputMode ? 0U : ES_READONLY);
        state->edit = CreateWindowExW(0, MSFTEDIT_CLASS, state->initialText.c_str(),
                                      editStyle, 0, 0, 0, 0, window, nullptr,
                                      GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", state->inputMode ? L"확인" : L"닫기",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        0, 0, 0, 0, window,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
                        GetModuleHandleW(nullptr), nullptr);
        if (state->inputMode)
        {
            CreateWindowExW(0, L"BUTTON", L"취소",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                            0, 0, 0, 0, window,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)),
                            GetModuleHandleW(nullptr), nullptr);
        }
        const UINT dpi = GetDpiForWindow(window);
        state->font = CreateFontW(
            -Maro_DipToPixel(14, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
        state->backgroundBrush = CreateSolidBrush(Maro_ColorWindow);
        state->panelBrush = CreateSolidBrush(Maro_ColorPanel);
        SendMessageW(state->edit, WM_SETFONT,
                     reinterpret_cast<WPARAM>(state->font), TRUE);
        SendMessageW(state->edit, EM_SETBKGNDCOLOR, 0, Maro_ColorPanel);
        CHARFORMAT2W format{};
        format.cbSize = sizeof(format);
        format.dwMask = CFM_COLOR;
        format.crTextColor = Maro_ColorText;
        SendMessageW(state->edit, EM_SETCHARFORMAT, SCF_ALL,
                     reinterpret_cast<LPARAM>(&format));
        SendMessageW(state->edit, EM_SETCHARFORMAT, SCF_DEFAULT,
                     reinterpret_cast<LPARAM>(&format));
        Maro_LayoutTextDialog(window, *state);
        SetFocus(state->edit);
        return 0;
    }
    case WM_SIZE:
        Maro_LayoutTextDialog(window, *state);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK)
        {
            if (state->inputMode)
            {
                state->result = Maro_GetWindowTextString(state->edit);
                state->accepted = true;
            }
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL)
        {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    {
        const HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, Maro_ColorText);
        SetBkColor(dc, Maro_ColorPanel);
        return reinterpret_cast<LRESULT>(state->panelBrush);
    }
    case WM_ERASEBKGND:
    {
        RECT client{};
        GetClientRect(window, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client, state->backgroundBrush);
        return 1;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        state->finished = true;
        if (state->font != nullptr)
        {
            DeleteObject(state->font);
            state->font = nullptr;
        }
        if (state->backgroundBrush != nullptr)
        {
            DeleteObject(state->backgroundBrush);
            state->backgroundBrush = nullptr;
        }
        if (state->panelBrush != nullptr)
        {
            DeleteObject(state->panelBrush);
            state->panelBrush = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool Maro_RegisterTextDialogClass(HINSTANCE instance)
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = Maro_TextDialogProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = Maro_TextDialogClass;
    if (RegisterClassExW(&windowClass) != 0)
    {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool Maro_ShowTextDialog(HWND owner, const wchar_t* title, bool inputMode,
                         std::wstring& text)
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!Maro_RegisterTextDialogClass(instance))
    {
        return false;
    }
    Maro_TextDialogState state{};
    state.inputMode = inputMode;
    state.initialText = text;
    const UINT dpi = GetDpiForWindow(owner);
    const int width = Maro_DipToPixel(inputMode ? 560 : 760, dpi);
    const int height = Maro_DipToPixel(inputMode ? 280 : 560, dpi);
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    const int x = static_cast<int>(ownerRect.left) +
                  (static_cast<int>(ownerRect.right - ownerRect.left) - width) / 2;
    const int y = static_cast<int>(ownerRect.top) +
                  (static_cast<int>(ownerRect.bottom - ownerRect.top) - height) / 2;
    EnableWindow(owner, FALSE);
    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME, Maro_TextDialogClass, title,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        x, y, width, height, owner, nullptr, instance, &state);
    if (dialog == nullptr)
    {
        EnableWindow(owner, TRUE);
        return false;
    }
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    MSG message{};
    bool sawQuit = false;
    int quitCode = 0;
    while (!state.finished)
    {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0)
        {
            sawQuit = result == 0;
            quitCode = static_cast<int>(message.wParam);
            break;
        }
        if (!IsDialogMessageW(dialog, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (IsWindow(dialog))
    {
        DestroyWindow(dialog);
    }
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    if (sawQuit)
    {
        PostQuitMessage(quitCode);
    }
    if (state.accepted)
    {
        text = std::move(state.result);
    }
    return state.accepted;
}
} // namespace

struct Maro_App::Maro_Impl
{
    explicit Maro_Impl(Maro_App& application, HINSTANCE applicationInstance)
        : owner(application), instance(applicationInstance)
    {
    }

    ~Maro_Impl()
    {
        closing.store(true);
        if (engine != nullptr)
        {
            engine->Shutdown();
            engine.reset();
        }
        DestroyThemeResources();
        if (richEditModule != nullptr)
        {
            FreeLibrary(richEditModule);
            richEditModule = nullptr;
        }
    }

    bool RegisterMainClass();
    bool CreateMainWindow(int showCommand);
    bool CreateControls();
    void CreateEngine();
    void DestroyThemeResources();
    void RecreateThemeResources();
    void ApplyRichEditTheme(HWND control) const;
    Maro_LayoutRects CalculateLayout() const;
    void Layout();
    void Paint();
    void DrawOwnerControl(const DRAWITEMSTRUCT& item) const;
    void PollVisualStudio(bool forceRun);
    void InvalidateSnapshot(const Maro_VisualStudioReadResult& read);
    void PresentBridgeState(const Maro_VisualStudioReadResult& read);
    void UpdateVisualStudioLabels();
    void SubmitCurrentSnapshot();
    void CancelRequest();
    void EnqueueResult(Maro_ResultEnvelope result);
    void DrainResults();
    void ApplyResult(const Maro_ResultEnvelope& result);
    void UpdateOutput(const Maro_ResultEnvelope& result);
    void UpdateAnalysis(const Maro_ResultEnvelope& result);
    void UpdateButtonStates();
    void SetStatus(const std::wstring& text);
    void ShowInputDialog();
    void ShowGeneratedSource();
    void ApplyAvailableFix();
    void BeginDrag(POINT point);
    void ContinueDrag(POINT point);
    void EndDrag();
    void UpdateHover(POINT point);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    Maro_App& owner;
    HINSTANCE instance = nullptr;
    HMODULE richEditModule = nullptr;
    HWND window = nullptr;
    HWND analyzeButton = nullptr;
    HWND cancelButton = nullptr;
    HWND inputButton = nullptr;
    HWND generatedButton = nullptr;
    HWND fixButton = nullptr;
    HWND connectionLabel = nullptr;
    HWND activeFileLabel = nullptr;
    HWND languageLabel = nullptr;
    HWND statusLabel = nullptr;
    HWND outputHeader = nullptr;
    HWND analysisHeader = nullptr;
    HWND output = nullptr;
    HWND analysis = nullptr;
    HFONT uiFont = nullptr;
    HFONT codeFont = nullptr;
    HFONT headerFont = nullptr;
    HBRUSH windowBrush = nullptr;
    HBRUSH surfaceBrush = nullptr;
    HBRUSH panelBrush = nullptr;
    HACCEL accelerators = nullptr;
    UINT dpi = 96;
    double outputRatio = 0.45;
    Maro_LayoutRects layout{};
    bool dragging = false;
    bool splitterHovered = false;
    bool trackingMouse = false;
    bool busy = false;
    std::atomic_bool closing{false};
    std::uint64_t sourceVersion = 0;
    std::uint64_t activeRequestId = 0;
    Maro_Phase currentPhase = Maro_Phase::Idle;
    Maro_Status currentStatus = Maro_Status::Pending;
    Maro_VisualStudioStatus visualStudioStatus =
        Maro_VisualStudioStatus::NotRunning;
    std::wstring visualStudioMessage;
    std::wstring standardInput;
    std::optional<Maro_VisualStudioSnapshot> snapshot;
    std::optional<Maro_ResultEnvelope> latestResult;
    std::optional<Maro_FixSuggestion> latestFix;
    Maro_VisualStudio visualStudio;
    std::unique_ptr<Maro_Engine> engine;
    std::mutex resultMutex;
    std::deque<Maro_ResultEnvelope> pendingResults;
};

bool Maro_App::Maro_Impl::RegisterMainClass()
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = Maro_App::WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = Maro_MainWindowClass;
    windowClass.hIconSm = windowClass.hIcon;
    if (RegisterClassExW(&windowClass) != 0)
    {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool Maro_App::Maro_Impl::CreateMainWindow(int showCommand)
{
    richEditModule = LoadLibraryW(L"Msftedit.dll");
    if (richEditModule == nullptr || !RegisterMainClass())
    {
        MessageBoxW(nullptr, L"Windows RichEdit 구성 요소를 시작하지 못했습니다.",
                    L"CLive_Maro", MB_OK | MB_ICONERROR);
        return false;
    }
    window = CreateWindowExW(
        0, Maro_MainWindowClass, L"CLive_Maro",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1080, 760,
        nullptr, nullptr, instance, &owner);
    if (window == nullptr)
    {
        return false;
    }
    ACCEL entries[] = {
        {FVIRTKEY, VK_F5, static_cast<WORD>(Maro_AnalyzeButtonId)},
        {FVIRTKEY, VK_ESCAPE, static_cast<WORD>(Maro_CancelButtonId)}};
    accelerators = CreateAcceleratorTableW(entries, static_cast<int>(std::size(entries)));
    ShowWindow(window, showCommand);
    UpdateWindow(window);
    return true;
}

bool Maro_App::Maro_Impl::CreateControls()
{
    const auto createButton = [this](const wchar_t* text, int id) {
        return CreateWindowExW(
            0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    };
    const auto createLabel = [this](const wchar_t* text, int id, DWORD style) {
        return CreateWindowExW(
            0, L"STATIC", text, WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    };

    analyzeButton = createButton(L"Analyze / Run", Maro_AnalyzeButtonId);
    cancelButton = createButton(L"Cancel", Maro_CancelButtonId);
    inputButton = createButton(L"Input", Maro_InputButtonId);
    generatedButton = createButton(L"Generated", Maro_GeneratedButtonId);
    fixButton = createButton(L"Fix", Maro_FixButtonId);
    connectionLabel = createLabel(L"Visual Studio: 확인 중…",
                                  Maro_ConnectionLabelId, SS_LEFT | SS_CENTERIMAGE);
    activeFileLabel = createLabel(L"Active file: 확인 중…",
                                  Maro_ActiveFileLabelId, SS_LEFT | SS_CENTERIMAGE);
    languageLabel = createLabel(L"Language: —", Maro_LanguageLabelId,
                                SS_RIGHT | SS_CENTERIMAGE);
    statusLabel = createLabel(L"연결 확인 중…", Maro_StatusLabelId,
                              SS_RIGHT | SS_CENTERIMAGE);
    outputHeader = createLabel(L"OUTPUT", Maro_OutputHeaderId,
                               SS_LEFT | SS_CENTERIMAGE);
    analysisHeader = createLabel(L"CODE ANALYSIS", Maro_AnalysisHeaderId,
                                 SS_LEFT | SS_CENTERIMAGE);

    const DWORD readOnlyStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY;
    output = CreateWindowExW(
        0, MSFTEDIT_CLASS, nullptr, readOnlyStyle,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(Maro_OutputId)), instance, nullptr);
    analysis = CreateWindowExW(
        0, MSFTEDIT_CLASS, nullptr, readOnlyStyle,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(Maro_AnalysisId)), instance, nullptr);

    const HWND controls[] = {
        analyzeButton, cancelButton, inputButton, generatedButton, fixButton,
        connectionLabel, activeFileLabel, languageLabel, statusLabel,
        outputHeader, analysisHeader, output, analysis};
    if (std::any_of(std::begin(controls), std::end(controls),
                    [](HWND control) { return control == nullptr; }))
    {
        return false;
    }
    SendMessageW(output, EM_EXLIMITTEXT, 0, 4U << 20);
    SendMessageW(analysis, EM_EXLIMITTEXT, 0, 4U << 20);
    RecreateThemeResources();
    SetWindowTextW(output,
                   L"IDE STATUS\r\n----------------\r\n"
                   L"Visual Studio의 활성 C/C++ 편집 버퍼를 확인하고 있습니다.");
    SetWindowTextW(analysis,
                   L"? 판단 불가\r\nVisual Studio 연결 결과를 기다리고 있습니다.");
    UpdateButtonStates();
    return true;
}

void Maro_App::Maro_Impl::CreateEngine()
{
    engine = std::make_unique<Maro_Engine>(
        [this](Maro_ResultEnvelope result) { EnqueueResult(std::move(result)); });
}

void Maro_App::Maro_Impl::DestroyThemeResources()
{
    if (uiFont != nullptr)
    {
        DeleteObject(uiFont);
        uiFont = nullptr;
    }
    if (codeFont != nullptr)
    {
        DeleteObject(codeFont);
        codeFont = nullptr;
    }
    if (headerFont != nullptr)
    {
        DeleteObject(headerFont);
        headerFont = nullptr;
    }
    if (windowBrush != nullptr)
    {
        DeleteObject(windowBrush);
        windowBrush = nullptr;
    }
    if (surfaceBrush != nullptr)
    {
        DeleteObject(surfaceBrush);
        surfaceBrush = nullptr;
    }
    if (panelBrush != nullptr)
    {
        DeleteObject(panelBrush);
        panelBrush = nullptr;
    }
}

void Maro_App::Maro_Impl::RecreateThemeResources()
{
    HFONT newUiFont = CreateFontW(
        -Maro_DipToPixel(14, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT newCodeFont = CreateFontW(
        -Maro_DipToPixel(14, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
    HFONT newHeaderFont = CreateFontW(
        -Maro_DipToPixel(13, dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HBRUSH newWindowBrush = CreateSolidBrush(Maro_ColorWindow);
    HBRUSH newSurfaceBrush = CreateSolidBrush(Maro_ColorSurface);
    HBRUSH newPanelBrush = CreateSolidBrush(Maro_ColorPanel);

    const HWND uiControls[] = {
        analyzeButton, cancelButton, inputButton, generatedButton, fixButton,
        connectionLabel, activeFileLabel, languageLabel, statusLabel};
    for (HWND control : uiControls)
    {
        if (control != nullptr)
        {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(newUiFont), TRUE);
        }
    }
    for (HWND control : {outputHeader, analysisHeader})
    {
        if (control != nullptr)
        {
            SendMessageW(control, WM_SETFONT,
                         reinterpret_cast<WPARAM>(newHeaderFont), TRUE);
        }
    }
    for (HWND control : {output, analysis})
    {
        if (control != nullptr)
        {
            SendMessageW(control, WM_SETFONT,
                         reinterpret_cast<WPARAM>(newCodeFont), TRUE);
        }
    }
    DestroyThemeResources();
    uiFont = newUiFont;
    codeFont = newCodeFont;
    headerFont = newHeaderFont;
    windowBrush = newWindowBrush;
    surfaceBrush = newSurfaceBrush;
    panelBrush = newPanelBrush;
    ApplyRichEditTheme(output);
    ApplyRichEditTheme(analysis);
    BOOL dark = TRUE;
    constexpr DWORD Maro_ImmersiveDarkModeAttribute = 20;
    DwmSetWindowAttribute(window,
                          static_cast<DWMWINDOWATTRIBUTE>(Maro_ImmersiveDarkModeAttribute),
                          &dark, sizeof(dark));
}

void Maro_App::Maro_Impl::ApplyRichEditTheme(HWND control) const
{
    if (control == nullptr)
    {
        return;
    }
    SendMessageW(control, EM_SETBKGNDCOLOR, 0, Maro_ColorPanel);
    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR;
    format.crTextColor = Maro_ColorText;
    SendMessageW(control, EM_SETCHARFORMAT, SCF_ALL,
                 reinterpret_cast<LPARAM>(&format));
    SendMessageW(control, EM_SETCHARFORMAT, SCF_DEFAULT,
                 reinterpret_cast<LPARAM>(&format));
}

Maro_LayoutRects Maro_App::Maro_Impl::CalculateLayout() const
{
    RECT client{};
    GetClientRect(window, &client);
    const int width = (std::max)(0, static_cast<int>(client.right - client.left));
    const int height = (std::max)(0, static_cast<int>(client.bottom - client.top));
    const int toolbarHeight = Maro_DipToPixel(82, dpi);
    const int divider = Maro_DipToPixel(6, dpi);
    const int headerHeight = Maro_DipToPixel(30, dpi);
    const int contentTop = (std::min)(height, toolbarHeight);
    const int usableHeight = (std::max)(0, height - contentTop - divider);
    const int minOutput = Maro_DipToPixel(120, dpi);
    const int minAnalysis = Maro_DipToPixel(150, dpi);
    int outputHeight = static_cast<int>(static_cast<double>(usableHeight) * outputRatio);
    if (usableHeight >= minOutput + minAnalysis)
    {
        outputHeight = std::clamp(outputHeight, minOutput,
                                  usableHeight - minAnalysis);
    }
    else
    {
        outputHeight = std::clamp(outputHeight, 0, usableHeight);
    }

    Maro_LayoutRects result{};
    result.toolbar = {0, 0, width, contentTop};
    result.outputHeader = {0, contentTop, width,
                           (std::min)(height, contentTop + headerHeight)};
    result.outputBody = {0, result.outputHeader.bottom, width,
                         contentTop + outputHeight};
    result.splitter = {0, result.outputBody.bottom, width,
                       (std::min)(height,
                                  static_cast<int>(result.outputBody.bottom) + divider)};
    result.analysisHeader = {0, result.splitter.bottom, width,
                             (std::min)(height,
                                        static_cast<int>(result.splitter.bottom) +
                                            headerHeight)};
    result.analysisBody = {0, result.analysisHeader.bottom, width, height};
    return result;
}

void Maro_App::Maro_Impl::Layout()
{
    if (window == nullptr)
    {
        return;
    }
    layout = CalculateLayout();
    const auto place = [](HDWP defer, HWND control, const RECT& rect) {
        return DeferWindowPos(
            defer, control, nullptr, rect.left, rect.top,
            (std::max)(0, static_cast<int>(rect.right - rect.left)),
            (std::max)(0, static_cast<int>(rect.bottom - rect.top)),
            SWP_NOZORDER | SWP_NOACTIVATE);
    };
    HDWP defer = BeginDeferWindowPos(13);
    if (defer == nullptr)
    {
        return;
    }
    defer = place(defer, outputHeader, layout.outputHeader);
    defer = place(defer, output, layout.outputBody);
    defer = place(defer, analysisHeader, layout.analysisHeader);
    defer = place(defer, analysis, layout.analysisBody);

    const int pad = Maro_DipToPixel(10, dpi);
    const int gap = Maro_DipToPixel(6, dpi);
    const int buttonHeight = Maro_DipToPixel(28, dpi);
    const int firstRowY = Maro_DipToPixel(7, dpi);
    int x = pad;
    const struct
    {
        HWND control;
        int widthDip;
    } buttons[] = {
        {analyzeButton, 100}, {cancelButton, 64}, {inputButton, 56},
        {generatedButton, 86}, {fixButton, 50}};
    for (const auto& button : buttons)
    {
        const int controlWidth = Maro_DipToPixel(button.widthDip, dpi);
        RECT rect{x, firstRowY, x + controlWidth, firstRowY + buttonHeight};
        defer = place(defer, button.control, rect);
        x += controlWidth + gap;
    }

    RECT client{};
    GetClientRect(window, &client);
    const int right = static_cast<int>(client.right) - pad;
    const int languageWidth = Maro_DipToPixel(142, dpi);
    RECT languageRect{(std::max)(x, right - languageWidth), firstRowY,
                      right, firstRowY + buttonHeight};
    defer = place(defer, languageLabel, languageRect);
    RECT connectionRect{x + Maro_DipToPixel(8, dpi), firstRowY,
                        (std::max)(x + Maro_DipToPixel(8, dpi),
                                   static_cast<int>(languageRect.left) - gap),
                        firstRowY + buttonHeight};
    defer = place(defer, connectionLabel, connectionRect);

    const int secondRowY = Maro_DipToPixel(44, dpi);
    const int secondRowHeight = Maro_DipToPixel(26, dpi);
    const int statusWidth = Maro_DipToPixel(300, dpi);
    const int statusLeft = (std::max)(pad, right - statusWidth);
    RECT activeRect{pad, secondRowY,
                    (std::max)(pad, statusLeft - gap),
                    secondRowY + secondRowHeight};
    RECT statusRect{statusLeft, secondRowY, right,
                    secondRowY + secondRowHeight};
    defer = place(defer, activeFileLabel, activeRect);
    defer = place(defer, statusLabel, statusRect);
    EndDeferWindowPos(defer);
    InvalidateRect(window, nullptr, FALSE);
}

void Maro_App::Maro_Impl::Paint()
{
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, windowBrush);
    FillRect(dc, &layout.toolbar, surfaceBrush);
    HBRUSH splitterBrush = CreateSolidBrush(splitterHovered ? Maro_ColorAccent
                                                            : Maro_ColorBorder);
    FillRect(dc, &layout.splitter, splitterBrush);
    DeleteObject(splitterBrush);
    EndPaint(window, &paint);
}

void Maro_App::Maro_Impl::DrawOwnerControl(const DRAWITEMSTRUCT& item) const
{
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    COLORREF background = Maro_ColorAccent;
    if (disabled)
    {
        background = Maro_ColorWindow;
    }
    else if (selected)
    {
        background = Maro_ColorAccentPressed;
    }
    HBRUSH brush = CreateSolidBrush(background);
    FillRect(item.hDC, &item.rcItem, brush);
    DeleteObject(brush);
    SetDCBrushColor(item.hDC, Maro_ColorBorder);
    FrameRect(item.hDC, &item.rcItem,
              static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? Maro_ColorDisabled : Maro_ColorText);
    const HFONT font = reinterpret_cast<HFONT>(
        SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
    const HGDIOBJ oldFont = SelectObject(item.hDC, font);
    RECT textRect = item.rcItem;
    InflateRect(&textRect, -Maro_DipToPixel(6, dpi), 0);
    const std::wstring text = Maro_GetWindowTextString(item.hwndItem);
    DrawTextW(item.hDC, text.c_str(), static_cast<int>(text.size()), &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(item.hDC, oldFont);
    if (focused)
    {
        RECT focusRect = item.rcItem;
        InflateRect(&focusRect, -2, -2);
        DrawFocusRect(item.hDC, &focusRect);
    }
}

void Maro_App::Maro_Impl::SetStatus(const std::wstring& text)
{
    Maro_SetWindowTextIfChanged(statusLabel, text);
}

void Maro_App::Maro_Impl::UpdateVisualStudioLabels()
{
    std::wstring connection = L"Visual Studio: ";
    connection.append(Maro_VisualStudioStatusName(visualStudioStatus));
    if (snapshot.has_value() && snapshot->processId != 0)
    {
        connection.append(L" · PID ");
        connection.append(std::to_wstring(snapshot->processId));
    }
    Maro_SetWindowTextIfChanged(connectionLabel, connection);

    if (snapshot.has_value())
    {
        std::wstring active = L"Active file: ";
        active.append(snapshot->path.empty() ? snapshot->name : snapshot->path);
        if (!snapshot->saved)
        {
            active.append(L" · 저장 전 버퍼");
        }
        if (snapshot->readOnly)
        {
            active.append(L" · 읽기 전용");
        }
        Maro_SetWindowTextIfChanged(activeFileLabel, active);
        std::wstring language = L"Language: ";
        language.append(Maro_LanguageName(snapshot->language));
        language.append(L" (자동 감지)");
        Maro_SetWindowTextIfChanged(languageLabel, language);
        Maro_SetWindowTextIfChanged(
            analyzeButton,
            Maro_IsHeaderDocument(*snapshot) ? L"Analyze" : L"Run");
    }
    else
    {
        Maro_SetWindowTextIfChanged(activeFileLabel, L"Active file: 없음");
        Maro_SetWindowTextIfChanged(languageLabel, L"Language: —");
        Maro_SetWindowTextIfChanged(analyzeButton, L"Analyze / Run");
    }
}

void Maro_App::Maro_Impl::PresentBridgeState(
    const Maro_VisualStudioReadResult& read)
{
    const std::wstring guidance = Maro_VisualStudioGuidance(read);
    std::wostringstream outputText;
    outputText << L"IDE STATUS\r\n----------------\r\n? 판단 불가\r\n"
               << guidance << L"\r\n";
    if (read.instancesInspected != 0)
    {
        outputText << L"확인한 Visual Studio 인스턴스: "
                   << read.instancesInspected << L"\r\n";
    }
    SetWindowTextW(output, outputText.str().c_str());

    std::wostringstream analysisText;
    analysisText << L"? " << Maro_VisualStudioStatusName(read.status) << L"\r\n"
                 << guidance << L"\r\n\r\n"
                 << L"CLive_Maro는 Visual Studio의 활성 C/C++ 편집 버퍼를 "
                    L"확인한 경우에만 분석을 시작합니다.";
    SetWindowTextW(analysis, analysisText.str().c_str());
    SetStatus(Maro_VisualStudioStatusName(read.status));
}

void Maro_App::Maro_Impl::InvalidateSnapshot(
    const Maro_VisualStudioReadResult& read)
{
    visualStudioStatus = read.status;
    visualStudioMessage = read.message;
    if (snapshot.has_value())
    {
        if (sourceVersion != (std::numeric_limits<std::uint64_t>::max)())
        {
            ++sourceVersion;
        }
        if (engine != nullptr)
        {
            engine->Cancel();
        }
        snapshot.reset();
        activeRequestId = 0;
        latestResult.reset();
        latestFix.reset();
        busy = false;
        currentPhase = Maro_Phase::Idle;
        currentStatus = Maro_Status::Pending;
    }
    UpdateVisualStudioLabels();
    PresentBridgeState(read);
    UpdateButtonStates();
}

void Maro_App::Maro_Impl::PollVisualStudio(bool forceRun)
{
    if (closing.load())
    {
        return;
    }
    const std::uint64_t candidateVersion =
        sourceVersion == (std::numeric_limits<std::uint64_t>::max)()
            ? sourceVersion
            : sourceVersion + 1;
    const std::wstring preferredInstance =
        snapshot.has_value() ? snapshot->instanceMoniker : std::wstring{};
    Maro_VisualStudioReadResult read = visualStudio.ReadActiveDocument(
        candidateVersion, preferredInstance);
    if (!read || !read.snapshot.has_value())
    {
        if (read.status == Maro_VisualStudioStatus::Busy && snapshot.has_value())
        {
            visualStudioStatus = read.status;
            visualStudioMessage = read.message;
            UpdateVisualStudioLabels();
            SetStatus(L"? Visual Studio 사용 중 · 표시 결과는 마지막 확인 버전입니다.");
            UpdateButtonStates();
            return;
        }
        InvalidateSnapshot(read);
        return;
    }

    Maro_VisualStudioSnapshot next = std::move(*read.snapshot);
    const bool changed = !snapshot.has_value() || !Maro_SameSnapshot(*snapshot, next);
    const bool changedDocument = !snapshot.has_value() ||
                                 !Maro_SameDocument(*snapshot, next);
    const Maro_VisualStudioStatus previousVisualStudioStatus = visualStudioStatus;
    visualStudioStatus = Maro_VisualStudioStatus::Success;
    visualStudioMessage = read.message;
    if (changed)
    {
        sourceVersion = candidateVersion;
        next.sourceVersion = sourceVersion;
        if (engine != nullptr)
        {
            engine->Cancel();
        }
        snapshot = std::move(next);
        activeRequestId = 0;
        latestResult.reset();
        latestFix.reset();
        busy = false;
        if (changedDocument)
        {
            standardInput.clear();
        }
        UpdateVisualStudioLabels();
        SetWindowTextW(
            output,
            L"IDE STATUS\r\n----------------\r\n"
            L"Visual Studio의 최신 저장 전 편집 버퍼를 읽었습니다.\r\n"
            L"이전 결과를 지우고 분석을 시작합니다.");
        SetWindowTextW(analysis, L"◆ 정적 분석\r\n최신 편집 버퍼를 분석 중입니다.");
        SubmitCurrentSnapshot();
        return;
    }

    next.sourceVersion = sourceVersion;
    snapshot = std::move(next);
    UpdateVisualStudioLabels();
    if (previousVisualStudioStatus == Maro_VisualStudioStatus::Busy)
    {
        SetStatus(L"Visual Studio 최신 편집 버퍼를 다시 확인했습니다.");
    }
    UpdateButtonStates();
    if (forceRun)
    {
        SubmitCurrentSnapshot();
    }
}

void Maro_App::Maro_Impl::SubmitCurrentSnapshot()
{
    if (!snapshot.has_value() || engine == nullptr || closing.load())
    {
        return;
    }
    Maro_SourceRequest request{};
    request.sourceVersion = sourceVersion;
    request.sourcePath = snapshot->path;
    request.sourceText = snapshot->text;
    request.standardInput = standardInput;
    request.language = snapshot->language;
    request.mode = Maro_SourceMode::Program;
    request.execute = !Maro_IsHeaderDocument(*snapshot);
    const bool execute = request.execute;
    activeRequestId = engine->Submit(std::move(request));
    currentPhase = Maro_Phase::Analyzing;
    currentStatus = Maro_Status::Pending;
    busy = true;
    latestResult.reset();
    latestFix.reset();
    SetStatus(execute ? L"최신 Visual Studio 버퍼 실행 요청 중…"
                      : L"헤더 문서 정적 분석 요청 중…");
    UpdateButtonStates();
}

void Maro_App::Maro_Impl::CancelRequest()
{
    if (engine != nullptr)
    {
        engine->Cancel();
    }
    busy = false;
    currentStatus = Maro_Status::Cancelled;
    SetStatus(L"취소됨 · Visual Studio 연결 확인은 계속됩니다.");
    UpdateButtonStates();
}

void Maro_App::Maro_Impl::EnqueueResult(Maro_ResultEnvelope result)
{
    if (closing.load())
    {
        return;
    }
    {
        std::lock_guard lock(resultMutex);
        pendingResults.push_back(std::move(result));
    }
    PostMessageW(window, Maro_ResultReadyMessage, 0, 0);
}

void Maro_App::Maro_Impl::DrainResults()
{
    std::deque<Maro_ResultEnvelope> results;
    {
        std::lock_guard lock(resultMutex);
        results.swap(pendingResults);
    }
    for (const auto& result : results)
    {
        if (!snapshot.has_value() || result.sourceVersion != sourceVersion ||
            result.requestId != activeRequestId)
        {
            continue;
        }
        ApplyResult(result);
    }
}

void Maro_App::Maro_Impl::ApplyResult(const Maro_ResultEnvelope& result)
{
    currentPhase = result.phase;
    currentStatus = result.status;
    busy = Maro_IsBusy(result.phase, result.status);
    latestResult = result;
    latestFix.reset();
    for (const auto& diagnostic : result.diagnostics)
    {
        if (diagnostic.sourceVersion == sourceVersion && diagnostic.fix.has_value() &&
            !diagnostic.fix->edits.empty())
        {
            latestFix = diagnostic.fix;
            break;
        }
    }
    std::wstring status = Maro_PhaseName(result.phase);
    status.append(L" · ");
    status.append(Maro_StatusName(result.status));
    if (!result.statusText.empty())
    {
        status.append(L" · ");
        status.append(result.statusText);
    }
    SetStatus(status);
    UpdateOutput(result);
    UpdateAnalysis(result);
    UpdateButtonStates();
}

void Maro_App::Maro_Impl::UpdateOutput(const Maro_ResultEnvelope& result)
{
    std::wostringstream stream;
    stream << L"PROGRAM OUTPUT\r\n----------------\r\n";
    if (!result.standardOutput.empty())
    {
        stream << Maro_SanitizeDisplayText(result.standardOutput);
        if (result.standardOutput.back() != L'\n')
        {
            stream << L"\r\n";
        }
    }
    else if (result.phase == Maro_Phase::Completed && result.hasExitCode)
    {
        stream << L"없음\r\n";
    }
    else if (result.phase == Maro_Phase::Completed)
    {
        stream << L"(프로그램이 실행되지 않음)\r\n";
    }
    else
    {
        stream << L"(아직 출력 없음)\r\n";
    }
    if (!result.standardError.empty())
    {
        stream << L"\r\nSTDERR\r\n----------------\r\n"
               << Maro_SanitizeDisplayText(result.standardError) << L"\r\n";
    }
    stream << L"\r\nIDE STATUS\r\n----------------\r\n"
           << Maro_PhaseName(result.phase) << L" · "
           << Maro_StatusName(result.status) << L"\r\n";
    if (result.phase == Maro_Phase::Completed && result.hasExitCode)
    {
        stream << L"✓ 실행 확인\r\n";
    }
    if (result.executionId != 0)
    {
        stream << L"Execution ID: " << result.executionId << L"\r\n";
        if (result.resourceLimitsApplied)
        {
            stream << L"ⓘ 제한 실행: 시간·메모리·출력량·프로세스 트리 제한이 "
                      L"적용되었습니다.\r\n";
        }
    }
    if (!result.statusText.empty())
    {
        stream << result.statusText << L"\r\n";
    }
    if (!result.compilerName.empty())
    {
        stream << L"Compiler: " << result.compilerName;
        if (!result.compilerVersion.empty())
        {
            stream << L" " << result.compilerVersion;
        }
        if (result.usedFallbackCompiler)
        {
            stream << L" (fallback)";
        }
        stream << L"\r\n";
    }
    if (result.hasExitCode)
    {
        stream << L"Exit code: " << result.exitCode << L"\r\n";
    }
    SetWindowTextW(output, stream.str().c_str());
}

void Maro_App::Maro_Impl::UpdateAnalysis(const Maro_ResultEnvelope& result)
{
    std::wostringstream stream;
    if (result.diagnostics.empty())
    {
        if (result.phase == Maro_Phase::Completed && result.status == Maro_Status::Success)
        {
            stream << L"✓ 문제를 찾지 못했습니다.\r\n";
        }
        else
        {
            stream << (result.phase == Maro_Phase::Completed
                           ? L"구조화된 진단이 없습니다.\r\n"
                           : L"분석 중…\r\n");
        }
    }
    else
    {
        stream << L"문제 " << result.diagnostics.size() << L"개\r\n\r\n";
    }
    for (const auto& diagnostic : result.diagnostics)
    {
        stream << L"[" << Maro_SeverityName(diagnostic.severity) << L"]";
        if (!diagnostic.code.empty())
        {
            stream << L" " << diagnostic.code;
        }
        if (diagnostic.range.start.line != 0)
        {
            stream << L"  Line " << diagnostic.range.start.line;
            if (diagnostic.range.start.column != 0)
            {
                stream << L" · Col " << diagnostic.range.start.column;
            }
        }
        stream << L"\r\n" << Maro_EvidenceSymbol(diagnostic.evidence) << L" "
               << Maro_EvidenceName(diagnostic.evidence) << L"\r\n";
        if (!diagnostic.friendlyMessage.empty())
        {
            stream << diagnostic.friendlyMessage << L"\r\n";
        }
        if (diagnostic.fix.has_value())
        {
            stream << L"수정 제안: " << diagnostic.fix->description << L"\r\n";
        }
        if (!diagnostic.originalDiagnostic.empty())
        {
            stream << L"원본 진단: " << diagnostic.originalDiagnostic << L"\r\n";
        }
        stream << L"\r\n";
    }
    if (!result.compilerOutput.empty())
    {
        stream << L"\r\n원본 컴파일러 출력\r\n----------------\r\n"
               << Maro_SanitizeDisplayText(result.compilerOutput);
        if (result.compilerOutput.back() != L'\n')
        {
            stream << L"\r\n";
        }
    }
    SetWindowTextW(analysis, stream.str().c_str());
}

void Maro_App::Maro_Impl::UpdateButtonStates()
{
    const bool ready = snapshot.has_value() &&
                       visualStudioStatus == Maro_VisualStudioStatus::Success;
    EnableWindow(analyzeButton, ready ? TRUE : FALSE);
    const bool canAcceptInput = ready && !Maro_IsHeaderDocument(*snapshot);
    EnableWindow(inputButton, canAcceptInput ? TRUE : FALSE);
    EnableWindow(cancelButton, busy ? TRUE : FALSE);
    const bool hasGenerated = ready && latestResult.has_value() &&
                              latestResult->sourceVersion == sourceVersion &&
                              !latestResult->generatedSource.empty();
    EnableWindow(generatedButton, hasGenerated ? TRUE : FALSE);
    const bool canFix = ready && !snapshot->readOnly && latestFix.has_value();
    EnableWindow(fixButton, canFix ? TRUE : FALSE);
    for (HWND button : {analyzeButton, cancelButton, inputButton,
                        generatedButton, fixButton})
    {
        InvalidateRect(button, nullptr, TRUE);
    }
}

void Maro_App::Maro_Impl::ShowInputDialog()
{
    if (!snapshot.has_value())
    {
        return;
    }
    std::wstring updatedInput = standardInput;
    if (Maro_ShowTextDialog(window, L"CLive_Maro — Program Input", true,
                            updatedInput))
    {
        standardInput = std::move(updatedInput);
        PollVisualStudio(true);
    }
}

void Maro_App::Maro_Impl::ShowGeneratedSource()
{
    if (!latestResult.has_value() || latestResult->sourceVersion != sourceVersion ||
        latestResult->generatedSource.empty())
    {
        MessageBoxW(window, L"현재 Visual Studio 버전에 생성된 코드가 없습니다.",
                    L"CLive_Maro", MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring generated = latestResult->generatedSource;
    Maro_ShowTextDialog(window, L"CLive_Maro — Generated Source", false, generated);
}

void Maro_App::Maro_Impl::ApplyAvailableFix()
{
    if (!snapshot.has_value() || snapshot->readOnly || !latestFix.has_value() ||
        latestFix->edits.empty())
    {
        return;
    }

    const Maro_VisualStudioSnapshot expectedSnapshot = *snapshot;
    const std::uint64_t expectedVersion = sourceVersion;
    const Maro_FixSuggestion fix = *latestFix;
    Maro_VisualStudioReadResult freshRead = visualStudio.ReadActiveDocument(
        expectedVersion, expectedSnapshot.instanceMoniker);
    if (!freshRead || !freshRead.snapshot.has_value() ||
        !Maro_SameSnapshot(expectedSnapshot, *freshRead.snapshot))
    {
        MessageBoxW(window,
                    L"Visual Studio 편집 버퍼가 분석 이후 변경되어 수정 제안을 "
                    L"적용하지 않았습니다. 최신 버전을 다시 분석합니다.",
                    L"CLive_Maro", MB_OK | MB_ICONWARNING);
        PollVisualStudio(false);
        return;
    }

    std::wstring replacementText = expectedSnapshot.text;
    std::vector<Maro_TextEdit> edits = fix.edits;
    std::sort(edits.begin(), edits.end(),
              [](const Maro_TextEdit& left, const Maro_TextEdit& right) {
                  return left.startOffsetUtf16 > right.startOffsetUtf16;
              });
    std::size_t nextStart = replacementText.size();
    for (const auto& edit : edits)
    {
        if (edit.sourceVersion != expectedVersion ||
            edit.startOffsetUtf16 > replacementText.size() ||
            edit.lengthUtf16 > replacementText.size() - edit.startOffsetUtf16 ||
            edit.startOffsetUtf16 + edit.lengthUtf16 > nextStart ||
            (!edit.expectedText.empty() &&
             replacementText.substr(edit.startOffsetUtf16, edit.lengthUtf16) !=
                 edit.expectedText))
        {
            MessageBoxW(window,
                        L"수정 제안이 현재 Visual Studio 편집 버퍼와 일치하지 않습니다.",
                        L"CLive_Maro", MB_OK | MB_ICONWARNING);
            PollVisualStudio(false);
            return;
        }
        nextStart = edit.startOffsetUtf16;
    }

    std::wostringstream previewStream;
    previewStream << (fix.description.empty()
                          ? L"컴파일러 수정 제안을 Visual Studio 버퍼에 적용하시겠습니까?"
                          : fix.description)
                  << L"\r\n\r\n변경 미리보기";
    std::size_t editNumber = 0;
    for (const auto& edit : edits)
    {
        ++editNumber;
        previewStream << L"\r\n\r\n[변경 " << editNumber << L"]\r\n기존: "
                      << (edit.expectedText.empty()
                              ? L"(빈 문자열)"
                              : Maro_SanitizeDisplayText(edit.expectedText))
                      << L"\r\n→ 변경: "
                      << (edit.replacement.empty()
                              ? L"(빈 문자열)"
                              : Maro_SanitizeDisplayText(edit.replacement));
    }
    std::wstring preview = previewStream.str();
    constexpr std::size_t Maro_MaxFixPreviewCharacters = 1'000;
    if (preview.size() > Maro_MaxFixPreviewCharacters)
    {
        preview.resize(Maro_MaxFixPreviewCharacters - 32);
        preview.append(L"\r\n… 나머지 변경은 생략되었습니다.");
    }
    if (MessageBoxW(window, preview.c_str(),
                    L"CLive_Maro — Visual Studio Fix",
                    MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
    {
        return;
    }
    if (sourceVersion != expectedVersion || !snapshot.has_value() ||
        !Maro_SameSnapshot(expectedSnapshot, *snapshot))
    {
        MessageBoxW(window,
                    L"승인하는 동안 Visual Studio 편집 버퍼가 변경되어 수정 제안을 "
                    L"적용하지 않았습니다.",
                    L"CLive_Maro", MB_OK | MB_ICONWARNING);
        return;
    }
    for (const auto& edit : edits)
    {
        replacementText.replace(edit.startOffsetUtf16, edit.lengthUtf16,
                                edit.replacement);
    }

    const Maro_VisualStudioApplyResult applied = visualStudio.ApplyFullText(
        expectedSnapshot, expectedVersion, replacementText);
    if (!applied)
    {
        std::wstring message = applied.message.empty()
                                   ? Maro_VisualStudioStatusName(applied.status)
                                   : applied.message;
        MessageBoxW(window, message.c_str(), L"CLive_Maro — Fix",
                    MB_OK | MB_ICONWARNING);
        PollVisualStudio(false);
        return;
    }
    latestFix.reset();
    SetStatus(L"Visual Studio 편집 버퍼에 수정 제안을 적용했습니다.");
    PollVisualStudio(false);
}

void Maro_App::Maro_Impl::BeginDrag(POINT point)
{
    if (PtInRect(&layout.splitter, point))
    {
        dragging = true;
        SetCapture(window);
        ContinueDrag(point);
    }
}

void Maro_App::Maro_Impl::ContinueDrag(POINT point)
{
    if (!dragging)
    {
        return;
    }
    RECT client{};
    GetClientRect(window, &client);
    const int contentTop = Maro_DipToPixel(82, dpi);
    const int divider = Maro_DipToPixel(6, dpi);
    const int usable = (std::max)(
        1, static_cast<int>(client.bottom) - contentTop - divider);
    const int minTop = Maro_DipToPixel(120, dpi);
    const int minBottom = Maro_DipToPixel(150, dpi);
    const int low = usable >= minTop + minBottom ? minTop : 0;
    const int high = usable >= minTop + minBottom ? usable - minBottom : usable;
    const int position = std::clamp(
        static_cast<int>(point.y) - contentTop, low, high);
    outputRatio = static_cast<double>(position) / static_cast<double>(usable);
    Layout();
}

void Maro_App::Maro_Impl::EndDrag()
{
    dragging = false;
    if (GetCapture() == window)
    {
        ReleaseCapture();
    }
}

void Maro_App::Maro_Impl::UpdateHover(POINT point)
{
    const bool hovered = PtInRect(&layout.splitter, point) != FALSE;
    if (hovered != splitterHovered)
    {
        splitterHovered = hovered;
        InvalidateRect(window, &layout.splitter, FALSE);
    }
    if (!trackingMouse)
    {
        TRACKMOUSEEVENT tracking{};
        tracking.cbSize = sizeof(tracking);
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = window;
        trackingMouse = TrackMouseEvent(&tracking) != FALSE;
    }
}

LRESULT Maro_App::Maro_Impl::HandleMessage(UINT message, WPARAM wParam,
                                            LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        dpi = GetDpiForWindow(window);
        if (!CreateControls())
        {
            return -1;
        }
        CreateEngine();
        Layout();
        SetTimer(window, Maro_VisualStudioPollTimer,
                 Maro_VisualStudioPollMilliseconds, nullptr);
        PollVisualStudio(false);
        return 0;
    case WM_SIZE:
        Layout();
        return 0;
    case WM_DPICHANGED:
    {
        dpi = HIWORD(wParam);
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        RecreateThemeResources();
        Layout();
        return 0;
    }
    case WM_GETMINMAXINFO:
    {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize.x = Maro_DipToPixel(720, dpi);
        limits->ptMinTrackSize.y = Maro_DipToPixel(500, dpi);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case Maro_AnalyzeButtonId:
            PollVisualStudio(true);
            return 0;
        case Maro_CancelButtonId:
            CancelRequest();
            return 0;
        case Maro_InputButtonId:
            ShowInputDialog();
            return 0;
        case Maro_GeneratedButtonId:
            ShowGeneratedSource();
            return 0;
        case Maro_FixButtonId:
            ApplyAvailableFix();
            return 0;
        default:
            break;
        }
        break;
    case WM_TIMER:
        if (wParam == Maro_VisualStudioPollTimer)
        {
            PollVisualStudio(false);
            return 0;
        }
        break;
    case Maro_ResultReadyMessage:
        DrainResults();
        return 0;
    case WM_DRAWITEM:
        if (lParam != 0)
        {
            DrawOwnerControl(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC:
    {
        const HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, Maro_ColorText);
        SetBkColor(dc, Maro_ColorSurface);
        return reinterpret_cast<LRESULT>(surfaceBrush);
    }
    case WM_PAINT:
        Paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        BeginDrag({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;
    case WM_MOUSEMOVE:
    {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (dragging)
        {
            ContinueDrag(point);
        }
        else
        {
            UpdateHover(point);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        trackingMouse = false;
        splitterHovered = false;
        InvalidateRect(window, &layout.splitter, FALSE);
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        EndDrag();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(window, &point);
            if (PtInRect(&layout.splitter, point))
            {
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                return TRUE;
            }
        }
        break;
    case WM_CLOSE:
        closing.store(true);
        KillTimer(window, Maro_VisualStudioPollTimer);
        if (engine != nullptr)
        {
            engine->Shutdown();
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        closing.store(true);
        KillTimer(window, Maro_VisualStudioPollTimer);
        if (engine != nullptr)
        {
            engine->Shutdown();
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

Maro_App::Maro_App(HINSTANCE instance)
    : impl_(std::make_unique<Maro_Impl>(*this, instance))
{
}

Maro_App::~Maro_App() = default;

int Maro_App::Run(int showCommand)
{
    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&commonControls);
    if (!impl_->CreateMainWindow(showCommand))
    {
        return 1;
    }
    MSG message{};
    while (true)
    {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result == 0)
        {
            break;
        }
        if (result < 0)
        {
            return 1;
        }
        if (impl_->accelerators == nullptr ||
            !TranslateAcceleratorW(impl_->window, impl_->accelerators, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (impl_->accelerators != nullptr)
    {
        DestroyAcceleratorTable(impl_->accelerators);
        impl_->accelerators = nullptr;
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK Maro_App::WindowProcedure(HWND window, UINT message,
                                            WPARAM wParam, LPARAM lParam)
{
    auto* application = reinterpret_cast<Maro_App*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        application = static_cast<Maro_App*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(application));
        application->impl_->window = window;
    }
    if (application == nullptr)
    {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return application->impl_->HandleMessage(message, wParam, lParam);
}
