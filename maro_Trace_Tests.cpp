#include "maro_Trace.hpp"
#include "maro_Analyzer.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string_view>
#include <thread>

namespace
{
using maro_TraceTestCheck = std::function<void(bool, std::string_view)>;

struct maro_TraceTestDirectory
{
    std::filesystem::path maro_path;
    maro_TraceTestDirectory()
    {
        maro_path = std::filesystem::temp_directory_path() /
            (L"maro_추적_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
        if (!std::filesystem::create_directory(maro_path)) maro_path.clear();
    }
    ~maro_TraceTestDirectory()
    {
        if (!maro_path.empty())
        {
            std::error_code maro_error;
            std::filesystem::remove_all(maro_path, maro_error);
        }
    }
};

maro_TraceResult maro_TestDebugProcess(const std::filesystem::path& maro_executable,
    const std::filesystem::path& maro_source, const std::shared_ptr<maro_TraceSession>& maro_session,
    const std::function<bool()>& maro_cancelled = {})
{
    STARTUPINFOW maro_startup{};
    maro_startup.cb = sizeof(maro_startup);
    PROCESS_INFORMATION maro_process{};
    if (!CreateProcessW(maro_executable.c_str(), nullptr, nullptr, nullptr, FALSE,
            DEBUG_ONLY_THIS_PROCESS | CREATE_NO_WINDOW, nullptr, nullptr, &maro_startup, &maro_process))
        return {false, 0, GetLastError(), false};
    const ULONGLONG maro_started = GetTickCount64();
    auto maro_result = maro_RunDebugLoop(maro_process.hProcess, maro_process.dwProcessId,
        maro_executable.wstring(), maro_source.wstring(), maro_session, [&] {
            return GetTickCount64() - maro_started > 15'000 || (maro_cancelled && maro_cancelled());
        });
    CloseHandle(maro_process.hThread);
    CloseHandle(maro_process.hProcess);
    return maro_result;
}

bool maro_HasObserved(const std::vector<maro_TraceSnapshot>& maro_snapshots,
    std::wstring_view maro_name, std::wstring_view maro_value)
{
    for (const auto& maro_snapshot : maro_snapshots)
        for (const auto& maro_variable : maro_snapshot.maro_variables)
            if (maro_variable.maro_name == maro_name && maro_variable.maro_value == maro_value &&
                maro_variable.maro_available) return true;
    return false;
}

bool maro_HasArray(const std::vector<maro_TraceSnapshot>& maro_snapshots,
    std::wstring_view maro_name, const std::vector<std::size_t>& maro_dimensions,
    const std::vector<std::wstring>& maro_values)
{
    return std::any_of(maro_snapshots.begin(), maro_snapshots.end(), [&](const auto& maro_snapshot) {
        return std::any_of(maro_snapshot.maro_variables.begin(), maro_snapshot.maro_variables.end(), [&](const auto& maro_variable) {
            if (maro_variable.maro_name != maro_name || maro_variable.maro_dimensions != maro_dimensions ||
                maro_variable.maro_children.size() != maro_values.size()) return false;
            for (std::size_t maro_index = 0; maro_index < maro_values.size(); ++maro_index)
                if (!maro_variable.maro_children[maro_index].maro_available ||
                    maro_variable.maro_children[maro_index].maro_value != maro_values[maro_index]) return false;
            return true;
        });
    });
}
}

void maro_TestTrace(const maro_TraceTestCheck& maro_expect)
{
    {
        maro_TraceSession maro_session;
        maro_expect(!maro_session.maro_IsPaused() && !maro_session.maro_IsCancelled(),
            "trace session starts idle and uncancelled");
        maro_session.maro_Cancel();
        maro_expect(maro_session.maro_IsCancelled(), "trace cancel is durable before launch");
    }
    const auto maro_toolchain = Maro_DetectToolchain();
    if (maro_toolchain.kind != Maro_ToolchainKind::Msvc)
    {
        maro_expect(false, "native trace regression requires an installed MSVC x64 compiler");
        return;
    }
    maro_TraceTestDirectory maro_directory;
    maro_expect(!maro_directory.maro_path.empty(), "native trace creates its owned Unicode-path fixture directory");
    if (maro_directory.maro_path.empty()) return;
    const auto maro_source = maro_directory.maro_path / L"maro_TraceFixture.c";
    const auto maro_executable = maro_directory.maro_path / L"maro_TraceFixture.exe";
    {
        std::ofstream maro_stream(maro_source, std::ios::binary);
        maro_stream <<
            "struct maro_pair { int maro_a; int maro_b; };\n"
            "int maro_add(int maro_input)\n"
            "{\n"
            "    int maro_sum = maro_input + 1;\n"
            "    return maro_sum;\n"
            "}\n"
            "int maro_recursive(int maro_depth)\n"
            "{\n"
            "    if (maro_depth == 0) return 0;\n"
            "    return maro_recursive(maro_depth - 1) + 1;\n"
            "}\n"
            "int main(void)\n"
            "{\n"
            "    int maro_value = 3;\n"
            "    struct maro_pair maro_object = {1, 2};\n"
            "    maro_value = maro_add(maro_value);\n"
            "    for (int maro_i = 0; maro_i < 3; ++maro_i)\n"
            "    {\n"
            "        maro_value += maro_i;\n"
            "    }\n"
            "    int maro_depth = maro_recursive(3);\n"
            "    int maro_grid[2][3] = {{1, 2, 3}, {4, 5, 6}};\n"
            "    maro_grid[1][2] = 42;\n"
            "    int maro_sorted[4] = {4, 1, 3, 2};\n"
            "    for (int maro_i = 0; maro_i < 3; ++maro_i)\n"
            "    {\n"
            "        for (int maro_j = 0; maro_j < 3 - maro_i; ++maro_j)\n"
            "        {\n"
            "            if (maro_sorted[maro_j] > maro_sorted[maro_j + 1])\n"
            "            {\n"
            "                int maro_swap = maro_sorted[maro_j];\n"
            "                maro_sorted[maro_j] = maro_sorted[maro_j + 1];\n"
            "                maro_sorted[maro_j + 1] = maro_swap;\n"
            "            }\n"
            "        }\n"
            "    }\n"
            "    int maro_cube[2][2][2] = {{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}};\n"
            "    int maro_large[128] = {0};\n"
            "    int maro_deep[2][2][2][2] = {0};\n"
            "    int *maro_unreadable = (int*)1;\n"
            "    int *maro_pointers[2] = {(int*)1, (int*)2};\n"
            "    return maro_value == 7 && maro_depth == 3 && maro_object.maro_a == 1 &&\n"
            "        maro_grid[1][2] == 42 && maro_sorted[0] == 1 && maro_sorted[3] == 4 &&\n"
            "        maro_cube[1][1][1] == 8 && maro_large[127] == 0 && maro_deep[1][1][1][1] == 0 &&\n"
            "        maro_unreadable == (int*)1 && maro_pointers[1] == (int*)2 ? 0 : 1;\n"
            "}\n";
    }
    Maro_ProcessRequest maro_compile;
    maro_compile.executable = maro_toolchain.compilerPath.wstring();
    maro_compile.workingDirectory = maro_directory.maro_path.wstring();
    maro_compile.environmentOverrides = maro_toolchain.environment;
    maro_compile.arguments = {L"/nologo", L"/TC", L"/std:c17", L"/utf-8", L"/Od", L"/Z7", L"/MT",
        maro_source.wstring(), L"/Fe:" + maro_executable.wstring(),
        L"/Fo:" + (maro_directory.maro_path / L"maro_TraceFixture.obj").wstring(),
        L"/link", L"/DEBUG", L"/PDB:" + (maro_directory.maro_path / L"maro_TraceFixture.pdb").wstring()};
    maro_compile.limits.wallMilliseconds = 20'000;
    maro_compile.limits.cpuMilliseconds = 0;
    maro_compile.limits.memoryBytes = 1ull << 30;
    maro_compile.limits.activeProcessLimit = 8;
    const auto maro_compiled = Maro_RunProcess(maro_compile);
    maro_expect(maro_compiled.hasExitCode && maro_compiled.exitCode == 0,
        "native trace fixture compiles with embedded MSVC line and scalar symbols");
    if (!maro_compiled.hasExitCode || maro_compiled.exitCode != 0) return;

    {
        std::vector<maro_TraceSnapshot> maro_snapshots;
        std::shared_ptr<maro_TraceSession> maro_session;
        maro_session = std::make_shared<maro_TraceSession>([&](const maro_TraceSnapshot& maro_snapshot) {
            maro_snapshots.push_back(maro_snapshot);
            if (maro_snapshots.size() > 256) maro_session->maro_Cancel();
            else maro_session->maro_Step();
        });
        const auto maro_result = maro_TestDebugProcess(maro_executable, maro_source, maro_session);
        maro_expect(maro_result.maro_exited && maro_result.maro_exitCode == 0 && !maro_result.maro_error,
            "real native step execution exits normally without printf instrumentation");
        maro_expect(maro_snapshots.size() >= 15 && maro_snapshots.size() < 256,
            "trace stops repeatedly on real user source line breakpoints");
        maro_expect(maro_HasObserved(maro_snapshots, L"maro_value", L"3") &&
            maro_HasObserved(maro_snapshots, L"maro_sum", L"4") &&
            maro_HasObserved(maro_snapshots, L"maro_value", L"7"),
            "trace reads actual scalar memory changes through calls and loops");
        maro_expect(maro_HasObserved(maro_snapshots, L"maro_i", L"0") &&
            maro_HasObserved(maro_snapshots, L"maro_i", L"1") &&
            maro_HasObserved(maro_snapshots, L"maro_i", L"2"),
            "line breakpoints rearm correctly on each loop iteration");
        maro_expect(std::all_of(maro_snapshots.begin(), maro_snapshots.end(), [&](const auto& maro_snapshot) {
            return maro_snapshot.maro_file == maro_source.wstring() && maro_snapshot.maro_line > 0 &&
                maro_snapshot.maro_line <= 47 && maro_snapshot.maro_waiting &&
                maro_snapshot.maro_variables.size() <= 128 && maro_snapshot.maro_frames.size() <= 64;
        }), "trace snapshots have bounded real-source locations and state");
        maro_expect(std::all_of(maro_snapshots.begin(), maro_snapshots.end(), [](const auto& maro_snapshot) {
            if (maro_snapshot.maro_line != 3 && maro_snapshot.maro_line != 8 && maro_snapshot.maro_line != 13)
                return true;
            return std::none_of(maro_snapshot.maro_variables.begin(), maro_snapshot.maro_variables.end(),
                [](const auto& maro_variable) { return maro_variable.maro_available; });
        }), "function entry does not misreport values from an unestablished stack frame");
        maro_expect(std::any_of(maro_snapshots.begin(), maro_snapshots.end(), [](const auto& maro_snapshot) {
            return std::count_if(maro_snapshot.maro_frames.begin(), maro_snapshot.maro_frames.end(),
                [](const auto& maro_frame) { return maro_frame.maro_function == L"maro_recursive"; }) >= 3;
        }), "native trace stack shows recursive function frames");
        maro_expect(std::any_of(maro_snapshots.begin(), maro_snapshots.end(), [](const auto& maro_snapshot) {
            return std::any_of(maro_snapshot.maro_variables.begin(), maro_snapshot.maro_variables.end(),
                [](const auto& maro_variable) {
                    return maro_variable.maro_name == L"maro_object" && !maro_variable.maro_available &&
                        maro_variable.maro_value == L"복합 형식 관측 미지원";
                });
        }), "unsupported object layouts are explicitly unavailable rather than invented");
        maro_expect(maro_HasArray(maro_snapshots, L"maro_grid", {2, 3}, {L"1", L"2", L"3", L"4", L"5", L"6"}) &&
            maro_HasArray(maro_snapshots, L"maro_grid", {2, 3}, {L"1", L"2", L"3", L"4", L"5", L"42"}),
            "trace observes two-dimensional array cells before and after real assignments");
        maro_expect(maro_HasArray(maro_snapshots, L"maro_sorted", {4}, {L"4", L"1", L"3", L"2"}) &&
            maro_HasArray(maro_snapshots, L"maro_sorted", {4}, {L"1", L"4", L"3", L"2"}) &&
            maro_HasArray(maro_snapshots, L"maro_sorted", {4}, {L"1", L"2", L"3", L"4"}),
            "trace retains actual intermediate sorting states instead of computing a simulated answer");
        maro_expect(maro_HasArray(maro_snapshots, L"maro_cube", {2, 2, 2},
            {L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8"}),
            "three-dimensional arrays are flattened in row-major order with preserved dimensions");
        maro_expect(std::any_of(maro_snapshots.begin(), maro_snapshots.end(), [](const auto& maro_snapshot) {
            return std::any_of(maro_snapshot.maro_variables.begin(), maro_snapshot.maro_variables.end(), [](const auto& maro_variable) {
                return maro_variable.maro_name == L"maro_grid" && maro_variable.maro_children.size() == 6 &&
                    maro_variable.maro_children[5].maro_name == L"[1][2]" &&
                    maro_variable.maro_children[5].maro_address == maro_variable.maro_address + 5 * sizeof(int) &&
                    maro_variable.maro_type == L"int[2][3]";
            });
        }), "array cell labels, types and addresses correspond to debug symbol memory layout");
        maro_expect(std::any_of(maro_snapshots.begin(), maro_snapshots.end(), [](const auto& maro_snapshot) {
            return std::any_of(maro_snapshot.maro_variables.begin(), maro_snapshot.maro_variables.end(), [](const auto& maro_variable) {
                return maro_variable.maro_name == L"maro_large" && maro_variable.maro_children.size() == 64 &&
                    maro_variable.maro_truncated && maro_variable.maro_dimensions == std::vector<std::size_t>{128};
            });
        }), "large arrays expose a bounded 64-cell prefix and explicitly report truncation");
        maro_expect(std::all_of(maro_snapshots.begin(), maro_snapshots.end(), [](const auto& maro_snapshot) {
            std::size_t maro_count = 0;
            for (const auto& maro_variable : maro_snapshot.maro_variables)
            {
                if (maro_variable.maro_children.size() > 64 || maro_variable.maro_dimensions.size() > 3) return false;
                maro_count += maro_variable.maro_children.size();
            }
            return maro_count <= 512;
        }), "every snapshot bounds total array reads, per-variable cells and recursive type depth");
        maro_expect(std::any_of(maro_snapshots.begin(), maro_snapshots.end(), [](const auto& maro_snapshot) {
            return std::any_of(maro_snapshot.maro_variables.begin(), maro_snapshot.maro_variables.end(), [](const auto& maro_variable) {
                return maro_variable.maro_name == L"maro_deep" && maro_variable.maro_truncated &&
                    !maro_variable.maro_available && maro_variable.maro_children.empty();
            });
        }), "unsupported array depth stays explicitly unavailable without unbounded recursion");
        maro_expect(maro_HasObserved(maro_snapshots, L"maro_unreadable", L"0x1") &&
            maro_HasArray(maro_snapshots, L"maro_pointers", {2}, {L"0x1", L"0x2"}),
            "invalid scalar and array pointer targets are displayed as addresses and never dereferenced");
        maro_expect(std::all_of(maro_snapshots.begin(), maro_snapshots.end(), [](const auto& maro_snapshot) {
            return maro_snapshot.maro_message.find(L"초기화 여부 미확인") != std::wstring::npos;
        }), "raw observed memory is explicitly labelled with initialization uncertainty");
        maro_expect(!maro_session->maro_IsPaused(), "completed trace clears paused state");
    }
    {
        unsigned maro_pauses = 0;
        std::shared_ptr<maro_TraceSession> maro_session;
        maro_session = std::make_shared<maro_TraceSession>([&](const maro_TraceSnapshot& maro_snapshot) {
            if (maro_snapshot.maro_waiting) ++maro_pauses;
            maro_session->maro_Continue();
        });
        const auto maro_result = maro_TestDebugProcess(maro_executable, maro_source, maro_session);
        maro_expect(maro_result.maro_exited && maro_result.maro_exitCode == 0 && maro_pauses == 1,
            "Continue removes trace breakpoints and finishes without further pauses");
    }
    {
        std::atomic<bool> maro_paused{false};
        std::atomic<bool> maro_cancel{false};
        auto maro_session = std::make_shared<maro_TraceSession>([&](const maro_TraceSnapshot& maro_snapshot) {
            if (maro_snapshot.maro_waiting) maro_paused.store(true);
        });
        auto maro_future = std::async(std::launch::async, [&] {
            return maro_TestDebugProcess(maro_executable, maro_source, maro_session,
                [&] { return maro_cancel.load(); });
        });
        const auto maro_started = std::chrono::steady_clock::now();
        while (!maro_paused.load() && std::chrono::steady_clock::now() - maro_started < std::chrono::seconds(5))
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        maro_expect(maro_paused.load() && maro_session->maro_IsPaused(),
            "native trace remains paused until an explicit user step or continue");
        const auto maro_cancelTime = std::chrono::steady_clock::now();
        maro_cancel.store(true);
        const auto maro_result = maro_future.get();
        maro_expect(maro_result.maro_cancelled && maro_result.maro_exited &&
            std::chrono::steady_clock::now() - maro_cancelTime < std::chrono::seconds(3),
            "source-change cancellation terminates and drains a paused debug session promptly");
        maro_expect(!maro_session->maro_IsPaused(), "cancelled trace clears paused state");
    }
    {
        auto maro_session = std::make_shared<maro_TraceSession>();
        const auto maro_result = maro_TestDebugProcess(maro_executable,
            maro_directory.maro_path / L"maro_Missing.c", maro_session);
        maro_expect(maro_result.maro_error == ERROR_NOT_FOUND && maro_result.maro_exited,
            "missing source symbols produce an explicit failure and no fabricated trace");
    }
    {
        unsigned maro_pauses = 0;
        Maro_ProcessRequest maro_request;
        maro_request.executable = maro_executable.wstring();
        maro_request.maro_traceSource = maro_source.wstring();
        maro_request.maro_interactiveInput = std::make_shared<maro_ProcessInput>();
        maro_request.maro_trace = std::make_shared<maro_TraceSession>([&](const maro_TraceSnapshot& maro_snapshot) {
            if (maro_snapshot.maro_waiting) ++maro_pauses;
            maro_request.maro_trace->maro_Step();
        });
        const ULONGLONG maro_started = GetTickCount64();
        const auto maro_result = Maro_RunProcess(maro_request,
            [&] { return GetTickCount64() - maro_started > 15'000; });
        maro_expect(maro_result.termination == Maro_ProcessTermination::Exited &&
            maro_result.hasExitCode && maro_result.exitCode == 0 && maro_result.jobObjectApplied && maro_pauses > 15,
            "process runner integrates native tracing with job limits and source stepping");
        maro_expect(maro_result.standardOutputUtf8.empty() && maro_result.standardErrorUtf8.empty(),
            "trace scalar observations do not require or synthesize program stdout");
        maro_expect(!maro_request.maro_interactiveInput->maro_IsOpen(),
            "traced process completion closes stdin writer without hanging");
    }
    {
        Maro_ProcessRequest maro_request;
        maro_request.executable = maro_executable.wstring();
        maro_request.maro_traceSource = maro_source.wstring();
        maro_request.maro_trace = std::make_shared<maro_TraceSession>([&](const maro_TraceSnapshot& maro_snapshot) {
            if (maro_snapshot.maro_waiting) maro_request.maro_trace->maro_Cancel();
        });
        const ULONGLONG maro_started = GetTickCount64();
        const auto maro_result = Maro_RunProcess(maro_request,
            [&] { return GetTickCount64() - maro_started > 15'000; });
        maro_expect(maro_result.termination == Maro_ProcessTermination::Cancelled &&
            GetTickCount64() - maro_started < 3'000,
            "process runner safely drains debug events when tracing is cancelled at a breakpoint");
    }
    {
        const auto maro_cppSource = maro_directory.maro_path / L"maro_ArrayBudget.cpp";
        const auto maro_cppExecutable = maro_directory.maro_path / L"maro_ArrayBudget.exe";
        {
            std::ofstream maro_stream(maro_cppSource, std::ios::binary);
            maro_stream << "using maro_matrix = int[2][2];\nint main()\n{\n"
                "    maro_matrix maro_grid = {{-1, 2}, {3, -4}};\n"
                "    float maro_float[2] = {1.5f, -2.5f};\n"
                "    bool maro_bool[2] = {true, false};\n";
            for (unsigned maro_index = 0; maro_index < 10; ++maro_index)
                maro_stream << "    int maro_large" << maro_index << "[128] = {0};\n";
            maro_stream << "    return maro_grid[1][1] == -4 && maro_float[0] == 1.5f &&"
                " maro_bool[0] && maro_large9[127] == 0 ? 0 : 1;\n}\n";
        }
        auto maro_cppCompile = maro_compile;
        maro_cppCompile.arguments = {L"/nologo", L"/TP", L"/std:c++20", L"/utf-8", L"/Od", L"/Z7", L"/MT",
            maro_cppSource.wstring(), L"/Fe:" + maro_cppExecutable.wstring(),
            L"/Fo:" + (maro_directory.maro_path / L"maro_ArrayBudget.obj").wstring(),
            L"/link", L"/DEBUG", L"/PDB:" + (maro_directory.maro_path / L"maro_ArrayBudget.pdb").wstring()};
        const auto maro_cppCompiled = Maro_RunProcess(maro_cppCompile);
        maro_expect(maro_cppCompiled.hasExitCode && maro_cppCompiled.exitCode == 0,
            "C++ array budget fixture compiles with native typedef and scalar array symbols");
        if (maro_cppCompiled.hasExitCode && maro_cppCompiled.exitCode == 0)
        {
            std::vector<maro_TraceSnapshot> maro_snapshots;
            std::shared_ptr<maro_TraceSession> maro_session;
            maro_session = std::make_shared<maro_TraceSession>([&](const auto& maro_snapshot) {
                maro_snapshots.push_back(maro_snapshot);
                if (maro_snapshots.size() > 128) maro_session->maro_Cancel();
                else maro_session->maro_Step();
            });
            const auto maro_result = maro_TestDebugProcess(maro_cppExecutable, maro_cppSource, maro_session);
            maro_expect(maro_result.maro_exited && maro_result.maro_exitCode == 0 && !maro_result.maro_error,
                "large-array C++ trace completes normally while bounded values are sampled");
            maro_expect(maro_HasArray(maro_snapshots, L"maro_grid", {2, 2}, {L"-1", L"2", L"3", L"-4"}) &&
                maro_HasArray(maro_snapshots, L"maro_float", {2}, {L"1.500000", L"-2.500000"}) &&
                maro_HasArray(maro_snapshots, L"maro_bool", {2}, {L"true", L"false"}),
                "C++ typedef matrices signed floats and bool cells preserve real native types and values");
            bool maro_reached = false;
            bool maro_bounded = true;
            bool maro_exhausted = false;
            for (const auto& maro_snapshot : maro_snapshots)
            {
                std::size_t maro_count = 0;
                for (const auto& maro_variable : maro_snapshot.maro_variables)
                {
                    maro_count += maro_variable.maro_children.size();
                    if (!maro_variable.maro_dimensions.empty() && maro_variable.maro_truncated &&
                        maro_variable.maro_children.empty() && !maro_variable.maro_available) maro_exhausted = true;
                }
                maro_reached = maro_reached || maro_count == 512;
                maro_bounded = maro_bounded && maro_count <= 512;
            }
            maro_expect(maro_reached && maro_bounded && maro_exhausted,
                "many large arrays exhaust exactly512cells without additional reads or fabricated remaining values");
        }
    }
}
