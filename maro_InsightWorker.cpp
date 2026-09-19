#include "maro_InsightWorker.hpp"

maro_InsightWorker::maro_InsightWorker() : maro_thread_([this] { maro_Run(); }) {}
maro_InsightWorker::~maro_InsightWorker()
{
    { std::lock_guard maro_lock(maro_mutex_); maro_stopping_ = true; maro_pending_.reset(); }
    maro_changed_.notify_one();
    maro_thread_.join();
}
void maro_InsightWorker::maro_Submit(std::uint64_t maro_version, std::wstring maro_source, std::wstring maro_path)
{
    { std::lock_guard maro_lock(maro_mutex_); maro_pending_ = maro_Request{maro_version, std::move(maro_source), std::move(maro_path)}; }
    maro_changed_.notify_one();
}
std::optional<maro_InsightSnapshot> maro_InsightWorker::maro_Take()
{
    std::lock_guard maro_lock(maro_mutex_);
    auto maro_result = std::move(maro_result_);
    maro_result_.reset();
    return maro_result;
}
void maro_InsightWorker::maro_Run()
{
    for (;;)
    {
        maro_Request maro_request;
        {
            std::unique_lock maro_lock(maro_mutex_);
            maro_changed_.wait(maro_lock, [this] { return maro_stopping_ || maro_pending_.has_value(); });
            if (maro_stopping_) return;
            maro_request = std::move(*maro_pending_);
            maro_pending_.reset();
        }
        try
        {
            maro_InsightSnapshot maro_snapshot{maro_request.maro_version,
                maro_InspectSource(maro_request.maro_source, maro_request.maro_path)};
            std::lock_guard maro_lock(maro_mutex_);
            if (!maro_stopping_ && !maro_pending_) maro_result_ = std::move(maro_snapshot);
        }
        catch (...) {}
    }
}
