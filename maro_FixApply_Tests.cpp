#include "maro_FixApply.hpp"
#include "maro_Analyzer.hpp"
#include <functional>

void maro_TestSafeFixes(const std::function<void(bool, std::string_view)>& maro_expect)
{
    const auto maro_parse = [](const Maro_SourceRequest& maro_source, std::size_t maro_line, std::wstring_view maro_code = L"C2143") {
        return Maro_ParseCompilerDiagnostics(L"maro_UserSource.c(" + std::to_wstring(maro_line) +
            L",1): error " + std::wstring(maro_code) + L": syntax error: missing ';' before 'return'",
            maro_source, maro_BuildGeneratedSource(maro_source), L"MSVC", L"test").at(0);
    };
    Maro_SourceRequest maro_source;
    maro_source.sourceVersion = 27;
    maro_source.sourcePath = L"C:\\maro_project\\maro_main.c";
    maro_source.language = Maro_Language::C17;
    maro_source.sourceText = L"#include <stdio.h>\r\nint main(void) {\r\n    printf(\"한글\")\r\n    return 0;\r\n}";
    const auto maro_diagnostic = maro_parse(maro_source, 4);
    maro_expect(maro_diagnostic.fix && maro_diagnostic.fix->edits.size() == 1, "confirmed missing semicolon offers a guarded fix");
    const auto maro_validate = [&](const Maro_Diagnostic& maro_fix, std::uint64_t maro_version,
        std::wstring_view maro_path, std::wstring_view maro_current) {
        return maro_ValidateSemicolonEdit(maro_source, maro_fix, maro_version, maro_path, maro_current);
    };
    const auto maro_position = maro_validate(maro_diagnostic, 27, maro_source.sourcePath, maro_source.sourceText);
    maro_expect(maro_position && maro_position->line == 3 && maro_position->column == std::wstring_view(L"    printf(\"한글\")").size() + 1,
        "safe edit resolves original CRLF and Korean editor position");
    maro_expect(!maro_validate(maro_diagnostic, 28, maro_source.sourcePath, maro_source.sourceText), "stale diagnostic version cannot modify source");
    maro_expect(!maro_validate(maro_diagnostic, 27, L"maro_other.c", maro_source.sourceText), "different document cannot receive the fix");
    maro_expect(!maro_validate(maro_diagnostic, 27, maro_source.sourcePath, maro_source.sourceText + L" "), "any concurrent source edit rejects the fix");
    if (maro_diagnostic.fix)
    {
        auto maro_bad = maro_diagnostic;
        maro_bad.fix->edits[0].startOffsetUtf16 = 1000000;
        maro_expect(!maro_validate(maro_bad, 27, maro_source.sourcePath, maro_source.sourceText), "out-of-range edits are rejected");
        maro_bad = maro_diagnostic;
        maro_bad.fix->edits[0].replacement = L"while(1){}";
        maro_expect(!maro_validate(maro_bad, 27, maro_source.sourcePath, maro_source.sourceText), "non-semicolon replacements are rejected");
        maro_bad = maro_diagnostic;
        maro_bad.fix->edits.push_back(maro_bad.fix->edits.front());
        maro_expect(!maro_validate(maro_bad, 27, maro_source.sourcePath, maro_source.sourceText), "multi-edit proposals are not partially applied");
        maro_bad = maro_diagnostic;
        maro_bad.fix->edits[0].expectedText = L"other source";
        maro_expect(!maro_validate(maro_bad, 27, maro_source.sourcePath, maro_source.sourceText), "expected-text mismatch is rejected");
        auto maro_fixed = maro_source.sourceText;
        const auto& maro_edit = maro_diagnostic.fix->edits.front();
        maro_fixed.replace(maro_edit.startOffsetUtf16, maro_edit.lengthUtf16, maro_edit.replacement);
        maro_expect(maro_fixed.find(L"printf(\"한글\");\r\n") != std::wstring::npos &&
            !maro_validate(maro_diagnostic, 27, maro_source.sourcePath, maro_fixed), "second click cannot add a duplicate semicolon");
    }
    for (const auto maro_text : {
        L"int main(){\n if(1)\n return 0;\n}",
        L"#define maro_put puts\nint main(){\n maro_put(\"x\")\n return 0;\n}",
        L"int main(){\n puts(R\"tag(x)tag\")\n return 0;\n}",
        L"int main(){\n puts(\"x\"\n return 0;\n}"})
    {
        maro_source.sourceText = maro_text;
        const auto maro_line = maro_source.sourceText.starts_with(L"#") ? 4 : 3;
        maro_expect(!maro_parse(maro_source, maro_line).fix, "ambiguous control/macro/literal/unbalanced source offers no automatic fix");
    }
    maro_source.sourceText = L"int main(void)\r\n{\r\n while (1) {}\r\n}";
    auto maro_timeout = maro_MakeTimeoutDiagnostic(maro_source);
    maro_expect(maro_timeout.code.empty() && maro_timeout.range.start.line == 3 && maro_timeout.range.start.column == 2 &&
        maro_timeout.friendlyMessage.starts_with(L"무한 반복 오류"), "observed timeout in immediate empty constant loop reports its exact location without a code");
    for (const auto maro_text : {L"int main(){ for(;;); }", L"int main(){ while(true) {} }"})
    {
        maro_source.sourceText = maro_text;
        maro_expect(maro_MakeTimeoutDiagnostic(maro_source).friendlyMessage.starts_with(L"무한 반복 오류"), "empty endless loop variants are recognized");
    }
    for (const auto maro_text : {L"int main(){ if(0) while(1){} }", L"int main(){ while(1){break;} }",
        L"int main(){ int i=0; while(i<100)++i; }", L"int main(){ puts(\"while(1){}\"); }", L"#define while(x) if(x)\nint main(){while(1){}}"})
    {
        maro_source.sourceText = maro_text;
        maro_timeout = maro_MakeTimeoutDiagnostic(maro_source);
        maro_expect(maro_timeout.code.empty() && maro_timeout.range.start.line == 0 &&
            maro_timeout.friendlyMessage.starts_with(L"실행 무응답"), "unproven infinite loops do not claim a fabricated runtime line");
    }
    maro_source.sourceText = L"int main(){while(1){}}";
    maro_expect(maro_MakeTimeoutDiagnostic(maro_source, true).range.start.line == 0, "project timeout cannot assume the active document is executing");
}
