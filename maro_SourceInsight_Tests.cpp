#include "maro_SourceInsight.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>

namespace
{
const maro_SourceItem* maro_FindInsightItem(const maro_SourceInsight& maro_model, std::wstring_view maro_name,
    maro_SourceItemKind maro_kind)
{
    const auto maro_found = std::find_if(maro_model.maro_items.begin(), maro_model.maro_items.end(),
        [&](const maro_SourceItem& maro_item) { return maro_item.maro_name == maro_name && maro_item.maro_kind == maro_kind; });
    return maro_found == maro_model.maro_items.end() ? nullptr : &*maro_found;
}
}

void maro_TestSourceInsight(const std::function<void(bool, std::string_view)>& maro_expect)
{
    const auto maro_basic = maro_InspectSource(
        L"#include <stdio.h>\r\n#pragma comment(lib, \"user32.lib\")\r\nint sum(int left, int right) {\r\n"
        L"    int result = left + right;\r\n    return result;\r\n}\r\n", L"C:\\sample\\maro_main.c");
    maro_expect(maro_basic.maro_items.size() == 6, "source insight indexes include/library/function/parameters/local variable");
    maro_expect(maro_basic.maro_items.size() > 1 && maro_basic.maro_items[0].maro_kind == maro_SourceItemKind::maro_Library &&
        maro_basic.maro_items[1].maro_kind == maro_SourceItemKind::maro_Header, "source insight groups libraries before headers");
    const auto* maro_function = maro_FindInsightItem(maro_basic, L"sum", maro_SourceItemKind::maro_Function);
    maro_expect(maro_function && maro_function->maro_line == 3 && maro_function->maro_column == 5,
        "source insight function source location uses one-based CRLF coordinates");
    const auto* maro_variable = maro_FindInsightItem(maro_basic, L"result", maro_SourceItemKind::maro_Variable);
    maro_expect(maro_variable && maro_variable->maro_line == 4 && maro_variable->maro_column == 9 &&
        maro_variable->maro_path == L"C:\\sample\\maro_main.c", "source insight variable links to declaring file and column");
    maro_expect(maro_variable && maro_variable->maro_detail.find(L"실행값 아님") != std::wstring::npos,
        "source insight initializer is explicitly not a runtime value");
    maro_expect(maro_basic.maro_lines.size() == 7 && maro_basic.maro_lines[3].maro_text == L"    int result = left + right;" &&
        !maro_basic.maro_lines[3].maro_explanation.empty(), "source insight has a per-line preview without printf");

    const auto maro_incomplete = maro_InspectSource(L"int main() {\n    int count =\n", L"maro_incomplete.c");
    const auto* maro_count = maro_FindInsightItem(maro_incomplete, L"count", maro_SourceItemKind::maro_Variable);
    maro_expect(maro_count && maro_count->maro_line == 2 && maro_count->maro_detail.find(L"작성 중") != std::wstring::npos,
        "source insight works on incomplete uncompiled declaration");
    const auto maro_incompleteFunction = maro_InspectSource(L"int work(", L"maro_incomplete.c");
    maro_expect(maro_FindInsightItem(maro_incompleteFunction, L"work", maro_SourceItemKind::maro_Function) != nullptr,
        "source insight recognizes incomplete function signature");

    const auto maro_literals = maro_InspectSource(
        L"// int fake1;\n/* int fake2; */\nconst char* real = R\"tag(int fake3;\nvoid fake4() {})tag\";\n"
        L"const char* text = \"int fake5; \\\"\";\nchar letter = '\\'';\n", L"maro_literals.cpp");
    maro_expect(maro_literals.maro_items.size() == 3 &&
        maro_FindInsightItem(maro_literals, L"real", maro_SourceItemKind::maro_Variable) &&
        !maro_FindInsightItem(maro_literals, L"fake3", maro_SourceItemKind::maro_Variable),
        "source insight ignores comments raw strings quoted strings and escaped character literals");
    const auto maro_spliced = maro_InspectSource(L"// continued \\\r\nint hidden;\r\nint visible;", L"maro_comments.c");
    maro_expect(maro_spliced.maro_items.size() == 1 && maro_spliced.maro_items[0].maro_name == L"visible" &&
        maro_spliced.maro_items[0].maro_line == 3, "source insight honors line-comment backslash continuation");
    const auto maro_unclosedRaw = maro_InspectSource(L"const char* text = R\"(\nint hidden;\n", L"maro_raw.cpp");
    maro_expect(maro_unclosedRaw.maro_items.size() == 1 && maro_unclosedRaw.maro_items[0].maro_name == L"text",
        "source insight does not invent declarations from unterminated raw string");

    const auto maro_directives = maro_InspectSource(L"#include /* note */ \\\r\n\"maro_math.h\"\r\n"
        L"#define MAKE int not_a_variable;\r\n#pragma comment(lib, \"mylib.lib\")\r\n", L"maro_main.c");
    maro_expect(maro_directives.maro_items.size() == 2 &&
        maro_FindInsightItem(maro_directives, L"maro_math.h", maro_SourceItemKind::maro_Header) &&
        maro_FindInsightItem(maro_directives, L"mylib.lib", maro_SourceItemKind::maro_Library),
        "source insight handles continued include and skips declarations inside macros");
    const auto maro_dynamicHeader = maro_InspectSource(L"#include MY_HEADER\n#include \"maro_half", L"maro_main.c");
    maro_expect(maro_dynamicHeader.maro_items.size() == 2 && maro_dynamicHeader.maro_items[0].maro_name == L"MY_HEADER" &&
        maro_dynamicHeader.maro_items[1].maro_name == L"maro_half", "source insight labels macro and incomplete includes without inventing resolved paths");

    const auto maro_calls = maro_InspectSource(L"void work() {\n    foo();\n    object.call();\n    return bar();\n    if (test()) {}\n}", L"maro_calls.c");
    maro_expect(maro_calls.maro_items.size() == 1 && maro_calls.maro_items[0].maro_name == L"work",
        "source insight does not misclassify calls return expressions or controls as function declarations");
    const auto maro_declarations = maro_InspectSource(L"int forward(int value);\nint first = 1, second = 2;\n"
        L"std::vector<int> values;\nstruct Node {\n    Node* next;\n    int count;\n};\n", L"maro_types.cpp");
    maro_expect(maro_FindInsightItem(maro_declarations, L"forward", maro_SourceItemKind::maro_Function) &&
        maro_FindInsightItem(maro_declarations, L"first", maro_SourceItemKind::maro_Variable) &&
        maro_FindInsightItem(maro_declarations, L"second", maro_SourceItemKind::maro_Variable) &&
        maro_FindInsightItem(maro_declarations, L"values", maro_SourceItemKind::maro_Variable) &&
        maro_FindInsightItem(maro_declarations, L"next", maro_SourceItemKind::maro_Variable),
        "source insight supports prototypes multi-declarators template types and fields");
    maro_expect(!maro_FindInsightItem(maro_declarations, L"Node", maro_SourceItemKind::maro_Variable),
        "source insight does not misclassify class tag as a variable");

    const std::wstring maro_unicodeSource = L"const wchar_t* text = L\"\xD83D\xDE00\"; int 이름 = 2;\r\nint 값;";
    const auto maro_unicode = maro_InspectSource(maro_unicodeSource, L"C:\\한글\\maro_코드.cpp");
    const auto* maro_unicodeItem = maro_FindInsightItem(maro_unicode, L"이름", maro_SourceItemKind::maro_Variable);
    maro_expect(maro_unicodeItem && maro_unicodeItem->maro_column == maro_unicodeSource.find(L"이름") + 1 &&
        maro_unicodeItem->maro_path == L"C:\\한글\\maro_코드.cpp", "source insight preserves UTF-16 columns and Unicode source paths");

    const auto maro_empty = maro_InspectSource({}, L"maro_empty.c");
    const auto maro_cr = maro_InspectSource(L"#include <stdio.h>\rint first;\rint second;", L"maro_cr.c");
    const auto* maro_crItem = maro_FindInsightItem(maro_cr, L"second", maro_SourceItemKind::maro_Variable);
    maro_expect(maro_cr.maro_lines.size() == 3 && maro_crItem && maro_crItem->maro_line == 3 && maro_crItem->maro_column == 5,
        "source insight normalizes CR-only lines without shifting UTF-16 columns");
    maro_expect(maro_empty.maro_items.empty() && maro_empty.maro_lines.size() == 1 && !maro_empty.maro_truncated,
        "source insight handles empty documents");
    std::wstring maro_many;
    for (std::size_t maro_index = 0; maro_index < 2200; ++maro_index)
        maro_many += L"int maro_v" + std::to_wstring(maro_index) + L";\n";
    const auto maro_boundedItems = maro_InspectSource(maro_many, L"maro_large.c");
    maro_expect(maro_boundedItems.maro_items.size() == 2048 && maro_boundedItems.maro_truncated,
        "source insight caps symbol count for large source");
    const auto maro_boundedLines = maro_InspectSource(std::wstring(33000, L'\n'), L"maro_lines.c");
    maro_expect(maro_boundedLines.maro_lines.size() == 32768 && maro_boundedLines.maro_truncated,
        "source insight caps source line count");
    const auto maro_boundedSource = maro_InspectSource(L"/*" + std::wstring(1048576, L' '), L"maro_bytes.c");
    maro_expect(maro_boundedSource.maro_truncated && maro_boundedSource.maro_lines[0].maro_text.size() == 1048576,
        "source insight caps snapshot input at two MiB UTF-16");
    std::wstring maro_pathological = L"int ";
    maro_pathological.append(20000, L'(');
    const auto maro_boundedSearch = maro_InspectSource(maro_pathological, L"maro_malformed.c");
    maro_expect(maro_boundedSearch.maro_items.empty(), "source insight handles long malformed parenthesis sequence without unbounded prefix search");
}
