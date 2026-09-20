#include "maro_FixApply.hpp"
#include "maro_Analyzer.hpp"
#include "maro_DiagnosticLinks.hpp"
#include <functional>

void maro_TestSafeFixes(const std::function<void(bool, std::string_view)>& maro_expect)
{
    maro_expect(maro_DiagnosticDocumentation(L"C2143") == L"https://learn.microsoft.com/ko-kr/search/?terms=C2143",
        "MSVC diagnostic code links only to official documentation search");
    maro_expect(maro_DiagnosticDocumentation(L"C2143&secret=x").empty() &&
        maro_DiagnosticDocumentation(L"https://untrusted.invalid").empty(), "diagnostic text cannot inject a URL or search arguments");
    maro_expect(maro_DiagnosticDocumentation(L"-Wformat").ends_with(L"#wformat") &&
        maro_DiagnosticDocumentation(L"MARO-MACRO-VALUE").ends_with(L"#maro-macro-value"), "Clang and local rule documentation links");
    maro_expect(maro_DiagnosticDocumentation(L"-W").empty() && maro_DiagnosticDocumentation(L"MARO-UNKNOWN").empty(),
        "empty warning identifiers and undocumented internal codes have no invented links");
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
    const auto maro_atomic = maro_ValidateFix(maro_source, maro_diagnostic, 27, maro_source.sourcePath, maro_source.sourceText);
    maro_expect(maro_atomic && maro_atomic->maro_replacement == L";" && maro_atomic->maro_start.line == 3 &&
        maro_atomic->maro_start.column == maro_atomic->maro_end.column, "general fix application reduces a guarded semicolon to one atomic insertion");
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
    maro_source.sourceText = L"#define MAX_SIZE = 100;\r\nint main(void){int a[MAX_SIZE]; return 0;}\r\n";
    std::vector<Maro_Diagnostic> maro_macroFindings;
    maro_ImproveDiagnostics(maro_source, maro_macroFindings);
    bool maro_macroApplied = false;
    for (const auto& maro_finding : maro_macroFindings)
    {
        if (!maro_finding.fix) continue;
        const auto maro_fix = maro_ValidateFix(maro_source, maro_finding, 27, maro_source.sourcePath, maro_source.sourceText);
        maro_expect(maro_fix.has_value(), "source-aware macro replacement is approved from the unchanged source");
        if (maro_fix && maro_fix->maro_start.line == 1) maro_macroApplied = true;
        auto maro_tampered = maro_finding;
        maro_tampered.fix->edits[0].replacement = L"while(1){}";
        maro_expect(!maro_ValidateFix(maro_source, maro_tampered, 27, maro_source.sourcePath, maro_source.sourceText),
            "a replacement that does not match a regenerated rule is rejected");
        maro_expect(!maro_ValidateFix(maro_source, maro_finding, 28, maro_source.sourcePath, maro_source.sourceText) &&
            !maro_ValidateFix(maro_source, maro_finding, 27, L"other.c", maro_source.sourceText) &&
            !maro_ValidateFix(maro_source, maro_finding, 27, maro_source.sourcePath, maro_source.sourceText + L" "),
            "general replacements retain version, document and complete source guards");
    }
    maro_expect(maro_macroApplied, "non-semicolon source fix is available for malformed numeric macro");
    maro_source.sourceText = L"int main(){ if(1) return 0; }";
    Maro_Diagnostic maro_falseSemicolon;
    maro_falseSemicolon.sourcePath = maro_source.sourcePath;
    maro_falseSemicolon.sourceVersion = maro_source.sourceVersion;
    const auto maro_falseEnd = maro_source.sourceText.find(L"if(1)") + 5;
    const auto maro_falsePrefix = maro_source.sourceText.substr(0, maro_falseEnd);
    maro_falseSemicolon.fix = Maro_FixSuggestion{L"invalid proposal", {{maro_source.sourceVersion, 0, maro_falseEnd,
        maro_falsePrefix, maro_falsePrefix + L";"}}};
    maro_expect(!maro_ValidateFix(maro_source, maro_falseSemicolon, 27, maro_source.sourcePath, maro_source.sourceText),
        "semicolon-shaped proposals must also pass the original conservative syntax rule");
}
