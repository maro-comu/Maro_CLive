#include "maro_Process.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <future>
#include <string>
#include <string_view>
#include <thread>

namespace
{
using maro_TestCheck = std::function<void(bool, std::string_view)>;

Maro_ProcessRequest maro_ChildRequest(std::wstring_view maro_mode)
{
    std::wstring maro_path(32'768, L'\0');
    const DWORD maro_length = GetModuleFileNameW(nullptr, maro_path.data(),
        static_cast<DWORD>(maro_path.size()));
    maro_path.resize(maro_length);
    Maro_ProcessRequest maro_request;
    maro_request.executable = std::move(maro_path);
    maro_request.arguments = {L"--maro-process-input-child", std::wstring(maro_mode)};
    maro_request.limits.wallMilliseconds = 5'000;
    maro_request.limits.cpuMilliseconds = 0;
    return maro_request;
}

bool maro_SubmitEventually(const std::shared_ptr<maro_ProcessInput>& maro_input,
    std::string_view maro_text)
{
    for (unsigned maro_attempt = 0; maro_attempt < 100; ++maro_attempt)
    {
        if (maro_input->maro_Submit(maro_text))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
}

int maro_RunProcessInputChild(int maro_argc, char** maro_argv)
{
    if (maro_argc != 3 || std::string_view(maro_argv[1]) != "--maro-process-input-child")
    {
        return -1;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    const std::string_view maro_mode(maro_argv[2]);
    if (maro_mode == "silent-input")
    {
        char maro_buffer[1024];
        if (!std::fgets(maro_buffer, sizeof(maro_buffer), stdin)) return 0;
        std::fputs(maro_buffer[0] == '\n' ? "empty-line-ok" : "input-ok", stdout);
        return 0;
    }
    if (maro_mode == "busy-worker-input" || maro_mode == "busy-main-input")
    {
        std::atomic_bool maro_done{false};
        bool maro_empty = false;
        const auto maro_read = [&] {
            char maro_buffer[1024];
            maro_empty = std::fgets(maro_buffer, sizeof(maro_buffer), stdin) && maro_buffer[0] == '\n';
            maro_done.store(true, std::memory_order_release);
        };
        const auto maro_compute = [&] {
            while (!maro_done.load(std::memory_order_acquire)) std::atomic_signal_fence(std::memory_order_seq_cst);
        };
        if (maro_mode == "busy-worker-input")
        {
            std::thread maro_worker(maro_compute);
            maro_read();
            maro_worker.join();
        }
        else
        {
            std::thread maro_worker(maro_read);
            maro_compute();
            maro_worker.join();
        }
        std::fputs(maro_empty ? "empty-line-with-cpu-ok" : "unexpected-input", stdout);
        return maro_empty ? 0 : 1;
    }
    if (maro_mode == "repeat")
    {
        for (int maro_index = 0; maro_index < 12; ++maro_index)
        {
            std::fputs("tick\n", stdout);
            Sleep(100);
        }
        return 0;
    }
    if (maro_mode == "busy")
    {
        const auto maro_start = GetTickCount64();
        while (GetTickCount64() - maro_start < 5'000) std::atomic_signal_fence(std::memory_order_seq_cst);
        return 0;
    }
    if (maro_mode == "job")
    {
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION maro_cpu{};
        const BOOL maro_query = QueryInformationJobObject(nullptr, JobObjectCpuRateControlInformation,
            &maro_cpu, sizeof(maro_cpu), nullptr);
        std::printf("priority=%lu query=%d flags=%lu rate=%lu\n", GetPriorityClass(GetCurrentProcess()),
            maro_query, maro_cpu.ControlFlags, maro_cpu.CpuRate);
        return 0;
    }
    if (maro_mode == "echo")
    {
        std::fputs("ready\n", stdout);
        char maro_buffer[1024];
        while (std::fgets(maro_buffer, static_cast<int>(sizeof(maro_buffer)), stdin))
        {
            std::fputs("echo:", stdout);
            std::fputs(maro_buffer, stdout);
        }
        std::fputs("eof\n", stdout);
        return 0;
    }
    if (maro_mode == "blocked")
    {
        std::fputs("ready\n", stdout);
        Sleep(10'000);
        return 0;
    }
    if (maro_mode == "burst")
    {
        const std::string maro_chunk(8192, 'x');
        for (unsigned maro_index = 0; maro_index < 256; ++maro_index)
        {
            std::fwrite(maro_chunk.data(), 1, maro_chunk.size(), stdout);
        }
        std::fputs("\n끝-END\n", stdout);
        std::fputs("ERR-END\n", stderr);
        return 0;
    }
    return 2;
}

void maro_TestProcessInput(const maro_TestCheck& maro_expect)
{
    for (const auto maro_mode : {L"busy-worker-input", L"busy-main-input"})
    {
        auto maro_request = maro_ChildRequest(maro_mode);
        maro_request.limits.maro_idleMilliseconds = 500;
        maro_request.maro_interactiveInput = std::make_shared<maro_ProcessInput>();
        auto maro_future = std::async(std::launch::async, [&] { return Maro_RunProcess(maro_request); });
        maro_expect(maro_future.wait_for(std::chrono::milliseconds(1600)) == std::future_status::timeout,
            maro_mode == std::wstring_view(L"busy-worker-input")
                ? "main-thread stdin wait survives a CPU-busy helper thread"
                : "worker-thread stdin wait survives a CPU-busy main thread");
        maro_SubmitEventually(maro_request.maro_interactiveInput, "\n");
        maro_request.maro_interactiveInput->maro_End();
        const auto maro_result = maro_future.get();
        maro_expect(maro_result.hasExitCode && maro_result.exitCode == 0 &&
            maro_result.standardOutputUtf8 == "empty-line-with-cpu-ok",
            "empty stdin line resumes an input wait alongside active computation");
    }
    {
        auto maro_request = maro_ChildRequest(L"repeat");
        maro_request.limits.wallMilliseconds = 0;
        maro_request.limits.maro_idleMilliseconds = 500;
        const auto maro_result = Maro_RunProcess(maro_request);
        maro_expect(maro_result.hasExitCode && maro_result.exitCode == 0 &&
            maro_result.termination == Maro_ProcessTermination::Exited,
            "repeating stdout renews idle timeout beyond the total runtime limit");
    }
    {
        auto maro_request = maro_ChildRequest(L"silent-input");
        maro_request.limits.wallMilliseconds = 0;
        maro_request.limits.maro_idleMilliseconds = 500;
        maro_request.maro_interactiveInput = std::make_shared<maro_ProcessInput>();
        auto maro_future = std::async(std::launch::async, [&] { return Maro_RunProcess(maro_request); });
        maro_expect(maro_future.wait_for(std::chrono::milliseconds(1600)) == std::future_status::timeout,
            "stdin without a printed prompt is not timed out while waiting");
        maro_SubmitEventually(maro_request.maro_interactiveInput, "\n");
        maro_request.maro_interactiveInput->maro_End();
        const auto maro_result = maro_future.get();
        maro_expect(maro_result.hasExitCode && maro_result.exitCode == 0 &&
            maro_result.standardOutputUtf8 == "empty-line-ok", "empty input line resumes a waiting program");
    }
    {
        auto maro_request = maro_ChildRequest(L"busy");
        maro_request.limits.wallMilliseconds = 0;
        maro_request.limits.maro_idleMilliseconds = 500;
        maro_request.maro_interactiveInput = std::make_shared<maro_ProcessInput>();
        const auto maro_result = Maro_RunProcess(maro_request);
        maro_expect(maro_result.termination == Maro_ProcessTermination::WallTimedOut,
            "silent CPU-bound loop times out even with stdin open");
    }
    {
        auto maro_request = maro_ChildRequest(L"job");
        const auto maro_manual = Maro_RunProcess(maro_request);
        maro_expect(maro_manual.hasExitCode && maro_manual.exitCode == 0 &&
            maro_manual.standardOutputUtf8.find("flags=0 rate=0") != std::string::npos,
            "manual processes do not inherit the automatic CPU cap");
        maro_request.maro_background = true;
        const auto maro_background = Maro_RunProcess(maro_request);
        maro_expect(maro_background.hasExitCode && maro_background.exitCode == 0 &&
            maro_background.standardOutputUtf8.find("priority=16384 query=1 flags=5 rate=2000") != std::string::npos,
            "automatic processes use below-normal priority and a 20 percent CPU hard cap");
    }
    {
        maro_ProcessInput maro_queue(4);
        maro_expect(maro_queue.maro_Submit("abcd"), "stdin accepts its bounded capacity");
        maro_expect(!maro_queue.maro_Submit("e"), "stdin rejects overflow without blocking");
        maro_queue.maro_End();
        maro_expect(!maro_queue.maro_Submit(""), "stdin rejects all input after EOF");
        std::string maro_bytes;
        maro_expect(maro_queue.maro_Read(maro_bytes) && maro_bytes == "abcd",
            "EOF preserves input already queued");
        maro_expect(!maro_queue.maro_Read(maro_bytes), "EOF closes after queued input drains");
    }
    {
        maro_ProcessInput maro_queue;
        maro_queue.maro_Submit("stale\n");
        maro_queue.maro_Close();
        std::string maro_bytes;
        maro_expect(!maro_queue.maro_Read(maro_bytes), "cancel discards queued input");
        maro_expect(!maro_queue.maro_Claim() && !maro_queue.maro_Submit("new\n"),
            "closed input cannot be reused by a later run");
    }
    {
        auto maro_request = maro_ChildRequest(L"echo");
        maro_request.standardInputUtf8 = "fixed\n";
        const auto maro_result = Maro_RunProcess(maro_request);
        maro_expect(maro_result.termination == Maro_ProcessTermination::Exited &&
            maro_result.exitCode == 0 && maro_result.standardOutputUtf8.find("echo:fixed") != std::string::npos &&
            maro_result.standardOutputUtf8.find("eof") != std::string::npos,
            "fixed stdin still closes automatically without an interactive queue");
    }
    {
        auto maro_request = maro_ChildRequest(L"echo");
        maro_request.standardInputUtf8 = "initial\n";
        maro_request.maro_interactiveInput = std::make_shared<maro_ProcessInput>();
        std::mutex maro_mutex;
        std::condition_variable maro_changed;
        std::string maro_stream;
        auto maro_future = std::async(std::launch::async, [&] {
            return Maro_RunProcess(maro_request, {}, [&](bool maro_error, std::string_view maro_bytes) {
                if (!maro_error)
                {
                    std::lock_guard maro_lock(maro_mutex);
                    maro_stream.append(maro_bytes);
                    maro_changed.notify_all();
                }
            });
        });
        {
            std::unique_lock maro_lock(maro_mutex);
            maro_expect(maro_changed.wait_for(maro_lock, std::chrono::seconds(3), [&] {
                return maro_stream.find("echo:initial") != std::string::npos;
            }), "initial stdin streams before interactive input arrives");
        }
        maro_expect(maro_future.wait_for(std::chrono::milliseconds(80)) == std::future_status::timeout,
            "interactive process remains running while awaiting input");
        const auto maro_duplicate = Maro_RunProcess(maro_request);
        maro_expect(maro_duplicate.termination == Maro_ProcessTermination::StartFailed &&
            maro_request.maro_interactiveInput->maro_IsOpen(),
            "one-shot input rejects a duplicate process without closing the original");
        maro_expect(maro_SubmitEventually(maro_request.maro_interactiveInput, "한글 입력\n"),
            "interactive stdin accepts UTF-8 text");
        {
            std::unique_lock maro_lock(maro_mutex);
            maro_expect(maro_changed.wait_for(maro_lock, std::chrono::seconds(3), [&] {
                return maro_stream.find("echo:한글 입력") != std::string::npos;
            }), "interactive response streams before the process exits");
        }
        maro_request.maro_interactiveInput->maro_End();
        const auto maro_result = maro_future.get();
        maro_expect(maro_result.termination == Maro_ProcessTermination::Exited &&
            maro_result.exitCode == 0 && maro_result.standardOutputUtf8.find("eof") != std::string::npos,
            "explicit interactive EOF reaches the child and allows clean exit");
        maro_expect(!maro_request.maro_interactiveInput->maro_IsOpen() &&
            !maro_request.maro_interactiveInput->maro_Submit("late\n"),
            "completed process permanently closes its input");
    }
    {
        auto maro_request = maro_ChildRequest(L"blocked");
        maro_request.maro_interactiveInput = std::make_shared<maro_ProcessInput>();
        maro_request.limits.wallMilliseconds = 0;
        maro_request.limits.cpuMilliseconds = 0;
        maro_request.limits.memoryBytes = 0;
        maro_request.limits.activeProcessLimit = 0;
        std::atomic<bool> maro_cancel{false};
        std::atomic<bool> maro_ready{false};
        auto maro_future = std::async(std::launch::async, [&] {
            return Maro_RunProcess(maro_request, [&] { return maro_cancel.load(); },
                [&](bool, std::string_view) { maro_ready.store(true); });
        });
        const auto maro_start = std::chrono::steady_clock::now();
        while (!maro_ready.load() && std::chrono::steady_clock::now() - maro_start < std::chrono::seconds(3))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        maro_expect(maro_ready.load(), "zero execution limits permit a trusted interactive session");
        const std::string maro_large(64u << 10, 'a');
        const auto maro_submitStart = std::chrono::steady_clock::now();
        maro_expect(maro_SubmitEventually(maro_request.maro_interactiveInput, maro_large),
            "input queues when the child never reads stdin");
        maro_expect(std::chrono::steady_clock::now() - maro_submitStart < std::chrono::milliseconds(250),
            "UI input submission does not wait for the child pipe");
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        const auto maro_cancelStart = std::chrono::steady_clock::now();
        maro_cancel.store(true);
        const auto maro_result = maro_future.get();
        maro_expect(maro_result.termination == Maro_ProcessTermination::Cancelled &&
            std::chrono::steady_clock::now() - maro_cancelStart < std::chrono::seconds(3),
            "cancelling a blocked writer joins without freezing");
        maro_expect(!maro_request.maro_interactiveInput->maro_IsOpen(),
            "cancelled session rejects later input");
    }
    {
        auto maro_request = maro_ChildRequest(L"burst");
        maro_request.maro_rollingOutput = true;
        maro_request.limits.stdoutBytes = 4096;
        maro_request.limits.stderrBytes = 4;
        std::size_t maro_streamed = 0;
        const auto maro_result = Maro_RunProcess(maro_request, {},
            [&](bool maro_error, std::string_view maro_bytes) {
                if (!maro_error)
                {
                    maro_streamed += maro_bytes.size();
                }
            });
        maro_expect(maro_result.termination == Maro_ProcessTermination::Exited && maro_result.exitCode == 0,
            "rolling output does not terminate a high-volume long session");
        maro_expect(maro_streamed > 2u * 1024u * 1024u,
            "all output streams even when retained history is bounded");
        maro_expect(maro_result.standardOutputUtf8.size() <= 4096 &&
            maro_result.standardOutputUtf8.ends_with("끝-END\r\n"),
            "rolling stdout retains the newest UTF-8 output");
        maro_expect(maro_result.standardErrorUtf8.size() <= 4 &&
            maro_result.standardErrorUtf8.ends_with("ND\r\n"),
            "rolling stderr has its own bounded history");
        maro_request.maro_rollingOutput = false;
        const auto maro_limited = Maro_RunProcess(maro_request);
        maro_expect(maro_limited.termination == Maro_ProcessTermination::OutputLimit &&
            maro_limited.standardOutputUtf8.size() <= 4096,
            "noninteractive default output limits remain enforced");
    }
    {
        Maro_ProcessRequest maro_request;
        maro_request.maro_interactiveInput = std::make_shared<maro_ProcessInput>();
        const auto maro_result = Maro_RunProcess(maro_request);
        maro_expect(maro_result.termination == Maro_ProcessTermination::StartFailed &&
            !maro_request.maro_interactiveInput->maro_IsOpen(),
            "failed process startup closes pending input");
    }
}
