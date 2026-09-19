#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

class maro_OutputQueue
{
public:
    explicit maro_OutputQueue(std::size_t capacity = 1024 * 1024) : capacity_(capacity) {}

    void Push(std::wstring_view text) noexcept
    {
        try
        {
            std::lock_guard lock(mutex_);
            maro_Append(text);
        }
        catch (...)
        {
        }
    }

    void maro_PushVersion(std::uint64_t version, std::wstring_view text) noexcept
    {
        try
        {
            std::lock_guard lock(mutex_);
            if (version != 0 && version == maro_version_)
            {
                maro_Append(text);
            }
        }
        catch (...)
        {
        }
    }

    void maro_Reset(std::uint64_t version) noexcept
    {
        std::lock_guard lock(mutex_);
        maro_version_ = version;
        maro_size_ = 0;
        maro_start_ = 0;
    }

    std::wstring Take(std::size_t maximum)
    {
        std::lock_guard lock(mutex_);
        auto count = (std::min)(maximum, maro_size_);
        if (count && count < maro_size_ && maro_buffer_[(maro_start_ + count - 1) % capacity_] >= 0xd800 &&
            maro_buffer_[(maro_start_ + count - 1) % capacity_] <= 0xdbff) --count;
        std::wstring text(count, L'\0');
        if (count)
        {
            const auto maro_first = (std::min)(count, capacity_ - maro_start_);
            std::copy_n(maro_buffer_.data() + maro_start_, maro_first, text.data());
            std::copy_n(maro_buffer_.data(), count - maro_first, text.data() + maro_first);
            maro_start_ = (maro_start_ + count) % capacity_;
            maro_size_ -= count;
        }
        return text;
    }

    void Clear() noexcept
    {
        std::lock_guard lock(mutex_);
        maro_size_ = 0;
        maro_start_ = 0;
    }

private:
    void maro_Append(std::wstring_view text)
    {
        if (capacity_ == 0 || text.empty()) return;
        if (maro_buffer_.empty()) maro_buffer_.resize(capacity_);
        if (text.size() >= capacity_)
        {
            text.remove_prefix(text.size() - capacity_);
            maro_size_ = 0;
            maro_start_ = 0;
        }
        if (maro_size_ + text.size() > capacity_)
        {
            const auto maro_drop = maro_size_ + text.size() - capacity_;
            maro_start_ = (maro_start_ + maro_drop) % capacity_;
            maro_size_ -= maro_drop;
        }
        const auto maro_end = (maro_start_ + maro_size_) % capacity_;
        const auto maro_first = (std::min)(text.size(), capacity_ - maro_end);
        std::copy_n(text.data(), maro_first, maro_buffer_.data() + maro_end);
        std::copy_n(text.data() + maro_first, text.size() - maro_first, maro_buffer_.data());
        maro_size_ += text.size();
        if (maro_size_ && maro_buffer_[maro_start_] >= 0xdc00 && maro_buffer_[maro_start_] <= 0xdfff)
        {
            maro_start_ = (maro_start_ + 1) % capacity_;
            --maro_size_;
        }
    }

    const std::size_t capacity_;
    std::mutex mutex_;
    std::vector<wchar_t> maro_buffer_;
    std::size_t maro_start_ = 0, maro_size_ = 0;
    std::uint64_t maro_version_ = 0;
};
