#pragma once

#include "Maro_Models.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

using Maro_ResultCallback = std::function<void(Maro_ResultEnvelope)>;

class Maro_Engine
{
public:
    explicit Maro_Engine(Maro_ResultCallback callback);
    ~Maro_Engine();

    Maro_Engine(const Maro_Engine&) = delete;
    Maro_Engine& operator=(const Maro_Engine&) = delete;

    std::uint64_t Submit(Maro_SourceRequest request);
    void Cancel();
    void Shutdown();

    std::uint64_t CurrentRequestId() const noexcept;
    bool IsCurrent(std::uint64_t requestId, std::uint64_t sourceVersion) const noexcept;

    void SetLimits(Maro_ExecutionLimits limits);

private:
    struct Maro_PendingWork
    {
        std::uint64_t requestId = 0;
        std::uint64_t cancellationGeneration = 0;
        Maro_SourceRequest request;
    };

    void WorkerLoop(std::stop_token stopToken);
    void ProcessOne(const Maro_PendingWork& work, std::stop_token stopToken);
    void Publish(const Maro_PendingWork& work, Maro_ResultEnvelope result);

    Maro_ResultCallback callback_;
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::optional<Maro_PendingWork> pending_;
    Maro_ExecutionLimits limits_;
    std::jthread worker_;
    std::atomic<std::uint64_t> nextRequestId_{0};
    std::atomic<std::uint64_t> currentRequestId_{0};
    std::atomic<std::uint64_t> currentSourceVersion_{0};
    std::atomic<std::uint64_t> cancellationGeneration_{0};
    std::atomic<bool> shuttingDown_{false};
};
