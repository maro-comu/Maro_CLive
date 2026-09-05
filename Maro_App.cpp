#include "Maro_App.hpp"

#include "Maro_Engine.hpp"
#include "Maro_Models.hpp"
#include "Maro_VisualStudio.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
using Maro_Clock = std::chrono::steady_clock;

constexpr DWORD Maro_PollMilliseconds = 400;
constexpr DWORD Maro_DebounceMilliseconds = 650;
constexpr std::size_t Maro_MaxDisplayCharacters = 1U << 20;
constexpr std::size_t Maro_MaxInputCharacters = 64U << 10;

constexpr WORD Maro_ColorText = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
constexpr WORD Maro_ColorBright = Maro_ColorText | FOREGROUND_INTENSITY;
constexpr WORD Maro_ColorBorder = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
constexpr WORD Maro_ColorSelected = FOREGROUND_GREEN | FOREGROUND_INTENSITY;

std::atomic_bool Maro_StopRequested{false};
std::atomic<HANDLE> Maro_WakeHandle{nullptr};

BOOL WINAPI Maro_ConsoleControlHandler(DWORD controlType)
{
    switch (controlType)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        Maro_StopRequested.store(true, std::memory_order_release);
        if (const HANDLE wake = Maro_WakeHandle.load(std::memory_order_acquire);
            wake != nullptr)
        {
            SetEvent(wake);
        }
        return TRUE;
    default:
        return FALSE;
    }
}

bool Maro_IsValidHandle(HANDLE handle) noexcept
{
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

bool Maro_IsWideCharacter(wchar_t character) noexcept
{
    const unsigned int value = static_cast<unsigned int>(character);
    return (value >= 0x1100U && value <= 0x115fU) ||
           (value >= 0x2329U && value <= 0x232aU) ||
           (value >= 0x2e80U && value <= 0xa4cfU) ||
           (value >= 0xac00U && value <= 0xd7a3U) ||
           (value >= 0xf900U && value <= 0xfaffU) ||
           (value >= 0xfe10U && value <= 0xfe6fU) ||
           (value >= 0xff01U && value <= 0xff60U) ||
           (value >= 0xffe0U && value <= 0xffe6U);
}

bool Maro_IsHighSurrogate(wchar_t character) noexcept
{
    const unsigned int value = static_cast<unsigned int>(character);
    return value >= 0xd800U && value <= 0xdbffU;
}

bool Maro_IsLowSurrogate(wchar_t character) noexcept
{
    const unsigned int value = static_cast<unsigned int>(character);
    return value >= 0xdc00U && value <= 0xdfffU;
}

bool Maro_IsUnsafeControl(wchar_t character) noexcept
{
    const unsigned int value = static_cast<unsigned int>(character);
    return value < 0x20U || (value >= 0x7fU && value <= 0x9fU);
}

std::vector<std::wstring> Maro_WrapForConsole(
    std::wstring_view text,
    int columns)
{
    std::vector<std::wstring> lines;
    if (columns <= 0)
    {
        lines.emplace_back();
        return lines;
    }

    std::wstring line;
    int lineColumns = 0;
    std::size_t consumed = 0;
    bool truncated = false;

    const auto flushLine = [&lines, &line, &lineColumns] {
        lines.push_back(line);
        line.clear();
        lineColumns = 0;
    };

    const auto appendCharacter = [&line, &lineColumns, columns, &flushLine](
                                     wchar_t character) {
        int characterColumns = Maro_IsWideCharacter(character) ? 2 : 1;
        if (characterColumns > columns)
        {
            character = L'?';
            characterColumns = 1;
        }
        if (lineColumns + characterColumns > columns && !line.empty())
        {
            flushLine();
        }
        line.push_back(character);
        lineColumns += characterColumns;
    };

    for (std::size_t index = 0; index < text.size(); ++index)
    {
        if (consumed >= Maro_MaxDisplayCharacters)
        {
            truncated = true;
            break;
        }
        ++consumed;

        wchar_t character = text[index];
        if (character == L'\r')
        {
            if (index + 1U < text.size() && text[index + 1U] == L'\n')
            {
                ++index;
                ++consumed;
                flushLine();
            }
            else
            {
                appendCharacter(L'?');
            }
            continue;
        }
        if (character == L'\n')
        {
            flushLine();
            continue;
        }
        if (character == L'\t')
        {
            const int spaces = 4 - (lineColumns % 4);
            for (int count = 0; count < spaces; ++count)
            {
                appendCharacter(L' ');
            }
            continue;
        }
        if (Maro_IsHighSurrogate(character))
        {
            if (index + 1U < text.size() && Maro_IsLowSurrogate(text[index + 1U]))
            {
                ++index;
                ++consumed;
            }
            appendCharacter(L'?');
            continue;
        }
        if (Maro_IsLowSurrogate(character) || Maro_IsUnsafeControl(character))
        {
            character = L'?';
        }
        appendCharacter(character);
    }

    if (truncated)
    {
        flushLine();
        for (const wchar_t character : std::wstring_view(L"[출력 생략]"))
        {
            appendCharacter(character);
        }
    }
    lines.push_back(line);
    return lines;
}

std::wstring Maro_FileName(std::wstring_view path, std::wstring_view fallback)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    std::wstring_view name = path;
    if (slash != std::wstring_view::npos && slash + 1U < path.size())
    {
        name = path.substr(slash + 1U);
    }
    if (name.empty())
    {
        name = fallback;
    }
    return name.empty() ? std::wstring(L"-") : std::wstring(name);
}

const wchar_t* Maro_LanguageName(Maro_Language language) noexcept
{
    return language == Maro_Language::C17 ? L"C17" : L"C++20";
}

const wchar_t* Maro_PhaseName(Maro_Phase phase) noexcept
{
    switch (phase)
    {
    case Maro_Phase::Idle:
        return L"대기";
    case Maro_Phase::Debouncing:
        return L"대기";
    case Maro_Phase::Analyzing:
        return L"분석 중";
    case Maro_Phase::Generating:
        return L"준비 중";
    case Maro_Phase::Compiling:
        return L"컴파일 중";
    case Maro_Phase::Running:
        return L"실행 중";
    case Maro_Phase::Completed:
        return L"완료";
    }
    return L"대기";
}

const wchar_t* Maro_StatusName(Maro_Status status) noexcept
{
    switch (status)
    {
    case Maro_Status::Pending:
        return L"진행 중";
    case Maro_Status::Success:
        return L"완료";
    case Maro_Status::Cancelled:
        return L"취소";
    case Maro_Status::Stale:
        return L"변경됨";
    case Maro_Status::CompileFailed:
        return L"컴파일 실패";
    case Maro_Status::RuntimeFailed:
        return L"실행 실패";
    case Maro_Status::TimedOut:
        return L"시간 초과";
    case Maro_Status::LimitExceeded:
        return L"제한 초과";
    case Maro_Status::ToolchainMissing:
        return L"컴파일러 없음";
    case Maro_Status::SandboxUnavailable:
        return L"실행 불가";
    case Maro_Status::InternalError:
        return L"내부 오류";
    }
    return L"오류";
}

const wchar_t* Maro_SeverityName(Maro_Severity severity) noexcept
{
    switch (severity)
    {
    case Maro_Severity::Info:
        return L"INFO";
    case Maro_Severity::Warning:
        return L"WARN";
    case Maro_Severity::Error:
        return L"ERROR";
    case Maro_Severity::Fatal:
        return L"FATAL";
    }
    return L"INFO";
}

const wchar_t* Maro_VisualStudioStatusName(Maro_VisualStudioStatus status) noexcept
{
    switch (status)
    {
    case Maro_VisualStudioStatus::Success:
        return L"연결됨";
    case Maro_VisualStudioStatus::NotRunning:
        return L"Visual Studio 없음";
    case Maro_VisualStudioStatus::NoActiveDocument:
        return L"활성 파일 없음";
    case Maro_VisualStudioStatus::UnsupportedDocument:
        return L"C/C++ 파일 아님";
    case Maro_VisualStudioStatus::Busy:
        return L"Visual Studio 사용 중";
    case Maro_VisualStudioStatus::AccessDenied:
        return L"접근 거부";
    case Maro_VisualStudioStatus::Disconnected:
        return L"연결 끊김";
    case Maro_VisualStudioStatus::ReadOnly:
        return L"읽기 전용";
    case Maro_VisualStudioStatus::ComUnavailable:
    case Maro_VisualStudioStatus::AutomationError:
        return L"연결 오류";
    case Maro_VisualStudioStatus::SourceVersionMismatch:
    case Maro_VisualStudioStatus::PathMismatch:
    case Maro_VisualStudioStatus::ContentMismatch:
        return L"소스 변경됨";
    case Maro_VisualStudioStatus::ReplaceFailed:
    case Maro_VisualStudioStatus::VerificationFailed:
        return L"수정 실패";
    }
    return L"연결 오류";
}

std::wstring Maro_VisualStudioGuidance(Maro_VisualStudioStatus status)
{
    switch (status)
    {
    case Maro_VisualStudioStatus::NotRunning:
        return L"Visual Studio를 실행하세요.";
    case Maro_VisualStudioStatus::NoActiveDocument:
        return L"C/C++ 파일을 활성화하세요.";
    case Maro_VisualStudioStatus::UnsupportedDocument:
        return L"C/C++ 파일만 지원합니다.";
    case Maro_VisualStudioStatus::Busy:
        return L"Visual Studio 확인 중...";
    case Maro_VisualStudioStatus::AccessDenied:
        return L"Visual Studio와 같은 권한으로 실행하세요.";
    default:
        return L"Visual Studio 연결을 확인하세요.";
    }
}

bool Maro_IsBusy(Maro_Phase phase, Maro_Status status) noexcept
{
    return status == Maro_Status::Pending && phase != Maro_Phase::Idle &&
           phase != Maro_Phase::Debouncing && phase != Maro_Phase::Completed;
}

bool Maro_SameSnapshot(
    const Maro_VisualStudioSnapshot& left,
    const Maro_VisualStudioSnapshot& right)
{
    return left.instanceMoniker == right.instanceMoniker &&
           left.processId == right.processId && left.path == right.path &&
           left.text == right.text && left.language == right.language;
}

bool Maro_SameDocument(
    const Maro_VisualStudioSnapshot& left,
    const Maro_VisualStudioSnapshot& right)
{
    return left.instanceMoniker == right.instanceMoniker &&
           left.processId == right.processId && left.path == right.path;
}

bool Maro_IsHeaderDocument(const Maro_VisualStudioSnapshot& document)
{
    std::wstring_view path = document.path.empty()
                                 ? std::wstring_view(document.name)
                                 : std::wstring_view(document.path);
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
    return extension == L".h" || extension == L".hh" ||
           extension == L".hpp" || extension == L".hxx" ||
           extension == L".inl" || extension == L".ipp" ||
           extension == L".tpp";
}

std::wstring Maro_TrimNewlines(std::wstring text)
{
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n'))
    {
        text.pop_back();
    }
    return text;
}

std::wstring Maro_FormatOutput(
    const Maro_ResultEnvelope& result,
    bool headerDocument)
{
    std::wostringstream stream;
    if (!result.standardOutput.empty())
    {
        stream << Maro_TrimNewlines(result.standardOutput);
    }
    if (!result.standardError.empty())
    {
        if (stream.tellp() > 0)
        {
            stream << L"\n\n";
        }
        stream << L"stderr:\n" << Maro_TrimNewlines(result.standardError);
    }
    if (result.phase == Maro_Phase::Completed)
    {
        if (stream.tellp() > 0)
        {
            stream << L'\n';
        }
        if (result.hasExitCode)
        {
            stream << L"[exit " << result.exitCode << L']';
        }
        else if (headerDocument)
        {
            stream << L"분석 완료.";
        }
        else if (result.status == Maro_Status::Success)
        {
            stream << L"출력 없음.";
        }
    }
    return stream.str();
}

std::wstring Maro_FormatAnalysis(const Maro_ResultEnvelope& result)
{
    std::wostringstream stream;
    if (result.diagnostics.empty())
    {
        if (result.phase != Maro_Phase::Completed)
        {
            stream << L"분석 중...";
        }
        else if (result.status == Maro_Status::Success)
        {
            stream << L"문제 없음.";
        }
        else if (!result.compilerOutput.empty())
        {
            stream << result.compilerOutput;
        }
        else if (!result.statusText.empty())
        {
            stream << result.statusText;
        }
        else
        {
            stream << Maro_StatusName(result.status);
        }
        return stream.str();
    }

    for (std::size_t index = 0; index < result.diagnostics.size(); ++index)
    {
        const Maro_Diagnostic& diagnostic = result.diagnostics[index];
        stream << L'[' << Maro_SeverityName(diagnostic.severity) << L']';
        if (!diagnostic.code.empty())
        {
            stream << L' ' << diagnostic.code;
        }
        if (diagnostic.range.start.line != 0)
        {
            stream << L' ' << diagnostic.range.start.line;
            if (diagnostic.range.start.column != 0)
            {
                stream << L':' << diagnostic.range.start.column;
            }
        }
        stream << L'\n';
        if (!diagnostic.friendlyMessage.empty())
        {
            stream << diagnostic.friendlyMessage;
        }
        else
        {
            stream << diagnostic.originalDiagnostic;
        }
        if (diagnostic.fix.has_value())
        {
            stream << L"\n[F] ";
            if (diagnostic.fix->description.empty())
            {
                stream << L"수정 가능";
            }
            else
            {
                stream << diagnostic.fix->description;
            }
        }
        if (index + 1U < result.diagnostics.size())
        {
            stream << L"\n\n";
        }
    }
    return stream.str();
}

class Maro_Frame
{
public:
    Maro_Frame(int width, int height)
        : width_(width),
          height_(height),
          cells_(static_cast<std::size_t>(width) *
                 static_cast<std::size_t>(height))
    {
        for (CHAR_INFO& cell : cells_)
        {
            cell.Char.UnicodeChar = L' ';
            cell.Attributes = Maro_ColorText;
        }
    }

    int Width() const noexcept { return width_; }
    int Height() const noexcept { return height_; }
    CHAR_INFO* Data() noexcept { return cells_.data(); }

    void Put(int x, int y, wchar_t character, WORD attributes)
    {
        if (x < 0 || y < 0 || x >= width_ || y >= height_)
        {
            return;
        }
        CHAR_INFO& cell = cells_[static_cast<std::size_t>(y) *
                                 static_cast<std::size_t>(width_) +
                                 static_cast<std::size_t>(x)];
        cell.Char.UnicodeChar = character;
        cell.Attributes = attributes;
    }

    void PutText(
        int x,
        int y,
        std::wstring_view text,
        int maxColumns,
        WORD attributes)
    {
        if (y < 0 || y >= height_ || maxColumns <= 0)
        {
            return;
        }
        const int right = (std::min)(width_, x + maxColumns);
        int column = x;
        for (std::size_t index = 0; index < text.size() && column < right; ++index)
        {
            wchar_t character = text[index];
            if (Maro_IsHighSurrogate(character))
            {
                if (index + 1U < text.size() && Maro_IsLowSurrogate(text[index + 1U]))
                {
                    ++index;
                }
                character = L'?';
            }
            else if (Maro_IsLowSurrogate(character) ||
                     Maro_IsUnsafeControl(character))
            {
                character = L'?';
            }

            if (Maro_IsWideCharacter(character))
            {
                if (column + 1 >= right)
                {
                    break;
                }
                Put(column, y, character,
                    static_cast<WORD>(attributes | COMMON_LVB_LEADING_BYTE));
                Put(column + 1, y, character,
                    static_cast<WORD>(attributes | COMMON_LVB_TRAILING_BYTE));
                column += 2;
            }
            else
            {
                Put(column, y, character, attributes);
                ++column;
            }
        }
    }

    void DrawOuterBox()
    {
        if (width_ < 2 || height_ < 2)
        {
            return;
        }
        for (int x = 1; x < width_ - 1; ++x)
        {
            Put(x, 0, L'─', Maro_ColorBorder);
            Put(x, height_ - 1, L'─', Maro_ColorBorder);
        }
        for (int y = 1; y < height_ - 1; ++y)
        {
            Put(0, y, L'│', Maro_ColorBorder);
            Put(width_ - 1, y, L'│', Maro_ColorBorder);
        }
        Put(0, 0, L'┌', Maro_ColorBorder);
        Put(width_ - 1, 0, L'┐', Maro_ColorBorder);
        Put(0, height_ - 1, L'└', Maro_ColorBorder);
        Put(width_ - 1, height_ - 1, L'┘', Maro_ColorBorder);
    }

    void DrawSeparator(int y, std::wstring_view label, WORD labelAttributes)
    {
        if (y <= 0 || y >= height_ - 1 || width_ < 2)
        {
            return;
        }
        for (int x = 1; x < width_ - 1; ++x)
        {
            Put(x, y, L'─', Maro_ColorBorder);
        }
        Put(0, y, L'├', Maro_ColorBorder);
        Put(width_ - 1, y, L'┤', Maro_ColorBorder);
        PutText(2, y, label, width_ - 4, labelAttributes);
    }

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<CHAR_INFO> cells_;
};

class Maro_ConsoleSession
{
public:
    ~Maro_ConsoleSession() { Stop(); }

    bool Start()
    {
        input_ = GetStdHandle(STD_INPUT_HANDLE);
        output_ = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!Maro_IsValidHandle(input_) || !Maro_IsValidHandle(output_) ||
            !GetConsoleMode(input_, &originalInputMode_) ||
            !GetConsoleMode(output_, &originalOutputMode_))
        {
            return false;
        }
        active_ = true;

        DWORD inputMode = originalInputMode_ | ENABLE_EXTENDED_FLAGS |
                          ENABLE_WINDOW_INPUT | ENABLE_PROCESSED_INPUT;
        inputMode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_MOUSE_INPUT |
                       ENABLE_QUICK_EDIT_MODE | ENABLE_INSERT_MODE |
                       ENABLE_VIRTUAL_TERMINAL_INPUT);
        if (!SetConsoleMode(input_, inputMode))
        {
            Stop();
            return false;
        }

        SetConsoleMode(output_, originalOutputMode_ | ENABLE_PROCESSED_OUTPUT |
                                    ENABLE_LVB_GRID_WORLDWIDE);

        if (GetConsoleCursorInfo(output_, &originalCursor_))
        {
            cursorSaved_ = true;
            CONSOLE_CURSOR_INFO hidden = originalCursor_;
            hidden.bVisible = FALSE;
            SetConsoleCursorInfo(output_, &hidden);
        }

        std::wstring title(1024, L'\0');
        const DWORD titleLength = GetConsoleTitleW(
            title.data(), static_cast<DWORD>(title.size()));
        if (titleLength < title.size())
        {
            title.resize(static_cast<std::size_t>(titleLength));
            originalTitle_ = std::move(title);
            titleSaved_ = true;
        }
        SetConsoleTitleW(L"CLive_Maro");
        DockLeft();
        return true;
    }

    void Stop()
    {
        if (!active_)
        {
            return;
        }
        if (placementSaved_ && window_ != nullptr && IsWindow(window_))
        {
            SetWindowPlacement(window_, &originalPlacement_);
        }
        if (titleSaved_)
        {
            SetConsoleTitleW(originalTitle_.c_str());
        }
        if (cursorSaved_)
        {
            SetConsoleCursorInfo(output_, &originalCursor_);
        }
        SetConsoleMode(output_, originalOutputMode_);
        SetConsoleMode(input_, originalInputMode_);
        active_ = false;
    }

    HANDLE Input() const noexcept { return input_; }
    HANDLE Output() const noexcept { return output_; }

    bool Render(Maro_Frame& frame)
    {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(output_, &info))
        {
            return false;
        }
        const int viewportWidth = static_cast<int>(info.srWindow.Right) -
                                  static_cast<int>(info.srWindow.Left) + 1;
        const int viewportHeight = static_cast<int>(info.srWindow.Bottom) -
                                   static_cast<int>(info.srWindow.Top) + 1;
        if (viewportWidth != frame.Width() || viewportHeight != frame.Height() ||
            viewportWidth <= 0 || viewportHeight <= 0 ||
            viewportWidth > SHRT_MAX || viewportHeight > SHRT_MAX)
        {
            return false;
        }
        COORD sourceSize{static_cast<SHORT>(viewportWidth),
                         static_cast<SHORT>(viewportHeight)};
        COORD sourceOrigin{0, 0};
        SMALL_RECT destination = info.srWindow;
        return WriteConsoleOutputW(
                   output_, frame.Data(), sourceSize, sourceOrigin, &destination) != FALSE;
    }

    bool ViewportSize(int& width, int& height) const
    {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(output_, &info))
        {
            return false;
        }
        width = static_cast<int>(info.srWindow.Right) -
                static_cast<int>(info.srWindow.Left) + 1;
        height = static_cast<int>(info.srWindow.Bottom) -
                 static_cast<int>(info.srWindow.Top) + 1;
        return width > 0 && height > 0 && width <= SHRT_MAX && height <= SHRT_MAX;
    }

private:
    void DockLeft()
    {
        window_ = GetConsoleWindow();
        if (window_ == nullptr || !IsWindowVisible(window_))
        {
            window_ = nullptr;
            return;
        }

        originalPlacement_.length = static_cast<UINT>(sizeof(originalPlacement_));
        placementSaved_ = GetWindowPlacement(window_, &originalPlacement_) != FALSE;

        ShowWindow(window_, SW_RESTORE);
        const HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = static_cast<DWORD>(sizeof(monitorInfo));
        if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo))
        {
            return;
        }

        const RECT work = monitorInfo.rcWork;
        const int workWidth = work.right - work.left;
        const int workHeight = work.bottom - work.top;
        if (workWidth <= 0 || workHeight <= 0)
        {
            return;
        }

        UINT dpi = GetDpiForWindow(window_);
        if (dpi == 0)
        {
            dpi = 96;
        }
        int cellWidth = MulDiv(8, static_cast<int>(dpi), 96);
        CONSOLE_FONT_INFOEX font{};
        font.cbSize = static_cast<ULONG>(sizeof(font));
        if (GetCurrentConsoleFontEx(output_, FALSE, &font) && font.dwFontSize.X > 0)
        {
            cellWidth = static_cast<int>(font.dwFontSize.X);
        }

        const int hardCap = (std::max)(1, workWidth * 45 / 100);
        const int lower = (std::min)(MulDiv(440, static_cast<int>(dpi), 96), hardCap);
        const int upper = (std::min)(MulDiv(640, static_cast<int>(dpi), 96), hardCap);
        const int desired = cellWidth * 72 + MulDiv(32, static_cast<int>(dpi), 96);
        const int targetWidth = (std::clamp)(desired, lower, (std::max)(lower, upper));

        SetWindowPos(window_, nullptr, work.left, work.top, targetWidth, workHeight,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                         SWP_FRAMECHANGED);
    }

    HANDLE input_ = INVALID_HANDLE_VALUE;
    HANDLE output_ = INVALID_HANDLE_VALUE;
    DWORD originalInputMode_ = 0;
    DWORD originalOutputMode_ = 0;
    CONSOLE_CURSOR_INFO originalCursor_{};
    bool cursorSaved_ = false;
    std::wstring originalTitle_;
    bool titleSaved_ = false;
    HWND window_ = nullptr;
    WINDOWPLACEMENT originalPlacement_{};
    bool placementSaved_ = false;
    bool active_ = false;
};

enum class Maro_Pane
{
    Output,
    Analysis
};

struct Maro_PendingFix
{
    Maro_VisualStudioSnapshot expectedSnapshot;
    std::uint64_t sourceVersion = 0;
    std::wstring replacementText;
    std::wstring preview;
};
} // namespace

struct Maro_App::Maro_Impl
{
    ~Maro_Impl()
    {
        closing.store(true, std::memory_order_release);
        if (engine != nullptr)
        {
            engine->Shutdown();
            engine.reset();
        }
    }

    int Run();
    void PollVisualStudio(bool submitImmediately);
    void InvalidateSnapshot(const Maro_VisualStudioReadResult& read);
    void SubmitCurrentSnapshot();
    void CancelCurrentRequest();
    void EnqueueResult(Maro_ResultEnvelope result);
    void DrainResults();
    void ApplyResult(const Maro_ResultEnvelope& result);
    void ReadConsoleEvents();
    void HandleKey(const KEY_EVENT_RECORD& key);
    void HandleNormalKey(const KEY_EVENT_RECORD& key);
    void HandleInputKey(const KEY_EVENT_RECORD& key);
    void HandleFixKey(const KEY_EVENT_RECORD& key);
    void BeginInput();
    void ToggleGeneratedSource();
    void BeginFix();
    void ApplyPendingFix();
    void ResetInteraction();
    void ScrollSelected(int amount);
    void ShowNotice(std::wstring text);
    void HandleTimers();
    DWORD WaitMilliseconds() const;
    void Render();
    std::wstring StatusLine() const;
    std::wstring AnalysisView() const;
    std::wstring BottomHint() const;
    void DrawPane(
        Maro_Frame& frame,
        int firstRow,
        int rowCount,
        std::wstring_view text,
        std::size_t& scroll,
        std::size_t& maximumScroll,
        int& pageRows);

    Maro_ConsoleSession console;
    HANDLE wakeEvent = nullptr;
    Maro_VisualStudio visualStudio;
    std::unique_ptr<Maro_Engine> engine;
    std::mutex resultMutex;
    std::deque<Maro_ResultEnvelope> pendingResults;
    std::atomic_bool closing{false};

    std::optional<Maro_VisualStudioSnapshot> snapshot;
    std::optional<Maro_ResultEnvelope> latestResult;
    std::optional<Maro_FixSuggestion> latestFix;
    std::optional<Maro_PendingFix> pendingFix;
    std::uint64_t sourceVersion = 0;
    std::uint64_t activeRequestId = 0;
    Maro_VisualStudioStatus visualStudioStatus =
        Maro_VisualStudioStatus::NotRunning;
    Maro_Phase currentPhase = Maro_Phase::Idle;
    Maro_Status currentStatus = Maro_Status::Pending;
    bool busy = false;

    std::wstring standardInput;
    std::wstring inputBuffer;
    bool editingInput = false;
    bool showingGenerated = false;
    std::wstring outputText;
    std::wstring analysisText = L"Visual Studio 연결을 확인하세요.";
    std::wstring notice;
    Maro_Clock::time_point noticeUntil{};

    Maro_Pane selectedPane = Maro_Pane::Output;
    std::size_t outputScroll = 0;
    std::size_t analysisScroll = 0;
    std::size_t outputMaximumScroll = 0;
    std::size_t analysisMaximumScroll = 0;
    int outputPageRows = 1;
    int analysisPageRows = 1;
    int renderedWidth = 0;
    int renderedHeight = 0;

    Maro_Clock::time_point nextPoll{};
    std::optional<Maro_Clock::time_point> submitDue;
    bool dirty = true;
    bool quit = false;
};

void Maro_App::Maro_Impl::ShowNotice(std::wstring text)
{
    notice = std::move(text);
    noticeUntil = Maro_Clock::now() + std::chrono::milliseconds(1800);
    dirty = true;
}

void Maro_App::Maro_Impl::ResetInteraction()
{
    editingInput = false;
    inputBuffer.clear();
    showingGenerated = false;
    pendingFix.reset();
}

void Maro_App::Maro_Impl::InvalidateSnapshot(
    const Maro_VisualStudioReadResult& read)
{
    const std::wstring guidance = Maro_VisualStudioGuidance(read.status);
    if (!snapshot.has_value() && visualStudioStatus == read.status &&
        analysisText == guidance)
    {
        return;
    }
    visualStudioStatus = read.status;
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
    }
    snapshot.reset();
    latestResult.reset();
    latestFix.reset();
    activeRequestId = 0;
    currentPhase = Maro_Phase::Idle;
    currentStatus = Maro_Status::Pending;
    busy = false;
    submitDue.reset();
    ResetInteraction();
    outputText.clear();
    analysisText = guidance;
    outputScroll = 0;
    analysisScroll = 0;
    dirty = true;
}

void Maro_App::Maro_Impl::PollVisualStudio(bool submitImmediately)
{
    if (closing.load(std::memory_order_acquire))
    {
        return;
    }

    const std::uint64_t candidateVersion =
        sourceVersion == (std::numeric_limits<std::uint64_t>::max)()
            ? sourceVersion
            : sourceVersion + 1U;
    const std::wstring preferredInstance =
        snapshot.has_value() ? snapshot->instanceMoniker : std::wstring{};
    Maro_VisualStudioReadResult read = visualStudio.ReadActiveDocument(
        candidateVersion, preferredInstance);
    if (!read || !read.snapshot.has_value())
    {
        if (read.status == Maro_VisualStudioStatus::Busy && snapshot.has_value())
        {
            const bool changedStatus =
                visualStudioStatus != Maro_VisualStudioStatus::Busy;
            visualStudioStatus = read.status;
            dirty = dirty || changedStatus;
            return;
        }
        InvalidateSnapshot(read);
        return;
    }

    Maro_VisualStudioSnapshot next = std::move(*read.snapshot);
    const bool changed = !snapshot.has_value() || !Maro_SameSnapshot(*snapshot, next);
    const bool changedDocument =
        !snapshot.has_value() || !Maro_SameDocument(*snapshot, next);
    const bool changedDisplayState =
        visualStudioStatus != Maro_VisualStudioStatus::Success ||
        (snapshot.has_value() &&
         (snapshot->saved != next.saved || snapshot->readOnly != next.readOnly));
    visualStudioStatus = Maro_VisualStudioStatus::Success;

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
        currentPhase = Maro_Phase::Debouncing;
        currentStatus = Maro_Status::Pending;
        busy = false;
        if (changedDocument)
        {
            standardInput.clear();
        }
        ResetInteraction();
        outputText.clear();
        analysisText = L"분석 대기...";
        outputScroll = 0;
        analysisScroll = 0;
        if (submitImmediately)
        {
            SubmitCurrentSnapshot();
        }
        else
        {
            submitDue = Maro_Clock::now() +
                        std::chrono::milliseconds(Maro_DebounceMilliseconds);
        }
        dirty = true;
        return;
    }

    next.sourceVersion = sourceVersion;
    snapshot = std::move(next);
    dirty = dirty || changedDisplayState;
    if (submitImmediately)
    {
        SubmitCurrentSnapshot();
    }
}

void Maro_App::Maro_Impl::SubmitCurrentSnapshot()
{
    submitDue.reset();
    if (!snapshot.has_value() || engine == nullptr ||
        closing.load(std::memory_order_acquire))
    {
        return;
    }

    Maro_SourceRequest request{};
    request.sourceVersion = sourceVersion;
    request.sourceText = snapshot->text;
    request.sourcePath = snapshot->path;
    request.standardInput = standardInput;
    request.language = snapshot->language;
    request.mode = Maro_SourceMode::Program;
    request.execute = !Maro_IsHeaderDocument(*snapshot);

    const std::uint64_t requestId = engine->Submit(std::move(request));
    activeRequestId = requestId;
    if (requestId == 0)
    {
        currentPhase = Maro_Phase::Completed;
        currentStatus = Maro_Status::InternalError;
        busy = false;
        ShowNotice(L"분석 시작 실패");
        return;
    }

    currentPhase = Maro_Phase::Analyzing;
    currentStatus = Maro_Status::Pending;
    busy = true;
    latestResult.reset();
    latestFix.reset();
    pendingFix.reset();
    showingGenerated = false;
    outputText.clear();
    analysisText = L"분석 중...";
    outputScroll = 0;
    analysisScroll = 0;
    dirty = true;
}

void Maro_App::Maro_Impl::CancelCurrentRequest()
{
    if (engine != nullptr)
    {
        engine->Cancel();
    }
    activeRequestId = 0;
    submitDue.reset();
    busy = false;
    currentPhase = Maro_Phase::Completed;
    currentStatus = Maro_Status::Cancelled;
    ShowNotice(L"취소");
}

void Maro_App::Maro_Impl::EnqueueResult(Maro_ResultEnvelope result)
{
    if (closing.load(std::memory_order_acquire))
    {
        return;
    }
    {
        std::lock_guard lock(resultMutex);
        pendingResults.push_back(std::move(result));
    }
    if (wakeEvent != nullptr)
    {
        SetEvent(wakeEvent);
    }
}

void Maro_App::Maro_Impl::DrainResults()
{
    std::deque<Maro_ResultEnvelope> results;
    {
        std::lock_guard lock(resultMutex);
        results.swap(pendingResults);
    }
    for (const Maro_ResultEnvelope& result : results)
    {
        if (!snapshot.has_value() || result.requestId != activeRequestId ||
            result.sourceVersion != sourceVersion)
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
    for (const Maro_Diagnostic& diagnostic : result.diagnostics)
    {
        if (diagnostic.sourceVersion == sourceVersion &&
            diagnostic.fix.has_value() && !diagnostic.fix->edits.empty())
        {
            latestFix = diagnostic.fix;
            break;
        }
    }
    const bool headerDocument = snapshot.has_value() &&
                                Maro_IsHeaderDocument(*snapshot);
    outputText = Maro_FormatOutput(result, headerDocument);
    analysisText = Maro_FormatAnalysis(result);
    outputScroll = 0;
    analysisScroll = 0;
    dirty = true;
}

void Maro_App::Maro_Impl::BeginInput()
{
    if (!snapshot.has_value() || Maro_IsHeaderDocument(*snapshot))
    {
        ShowNotice(L"입력 사용 불가");
        return;
    }
    pendingFix.reset();
    showingGenerated = false;
    editingInput = true;
    inputBuffer = Maro_TrimNewlines(standardInput);
    for (wchar_t& character : inputBuffer)
    {
        if (character == L'\r' || character == L'\n' ||
            Maro_IsUnsafeControl(character))
        {
            character = L' ';
        }
    }
    dirty = true;
}

void Maro_App::Maro_Impl::ToggleGeneratedSource()
{
    if (!latestResult.has_value() ||
        latestResult->sourceVersion != sourceVersion ||
        latestResult->generatedSource.empty())
    {
        ShowNotice(L"생성된 소스 없음");
        return;
    }
    editingInput = false;
    pendingFix.reset();
    showingGenerated = !showingGenerated;
    analysisScroll = 0;
    dirty = true;
}

void Maro_App::Maro_Impl::BeginFix()
{
    if (!snapshot.has_value() || snapshot->readOnly || !latestFix.has_value() ||
        latestFix->edits.empty())
    {
        ShowNotice(L"적용할 수정 없음");
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
        ShowNotice(L"소스가 변경됨");
        PollVisualStudio(false);
        return;
    }

    std::vector<Maro_TextEdit> edits = fix.edits;
    std::sort(edits.begin(), edits.end(),
              [](const Maro_TextEdit& left, const Maro_TextEdit& right) {
                  return left.startOffsetUtf16 > right.startOffsetUtf16;
              });

    std::wstring replacement = expectedSnapshot.text;
    std::size_t nextStart = replacement.size();
    for (const Maro_TextEdit& edit : edits)
    {
        if (edit.sourceVersion != expectedVersion ||
            edit.startOffsetUtf16 > replacement.size() ||
            edit.lengthUtf16 > replacement.size() - edit.startOffsetUtf16 ||
            edit.startOffsetUtf16 + edit.lengthUtf16 > nextStart ||
            (!edit.expectedText.empty() &&
             replacement.substr(edit.startOffsetUtf16, edit.lengthUtf16) !=
                 edit.expectedText))
        {
            ShowNotice(L"수정 검증 실패");
            PollVisualStudio(false);
            return;
        }
        nextStart = edit.startOffsetUtf16;
    }

    std::wostringstream preview;
    preview << (fix.description.empty() ? L"수정 제안" : fix.description);
    std::size_t shown = 0;
    for (const Maro_TextEdit& edit : edits)
    {
        if (shown >= 4U)
        {
            preview << L"\n\n...";
            break;
        }
        ++shown;
        preview << L"\n\n- "
                << (edit.expectedText.empty() ? L"(빈 값)" : edit.expectedText)
                << L"\n+ "
                << (edit.replacement.empty() ? L"(빈 값)" : edit.replacement);
    }
    for (const Maro_TextEdit& edit : edits)
    {
        replacement.replace(edit.startOffsetUtf16, edit.lengthUtf16,
                            edit.replacement);
    }

    Maro_PendingFix pending{};
    pending.expectedSnapshot = expectedSnapshot;
    pending.sourceVersion = expectedVersion;
    pending.replacementText = std::move(replacement);
    pending.preview = preview.str();
    pendingFix = std::move(pending);
    editingInput = false;
    showingGenerated = false;
    analysisScroll = 0;
    dirty = true;
}

void Maro_App::Maro_Impl::ApplyPendingFix()
{
    if (!pendingFix.has_value())
    {
        return;
    }
    const Maro_PendingFix pending = *pendingFix;
    pendingFix.reset();
    if (!snapshot.has_value() || sourceVersion != pending.sourceVersion ||
        !Maro_SameSnapshot(*snapshot, pending.expectedSnapshot))
    {
        ShowNotice(L"소스가 변경됨");
        return;
    }

    const Maro_VisualStudioApplyResult applied = visualStudio.ApplyFullText(
        pending.expectedSnapshot, pending.sourceVersion, pending.replacementText);
    if (!applied)
    {
        ShowNotice(applied.message.empty()
                       ? std::wstring(Maro_VisualStudioStatusName(applied.status))
                       : applied.message);
        PollVisualStudio(false);
        return;
    }

    latestFix.reset();
    PollVisualStudio(false);
    ShowNotice(L"수정 완료");
}

void Maro_App::Maro_Impl::HandleInputKey(const KEY_EVENT_RECORD& key)
{
    if (key.wVirtualKeyCode == VK_ESCAPE)
    {
        editingInput = false;
        inputBuffer.clear();
        dirty = true;
        return;
    }
    if (key.wVirtualKeyCode == VK_RETURN)
    {
        standardInput = inputBuffer;
        if (!standardInput.empty())
        {
            standardInput.push_back(L'\n');
        }
        editingInput = false;
        inputBuffer.clear();
        PollVisualStudio(true);
        return;
    }

    const WORD repeats = (std::max)(static_cast<WORD>(1), key.wRepeatCount);
    if (key.wVirtualKeyCode == VK_BACK)
    {
        for (WORD count = 0; count < repeats && !inputBuffer.empty(); ++count)
        {
            inputBuffer.pop_back();
        }
        dirty = true;
        return;
    }

    const wchar_t character = key.uChar.UnicodeChar;
    if (character < L' ' || Maro_IsUnsafeControl(character) ||
        Maro_IsHighSurrogate(character) || Maro_IsLowSurrogate(character))
    {
        return;
    }
    for (WORD count = 0;
         count < repeats && inputBuffer.size() < Maro_MaxInputCharacters;
         ++count)
    {
        inputBuffer.push_back(character);
    }
    dirty = true;
}

void Maro_App::Maro_Impl::HandleFixKey(const KEY_EVENT_RECORD& key)
{
    if (key.wVirtualKeyCode == 'Y')
    {
        ApplyPendingFix();
        return;
    }
    if (key.wVirtualKeyCode == 'N' || key.wVirtualKeyCode == VK_ESCAPE ||
        key.wVirtualKeyCode == VK_RETURN)
    {
        pendingFix.reset();
        dirty = true;
    }
}

void Maro_App::Maro_Impl::ScrollSelected(int amount)
{
    std::size_t& scroll = selectedPane == Maro_Pane::Output
                              ? outputScroll
                              : analysisScroll;
    const std::size_t maximum = selectedPane == Maro_Pane::Output
                                    ? outputMaximumScroll
                                    : analysisMaximumScroll;
    if (amount < 0)
    {
        const std::size_t decrease = static_cast<std::size_t>(-amount);
        scroll = decrease > scroll ? 0U : scroll - decrease;
    }
    else
    {
        const std::size_t increase = static_cast<std::size_t>(amount);
        scroll = (std::min)(maximum, scroll + (std::min)(increase, maximum));
    }
    dirty = true;
}

void Maro_App::Maro_Impl::HandleNormalKey(const KEY_EVENT_RECORD& key)
{
    switch (key.wVirtualKeyCode)
    {
    case 'Q':
        quit = true;
        return;
    case 'R':
    case VK_F5:
        PollVisualStudio(true);
        return;
    case 'C':
    case VK_ESCAPE:
        CancelCurrentRequest();
        return;
    case 'I':
        BeginInput();
        return;
    case 'G':
        ToggleGeneratedSource();
        return;
    case 'F':
        BeginFix();
        return;
    case VK_TAB:
        selectedPane = selectedPane == Maro_Pane::Output
                           ? Maro_Pane::Analysis
                           : Maro_Pane::Output;
        dirty = true;
        return;
    case VK_UP:
        ScrollSelected(-1);
        return;
    case VK_DOWN:
        ScrollSelected(1);
        return;
    case VK_PRIOR:
        ScrollSelected(-(selectedPane == Maro_Pane::Output
                             ? outputPageRows
                             : analysisPageRows));
        return;
    case VK_NEXT:
        ScrollSelected(selectedPane == Maro_Pane::Output
                           ? outputPageRows
                           : analysisPageRows);
        return;
    case VK_HOME:
        if (selectedPane == Maro_Pane::Output)
        {
            outputScroll = 0;
        }
        else
        {
            analysisScroll = 0;
        }
        dirty = true;
        return;
    case VK_END:
        if (selectedPane == Maro_Pane::Output)
        {
            outputScroll = outputMaximumScroll;
        }
        else
        {
            analysisScroll = analysisMaximumScroll;
        }
        dirty = true;
        return;
    default:
        return;
    }
}

void Maro_App::Maro_Impl::HandleKey(const KEY_EVENT_RECORD& key)
{
    if (!key.bKeyDown)
    {
        return;
    }
    if (editingInput)
    {
        HandleInputKey(key);
    }
    else if (pendingFix.has_value())
    {
        if (key.wVirtualKeyCode == 'Q')
        {
            quit = true;
        }
        else
        {
            HandleFixKey(key);
        }
    }
    else
    {
        HandleNormalKey(key);
    }
}

void Maro_App::Maro_Impl::ReadConsoleEvents()
{
    INPUT_RECORD records[64]{};
    for (;;)
    {
        DWORD available = 0;
        if (!GetNumberOfConsoleInputEvents(console.Input(), &available) ||
            available == 0)
        {
            return;
        }
        DWORD read = 0;
        const DWORD requested = (std::min)(
            available, static_cast<DWORD>(std::size(records)));
        if (!ReadConsoleInputW(console.Input(), records, requested, &read))
        {
            quit = true;
            return;
        }
        for (DWORD index = 0; index < read; ++index)
        {
            if (records[index].EventType == KEY_EVENT)
            {
                HandleKey(records[index].Event.KeyEvent);
            }
            else if (records[index].EventType == WINDOW_BUFFER_SIZE_EVENT)
            {
                dirty = true;
            }
        }
    }
}

std::wstring Maro_App::Maro_Impl::StatusLine() const
{
    std::wstring result = L"VS: ";
    if (snapshot.has_value())
    {
        result.append(Maro_FileName(snapshot->path, snapshot->name));
        if (!snapshot->saved)
        {
            result.push_back(L'*');
        }
        if (snapshot->readOnly)
        {
            result.append(L" [RO]");
        }
        result.append(L" | ");
        result.append(Maro_LanguageName(snapshot->language));
    }
    else
    {
        result.append(Maro_VisualStudioStatusName(visualStudioStatus));
        result.append(L" | -");
    }
    result.append(L" | ");
    if (!notice.empty())
    {
        result.append(notice);
    }
    else if (visualStudioStatus == Maro_VisualStudioStatus::Busy && !busy)
    {
        result.append(Maro_VisualStudioStatusName(visualStudioStatus));
    }
    else if (currentStatus != Maro_Status::Pending)
    {
        result.append(Maro_StatusName(currentStatus));
    }
    else
    {
        result.append(Maro_PhaseName(currentPhase));
    }
    return result;
}

std::wstring Maro_App::Maro_Impl::AnalysisView() const
{
    if (editingInput)
    {
        return L"입력: " + inputBuffer + L"\n\nEnter 저장 | Esc 취소";
    }
    if (pendingFix.has_value())
    {
        return pendingFix->preview + L"\n\nY 적용 | Enter/Esc 취소";
    }
    if (showingGenerated && latestResult.has_value())
    {
        return latestResult->generatedSource;
    }
    return analysisText;
}

std::wstring Maro_App::Maro_Impl::BottomHint() const
{
    if (editingInput)
    {
        return L" 입력 | Enter 저장  Esc 취소 ";
    }
    if (pendingFix.has_value())
    {
        return L" 수정 | Y 적용  Enter/Esc 취소 ";
    }
    return L" R 실행  C 취소  I 입력  G 소스  F 수정  Q 종료 ";
}

void Maro_App::Maro_Impl::DrawPane(
    Maro_Frame& frame,
    int firstRow,
    int rowCount,
    std::wstring_view text,
    std::size_t& scroll,
    std::size_t& maximumScroll,
    int& pageRows)
{
    pageRows = (std::max)(1, rowCount);
    const int contentColumns = (std::max)(1, frame.Width() - 4);
    const std::vector<std::wstring> lines = Maro_WrapForConsole(
        text, contentColumns);
    const std::size_t visible = static_cast<std::size_t>((std::max)(0, rowCount));
    maximumScroll = lines.size() > visible ? lines.size() - visible : 0U;
    scroll = (std::min)(scroll, maximumScroll);
    for (int row = 0; row < rowCount; ++row)
    {
        const std::size_t lineIndex = scroll + static_cast<std::size_t>(row);
        if (lineIndex >= lines.size())
        {
            break;
        }
        frame.PutText(2, firstRow + row, lines[lineIndex], contentColumns,
                      Maro_ColorText);
    }
}

void Maro_App::Maro_Impl::Render()
{
    int width = 0;
    int height = 0;
    if (!console.ViewportSize(width, height))
    {
        quit = true;
        return;
    }

    Maro_Frame frame(width, height);
    if (width < 20 || height < 7)
    {
        frame.PutText(0, 0, L"CLive_Maro", width, Maro_ColorBright);
        if (height > 1)
        {
            frame.PutText(0, 1, L"창을 키워 주세요.", width, Maro_ColorText);
        }
        const bool rendered = console.Render(frame);
        if (rendered)
        {
            renderedWidth = width;
            renderedHeight = height;
        }
        dirty = !rendered;
        return;
    }

    frame.DrawOuterBox();
    frame.PutText(2, 0, L" CLive_Maro ", width - 4, Maro_ColorBright);
    frame.PutText(2, 1, StatusLine(), width - 4, Maro_ColorText);

    const int bodyRows = height - 5;
    const int outputRows = (std::max)(1, bodyRows * 45 / 100);
    const int analysisRows = bodyRows - outputRows;
    const int outputFirstRow = 3;
    const int analysisSeparatorRow = outputFirstRow + outputRows;
    const int analysisFirstRow = analysisSeparatorRow + 1;

    frame.DrawSeparator(
        2, L" OUTPUT ", selectedPane == Maro_Pane::Output
                               ? Maro_ColorSelected
                               : Maro_ColorBorder);
    frame.DrawSeparator(
        analysisSeparatorRow, L" ANALYSIS ",
        selectedPane == Maro_Pane::Analysis ? Maro_ColorSelected
                                            : Maro_ColorBorder);
    DrawPane(frame, outputFirstRow, outputRows, outputText, outputScroll,
             outputMaximumScroll, outputPageRows);
    DrawPane(frame, analysisFirstRow, analysisRows, AnalysisView(), analysisScroll,
             analysisMaximumScroll, analysisPageRows);
    frame.PutText(2, height - 1, BottomHint(), width - 4, Maro_ColorBorder);
    const bool rendered = console.Render(frame);
    if (rendered)
    {
        renderedWidth = width;
        renderedHeight = height;
    }
    dirty = !rendered;
}

void Maro_App::Maro_Impl::HandleTimers()
{
    const Maro_Clock::time_point now = Maro_Clock::now();
    int viewportWidth = 0;
    int viewportHeight = 0;
    if (console.ViewportSize(viewportWidth, viewportHeight) &&
        (viewportWidth != renderedWidth || viewportHeight != renderedHeight))
    {
        dirty = true;
    }
    if (now >= nextPoll)
    {
        PollVisualStudio(false);
        nextPoll = Maro_Clock::now() +
                   std::chrono::milliseconds(Maro_PollMilliseconds);
    }
    if (submitDue.has_value() && now >= *submitDue)
    {
        SubmitCurrentSnapshot();
    }
    if (!notice.empty() && now >= noticeUntil)
    {
        notice.clear();
        dirty = true;
    }
}

DWORD Maro_App::Maro_Impl::WaitMilliseconds() const
{
    const Maro_Clock::time_point now = Maro_Clock::now();
    Maro_Clock::time_point next = nextPoll;
    if (submitDue.has_value() && *submitDue < next)
    {
        next = *submitDue;
    }
    if (!notice.empty() && noticeUntil < next)
    {
        next = noticeUntil;
    }
    if (next <= now)
    {
        return 0;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count();
    return static_cast<DWORD>((std::clamp)(remaining, 1LL, 250LL));
}

int Maro_App::Maro_Impl::Run()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!console.Start())
    {
        constexpr char message[] =
            "CLive_Maro requires an interactive Windows console.\r\n";
        if (const HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
            Maro_IsValidHandle(error))
        {
            DWORD written = 0;
            WriteFile(error, message, static_cast<DWORD>(sizeof(message) - 1U),
                      &written, nullptr);
        }
        return 1;
    }

    wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (wakeEvent == nullptr)
    {
        return 1;
    }
    Maro_StopRequested.store(false, std::memory_order_release);
    Maro_WakeHandle.store(wakeEvent, std::memory_order_release);
    const bool controlHandlerInstalled =
        SetConsoleCtrlHandler(Maro_ConsoleControlHandler, TRUE) != FALSE;

    engine = std::make_unique<Maro_Engine>(
        [this](Maro_ResultEnvelope result) { EnqueueResult(std::move(result)); });
    nextPoll = Maro_Clock::now();

    HANDLE waitHandles[2]{console.Input(), wakeEvent};
    while (!quit && !Maro_StopRequested.load(std::memory_order_acquire))
    {
        DrainResults();
        HandleTimers();
        if (dirty)
        {
            Render();
        }
        if (quit || Maro_StopRequested.load(std::memory_order_acquire))
        {
            break;
        }

        const DWORD waitResult = WaitForMultipleObjects(
            static_cast<DWORD>(std::size(waitHandles)), waitHandles, FALSE,
            WaitMilliseconds());
        if (waitResult == WAIT_OBJECT_0)
        {
            ReadConsoleEvents();
        }
        else if (waitResult == WAIT_FAILED)
        {
            quit = true;
        }
    }

    closing.store(true, std::memory_order_release);
    if (engine != nullptr)
    {
        engine->Cancel();
        engine->Shutdown();
        engine.reset();
    }
    if (controlHandlerInstalled)
    {
        SetConsoleCtrlHandler(Maro_ConsoleControlHandler, FALSE);
    }
    Maro_WakeHandle.store(nullptr, std::memory_order_release);
    CloseHandle(wakeEvent);
    wakeEvent = nullptr;
    console.Stop();
    return 0;
}

Maro_App::Maro_App()
    : impl_(std::make_unique<Maro_Impl>())
{
}

Maro_App::~Maro_App() = default;

int Maro_App::Run()
{
    return impl_->Run();
}
