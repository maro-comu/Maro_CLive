#pragma once

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>

class maro_OutputQueue
{
public:
    explicit maro_OutputQueue(std::size_t capacity = 1024 * 1024) : capacity_(capacity) {}

    void Push(std::wstring_view text) noexcept
    {
        try
        {
            std::lock_guard lock(mutex_);
            if (text.size() >= capacity_)
            {
                text_.assign(text.substr(text.size() - capacity_));
                return;
            }
            if (text_.size() + text.size() > capacity_)
            {
                text_.erase(0, text_.size() + text.size() - capacity_);
            }
            text_.append(text);
        }
        catch (...)
        {
        }
    }

    std::wstring Take(std::size_t maximum)
    {
        std::lock_guard lock(mutex_);
        const auto count = (std::min)(maximum, text_.size());
        std::wstring text = text_.substr(0, count);
        text_.erase(0, count);
        return text;
    }

    void Clear() noexcept
    {
        std::lock_guard lock(mutex_);
        text_.clear();
    }

private:
    const std::size_t capacity_;
    std::mutex mutex_;
    std::wstring text_;
};
