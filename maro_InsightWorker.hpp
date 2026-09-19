#pragma once
#include "maro_SourceInsight.hpp"
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

struct maro_InsightSnapshot
{
    std::uint64_t maro_version = 0;
    maro_SourceInsight maro_insight;
};

class maro_InsightWorker
{
public:
    maro_InsightWorker();
    ~maro_InsightWorker();
    void maro_Submit(std::uint64_t maro_version, std::wstring maro_source, std::wstring maro_path);
    std::optional<maro_InsightSnapshot> maro_Take();
private:
    struct maro_Request { std::uint64_t maro_version; std::wstring maro_source, maro_path; };
    void maro_Run();
    std::mutex maro_mutex_;
    std::condition_variable maro_changed_;
    std::optional<maro_Request> maro_pending_;
    std::optional<maro_InsightSnapshot> maro_result_;
    bool maro_stopping_ = false;
    std::thread maro_thread_;
};
