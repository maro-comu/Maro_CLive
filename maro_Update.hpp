#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

struct Maro_SemanticVersion
{
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
};

struct Maro_UpdateAsset
{
    std::wstring fileName;
    std::wstring downloadUrl;
    std::uint64_t size = 0;
    std::array<std::uint8_t, 32> sha256{};
};

struct Maro_UpdateRelease
{
    Maro_SemanticVersion version;
    std::string tag;
    Maro_UpdateAsset asset;
};

enum class Maro_UpdateCheckStatus
{
    Available,
    Current,
    Cancelled,
    Failed
};

struct Maro_UpdateCheckResult
{
    Maro_UpdateCheckStatus status = Maro_UpdateCheckStatus::Failed;
    Maro_UpdateRelease release;
    std::wstring error;
};

enum class Maro_UpdateInstallStatus
{
    Launched,
    Downloaded,
    Cancelled,
    Failed
};

struct Maro_UpdateInstallResult
{
    Maro_UpdateInstallStatus status = Maro_UpdateInstallStatus::Failed;
    std::wstring installerPath;
    std::wstring error;
};

using Maro_UpdateProgress = std::function<void(std::uint64_t, std::uint64_t)>;

std::optional<Maro_SemanticVersion> maro_ParseSemanticVersion(std::string_view text) noexcept;
int maro_CompareSemanticVersions(
    const Maro_SemanticVersion& left,
    const Maro_SemanticVersion& right) noexcept;
std::string maro_FormatSemanticVersion(const Maro_SemanticVersion& version);

bool maro_ParseUpdateReleaseJson(
    std::string_view json,
    Maro_UpdateRelease& release,
    std::string& error);

Maro_UpdateCheckResult maro_CheckForUpdate(
    const Maro_SemanticVersion& currentVersion,
    const std::atomic_bool* cancelled = nullptr);

Maro_UpdateInstallResult maro_DownloadAndLaunchUpdate(
    const Maro_UpdateRelease& release,
    const std::atomic_bool* cancelled = nullptr,
    const Maro_UpdateProgress& progress = {});

Maro_UpdateInstallResult maro_DownloadUpdate(
    const Maro_UpdateRelease& release,
    const std::atomic_bool* cancelled = nullptr,
    const Maro_UpdateProgress& progress = {});
