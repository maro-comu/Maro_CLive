#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct maro_TraceVariable
{
    std::wstring maro_name;
    std::wstring maro_value;
    std::wstring maro_type;
    std::uint64_t maro_address = 0;
    bool maro_available = false;
    std::vector<maro_TraceVariable> maro_children;
    std::vector<std::size_t> maro_dimensions;
    bool maro_truncated = false;
};

struct maro_TraceFrame
{
    std::wstring maro_file;
    std::wstring maro_function;
    std::uint32_t maro_line = 0;
};

struct maro_TraceSnapshot
{
    std::wstring maro_file;
    std::wstring maro_function;
    std::uint32_t maro_line = 0;
    std::vector<maro_TraceVariable> maro_variables;
    std::vector<maro_TraceFrame> maro_frames;
    bool maro_waiting = false;
    std::wstring maro_message;
};

enum class maro_TraceAction { maro_wait, maro_step, maro_continue, maro_cancel };

class maro_TraceSession
{
public:
    using maro_Callback = std::function<void(const maro_TraceSnapshot&)>;
    explicit maro_TraceSession(maro_Callback maro_callback = {});
    void maro_Step() noexcept;
    void maro_Continue() noexcept;
    void maro_Cancel() noexcept;
    bool maro_IsPaused() const noexcept;
    bool maro_IsCancelled() const noexcept;
    bool maro_IsContinuing() const noexcept;
    maro_TraceAction maro_Pause(maro_TraceSnapshot maro_snapshot,
        const std::function<bool()>& maro_cancelled) noexcept;
    void maro_Publish(const maro_TraceSnapshot& maro_snapshot) noexcept;
    void maro_Finish() noexcept;

private:
    maro_Callback maro_callback_;
    std::atomic<maro_TraceAction> maro_action_{maro_TraceAction::maro_wait};
    std::atomic<bool> maro_paused_{false};
    std::atomic<bool> maro_cancelled_{false};
    std::atomic<bool> maro_continuing_{false};
    std::mutex maro_waitMutex_;
    std::condition_variable maro_changed_;
};

struct maro_TraceResult
{
    bool maro_exited = false;
    std::uint32_t maro_exitCode = 0;
    std::uint32_t maro_error = 0;
    bool maro_cancelled = false;
};

maro_TraceResult maro_RunDebugLoop(HANDLE maro_process, DWORD maro_pid,
    const std::wstring& maro_executable, const std::wstring& maro_source,
    const std::shared_ptr<maro_TraceSession>& maro_session,
    const std::function<bool()>& maro_cancelled = {});
