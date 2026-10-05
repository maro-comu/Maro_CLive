#include "maro_Preferences.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
struct maro_TemporaryPreferences
{
    std::filesystem::path maro_directory;
    ~maro_TemporaryPreferences()
    {
        if (maro_directory.empty()) return;
        std::error_code maro_error;
        std::filesystem::remove_all(maro_directory, maro_error);
    }
};

bool maro_WritePreferenceFixture(const std::filesystem::path& maro_path, std::string_view maro_bytes)
{
    const HANDLE maro_file = CreateFileW(maro_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (maro_file == INVALID_HANDLE_VALUE) return false;
    DWORD maro_written = 0;
    const bool maro_success = WriteFile(maro_file, maro_bytes.data(), static_cast<DWORD>(maro_bytes.size()),
        &maro_written, nullptr) && maro_written == maro_bytes.size();
    CloseHandle(maro_file);
    return maro_success;
}

bool maro_DefaultPreferences(const maro_Preferences& maro_preferences)
{
    return maro_preferences.maro_automatic && maro_preferences.maro_codePage == 0;
}

bool maro_AwaitPreferences(maro_PreferencesWorker& maro_worker, maro_Preferences& maro_preferences)
{
    const auto maro_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do
    {
        if (maro_worker.maro_TakeLoaded(maro_preferences)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < maro_deadline);
    return false;
}
}

void maro_TestPreferences(const std::function<void(bool, std::string_view)>& maro_expect)
{
    maro_TemporaryPreferences maro_owned;
    const auto maro_temp = std::filesystem::temp_directory_path();
    for (unsigned maro_attempt = 0; maro_attempt < 16; ++maro_attempt)
    {
        const auto maro_directory = maro_temp / (L"maro_Preferences_Test_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()) +
            L"_" + std::to_wstring(maro_attempt));
        if (CreateDirectoryW(maro_directory.c_str(), nullptr))
        {
            maro_owned.maro_directory = maro_directory;
            break;
        }
    }
    maro_expect(!maro_owned.maro_directory.empty(), "preferences tests own a newly created temporary directory");
    if (maro_owned.maro_directory.empty()) return;
    const auto maro_path = maro_owned.maro_directory / L"maro_Preferences.cfg";
    maro_expect(maro_DefaultPreferences(maro_LoadPreferences(maro_path)),
        "missing preferences preserve automatic execution and automatic encoding defaults");
    const auto maro_defaultPath = maro_PreferencesPath();
    maro_expect(!maro_defaultPath.empty() && maro_defaultPath.is_absolute() &&
        maro_defaultPath.filename() == L"maro_Preferences.cfg" &&
        maro_defaultPath.parent_path().filename() == L"CLive_Maro",
        "default preferences stay in a per-user CLive_Maro directory");
    for (const unsigned maro_page : {0u, 65001u, 949u, 1252u})
    {
        for (const bool maro_automatic : {false, true})
        {
            const maro_Preferences maro_expected{maro_automatic, maro_page};
            const bool maro_saved = maro_SavePreferences(maro_expected, maro_path);
            const auto maro_loaded = maro_LoadPreferences(maro_path);
            maro_expect(maro_saved && maro_loaded.maro_automatic == maro_automatic &&
                maro_loaded.maro_codePage == maro_page,
                "automatic execution and every supported encoding survive an atomic save/load");
        }
    }
    const std::vector<std::string> maro_corrupt = {
        "", "CLive_Maro preferences 2\nautomatic=0\nencoding=cp949\n",
        "CLive_Maro preferences 1\nautomatic=yes\nencoding=utf8\n",
        "CLive_Maro preferences 1\nautomatic=0\nencoding=65001\n",
        "CLive_Maro preferences 1\nautomatic=0\nencoding=utf8",
        "CLive_Maro preferences 1\nautomatic=0\nencoding=utf8\nunknown=1\n",
        "CLive_Maro preferences 1\nautomatic=0\nencoding=utf8\nautomatic=1\n",
        "CLive_Maro preferences 1\r\nautomatic=0\r\nencoding=cp949\r\n",
        std::string("CLive_Maro preferences 1\nautomatic=0\nencoding=utf8\n") + std::string(1, '\0'),
        std::string(4096, 'x')
    };
    for (const auto& maro_bytes : maro_corrupt)
    {
        maro_expect(maro_WritePreferenceFixture(maro_path, maro_bytes) &&
            maro_DefaultPreferences(maro_LoadPreferences(maro_path)),
            "corrupt, unsupported, truncated and oversized preferences revert as a complete default snapshot");
    }
    const maro_Preferences maro_previous{false, 949};
    maro_expect(maro_SavePreferences(maro_previous, maro_path) &&
        !maro_SavePreferences({true, 999999}, maro_path) &&
        !maro_LoadPreferences(maro_path).maro_automatic && maro_LoadPreferences(maro_path).maro_codePage == 949,
        "an unsupported encoding cannot overwrite a valid saved preference");
    const auto maro_otherFile = maro_owned.maro_directory / L"maro_Other.cfg";
    maro_expect(!maro_SavePreferences(maro_previous, maro_otherFile) &&
        !std::filesystem::exists(maro_otherFile), "saving preferences never overwrites an unrelated filename");
    const auto maro_relative = std::filesystem::path(L"maro_Preferences.cfg");
    maro_expect(!maro_SavePreferences(maro_previous, maro_relative) &&
        maro_DefaultPreferences(maro_LoadPreferences(maro_relative)),
        "relative paths cannot redirect preference reads or writes into a project");
    const auto maro_missingParent = maro_owned.maro_directory / L"maro_Missing" / L"maro_Preferences.cfg";
    maro_expect(!maro_SavePreferences(maro_previous, maro_missingParent) &&
        !std::filesystem::exists(maro_missingParent.parent_path()),
        "a caller-supplied missing directory is not created unexpectedly");
    const HANDLE maro_locked = CreateFileW(maro_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    maro_expect(maro_locked != INVALID_HANDLE_VALUE && !maro_SavePreferences({true, 65001}, maro_path),
        "a target locked against replacement fails without discarding previous preferences");
    if (maro_locked != INVALID_HANDLE_VALUE) CloseHandle(maro_locked);
    maro_expect(!maro_LoadPreferences(maro_path).maro_automatic && maro_LoadPreferences(maro_path).maro_codePage == 949,
        "failed replacement leaves the prior configuration readable");
    bool maro_onlyTarget = true;
    for (const auto& maro_entry : std::filesystem::directory_iterator(maro_owned.maro_directory))
        if (maro_entry.path() != maro_path) maro_onlyTarget = false;
    maro_expect(maro_onlyTarget, "failed preference saves clean up only their own temporary file");
    std::atomic<bool> maro_readFailed{false};
    std::atomic<unsigned> maro_writers{2};
    std::atomic<unsigned> maro_saved{0};
    const auto maro_writer = [&](maro_Preferences maro_snapshot) {
        for (unsigned maro_iteration = 0; maro_iteration < 12; ++maro_iteration)
            if (maro_SavePreferences(maro_snapshot, maro_path)) maro_saved.fetch_add(1);
        maro_writers.fetch_sub(1);
    };
    std::thread maro_reader([&] {
        do
        {
            const auto maro_snapshot = maro_LoadPreferences(maro_path);
            const bool maro_valid = (!maro_snapshot.maro_automatic && maro_snapshot.maro_codePage == 949) ||
                (maro_snapshot.maro_automatic && maro_snapshot.maro_codePage == 65001);
            if (!maro_valid) maro_readFailed.store(true);
            std::this_thread::yield();
        } while (maro_writers.load() != 0);
    });
    std::thread maro_first(maro_writer, maro_previous);
    std::thread maro_second(maro_writer, maro_Preferences{true, 65001});
    maro_first.join();
    maro_second.join();
    maro_reader.join();
    maro_expect(maro_saved.load() != 0 && !maro_readFailed.load(),
        "concurrent saves and reads expose complete preference snapshots without partial files");
    const auto maro_directoryTarget = maro_owned.maro_directory / L"maro_Directory";
    std::filesystem::create_directory(maro_directoryTarget);
    std::filesystem::create_directory(maro_directoryTarget / L"maro_Preferences.cfg");
    maro_expect(!maro_SavePreferences(maro_previous, maro_directoryTarget / L"maro_Preferences.cfg") &&
        std::filesystem::is_directory(maro_directoryTarget / L"maro_Preferences.cfg"),
        "a directory at the preference filename is never overwritten");
    const auto maro_workerParent = maro_owned.maro_directory / L"maro_Worker";
    std::filesystem::create_directory(maro_workerParent);
    const auto maro_workerPath = maro_workerParent / L"maro_Preferences.cfg";
    {
        maro_PreferencesWorker maro_worker(maro_workerPath);
        maro_Preferences maro_initial;
        const bool maro_loaded = maro_AwaitPreferences(maro_worker, maro_initial);
        maro_expect(maro_loaded && maro_DefaultPreferences(maro_initial) &&
            !maro_worker.maro_TakeLoaded(maro_initial),
            "an asynchronous preferences worker publishes its initial snapshot exactly once");
    }
    maro_expect(!std::filesystem::exists(maro_workerPath),
        "an idle preferences worker does not write a configuration or run periodic work");
    maro_SavePreferences(maro_previous, maro_workerPath);
    {
        maro_PreferencesWorker maro_worker(maro_workerPath);
        maro_Preferences maro_initial;
        maro_expect(maro_AwaitPreferences(maro_worker, maro_initial) && !maro_initial.maro_automatic &&
            maro_initial.maro_codePage == 949,
            "saved settings load asynchronously before UI initialization");
        std::thread maro_firstQueue([&] {
            for (unsigned maro_iteration = 0; maro_iteration < 100; ++maro_iteration)
                maro_worker.maro_QueueSave({false, 1252});
        });
        std::thread maro_secondQueue([&] {
            for (unsigned maro_iteration = 0; maro_iteration < 100; ++maro_iteration)
                maro_worker.maro_QueueSave({true, 949});
        });
        maro_firstQueue.join();
        maro_secondQueue.join();
        maro_worker.maro_QueueSave({false, 65001});
        maro_worker.maro_QueueSave({true, 999999});
    }
    const auto maro_final = maro_LoadPreferences(maro_workerPath);
    maro_expect(!maro_final.maro_automatic && maro_final.maro_codePage == 65001,
        "worker shutdown flushes the final coalesced preference snapshot after concurrent requests");
}
