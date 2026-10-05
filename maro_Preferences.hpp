#pragma once

#include <filesystem>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

struct maro_Preferences
{
    bool maro_automatic = true;
    unsigned maro_codePage = 0;
};

std::filesystem::path maro_PreferencesPath() noexcept;
maro_Preferences maro_LoadPreferences(const std::filesystem::path& maro_path = {}) noexcept;
bool maro_SavePreferences(const maro_Preferences& maro_preferences,
    const std::filesystem::path& maro_path = {}) noexcept;

class maro_PreferencesWorker
{
public:
    explicit maro_PreferencesWorker(std::filesystem::path maro_path = {});
    ~maro_PreferencesWorker();
    maro_PreferencesWorker(const maro_PreferencesWorker&) = delete;
    maro_PreferencesWorker& operator=(const maro_PreferencesWorker&) = delete;
    bool maro_TakeLoaded(maro_Preferences& maro_preferences) noexcept;
    void maro_QueueSave(maro_Preferences maro_preferences) noexcept;

private:
    void maro_Run() noexcept;
    std::filesystem::path maro_path_;
    std::mutex maro_mutex_;
    std::condition_variable maro_ready_;
    std::optional<maro_Preferences> maro_loaded_;
    std::optional<maro_Preferences> maro_pending_;
    bool maro_stopped_ = false;
    std::thread maro_thread_;
};
