#include "Maro_App.hpp"

#include "Maro_Engine.hpp"
#include "Maro_Models.hpp"

#include <commctrl.h>
#include <commdlg.h>
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
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace
{
constexpr wchar_t Maro_MainWindowClass[] = L"Maro_CLive_Maro_MainWindow";
constexpr wchar_t Maro_TextDialogClass[] = L"Maro_CLive_Maro_TextDialog";
constexpr UINT Maro_ResultReadyMessage = WM_APP + 41;
constexpr UINT_PTR Maro_DebounceTimer = 0xC117;
constexpr UINT Maro_DebounceMilliseconds = 400;

constexpr COLORREF Maro_ColorWindow = RGB(13, 17, 23);
constexpr COLORREF Maro_ColorSurface = RGB(22, 27, 34);
constexpr COLORREF Maro_ColorEditor = RGB(15, 20, 26);
constexpr COLORREF Maro_ColorBorder = RGB(48, 54, 61);
constexpr COLORREF Maro_ColorText = RGB(230, 237, 243);
constexpr COLORREF Maro_ColorMuted = RGB(139, 148, 158);
constexpr COLORREF Maro_ColorAccent = RGB(47, 129, 247);
constexpr COLORREF Maro_ColorAccentPressed = RGB(31, 111, 235);
constexpr COLORREF Maro_ColorDisabled = RGB(68, 76, 86);

enum Maro_ControlId : int
{
    Maro_RunButtonId = 1001,
    Maro_CancelButtonId,
    Maro_InputButtonId,
    Maro_OpenButtonId,
    Maro_SaveButtonId,
    Maro_GeneratedButtonId,
    Maro_FixButtonId,
    Maro_ModeComboId,
    Maro_LanguageComboId,
    Maro_EditorId,
    Maro_OutputId,
    Maro_AnalysisId,
    Maro_ModeLabelId,
    Maro_LanguageLabelId,
    Maro_StatusLabelId,
    Maro_OutputHeaderId,
    Maro_AnalysisHeaderId,
    Maro_EditorHeaderId
};

enum class Maro_DragTarget
{
    None,
    Vertical,
    Horizontal
};

struct Maro_LayoutRects
{
    RECT toolbar{};
    RECT verticalSplitter{};
    RECT horizontalSplitter{};
    RECT outputHeader{};
    RECT outputBody{};
    RECT analysisHeader{};
    RECT analysisBody{};
    RECT editorHeader{};
    RECT editorBody{};
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

std::wstring Maro_SanitizeConsoleText(std::wstring_view text)
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
        result.append(L"\r\n[표시 한도를 넘어 나머지 출력은 생략되었습니다.]\r\n");
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
        return L"작성 중";
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
        return L"실행 격리 사용 불가";
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

bool Maro_IsBusy(Maro_Phase phase, Maro_Status status)
{
    return status == Maro_Status::Pending &&
           phase != Maro_Phase::Idle &&
           phase != Maro_Phase::Debouncing &&
           phase != Maro_Phase::Completed;
}

std::wstring Maro_DecodeTextFile(const std::vector<char>& bytes)
{
    if (bytes.empty())
    {
        return {};
    }

    std::size_t offset = 0;
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xef &&
        static_cast<unsigned char>(bytes[1]) == 0xbb &&
        static_cast<unsigned char>(bytes[2]) == 0xbf)
    {
        offset = 3;
    }

    const auto sourceLength = bytes.size() - offset;
    if (sourceLength > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return {};
    }

    int required = MultiByteToWideChar(
        codePage,
        flags,
        bytes.data() + offset,
        static_cast<int>(sourceLength),
        nullptr,
        0);
    if (required == 0)
    {
        codePage = CP_ACP;
        flags = 0;
        required = MultiByteToWideChar(
            codePage,
            flags,
            bytes.data(),
            static_cast<int>(bytes.size()),
            nullptr,
            0);
        offset = 0;
    }
    if (required <= 0)
    {
        return {};
    }

    std::wstring text(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(
        codePage,
        flags,
        bytes.data() + offset,
        static_cast<int>(bytes.size() - offset),
        text.data(),
        required);
    return text;
}

std::optional<std::vector<char>> Maro_EncodeUtf8(std::wstring_view text)
{
    if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return std::nullopt;
    }
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required < 0 || (required == 0 && !text.empty()))
    {
        return std::nullopt;
    }
    std::vector<char> result(static_cast<std::size_t>(required));
    if (required > 0)
    {
        const int converted = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(text.size()),
            result.data(),
            required,
            nullptr,
            nullptr);
        if (converted != required)
        {
            return std::nullopt;
        }
    }
    return result;
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
    HBRUSH editorBrush = nullptr;
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
    const int buttonTop = client.bottom - padding - buttonHeight;
    MoveWindow(
        state.edit,
        padding,
        padding,
        (std::max)(0, static_cast<int>(client.right) - padding * 2),
        (std::max)(0, buttonTop - gap - padding),
        TRUE);

    HWND ok = GetDlgItem(window, IDOK);
    HWND cancel = GetDlgItem(window, IDCANCEL);
    if (state.inputMode)
    {
        MoveWindow(ok, client.right - padding * 2 - buttonWidth * 2 - gap,
                   buttonTop, buttonWidth, buttonHeight, TRUE);
        MoveWindow(cancel, client.right - padding - buttonWidth,
                   buttonTop, buttonWidth, buttonHeight, TRUE);
    }
    else
    {
        MoveWindow(ok, client.right - padding - buttonWidth,
                   buttonTop, buttonWidth, buttonHeight, TRUE);
    }
}

LRESULT CALLBACK Maro_TextDialogProcedure(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
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
        state->edit = CreateWindowExW(
            0,
            MSFTEDIT_CLASS,
            state->initialText.c_str(),
            editStyle,
            0,
            0,
            0,
            0,
            window,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        CreateWindowExW(
            0,
            L"BUTTON",
            state->inputMode ? L"확인" : L"닫기",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            0,
            0,
            0,
            0,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
            GetModuleHandleW(nullptr),
            nullptr);
        if (state->inputMode)
        {
            CreateWindowExW(
                0,
                L"BUTTON",
                L"취소",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                0,
                0,
                0,
                0,
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)),
                GetModuleHandleW(nullptr),
                nullptr);
        }
        const UINT dpi = GetDpiForWindow(window);
        state->font = CreateFontW(
            -Maro_DipToPixel(14, dpi),
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            FIXED_PITCH | FF_MODERN,
            L"Cascadia Mono");
        state->backgroundBrush = CreateSolidBrush(Maro_ColorWindow);
        state->editorBrush = CreateSolidBrush(Maro_ColorEditor);
        SendMessageW(state->edit, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
        SendMessageW(state->edit, EM_SETBKGNDCOLOR, 0, Maro_ColorEditor);
        CHARFORMAT2W format{};
        format.cbSize = sizeof(format);
        format.dwMask = CFM_COLOR;
        format.crTextColor = Maro_ColorText;
        SendMessageW(state->edit, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
        SendMessageW(state->edit, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&format));
        Maro_LayoutTextDialog(window, *state);
        SetFocus(state->edit);
        return 0;
    }
    case WM_SIZE:
        Maro_LayoutTextDialog(window, *state);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
            if (state->inputMode)
            {
                state->result = Maro_GetWindowTextString(state->edit);
                state->accepted = true;
            }
            DestroyWindow(window);
            return 0;
        case IDCANCEL:
            DestroyWindow(window);
            return 0;
        default:
            break;
        }
        break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    {
        const HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, Maro_ColorText);
        SetBkColor(dc, Maro_ColorEditor);
        return reinterpret_cast<LRESULT>(state->editorBrush);
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
        if (state->editorBrush != nullptr)
        {
            DeleteObject(state->editorBrush);
            state->editorBrush = nullptr;
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

bool Maro_ShowTextDialog(
    HWND owner,
    const wchar_t* title,
    bool inputMode,
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
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    EnableWindow(owner, FALSE);
    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        Maro_TextDialogClass,
        title,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        x,
        y,
        width,
        height,
        owner,
        nullptr,
        instance,
        &state);
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
    void RecreateThemeResources();
    void DestroyThemeResources();
    void ApplyRichEditTheme(HWND control, bool readOnly) const;
    void Layout();
    Maro_LayoutRects CalculateLayout() const;
    void Paint();
    void DrawOwnerControl(const DRAWITEMSTRUCT& item) const;
    void ScheduleAnalysis();
    void StartRequest(bool execute);
    void CancelRequest();
    void EnqueueResult(Maro_ResultEnvelope result);
    void DrainResults();
    void ApplyResult(const Maro_ResultEnvelope& result);
    void UpdateOutput(const Maro_ResultEnvelope& result);
    void UpdateAnalysis(const Maro_ResultEnvelope& result);
    void UpdateButtonStates();
    void SetStatus(const std::wstring& text);
    Maro_Language SelectedLanguage() const;
    Maro_SourceMode SelectedMode() const;
    void OnEditorChanged();
    void OnSemanticsChanged();
    bool OpenDocument();
    bool SaveDocument(bool forceChoosePath);
    bool PromptToSave();
    void UpdateTitle();
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
    HWND runButton = nullptr;
    HWND cancelButton = nullptr;
    HWND inputButton = nullptr;
    HWND openButton = nullptr;
    HWND saveButton = nullptr;
    HWND generatedButton = nullptr;
    HWND fixButton = nullptr;
    HWND modeCombo = nullptr;
    HWND languageCombo = nullptr;
    HWND modeLabel = nullptr;
    HWND languageLabel = nullptr;
    HWND statusLabel = nullptr;
    HWND outputHeader = nullptr;
    HWND analysisHeader = nullptr;
    HWND editorHeader = nullptr;
    HWND editor = nullptr;
    HWND output = nullptr;
    HWND analysis = nullptr;
    HFONT uiFont = nullptr;
    HFONT codeFont = nullptr;
    HFONT headerFont = nullptr;
    HBRUSH windowBrush = nullptr;
    HBRUSH surfaceBrush = nullptr;
    HBRUSH editorBrush = nullptr;
    HACCEL accelerators = nullptr;
    UINT dpi = 96;
    double leftRatio = 0.35;
    double outputRatio = 0.45;
    Maro_LayoutRects layout{};
    Maro_DragTarget dragging = Maro_DragTarget::None;
    Maro_DragTarget hoveredSplitter = Maro_DragTarget::None;
    bool trackingMouse = false;
    bool suppressEditorChange = false;
    bool dirty = false;
    bool busy = false;
    std::atomic_bool closing{false};
    std::uint64_t sourceVersion = 1;
    std::uint64_t activeRequestId = 0;
    Maro_Phase currentPhase = Maro_Phase::Idle;
    Maro_Status currentStatus = Maro_Status::Pending;
    std::wstring currentPath;
    std::wstring standardInput;
    std::optional<Maro_ResultEnvelope> latestResult;
    std::optional<Maro_FixSuggestion> latestFix;
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
        0,
        Maro_MainWindowClass,
        L"CLive_Maro",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1280,
        820,
        nullptr,
        nullptr,
        instance,
        &owner);
    if (window == nullptr)
    {
        return false;
    }

    ACCEL entries[] = {
        {FVIRTKEY, VK_F5, static_cast<WORD>(Maro_RunButtonId)},
        {static_cast<BYTE>(FVIRTKEY | FCONTROL), static_cast<WORD>('O'),
         static_cast<WORD>(Maro_OpenButtonId)},
        {static_cast<BYTE>(FVIRTKEY | FCONTROL), static_cast<WORD>('S'),
         static_cast<WORD>(Maro_SaveButtonId)},
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
            0,
            L"BUTTON",
            text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0,
            0,
            0,
            0,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            instance,
            nullptr);
    };
    const auto createLabel = [this](const wchar_t* text, int id, DWORD style = SS_LEFT) {
        return CreateWindowExW(
            0,
            L"STATIC",
            text,
            WS_CHILD | WS_VISIBLE | style,
            0,
            0,
            0,
            0,
            window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            instance,
            nullptr);
    };

    runButton = createButton(L"Run", Maro_RunButtonId);
    cancelButton = createButton(L"Cancel", Maro_CancelButtonId);
    inputButton = createButton(L"Input", Maro_InputButtonId);
    openButton = createButton(L"Open", Maro_OpenButtonId);
    saveButton = createButton(L"Save", Maro_SaveButtonId);
    generatedButton = createButton(L"Generated", Maro_GeneratedButtonId);
    fixButton = createButton(L"Fix", Maro_FixButtonId);
    modeLabel = createLabel(L"Mode", Maro_ModeLabelId, SS_CENTERIMAGE);
    languageLabel = createLabel(L"Language", Maro_LanguageLabelId, SS_CENTERIMAGE);
    statusLabel = createLabel(L"대기", Maro_StatusLabelId, SS_LEFT | SS_CENTERIMAGE);
    outputHeader = createLabel(L"OUTPUT", Maro_OutputHeaderId, SS_LEFT | SS_CENTERIMAGE);
    analysisHeader = createLabel(L"CODE ANALYSIS", Maro_AnalysisHeaderId,
                                 SS_LEFT | SS_CENTERIMAGE);
    editorHeader = createLabel(L"CODE EDITOR", Maro_EditorHeaderId,
                               SS_LEFT | SS_CENTERIMAGE);

    modeCombo = CreateWindowExW(
        0,
        WC_COMBOBOXW,
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
            CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        0,
        0,
        0,
        200,
        window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(Maro_ModeComboId)),
        instance,
        nullptr);
    languageCombo = CreateWindowExW(
        0,
        WC_COMBOBOXW,
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
            CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        0,
        0,
        0,
        200,
        window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(Maro_LanguageComboId)),
        instance,
        nullptr);
    SendMessageW(modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Program"));
    SendMessageW(modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Snippet"));
    SendMessageW(modeCombo, CB_SETCURSEL, 0, 0);
    SendMessageW(languageCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"C17"));
    SendMessageW(languageCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"C++20"));
    SendMessageW(languageCombo, CB_SETCURSEL, 0, 0);

    const DWORD editorStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                              WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
                              ES_AUTOHSCROLL | ES_WANTRETURN | ES_NOHIDESEL;
    editor = CreateWindowExW(
        0,
        MSFTEDIT_CLASS,
        nullptr,
        editorStyle,
        0,
        0,
        0,
        0,
        window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(Maro_EditorId)),
        instance,
        nullptr);
    output = CreateWindowExW(
        0,
        MSFTEDIT_CLASS,
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
            ES_READONLY,
        0,
        0,
        0,
        0,
        window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(Maro_OutputId)),
        instance,
        nullptr);
    analysis = CreateWindowExW(
        0,
        MSFTEDIT_CLASS,
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
            ES_READONLY,
        0,
        0,
        0,
        0,
        window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(Maro_AnalysisId)),
        instance,
        nullptr);

    const HWND requiredControls[] = {
        runButton, cancelButton, inputButton, openButton, saveButton,
        generatedButton, fixButton, modeCombo, languageCombo, modeLabel,
        languageLabel, statusLabel, outputHeader, analysisHeader, editorHeader,
        editor, output, analysis};
    if (std::any_of(std::begin(requiredControls), std::end(requiredControls),
                    [](HWND control) { return control == nullptr; }))
    {
        return false;
    }

    SendMessageW(editor, EM_EXLIMITTEXT, 0, 1U << 20);
    SendMessageW(output, EM_EXLIMITTEXT, 0, 4U << 20);
    SendMessageW(analysis, EM_EXLIMITTEXT, 0, 4U << 20);
    ApplyRichEditTheme(editor, false);
    ApplyRichEditTheme(output, true);
    ApplyRichEditTheme(analysis, true);
    RecreateThemeResources();

    suppressEditorChange = true;
    SetWindowTextW(
        editor,
        L"#include <stdio.h>\r\n\r\n"
        L"int main(void)\r\n"
        L"{\r\n"
        L"    printf(\"Hello from CLive_Maro!\\n\");\r\n"
        L"    return 0;\r\n"
        L"}\r\n");
    suppressEditorChange = false;
    SetWindowTextW(output,
                   L"코드를 입력하면 400ms 후 자동으로 분석·실행합니다.\r\n"
                   L"F5 또는 Run은 즉시 다시 실행합니다.");
    SetWindowTextW(analysis,
                   L"오류나 경고와 컴파일러의 원본 진단을 이곳에 표시합니다.");
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
    if (editorBrush != nullptr)
    {
        DeleteObject(editorBrush);
        editorBrush = nullptr;
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
    HBRUSH newEditorBrush = CreateSolidBrush(Maro_ColorEditor);

    const HWND uiControls[] = {
        runButton, cancelButton, inputButton, openButton, saveButton,
        generatedButton, fixButton, modeCombo, languageCombo, modeLabel,
        languageLabel, statusLabel};
    for (HWND control : uiControls)
    {
        if (control != nullptr)
        {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(newUiFont), TRUE);
        }
    }
    const HWND headerControls[] = {outputHeader, analysisHeader, editorHeader};
    for (HWND control : headerControls)
    {
        if (control != nullptr)
        {
            SendMessageW(control, WM_SETFONT,
                         reinterpret_cast<WPARAM>(newHeaderFont), TRUE);
        }
    }
    const HWND codeControls[] = {editor, output, analysis};
    for (HWND control : codeControls)
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
    editorBrush = newEditorBrush;

    const bool previousSuppression = suppressEditorChange;
    suppressEditorChange = true;
    ApplyRichEditTheme(editor, false);
    ApplyRichEditTheme(output, true);
    ApplyRichEditTheme(analysis, true);
    suppressEditorChange = previousSuppression;
    BOOL dark = TRUE;
    constexpr DWORD Maro_ImmersiveDarkModeAttribute = 20;
    DwmSetWindowAttribute(window,
                          static_cast<DWMWINDOWATTRIBUTE>(Maro_ImmersiveDarkModeAttribute),
                          &dark, sizeof(dark));
}

void Maro_App::Maro_Impl::ApplyRichEditTheme(HWND control, bool readOnly) const
{
    if (control == nullptr)
    {
        return;
    }
    SendMessageW(control, EM_SETBKGNDCOLOR, 0,
                 readOnly ? Maro_ColorSurface : Maro_ColorEditor);
    CHARRANGE selection{};
    SendMessageW(control, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR;
    format.crTextColor = Maro_ColorText;
    SendMessageW(control, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
    SendMessageW(control, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&format));
    SendMessageW(control, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
}

Maro_LayoutRects Maro_App::Maro_Impl::CalculateLayout() const
{
    RECT client{};
    GetClientRect(window, &client);
    const int width = (std::max)(
        0, static_cast<int>(client.right - client.left));
    const int height = (std::max)(
        0, static_cast<int>(client.bottom - client.top));
    const int toolbarHeight = Maro_DipToPixel(76, dpi);
    const int divider = Maro_DipToPixel(6, dpi);
    const int headerHeight = Maro_DipToPixel(30, dpi);
    const int contentTop = (std::min)(height, toolbarHeight);
    const int contentHeight = (std::max)(0, height - contentTop);
    const int usableWidth = (std::max)(0, width - divider);
    const int minLeft = Maro_DipToPixel(220, dpi);
    const int minEditor = Maro_DipToPixel(320, dpi);
    int leftWidth = static_cast<int>(static_cast<double>(usableWidth) * leftRatio);
    if (usableWidth >= minLeft + minEditor)
    {
        leftWidth = std::clamp(leftWidth, minLeft, usableWidth - minEditor);
    }
    else
    {
        leftWidth = usableWidth > 0
                        ? std::clamp(leftWidth, 0, usableWidth)
                        : 0;
    }

    const int usableHeight = (std::max)(0, contentHeight - divider);
    const int minOutput = Maro_DipToPixel(100, dpi);
    const int minAnalysis = Maro_DipToPixel(130, dpi);
    int outputHeight = static_cast<int>(static_cast<double>(usableHeight) * outputRatio);
    if (usableHeight >= minOutput + minAnalysis)
    {
        outputHeight = std::clamp(outputHeight, minOutput, usableHeight - minAnalysis);
    }
    else
    {
        outputHeight = usableHeight > 0
                           ? std::clamp(outputHeight, 0, usableHeight)
                           : 0;
    }

    Maro_LayoutRects result{};
    result.toolbar = {0, 0, width, contentTop};
    result.verticalSplitter = {leftWidth, contentTop,
                               (std::min)(width, leftWidth + divider), height};
    result.horizontalSplitter = {0, contentTop + outputHeight, leftWidth,
                                 (std::min)(height, contentTop + outputHeight + divider)};
    result.outputHeader = {0, contentTop, leftWidth,
                           (std::min)(height, contentTop + headerHeight)};
    result.outputBody = {0, result.outputHeader.bottom, leftWidth,
                         result.horizontalSplitter.top};
    result.analysisHeader = {0, result.horizontalSplitter.bottom, leftWidth,
                             (std::min)(
                                 height,
                                 static_cast<int>(result.horizontalSplitter.bottom) +
                                     headerHeight)};
    result.analysisBody = {0, result.analysisHeader.bottom, leftWidth, height};
    result.editorHeader = {result.verticalSplitter.right, contentTop, width,
                           (std::min)(height, contentTop + headerHeight)};
    result.editorBody = {result.verticalSplitter.right, result.editorHeader.bottom,
                         width, height};
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
            defer,
            control,
            nullptr,
            rect.left,
            rect.top,
            (std::max)(0, static_cast<int>(rect.right - rect.left)),
            (std::max)(0, static_cast<int>(rect.bottom - rect.top)),
            SWP_NOZORDER | SWP_NOACTIVATE);
    };
    HDWP defer = BeginDeferWindowPos(18);
    if (defer != nullptr)
    {
        defer = place(defer, outputHeader, layout.outputHeader);
        defer = place(defer, output, layout.outputBody);
        defer = place(defer, analysisHeader, layout.analysisHeader);
        defer = place(defer, analysis, layout.analysisBody);
        defer = place(defer, editorHeader, layout.editorHeader);
        defer = place(defer, editor, layout.editorBody);

        const int pad = Maro_DipToPixel(10, dpi);
        const int gap = Maro_DipToPixel(6, dpi);
        const int buttonHeight = Maro_DipToPixel(28, dpi);
        const int rowOneY = Maro_DipToPixel(7, dpi);
        int x = pad;
        const struct
        {
            HWND control;
            int widthDip;
        } buttons[] = {
            {runButton, 54},       {cancelButton, 64}, {inputButton, 56},
            {openButton, 56},      {saveButton, 56},   {generatedButton, 86},
            {fixButton, 50}};
        for (const auto& button : buttons)
        {
            const int controlWidth = Maro_DipToPixel(button.widthDip, dpi);
            RECT rect{x, rowOneY, x + controlWidth, rowOneY + buttonHeight};
            defer = place(defer, button.control, rect);
            x += controlWidth + gap;
        }

        const int rowTwoY = Maro_DipToPixel(42, dpi);
        const int labelHeight = Maro_DipToPixel(26, dpi);
        x = pad;
        const int modeLabelWidth = Maro_DipToPixel(38, dpi);
        RECT rect{x, rowTwoY, x + modeLabelWidth, rowTwoY + labelHeight};
        defer = place(defer, modeLabel, rect);
        x = rect.right + gap;
        const int modeWidth = Maro_DipToPixel(110, dpi);
        rect = {x, rowTwoY, x + modeWidth, rowTwoY + Maro_DipToPixel(200, dpi)};
        defer = place(defer, modeCombo, rect);
        x += modeWidth + Maro_DipToPixel(14, dpi);
        const int languageLabelWidth = Maro_DipToPixel(62, dpi);
        rect = {x, rowTwoY, x + languageLabelWidth, rowTwoY + labelHeight};
        defer = place(defer, languageLabel, rect);
        x = rect.right + gap;
        const int languageWidth = Maro_DipToPixel(92, dpi);
        rect = {x, rowTwoY, x + languageWidth,
                rowTwoY + Maro_DipToPixel(200, dpi)};
        defer = place(defer, languageCombo, rect);
        x += languageWidth + Maro_DipToPixel(14, dpi);
        RECT client{};
        GetClientRect(window, &client);
        rect = {x, rowTwoY,
                (std::max)(x, static_cast<int>(client.right) - pad),
                rowTwoY + labelHeight};
        defer = place(defer, statusLabel, rect);
        EndDeferWindowPos(defer);
    }
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
    HBRUSH splitterBrush = CreateSolidBrush(
        hoveredSplitter == Maro_DragTarget::None ? Maro_ColorBorder
                                                  : Maro_ColorAccent);
    FillRect(dc, &layout.verticalSplitter, splitterBrush);
    FillRect(dc, &layout.horizontalSplitter, splitterBrush);
    DeleteObject(splitterBrush);
    EndPaint(window, &paint);
}

void Maro_App::Maro_Impl::DrawOwnerControl(const DRAWITEMSTRUCT& item) const
{
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    COLORREF background = Maro_ColorSurface;
    if (disabled)
    {
        background = Maro_ColorWindow;
    }
    else if (selected)
    {
        background = Maro_ColorAccentPressed;
    }
    else if (item.CtlType == ODT_BUTTON)
    {
        background = Maro_ColorAccent;
    }

    HBRUSH brush = CreateSolidBrush(background);
    FillRect(item.hDC, &item.rcItem, brush);
    DeleteObject(brush);
    SetDCBrushColor(item.hDC, Maro_ColorBorder);
    FrameRect(item.hDC, &item.rcItem,
              static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));

    std::wstring text;
    if (item.CtlType == ODT_COMBOBOX && item.itemID != static_cast<UINT>(-1))
    {
        const LRESULT length = SendMessageW(item.hwndItem, CB_GETLBTEXTLEN,
                                            item.itemID, 0);
        if (length >= 0)
        {
            text.resize(static_cast<std::size_t>(length) + 1);
            SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID,
                         reinterpret_cast<LPARAM>(text.data()));
            text.resize(static_cast<std::size_t>(length));
        }
    }
    else
    {
        text = Maro_GetWindowTextString(item.hwndItem);
    }

    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? Maro_ColorDisabled : Maro_ColorText);
    HFONT font = reinterpret_cast<HFONT>(
        SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
    const HGDIOBJ oldFont = SelectObject(item.hDC, font);
    RECT textRect = item.rcItem;
    InflateRect(&textRect, -Maro_DipToPixel(6, dpi), 0);
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

Maro_Language Maro_App::Maro_Impl::SelectedLanguage() const
{
    return SendMessageW(languageCombo, CB_GETCURSEL, 0, 0) == 1
               ? Maro_Language::Cpp20
               : Maro_Language::C17;
}

Maro_SourceMode Maro_App::Maro_Impl::SelectedMode() const
{
    return SendMessageW(modeCombo, CB_GETCURSEL, 0, 0) == 1
               ? Maro_SourceMode::Snippet
               : Maro_SourceMode::Program;
}

void Maro_App::Maro_Impl::SetStatus(const std::wstring& text)
{
    Maro_SetWindowTextIfChanged(statusLabel, text);
}

void Maro_App::Maro_Impl::ScheduleAnalysis()
{
    if (closing.load())
    {
        return;
    }
    KillTimer(window, Maro_DebounceTimer);
    SetTimer(window, Maro_DebounceTimer, Maro_DebounceMilliseconds, nullptr);
    currentPhase = Maro_Phase::Debouncing;
    currentStatus = Maro_Status::Pending;
    busy = false;
    latestResult.reset();
    latestFix.reset();
    SetStatus(L"작성 중 · 400ms 후 분석");
    SetWindowTextW(output,
                   L"IDE STATUS\r\n----------------\r\n"
                   L"코드가 변경되어 이전 실행 결과를 지웠습니다.\r\n"
                   L"400ms 후 최신 버전을 실행합니다.");
    SetWindowTextW(analysis, L"● 작성 중\r\n입력이 멈추면 최신 코드를 분석합니다.");
    UpdateButtonStates();
}

void Maro_App::Maro_Impl::StartRequest(bool execute)
{
    if (engine == nullptr || closing.load())
    {
        return;
    }
    KillTimer(window, Maro_DebounceTimer);
    Maro_SourceRequest request{};
    request.sourceVersion = sourceVersion;
    request.sourceText = Maro_GetWindowTextString(editor);
    request.standardInput = standardInput;
    request.language = SelectedLanguage();
    request.mode = SelectedMode();
    request.execute = execute;
    activeRequestId = engine->Submit(std::move(request));
    currentPhase = Maro_Phase::Analyzing;
    currentStatus = Maro_Status::Pending;
    busy = true;
    latestResult.reset();
    latestFix.reset();
    SetStatus(L"분석 요청 중…");
    SetWindowTextW(output,
                   L"IDE STATUS\r\n----------------\r\n최신 코드를 분석 중입니다.");
    SetWindowTextW(analysis, L"분석 중…");
    UpdateButtonStates();
}

void Maro_App::Maro_Impl::CancelRequest()
{
    KillTimer(window, Maro_DebounceTimer);
    if (engine != nullptr)
    {
        engine->Cancel();
    }
    busy = false;
    currentStatus = Maro_Status::Cancelled;
    SetStatus(L"취소됨");
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
        if (result.sourceVersion != sourceVersion ||
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
        stream << Maro_SanitizeConsoleText(result.standardOutput);
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
               << Maro_SanitizeConsoleText(result.standardError) << L"\r\n";
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
    if (result.snippetWrapped)
    {
        stream << L"ⓘ 학습용 실행: 임시 main()을 생성했습니다.\r\n";
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
        stream << L"\r\n";
        stream << Maro_EvidenceSymbol(diagnostic.evidence) << L" "
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
               << Maro_SanitizeConsoleText(result.compilerOutput);
        if (result.compilerOutput.back() != L'\n')
        {
            stream << L"\r\n";
        }
    }
    SetWindowTextW(analysis, stream.str().c_str());
}

void Maro_App::Maro_Impl::UpdateButtonStates()
{
    const bool canCancel = busy ||
                           (currentPhase == Maro_Phase::Debouncing &&
                            currentStatus == Maro_Status::Pending);
    EnableWindow(cancelButton, canCancel ? TRUE : FALSE);
    const bool hasGenerated = latestResult.has_value() &&
                              !latestResult->generatedSource.empty() &&
                              latestResult->sourceVersion == sourceVersion;
    EnableWindow(generatedButton, hasGenerated ? TRUE : FALSE);
    EnableWindow(fixButton, latestFix.has_value() ? TRUE : FALSE);
    InvalidateRect(runButton, nullptr, TRUE);
    InvalidateRect(cancelButton, nullptr, TRUE);
    InvalidateRect(generatedButton, nullptr, TRUE);
    InvalidateRect(fixButton, nullptr, TRUE);
}

void Maro_App::Maro_Impl::OnEditorChanged()
{
    if (suppressEditorChange)
    {
        return;
    }
    ++sourceVersion;
    dirty = true;
    if (engine != nullptr)
    {
        engine->Cancel();
    }
    UpdateTitle();
    ScheduleAnalysis();
}

void Maro_App::Maro_Impl::OnSemanticsChanged()
{
    ++sourceVersion;
    if (engine != nullptr)
    {
        engine->Cancel();
    }
    ScheduleAnalysis();
}

bool Maro_App::Maro_Impl::OpenDocument()
{
    if (!PromptToSave())
    {
        return false;
    }
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window;
    dialog.lpstrFilter = L"C/C++ Source\0*.c;*.h;*.cpp;*.hpp;*.cc;*.cxx\0All Files\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER |
                   OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&dialog))
    {
        return false;
    }

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        MessageBoxW(window, L"파일을 열 수 없습니다.", L"CLive_Maro",
                    MB_OK | MB_ICONERROR);
        return false;
    }
    LARGE_INTEGER size{};
    constexpr LONGLONG Maro_MaxSourceFileBytes = 1LL << 20;
    const bool validSize = GetFileSizeEx(file, &size) != FALSE &&
                           size.QuadPart >= 0 &&
                           size.QuadPart <= Maro_MaxSourceFileBytes;
    if (!validSize)
    {
        CloseHandle(file);
        MessageBoxW(window, L"소스 파일은 최대 1MB까지 열 수 있습니다.",
                    L"CLive_Maro", MB_OK | MB_ICONERROR);
        return false;
    }
    std::vector<char> bytes(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL readOk = bytes.empty() ||
                        ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                                 &read, nullptr);
    CloseHandle(file);
    if (!readOk || read != bytes.size())
    {
        MessageBoxW(window, L"파일을 끝까지 읽지 못했습니다.", L"CLive_Maro",
                    MB_OK | MB_ICONERROR);
        return false;
    }
    bytes.resize(read);
    const std::wstring text = Maro_DecodeTextFile(bytes);
    if (text.find(L'\0') != std::wstring::npos)
    {
        MessageBoxW(window, L"텍스트 소스가 아닌 파일은 열 수 없습니다.",
                    L"CLive_Maro", MB_OK | MB_ICONERROR);
        return false;
    }

    suppressEditorChange = true;
    SetWindowTextW(editor, text.c_str());
    suppressEditorChange = false;
    currentPath = path;
    standardInput.clear();
    dirty = false;
    ++sourceVersion;

    std::wstring lowerPath = currentPath;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                   [](wchar_t character) {
                       return static_cast<wchar_t>(towlower(character));
                   });
    if (lowerPath.ends_with(L".cpp") || lowerPath.ends_with(L".cc") ||
        lowerPath.ends_with(L".cxx") || lowerPath.ends_with(L".hpp"))
    {
        SendMessageW(languageCombo, CB_SETCURSEL, 1, 0);
    }
    else if (lowerPath.ends_with(L".c") || lowerPath.ends_with(L".h"))
    {
        SendMessageW(languageCombo, CB_SETCURSEL, 0, 0);
    }
    UpdateTitle();
    ScheduleAnalysis();
    return true;
}

bool Maro_App::Maro_Impl::SaveDocument(bool forceChoosePath)
{
    std::wstring path = currentPath;
    if (forceChoosePath || path.empty())
    {
        wchar_t buffer[32768]{};
        if (!path.empty())
        {
            wcsncpy_s(buffer, path.c_str(), _TRUNCATE);
        }
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window;
        dialog.lpstrFilter = L"C Source\0*.c\0C++ Source\0*.cpp\0All Files\0*.*\0\0";
        dialog.lpstrFile = buffer;
        dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
        dialog.lpstrDefExt = SelectedLanguage() == Maro_Language::Cpp20 ? L"cpp" : L"c";
        dialog.Flags = OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_OVERWRITEPROMPT;
        if (!GetSaveFileNameW(&dialog))
        {
            return false;
        }
        path = buffer;
    }

    const auto encoded = Maro_EncodeUtf8(Maro_GetWindowTextString(editor));
    if (!encoded.has_value())
    {
        MessageBoxW(window, L"UTF-8로 변환할 수 없는 텍스트가 있습니다.",
                    L"CLive_Maro", MB_OK | MB_ICONERROR);
        return false;
    }
    if (path.size() > 32'000)
    {
        MessageBoxW(window, L"저장 경로가 너무 깁니다.", L"CLive_Maro",
                    MB_OK | MB_ICONERROR);
        return false;
    }
    const std::wstring temporaryPath =
        path + L".Maro_tmp_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::to_wstring(GetTickCount64());
    HANDLE file = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        MessageBoxW(window, L"파일을 저장할 수 없습니다.", L"CLive_Maro",
                    MB_OK | MB_ICONERROR);
        return false;
    }
    DWORD written = 0;
    const BOOL writeOk = encoded->empty() ||
                         WriteFile(file, encoded->data(),
                                   static_cast<DWORD>(encoded->size()),
                                   &written, nullptr);
    const BOOL flushOk = writeOk ? FlushFileBuffers(file) : FALSE;
    CloseHandle(file);
    if (!writeOk || !flushOk || written != encoded->size())
    {
        DeleteFileW(temporaryPath.c_str());
        MessageBoxW(window, L"파일을 끝까지 저장하지 못했습니다.", L"CLive_Maro",
                    MB_OK | MB_ICONERROR);
        return false;
    }
    if (!MoveFileExW(temporaryPath.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporaryPath.c_str());
        MessageBoxW(window, L"완성된 임시 파일을 저장 위치로 옮기지 못했습니다.",
                    L"CLive_Maro", MB_OK | MB_ICONERROR);
        return false;
    }
    currentPath = std::move(path);
    dirty = false;
    UpdateTitle();
    SetStatus(L"저장됨");
    return true;
}

bool Maro_App::Maro_Impl::PromptToSave()
{
    if (!dirty)
    {
        return true;
    }
    const int answer = MessageBoxW(
        window,
        L"변경한 코드를 저장하시겠습니까?",
        L"CLive_Maro",
        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL)
    {
        return false;
    }
    return answer != IDYES || SaveDocument(false);
}

void Maro_App::Maro_Impl::UpdateTitle()
{
    std::wstring title = L"CLive_Maro";
    if (!currentPath.empty())
    {
        const std::size_t slash = currentPath.find_last_of(L"\\/");
        title.append(L" — ");
        title.append(slash == std::wstring::npos ? currentPath
                                                 : currentPath.substr(slash + 1));
    }
    if (dirty)
    {
        title.append(L" *");
    }
    SetWindowTextW(window, title.c_str());
}

void Maro_App::Maro_Impl::ShowInputDialog()
{
    std::wstring updatedInput = standardInput;
    if (Maro_ShowTextDialog(window, L"CLive_Maro — Program Input", true,
                            updatedInput))
    {
        standardInput = std::move(updatedInput);
        StartRequest(true);
    }
}

void Maro_App::Maro_Impl::ShowGeneratedSource()
{
    if (!latestResult.has_value() || latestResult->sourceVersion != sourceVersion ||
        latestResult->generatedSource.empty())
    {
        MessageBoxW(window, L"현재 버전에 생성된 코드가 없습니다.", L"CLive_Maro",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring generated = latestResult->generatedSource;
    Maro_ShowTextDialog(window, L"CLive_Maro — Generated Source", false, generated);
}

void Maro_App::Maro_Impl::ApplyAvailableFix()
{
    if (!latestFix.has_value() || latestFix->edits.empty())
    {
        return;
    }
    std::wostringstream previewStream;
    previewStream << (latestFix->description.empty()
                          ? L"컴파일러 수정 제안을 적용하시겠습니까?"
                          : latestFix->description)
                  << L"\r\n\r\n변경 미리보기";
    std::size_t editNumber = 0;
    for (const auto& edit : latestFix->edits)
    {
        ++editNumber;
        previewStream << L"\r\n\r\n[변경 " << editNumber << L"]\r\n"
                      << L"기존: "
                      << (edit.expectedText.empty()
                              ? L"(빈 문자열)"
                              : Maro_SanitizeConsoleText(edit.expectedText))
                      << L"\r\n→ 변경: "
                      << (edit.replacement.empty()
                              ? L"(빈 문자열)"
                              : Maro_SanitizeConsoleText(edit.replacement));
    }
    std::wstring preview = previewStream.str();
    constexpr std::size_t Maro_MaxFixPreviewCharacters = 1'000;
    if (preview.size() > Maro_MaxFixPreviewCharacters)
    {
        preview.resize(Maro_MaxFixPreviewCharacters - 32);
        preview.append(L"\r\n… 나머지 변경은 생략되었습니다.");
    }

    const int answer = MessageBoxW(
        window,
        preview.c_str(),
        L"CLive_Maro — Compiler Fix Suggestion",
        MB_OKCANCEL | MB_ICONQUESTION);
    if (answer != IDOK)
    {
        return;
    }

    std::wstring source = Maro_GetWindowTextString(editor);
    std::vector<Maro_TextEdit> edits = latestFix->edits;
    std::sort(edits.begin(), edits.end(),
              [](const Maro_TextEdit& left, const Maro_TextEdit& right) {
                  return left.startOffsetUtf16 > right.startOffsetUtf16;
              });
    std::size_t nextStart = source.size();
    for (const auto& edit : edits)
    {
        if (edit.sourceVersion != sourceVersion ||
            edit.startOffsetUtf16 > source.size() ||
            edit.lengthUtf16 > source.size() - edit.startOffsetUtf16 ||
            edit.startOffsetUtf16 + edit.lengthUtf16 > nextStart)
        {
            MessageBoxW(window, L"수정 제안이 현재 코드와 일치하지 않습니다.",
                        L"CLive_Maro", MB_OK | MB_ICONWARNING);
            return;
        }
        if (!edit.expectedText.empty() &&
            source.substr(edit.startOffsetUtf16, edit.lengthUtf16) !=
                edit.expectedText)
        {
            MessageBoxW(window, L"수정할 원문이 변경되어 제안을 적용하지 않았습니다.",
                        L"CLive_Maro", MB_OK | MB_ICONWARNING);
            return;
        }
        nextStart = edit.startOffsetUtf16;
    }
    for (const auto& edit : edits)
    {
        source.replace(edit.startOffsetUtf16, edit.lengthUtf16, edit.replacement);
    }
    suppressEditorChange = true;
    SetWindowTextW(editor, source.c_str());
    suppressEditorChange = false;
    ++sourceVersion;
    dirty = true;
    UpdateTitle();
    ScheduleAnalysis();
}

void Maro_App::Maro_Impl::BeginDrag(POINT point)
{
    if (PtInRect(&layout.verticalSplitter, point))
    {
        dragging = Maro_DragTarget::Vertical;
    }
    else if (PtInRect(&layout.horizontalSplitter, point))
    {
        dragging = Maro_DragTarget::Horizontal;
    }
    if (dragging != Maro_DragTarget::None)
    {
        SetCapture(window);
        ContinueDrag(point);
    }
}

void Maro_App::Maro_Impl::ContinueDrag(POINT point)
{
    RECT client{};
    GetClientRect(window, &client);
    const int divider = Maro_DipToPixel(6, dpi);
    if (dragging == Maro_DragTarget::Vertical)
    {
        const int usable = (std::max)(
            1, static_cast<int>(client.right) - divider);
        const int minLeft = Maro_DipToPixel(220, dpi);
        const int minRight = Maro_DipToPixel(320, dpi);
        const int low = usable >= minLeft + minRight ? minLeft : 0;
        const int high = usable >= minLeft + minRight ? usable - minRight : usable;
        const int position = std::clamp(static_cast<int>(point.x), low, high);
        leftRatio = static_cast<double>(position) / static_cast<double>(usable);
        Layout();
    }
    else if (dragging == Maro_DragTarget::Horizontal)
    {
        const int contentTop = Maro_DipToPixel(76, dpi);
        const int usable = (std::max)(
            1, static_cast<int>(client.bottom) - contentTop - divider);
        const int minTop = Maro_DipToPixel(100, dpi);
        const int minBottom = Maro_DipToPixel(130, dpi);
        const int low = usable >= minTop + minBottom ? minTop : 0;
        const int high = usable >= minTop + minBottom ? usable - minBottom : usable;
        const int position = std::clamp(
            static_cast<int>(point.y) - contentTop, low, high);
        outputRatio = static_cast<double>(position) / static_cast<double>(usable);
        Layout();
    }
}

void Maro_App::Maro_Impl::EndDrag()
{
    dragging = Maro_DragTarget::None;
    if (GetCapture() == window)
    {
        ReleaseCapture();
    }
}

void Maro_App::Maro_Impl::UpdateHover(POINT point)
{
    Maro_DragTarget newHover = Maro_DragTarget::None;
    if (PtInRect(&layout.verticalSplitter, point))
    {
        newHover = Maro_DragTarget::Vertical;
    }
    else if (PtInRect(&layout.horizontalSplitter, point))
    {
        newHover = Maro_DragTarget::Horizontal;
    }
    if (newHover != hoveredSplitter)
    {
        hoveredSplitter = newHover;
        InvalidateRect(window, &layout.verticalSplitter, FALSE);
        InvalidateRect(window, &layout.horizontalSplitter, FALSE);
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

LRESULT Maro_App::Maro_Impl::HandleMessage(
    UINT message,
    WPARAM wParam,
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
        ScheduleAnalysis();
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
        limits->ptMinTrackSize.y = Maro_DipToPixel(520, dpi);
        return 0;
    }
    case WM_COMMAND:
    {
        const int id = LOWORD(wParam);
        const int notification = HIWORD(wParam);
        if (id == Maro_EditorId && notification == EN_CHANGE)
        {
            OnEditorChanged();
            return 0;
        }
        if ((id == Maro_ModeComboId || id == Maro_LanguageComboId) &&
            notification == CBN_SELCHANGE)
        {
            OnSemanticsChanged();
            return 0;
        }
        switch (id)
        {
        case Maro_RunButtonId:
            StartRequest(true);
            return 0;
        case Maro_CancelButtonId:
            CancelRequest();
            return 0;
        case Maro_InputButtonId:
            ShowInputDialog();
            return 0;
        case Maro_OpenButtonId:
            OpenDocument();
            return 0;
        case Maro_SaveButtonId:
            SaveDocument(false);
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
    }
    case WM_TIMER:
        if (wParam == Maro_DebounceTimer)
        {
            KillTimer(window, Maro_DebounceTimer);
            StartRequest(true);
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
    case WM_MEASUREITEM:
        if (lParam != 0)
        {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            measure->itemHeight = static_cast<UINT>(Maro_DipToPixel(26, dpi));
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX:
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
        if (dragging != Maro_DragTarget::None)
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
        hoveredSplitter = Maro_DragTarget::None;
        InvalidateRect(window, &layout.verticalSplitter, FALSE);
        InvalidateRect(window, &layout.horizontalSplitter, FALSE);
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
            if (PtInRect(&layout.verticalSplitter, point))
            {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
            if (PtInRect(&layout.horizontalSplitter, point))
            {
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                return TRUE;
            }
        }
        break;
    case WM_CLOSE:
        if (PromptToSave())
        {
            closing.store(true);
            KillTimer(window, Maro_DebounceTimer);
            if (engine != nullptr)
            {
                engine->Shutdown();
            }
            DestroyWindow(window);
        }
        return 0;
    case WM_DESTROY:
        closing.store(true);
        KillTimer(window, Maro_DebounceTimer);
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

LRESULT CALLBACK Maro_App::WindowProcedure(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
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
