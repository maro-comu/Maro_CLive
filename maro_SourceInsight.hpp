#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

enum class maro_SourceItemKind
{
    maro_Library,
    maro_Header,
    maro_Function,
    maro_Variable
};

struct maro_SourceItem
{
    maro_SourceItemKind maro_kind = maro_SourceItemKind::maro_Variable;
    std::wstring maro_name;
    std::wstring maro_detail;
    std::wstring maro_path;
    std::size_t maro_line = 1;
    std::size_t maro_column = 1;
};

struct maro_SourceLineInsight
{
    std::wstring maro_text;
    std::wstring maro_explanation;
    std::size_t maro_line = 1;
};

struct maro_SourceInsight
{
    std::wstring maro_path;
    std::vector<maro_SourceItem> maro_items;
    std::vector<maro_SourceLineInsight> maro_lines;
    bool maro_truncated = false;
};

maro_SourceInsight maro_InspectSource(std::wstring_view maro_source, std::wstring_view maro_path);
