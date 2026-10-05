#include "maro_CodeDiagnostics.hpp"
#include "maro_FixApply.hpp"

#include <algorithm>
#include <cstdio>
#include <string_view>

namespace
{
Maro_SourceRequest maro_testRequest(std::wstring_view maro_text)
{
    Maro_SourceRequest maro_request;
    maro_request.sourceVersion = 73;
    maro_request.sourceText = maro_text;
    maro_request.sourcePath = L"C:\\maro_tests\\maro_example.c";
    maro_request.language = Maro_Language::C17;
    maro_request.execute = false;
    return maro_request;
}

std::vector<Maro_Diagnostic> maro_testAnalyze(std::wstring_view maro_text, Maro_Language maro_language = Maro_Language::C17)
{
    auto maro_request = maro_testRequest(maro_text);
    maro_request.language = maro_language;
    std::vector<Maro_Diagnostic> maro_diagnostics;
    maro_ImproveDiagnostics(maro_request, maro_diagnostics);
    return maro_diagnostics;
}

const Maro_Diagnostic* maro_testFind(const std::vector<Maro_Diagnostic>& maro_diagnostics, std::wstring_view maro_code)
{
    const auto maro_found = std::find_if(maro_diagnostics.begin(), maro_diagnostics.end(), [&](const auto& maro_item) {
        return maro_item.code == maro_code;
    });
    return maro_found == maro_diagnostics.end() ? nullptr : &*maro_found;
}

bool maro_testReplacement(std::wstring_view maro_source, const Maro_Diagnostic* maro_diagnostic,
    std::wstring_view maro_expected)
{
    if (!maro_diagnostic || !maro_diagnostic->fix || maro_diagnostic->fix->edits.empty()) return false;
    auto maro_edits = maro_diagnostic->fix->edits;
    std::sort(maro_edits.begin(), maro_edits.end(), [](const auto& maro_left, const auto& maro_right) {
        return maro_left.startOffsetUtf16 > maro_right.startOffsetUtf16;
    });
    std::wstring maro_fixed(maro_source);
    auto maro_limit = maro_fixed.size();
    for (const auto& maro_edit : maro_edits)
    {
        if (maro_edit.sourceVersion != 73 || maro_edit.startOffsetUtf16 > maro_limit ||
            maro_edit.lengthUtf16 > maro_limit - maro_edit.startOffsetUtf16 ||
            maro_edit.expectedText != maro_fixed.substr(maro_edit.startOffsetUtf16, maro_edit.lengthUtf16)) return false;
        maro_fixed.replace(maro_edit.startOffsetUtf16, maro_edit.lengthUtf16, maro_edit.replacement);
        maro_limit = maro_edit.startOffsetUtf16;
    }
    return maro_fixed == maro_expected;
}

Maro_Diagnostic maro_testCompiler(const Maro_SourceRequest& maro_request, std::wstring_view maro_code,
    std::size_t maro_line, std::size_t maro_column)
{
    Maro_Diagnostic maro_diagnostic;
    maro_diagnostic.sourceVersion = maro_request.sourceVersion;
    maro_diagnostic.sourcePath = maro_request.sourcePath;
    maro_diagnostic.code = maro_code;
    maro_diagnostic.analyzer = L"MSVC";
    maro_diagnostic.range.start = {maro_line, maro_column};
    maro_diagnostic.range.end = maro_diagnostic.range.start;
    maro_diagnostic.originalDiagnostic = L"original " + std::wstring(maro_code);
    return maro_diagnostic;
}
}

bool maro_TestCodeDiagnostics()
{
    bool maro_passed = true;
    const auto maro_expect = [&](bool maro_condition, const char* maro_name) {
        if (!maro_condition)
        {
            std::fprintf(stderr, "[source diagnostics] FAIL: %s\n", maro_name);
            maro_passed = false;
        }
    };
    const std::wstring maro_example =
        L"#include <stdio.h>\n"
        L"#include <string.h>\n"
        L"#define SQUARE(x) x * x\n"
        L"#define MAX_SIZE = 100;\n"
        L"int* get_array() {\n"
        L"    int local_arr[3] = { 10, 20, 30 };\n"
        L"    return local_arr;\n"
        L"}\n"
        L"int main() {\n"
        L"    int buffer[MAX_SIZE];\n"
        L"    int result = 20 / SQUARE(2 + 3);\n"
        L"    printf(\"Result: %d\\n\", result);\n"
        L"    int* ptr = get_array();\n"
        L"    printf(\"First element: %d\\n\", ptr[0]);\n"
        L"    char* str1 = \"Hello\";\n"
        L"    char str2[10];\n"
        L"    if (str1 == \"Hello\") {\n"
        L"        str2 = str1;\n"
        L"    }\n"
        L"    double pi = 3.14159;\n"
        L"    int* iptr = (int*)&pi;\n"
        L"    printf(\"Pi as int: %d\\n\", iptr);\n"
        L"    int a = 10;\n"
        L"    int b = 0;\n"
        L"    if (b = 0) {\n"
        L"        printf(\"b is zero\\n\");\n"
        L"    } else {\n"
        L"        int c = a / b;\n"
        L"        printf(\"Value: %d\\n\", c);\n"
        L"    }\n"
        L"    return 0\n"
        L"}\n";
    auto maro_findings = maro_testAnalyze(maro_example);
    for (const auto* maro_code : {L"MARO-MACRO-VALUE", L"MARO-MACRO-PRECEDENCE", L"MARO-LOCAL-LIFETIME",
        L"MARO-STRING-COMPARE", L"MARO-ARRAY-ASSIGNMENT", L"MARO-POINTER-TYPE", L"MARO-PRINTF-POINTER",
        L"MARO-CONDITION-ASSIGNMENT", L"MARO-DIVIDE-ZERO", L"MARO-MISSING-SEMICOLON"})
    {
        const auto* maro_finding = maro_testFind(maro_findings, maro_code);
        if (!maro_finding) std::fwprintf(stderr, L"[source diagnostics] missing example code: %ls\n", maro_code);
        maro_expect(maro_finding != nullptr, "user example retains every independent supported finding");
        if (!maro_finding) continue;
        maro_expect(maro_finding->sourceVersion == 73 && maro_finding->sourcePath == maro_testRequest(L"").sourcePath &&
            !maro_finding->range.generated && maro_finding->range.start.line > 0 &&
            maro_finding->range.start.column > 0 && !maro_finding->friendlyMessage.empty(),
            "source diagnostics identify the exact original document version and usable position");
        maro_expect(maro_finding->friendlyMessage.find(L"문법을 완성") == std::wstring::npos &&
            maro_finding->friendlyMessage.find(L"C:\\") == std::wstring::npos &&
            maro_finding->friendlyMessage.find(L'\0') == std::wstring::npos,
            "guidance excludes vague grammar advice, distracting source paths and embedded NUL characters");
        maro_expect(maro_finding->friendlyMessage.size() <= 90,
            "source guidance stays concise instead of presenting a long explanation paragraph");
        if (maro_finding->fix)
        {
            const auto maro_request = maro_testRequest(maro_example);
            maro_expect(!maro_finding->fix->description.empty() && maro_finding->fix->description.find(L'\0') == std::wstring::npos &&
                maro_ValidateFix(maro_request, *maro_finding, 73, maro_request.sourcePath, maro_example).has_value(),
                "proposed example replacement is independently regenerated and validated");
            maro_expect(maro_finding->fix->description.size() <= 60,
                "clickable source replacement label is a short explicit action");
            maro_expect(!maro_ValidateFix(maro_request, *maro_finding, 74, maro_request.sourcePath, maro_example) &&
                !maro_ValidateFix(maro_request, *maro_finding, 73, maro_request.sourcePath, maro_example + L" "),
                "stale solution cannot modify a newer source snapshot");
        }
    }
    for (const auto* maro_code : {L"MARO-LOCAL-LIFETIME", L"MARO-POINTER-TYPE", L"MARO-DIVIDE-ZERO"})
    {
        const auto* maro_finding = maro_testFind(maro_findings, maro_code);
        maro_expect(maro_finding && !maro_finding->fix, "lifetime, representation and runtime values require an intentional manual repair");
    }
    for (const auto* maro_code : {L"MARO-PRINTF-POINTER", L"MARO-CONDITION-ASSIGNMENT", L"MARO-STRING-COMPARE"})
    {
        const auto* maro_finding = maro_testFind(maro_findings, maro_code);
        maro_expect(maro_finding && maro_finding->fix && maro_finding->evidence == Maro_Evidence::Conditional,
            "intent-dependent one-click alternatives explicitly carry conditional evidence");
        maro_expect(maro_finding && maro_finding->friendlyMessage.find(L"경우:") != std::wstring::npos &&
            maro_finding->fix && maro_finding->fix->description.find(L"경우:") != std::wstring::npos,
            "shortening the solution retains the user's intended behavior in both visible and clickable text");
    }
    const auto maro_before = maro_findings.size();
    maro_ImproveDiagnostics(maro_testRequest(maro_example), maro_findings);
    maro_expect(maro_findings.size() == maro_before, "improving the same snapshot twice does not duplicate findings");

    struct maro_ReplacementCase
    {
        const wchar_t* maro_code;
        const wchar_t* maro_source;
        const wchar_t* maro_expected;
        const char* maro_name;
    };
    const maro_ReplacementCase maro_replacements[] = {
        {L"MARO-MACRO-VALUE", L"#define MAX_SIZE = 100;\nint main(){int a[MAX_SIZE];return 0;}",
            L"#define MAX_SIZE 100\nint main(){int a[MAX_SIZE];return 0;}", "numeric macro changes only its replacement list"},
        {L"MARO-MACRO-VALUE", L"#define MAX_SIZE = 100;\nint main(){int x=MAX_SIZE;return x;}",
            L"#define MAX_SIZE 100\nint main(){int x=MAX_SIZE;return x;}", "numeric macro used in a scalar initializer receives the same precise fix"},
        {L"MARO-MACRO-VALUE", L"#define MAX_SIZE = 100;\nint x=MAX_SIZE;int main(){int a[MAX_SIZE];return x;}",
            L"#define MAX_SIZE 100\nint x=MAX_SIZE;int main(){int a[MAX_SIZE];return x;}", "global initialization and array bounds can share a proven numeric macro fix"},
        {L"MARO-MACRO-PRECEDENCE", L"#define SQUARE(x) x * x\nint main(){return SQUARE(2 + 3);}",
            L"#define SQUARE(x) ((x) * (x))\nint main(){return SQUARE(2 + 3);}", "square macro groups both parameter uses and the whole expression"},
        {L"MARO-STRING-COMPARE", L"#include <string.h>\nint main(){char* a=\"Hello\";if(a==\"Hello\"){return 1;}return 0;}",
            L"#include <string.h>\nint main(){char* a=\"Hello\";if((strcmp(a, \"Hello\") == 0)){return 1;}return 0;}", "string comparison becomes an explicitly grouped content comparison"},
        {L"MARO-ARRAY-ASSIGNMENT", L"#include <string.h>\nint main(){char* a=\"Hello\";char b[6];b=a;return 0;}",
            L"#include <string.h>\nint main(){char* a=\"Hello\";char b[6];memcpy(b, a, 6);return 0;}", "array copy includes the terminator and respects the exact known capacity"},
        {L"MARO-PRINTF-POINTER", L"#include <stdio.h>\nint main(){int a=1;int* p=&a;printf(\"%d\\n\",p);return 0;}",
            L"#include <stdio.h>\nint main(){int a=1;int* p=&a;printf(\"%p\\n\", (void*)p);return 0;}", "address output changes the format and pointer argument together"},
        {L"MARO-CONDITION-ASSIGNMENT", L"int main(){int b=0;if(b=0){return 1;}return 0;}",
            L"int main(){int b=0;if(b==0){return 1;}return 0;}", "assignment repair touches only the comparison operator"},
        {L"MARO-MISSING-SEMICOLON", L"int main(){return 0\n}",
            L"int main(){return 0;\n}", "missing return terminator is added on the statement rather than the next line"}
    };
    for (const auto& maro_case : maro_replacements)
    {
        const auto maro_diagnostics = maro_testAnalyze(maro_case.maro_source);
        const auto* maro_finding = maro_testFind(maro_diagnostics, maro_case.maro_code);
        maro_expect(maro_testReplacement(maro_case.maro_source, maro_finding,
            maro_case.maro_expected), maro_case.maro_name);
        const auto maro_request = maro_testRequest(maro_case.maro_source);
        maro_expect(maro_finding && maro_ValidateFix(maro_request, *maro_finding, 73, maro_request.sourcePath, maro_case.maro_source).has_value(),
            "each supported source replacement passes the independent apply-time safety check");
        maro_expect(!maro_testFind(maro_testAnalyze(maro_case.maro_expected), maro_case.maro_code),
            "applied repair removes its own root diagnosis without needing an unrelated edit");
    }

    const std::wstring maro_unicode = L"int main(){\r\n    const char* text = \"한글\xD83D\xDE00\";\r\n    return 0\r\n}\r\n";
    const auto maro_unicodeFindings = maro_testAnalyze(maro_unicode);
    const auto* maro_unicodeFinding = maro_testFind(maro_unicodeFindings, L"MARO-MISSING-SEMICOLON");
    maro_expect(maro_unicodeFinding && maro_unicodeFinding->range.start.line == 3 &&
        maro_unicodeFinding->range.start.column == 13 && maro_unicodeFinding->range.end.line == 3 &&
        maro_unicodeFinding->range.end.column == 13, "CRLF, Korean and surrogate pairs preserve the missing token line and UTF16 column");
    if (maro_unicodeFinding)
    {
        auto maro_expected = maro_unicode;
        maro_expected.insert(maro_expected.find(L"return 0") + 8, L";");
        maro_expect(maro_testReplacement(maro_unicode, maro_unicodeFinding, maro_expected),
            "Unicode source receives the semicolon at its original UTF16 offset");
    }
    const std::wstring maro_sameLineUnicode = L"int main(){const char* text=\"한글\xD83D\xDE00\";return 0}";
    const auto maro_sameLineFindings = maro_testAnalyze(maro_sameLineUnicode);
    const auto* maro_sameLineFinding = maro_testFind(maro_sameLineFindings, L"MARO-MISSING-SEMICOLON");
    maro_expect(maro_sameLineFinding && maro_sameLineFinding->range.start.line == 1 &&
        maro_sameLineFinding->range.start.column == maro_sameLineUnicode.find(L"return 0") + 9,
        "same-line Korean and supplementary characters use editor UTF16 columns");
    const std::wstring maro_tabUnicode = L"int main(){\r\n\tconst char* text=\"한글\xD83D\xDE00\";\treturn 0\r\n}";
    const auto maro_tabFindings = maro_testAnalyze(maro_tabUnicode);
    const auto* maro_tabFinding = maro_testFind(maro_tabFindings, L"MARO-MISSING-SEMICOLON");
    const auto maro_tabLine = maro_tabUnicode.find(L'\n') + 1;
    maro_expect(maro_tabFinding && maro_tabFinding->range.start.line == 2 &&
        maro_tabFinding->range.start.column == maro_tabUnicode.find(L"return 0") - maro_tabLine + 9,
        "tabs occupy one UTF16 editor column rather than their visual indentation width");

    for (const auto* maro_text : {
        L"#include <string.h>\nint main(){char* a=\"Hello\";const char b[6]={0};b=a;return 0;}",
        L"#include <string.h>\nint main(){char* a=\"Hello\";char b[5];b=a;return 0;}",
        L"#include <string.h>\nint main(){char* a=\"123456789\";char b[010];b=a;return 0;}",
        L"int main(){char* a=\"Hello\";char b[6];b=a;return 0;}",
        L"#include <string.h>\nint main(){char* a=\"Hello\";char b[6];a=\"a much longer string\";b=a;return 0;}",
        L"#include <string.h>\nint main(){char* a=\"a\\nb\";char b[6];b=a;return 0;}",
        L"#include <string.h>\nint main(){char* a=\"한글\";char b[6];b=a;return 0;}"})
    {
        const auto maro_diagnostics = maro_testAnalyze(maro_text);
        const auto* maro_finding = maro_testFind(maro_diagnostics, L"MARO-ARRAY-ASSIGNMENT");
        maro_expect(!maro_finding || !maro_finding->fix, "array copying refuses const, undersized, unknown, changed or encoding-dependent storage");
    }
    for (const auto* maro_text : {
        L"int main(){char* a=\"Hello\";if(a==\"Hello\"){return 1;}return 0;}",
        L"#include <string.h>\n#define strcmp(a,b) 0\nint main(){char* a=\"Hello\";if(a==\"Hello\"){return 1;}return 0;}",
        L"#include <string.h>\nint strcmp(const char*a,const char*b){return 0;}int main(){char* a=\"Hello\";if(a==\"Hello\"){return 1;}return 0;}",
        L"#include <string.h>\nint main(){char* a=\"Hello\";if(!a==\"Hello\"){return 1;}return 0;}",
        L"#include <string.h>\nint main(){char* a=\"Hello\";if(obj->a==\"Hello\"){return 1;}return 0;}",
        L"#include <string.h>\nint main(){char* a=\"Hello\";if(1+a==\"Hello\"){return 1;}return 0;}",
        L"#include <string.h>\nint main(){char* a=\"Hello\";if(a==\"Hello\"+1){return 1;}return 0;}",
        L"#include <string.h>\nint main(){char* a=\"Hello\";if(a /* preserve */ ==\"Hello\"){return 1;}return 0;}"})
    {
        const auto maro_diagnostics = maro_testAnalyze(maro_text);
        const auto* maro_finding = maro_testFind(maro_diagnostics, L"MARO-STRING-COMPARE");
        maro_expect(!maro_finding || !maro_finding->fix, "content comparison refuses missing headers, shadowed functions, member/unary operands and comments");
    }
    for (const auto* maro_text : {
        L"int main(){return 8 / 0.5;}",
        L"int main(){int b=0;if(b!=0){return 8/b;}return 0;}",
        L"int main(){int b=0;if(b==0){return 0;}else{return 8/b;}}",
        L"int main(){int b=0;b=2;return 8/b;}",
        L"int main(){int b=0;read(&b);return 8/b;}",
        L"int main(){int b=0;{int b=2;return 8/b;}}"})
        maro_expect(!maro_testFind(maro_testAnalyze(maro_text), L"MARO-DIVIDE-ZERO"),
            "zero denominator analysis respects fractional literals, nonzero branches, mutations and scope");
    for (const auto* maro_text : {
        L"int main(){int b=0;int& alias=b;alias=2;return 8/b;}",
        L"int main(){int b=0;auto& alias=b;alias=2;return 8/b;}",
        L"void change(int& value){value=2;}int main(){int b=0;change(b);return 8/b;}"})
        maro_expect(!maro_testFind(maro_testAnalyze(maro_text, Maro_Language::Cpp20), L"MARO-DIVIDE-ZERO"),
            "references and reference-taking calls invalidate a known zero before division");
    for (const auto* maro_text : {
        L"#include <string.h>\nint main(){const char* a=\"Hello\";const char*& alias=a;alias=\"a much longer string\";char b[6];b=a;return 0;}",
        L"#include <string.h>\nint main(){const char* a=\"Hello\";auto& alias=a;alias=\"a much longer string\";char b[6];b=a;return 0;}",
        L"#include <string.h>\nvoid change(const char*& a){a=\"a much longer string\";}int main(){const char* a=\"Hello\";change(a);char b[6];b=a;return 0;}"})
    {
        const auto maro_diagnostics = maro_testAnalyze(maro_text, Maro_Language::Cpp20);
        const auto* maro_finding = maro_testFind(maro_diagnostics, L"MARO-ARRAY-ASSIGNMENT");
        maro_expect(!maro_finding || !maro_finding->fix,
            "references and reference-taking calls invalidate a known string before a capacity-based copy");
    }
    for (const auto* maro_text : {
        L"#include <stdio.h>\nint* p;int run(int p){printf(\"%d\",p);return 0;}",
        L"#include <stdio.h>\nint* p;int main(){auto p=1;printf(\"%d\",p);return 0;}",
        L"#include <stdio.h>\ntypedef int number;int* p;int main(){number p=1;printf(\"%d\",p);return 0;}"})
        maro_expect(!maro_testFind(maro_testAnalyze(maro_text, Maro_Language::Cpp20), L"MARO-PRINTF-POINTER"),
            "parameter, auto and typedef shadows never inherit an outer pointer type for printf repair");
    const maro_ReplacementCase maro_shadowedCalls[] = {
        {L"MARO-ARRAY-ASSIGNMENT", L"#include <string.h>\nvoid* memcpy(void* a,const void* b,int n){return a;}int main(){char* a=\"Hello\";char b[6];b=a;return 0;}", nullptr,
            "a custom void-pointer-return function is not treated as the standard memcpy"},
        {L"MARO-STRING-COMPARE", L"#include <string.h>\ntypedef int number;number strcmp(const char* a,const char* b){return 0;}int main(){char* a=\"Hello\";if(a==\"Hello\"){return 1;}return 0;}", nullptr,
            "a custom typedef-return function is not treated as the standard strcmp"},
        {L"MARO-PRINTF-POINTER", L"#include <stdio.h>\ntypedef int number;number printf(const char* a,...){return 0;}int main(){int a=1;int* p=&a;printf(\"%d\",p);return 0;}", nullptr,
            "a custom typedef-return function is not treated as the standard printf"}
    };
    for (const auto& maro_case : maro_shadowedCalls)
    {
        const auto maro_diagnostics = maro_testAnalyze(maro_case.maro_source);
        const auto* maro_finding = maro_testFind(maro_diagnostics, maro_case.maro_code);
        maro_expect(!maro_finding || !maro_finding->fix, maro_case.maro_name);
    }
    for (const auto* maro_text : {L"int main(){return 8/0;}",
        L"int main(){int b=0;return 8/b;}", L"int main(){int b=0;if(b=0){return 0;}else{return 8/b;}}"})
    {
        const auto maro_diagnostics = maro_testAnalyze(maro_text);
        const auto* maro_finding = maro_testFind(maro_diagnostics, L"MARO-DIVIDE-ZERO");
        maro_expect(maro_finding && !maro_finding->fix, "known zero denominator warns without guessing a replacement value");
    }
    for (const auto* maro_text : {
        L"int* get_array(){static int a[3]={1,2,3};return a;}",
        L"int a[3]={1,2,3};int* get_array(){return a;}",
        L"int* get_array(int* a){return a;}"})
        maro_expect(!maro_testFind(maro_testAnalyze(maro_text), L"MARO-LOCAL-LIFETIME"),
            "returning static, global or caller-owned storage is not called a dangling local array");
    for (const auto* maro_text : {
        L"#define SET = 100;\nint main(){int a SET int b=SET;return a+b;}",
        L"#define SET = 100;\n#define ALIAS SET\nint main(){int a ALIAS int b=SET;return a+b;}",
        L"#define SET = /* preserve */ 100;\nint main(){int b=SET;return b;}",
        L"#define int Other\n#define SET = 100;\nint main(){int b=SET;return b;}",
        L"#define SET = 100;\nint main(){int b=(SET);return b;}",
        L"#define SET = 100;\nint main(){Custom b=SET;return 0;}",
        L"#define SET = 100;\nint main(){int b=SET+1;return b;}"})
    {
        const auto maro_diagnostics = maro_testAnalyze(maro_text);
        const auto* maro_finding = maro_testFind(maro_diagnostics, L"MARO-MACRO-VALUE");
        maro_expect(!maro_finding || !maro_finding->fix,
            "numeric macro repair preserves intentional fragments, aliases, comments and unproven initializer expressions");
    }
    maro_expect(!maro_testFind(maro_testAnalyze(L"int a[MAX_SIZE];\n#define MAX_SIZE = 100;\nint main(){return 0;}"), L"MARO-MACRO-VALUE"),
        "a macro defined after its apparent use is not mistaken for the cause of that identifier error");
    for (const auto* maro_text : {
        L"#define SET = 100;\nint main(){int a SET return 0;}",
        L"#define MAX_SIZE 100\n#define SQUARE(x) ((x)*(x))\nint main(){int a[MAX_SIZE];return SQUARE(2+3);}",
        L"#include <string.h>\n#include <stdio.h>\nint main(){const char* a=\"Hello\";char b[6];memcpy(b,a,6);if(strcmp(a,b)==0){printf(\"%s\",b);}return 0;}",
        L"int main(){int a=4;int b=2;if(b==0){return 0;}return a/b;}",
        L"int main(){const char* a=\"return 0 } int b=0; 8/b\";return 0;}",
        L"int main(){/* return 0 } int b=0; 8/b */return 0;}"})
        maro_expect(maro_testAnalyze(maro_text).empty(), "normal code, intentional fragments, comments and strings do not produce speculative diagnostics");

    auto maro_macroRequest = maro_testRequest(L"#define MAX_SIZE = 100;\n#define OTHER = 5\nint main(){int a[MAX_SIZE];int b[OTHER];unknown=1;return 0;}");
    const auto maro_lineStart = maro_macroRequest.sourceText.rfind(L'\n') + 1;
    const auto maro_macroColumn = maro_macroRequest.sourceText.find(L"MAX_SIZE]", maro_lineStart) - maro_lineStart + 1;
    const auto maro_otherColumn = maro_macroRequest.sourceText.find(L"OTHER]", maro_lineStart) - maro_lineStart + 1;
    const auto maro_unknownColumn = maro_macroRequest.sourceText.find(L"unknown", maro_lineStart) - maro_lineStart + 1;
    std::vector<Maro_Diagnostic> maro_compiler = {
        maro_testCompiler(maro_macroRequest, L"C2143", 3, maro_macroColumn),
        maro_testCompiler(maro_macroRequest, L"C4431", 3, maro_macroColumn + 1),
        maro_testCompiler(maro_macroRequest, L"C2143", 3, maro_otherColumn),
        maro_testCompiler(maro_macroRequest, L"C2065", 3, maro_unknownColumn)
    };
    maro_ImproveDiagnostics(maro_macroRequest, maro_compiler);
    const auto maro_compilerCount = [&](std::wstring_view maro_code) {
        return std::count_if(maro_compiler.begin(), maro_compiler.end(), [&](const auto& maro_item) { return maro_item.code == maro_code; });
    };
    maro_expect(maro_compilerCount(L"MARO-MACRO-VALUE") == 1 && maro_compilerCount(L"C2143") == 1 &&
        maro_compilerCount(L"C4431") == 0 && maro_compilerCount(L"C2065") == 1,
        "numeric macro cascade folds only the matching root while retaining a different macro and independent identifier error");
    auto maro_scalarRequest = maro_testRequest(L"#define MAX_SIZE = 100;\nint main(){int a=MAX_SIZE;int b=unknown;return 0;}");
    const auto maro_scalarLine = maro_scalarRequest.sourceText.find(L'\n') + 1;
    const auto maro_scalarMacro = maro_scalarRequest.sourceText.find(L"MAX_SIZE;", maro_scalarLine) - maro_scalarLine + 1;
    const auto maro_scalarUnknown = maro_scalarRequest.sourceText.find(L"unknown", maro_scalarLine) - maro_scalarLine + 1;
    maro_compiler = {
        maro_testCompiler(maro_scalarRequest, L"C2059", 2, maro_scalarMacro),
        maro_testCompiler(maro_scalarRequest, L"C2143", 2, maro_scalarMacro),
        maro_testCompiler(maro_scalarRequest, L"C2065", 2, maro_scalarUnknown),
        maro_testCompiler(maro_scalarRequest, L"C2143", 2, maro_scalarUnknown)
    };
    maro_ImproveDiagnostics(maro_scalarRequest, maro_compiler);
    const auto* maro_scalarRoot = maro_testFind(maro_compiler, L"MARO-MACRO-VALUE");
    maro_expect(maro_scalarRoot && maro_scalarRoot->maro_relatedCount == 2 && maro_compilerCount(L"C2059") == 0 &&
        maro_compilerCount(L"C2143") == 1 && maro_compilerCount(L"C2065") == 1,
        "scalar macro cascade merges only syntax errors at that macro and preserves unrelated same-line failures");
    auto maro_initializerRequest = maro_testRequest(L"#include <stdio.h>\r\n\t#define MARO_CAP = 100;\r\nint main(void) {\r\n    int a[MARO_CAP] = {7}; int b[MARO_CAP] = 7; Unknown c[MARO_CAP] = {0};\r\n    return 0;\r\n}");
    const auto maro_at = [&](std::wstring_view maro_code, std::wstring_view maro_text, std::size_t maro_delta = 0) {
        const auto maro_position = maro_OffsetPosition(maro_initializerRequest.sourceText, maro_initializerRequest.sourceText.find(maro_text) + maro_delta);
        return maro_testCompiler(maro_initializerRequest, maro_code, maro_position.line, maro_position.column);
    };
    maro_compiler = {
        maro_at(L"C2143", L"a[MARO_CAP]", 2),
        maro_at(L"C4431", L"a[MARO_CAP]"),
        maro_at(L"C4430", L"a[MARO_CAP]"),
        maro_at(L"C2075", L"a[MARO_CAP]", 2),
        maro_at(L"C2075", L"b[MARO_CAP]", 2),
        maro_at(L"C4431", L"c[MARO_CAP]"),
        maro_at(L"C4430", L"c[MARO_CAP]", 2),
        maro_at(L"C2075", L"c[MARO_CAP]", 2)
    };
    maro_ImproveDiagnostics(maro_initializerRequest, maro_compiler);
    const auto* maro_macroRoot = maro_testFind(maro_compiler, L"MARO-MACRO-VALUE");
    maro_expect(maro_macroRoot && maro_macroRoot->maro_relatedCount == 4 &&
        maro_macroRoot->originalDiagnostic.find(L"C4431") != std::wstring::npos &&
        maro_macroRoot->originalDiagnostic.find(L"C2075") != std::wstring::npos &&
        maro_compilerCount(L"C4431") == 1 && maro_compilerCount(L"C4430") == 1 && maro_compilerCount(L"C2075") == 2,
        "macro cascade folds proven builtin array declaration errors and preserves missing types and scalar initializers on the same line");
    auto maro_stale = maro_testCompiler(maro_macroRequest, L"C2143", 3, maro_macroColumn);
    maro_stale.sourceVersion = 72;
    auto maro_foreign = maro_testCompiler(maro_macroRequest, L"C2143", 3, maro_macroColumn);
    maro_foreign.sourcePath = L"C:\\maro_tests\\maro_other.c";
    auto maro_generated = maro_testCompiler(maro_macroRequest, L"C2143", 3, maro_macroColumn);
    maro_generated.range.generated = true;
    maro_compiler = {maro_stale, maro_foreign, maro_generated};
    maro_ImproveDiagnostics(maro_macroRequest, maro_compiler);
    maro_expect(maro_compilerCount(L"C2143") == 3,
        "cascade folding never removes stale, different-document or generated-code diagnostics");
    auto maro_arrayRequest = maro_testRequest(L"#include <string.h>\nint main(){char* a=\"Hello\";char b[6];b=a;1=2;return 0;}");
    const auto maro_arrayStart = maro_arrayRequest.sourceText.find(L'\n') + 1;
    maro_compiler = {
        maro_testCompiler(maro_arrayRequest, L"C2106", 2, maro_arrayRequest.sourceText.find(L"b=a") - maro_arrayStart + 1),
        maro_testCompiler(maro_arrayRequest, L"C2106", 2, maro_arrayRequest.sourceText.find(L"1=2") - maro_arrayStart + 1)
    };
    maro_ImproveDiagnostics(maro_arrayRequest, maro_compiler);
    maro_expect(maro_compilerCount(L"C2106") == 1 && maro_compilerCount(L"MARO-ARRAY-ASSIGNMENT") == 1,
        "same-line independent assignment error survives folding of the identified array assignment");
    for (const auto* maro_text : {
        L"#include \"maro_custom.h\"\n#define MAX_SIZE = 100;\nint main(){int a[MAX_SIZE];return 0}",
        L"#if ENABLED\n#define MAX_SIZE = 100;\n#endif\nint main(){int a[MAX_SIZE];return 0}",
        L"#define MAX_SIZE = 100;\n#define MAX_SIZE 3\nint main(){int a[MAX_SIZE];return 0}",
        L"int main(){puts(R\"tag(return 0)tag\");return 0}",
        L"int main(){puts(\"unterminated);return 0}",
        L"int main(){if(1){return 0}"})
    {
        auto maro_request = maro_testRequest(maro_text);
        auto maro_original = maro_testCompiler(maro_request, L"C2143", 2, 4);
        std::vector<Maro_Diagnostic> maro_diagnostics{maro_original};
        maro_ImproveDiagnostics(maro_request, maro_diagnostics);
        maro_expect(maro_diagnostics.size() == 1 && maro_diagnostics.front().code == L"C2143" &&
            maro_diagnostics.front().originalDiagnostic == maro_original.originalDiagnostic,
            "uncertain headers, preprocessing and lexical structure retain compiler diagnostics without guessed repairs");
    }
    const auto maro_headerDiagnostic = [&](const Maro_SourceRequest& maro_request, std::wstring_view maro_function,
        std::wstring_view maro_code = L"C3861") {
        const auto maro_position = maro_OffsetPosition(maro_request.sourceText, maro_request.sourceText.find(maro_function));
        auto maro_diagnostic = maro_testCompiler(maro_request, maro_code, maro_position.line, maro_position.column);
        maro_diagnostic.severity = Maro_Severity::Error;
        maro_diagnostic.originalDiagnostic = L"'" + std::wstring(maro_function) + L"': identifier not found";
        return maro_diagnostic;
    };
    struct maro_HeaderCase
    {
        const wchar_t* maro_function;
        const wchar_t* maro_header;
        const wchar_t* maro_source;
    };
    for (const auto& maro_case : {
        maro_HeaderCase{L"printf", L"stdio.h", L"int main(){printf(\"ok\\n\");return 0;}"},
        maro_HeaderCase{L"puts", L"stdio.h", L"int main(){puts(\"ok\");return 0;}"},
        maro_HeaderCase{L"strcmp", L"string.h", L"int main(){return strcmp(\"a\",\"b\");}"},
        maro_HeaderCase{L"memcpy", L"string.h", L"int main(){char a[2];memcpy(a,\"a\",2);return 0;}"},
        maro_HeaderCase{L"malloc", L"stdlib.h", L"int main(){void* a=malloc(2);return a==0;}"},
        maro_HeaderCase{L"free", L"stdlib.h", L"int main(){void* a=0;free(a);return 0;}"}})
    {
        const auto maro_request = maro_testRequest(maro_case.maro_source);
        auto maro_original = maro_headerDiagnostic(maro_request, maro_case.maro_function);
        std::vector<Maro_Diagnostic> maro_diagnostics{maro_original};
        maro_ImproveDiagnostics(maro_request, maro_diagnostics);
        const auto& maro_finding = maro_diagnostics.front();
        const auto maro_expected = L"#include <" + std::wstring(maro_case.maro_header) + L">\n" + maro_case.maro_source;
        maro_expect(maro_finding.code == maro_original.code && maro_finding.originalDiagnostic == maro_original.originalDiagnostic &&
            maro_finding.range.start.line == maro_original.range.start.line && maro_finding.range.start.column == maro_original.range.start.column &&
            maro_finding.friendlyMessage.size() < 45 && maro_testReplacement(maro_case.maro_source, &maro_finding, maro_expected),
            "compiler-confirmed missing standard function adds its exact C header with a concise clickable action");
        maro_expect(maro_finding.fix && maro_VerifyMissingHeaderFix(maro_request, maro_finding) &&
            maro_ValidateFix(maro_request, maro_finding, 73, maro_request.sourcePath, maro_case.maro_source).has_value(),
            "missing header replacement is rederived from compiler evidence during apply-time validation");
        maro_expect(maro_testAnalyze(maro_case.maro_source).empty(),
            "standard function names alone do not speculate about a missing declaration without compiler evidence");
        if (!maro_finding.fix) continue;
        for (int maro_mutation = 0; maro_mutation < 8; ++maro_mutation)
        {
            auto maro_tampered = maro_finding;
            if (maro_mutation == 0) maro_tampered.fix->edits.front().replacement += L"#define main maro_other\n";
            if (maro_mutation == 1) maro_tampered.code = L"C2143";
            if (maro_mutation == 2) maro_tampered.originalDiagnostic = L"'maro_unknown': identifier not found";
            if (maro_mutation == 3) maro_tampered.range.start.column = 1;
            if (maro_mutation == 4) maro_tampered.range.generated = true;
            if (maro_mutation == 5) maro_tampered.sourceVersion = 72;
            if (maro_mutation == 6) maro_tampered.sourcePath += L".other";
            if (maro_mutation == 7) maro_tampered.analyzer = L"Unknown";
            maro_expect(!maro_VerifyMissingHeaderFix(maro_request, maro_tampered) &&
                !maro_ValidateFix(maro_request, maro_tampered, 73, maro_request.sourcePath, maro_case.maro_source),
                "altered include, compiler evidence, location or document identity cannot authorize an insertion");
        }
        auto maro_current = maro_request;
        maro_current.sourceText = maro_expected;
        const auto maro_currentPosition = maro_OffsetPosition(maro_current.sourceText, maro_current.sourceText.find(maro_case.maro_function));
        maro_original.range.start = maro_currentPosition;
        maro_original.range.end = maro_currentPosition;
        std::vector<Maro_Diagnostic> maro_currentDiagnostics{maro_original};
        maro_ImproveDiagnostics(maro_current, maro_currentDiagnostics);
        maro_expect(!maro_currentDiagnostics.front().fix, "a header already present is never inserted a second time");
    }
    for (const auto* maro_code : {L"C2065", L"C4013"})
    {
        const auto maro_request = maro_testRequest(L"int main(){printf(\"ok\");return 0;}");
        std::vector<Maro_Diagnostic> maro_diagnostics{maro_headerDiagnostic(maro_request, L"printf", maro_code)};
        maro_ImproveDiagnostics(maro_request, maro_diagnostics);
        maro_expect(maro_diagnostics.front().fix && maro_VerifyMissingHeaderFix(maro_request, maro_diagnostics.front()),
            "MSVC undeclared identifier and C implicit-function diagnostics can add the proven missing standard header");
    }
    for (const auto* maro_code : {L"C-COMP-1001", L"CPP-NAME-1001", L"-Wimplicit-function-declaration", L"-Wimplicit-library-function-declaration"})
    {
        const auto maro_request = maro_testRequest(L"int main(){puts(\"ok\");return 0;}");
        auto maro_original = maro_headerDiagnostic(maro_request, L"puts", maro_code);
        maro_original.analyzer = L"Clang";
        maro_original.originalDiagnostic = L"call to undeclared function 'puts'; ISO C99 and later do not support implicit function declarations";
        std::vector<Maro_Diagnostic> maro_diagnostics{maro_original};
        maro_ImproveDiagnostics(maro_request, maro_diagnostics);
        maro_expect(maro_diagnostics.front().fix && maro_VerifyMissingHeaderFix(maro_request, maro_diagnostics.front()),
            "Clang undeclared standard call diagnostics carry the same exact header repair");
        maro_original.originalDiagnostic = L"incompatible pointer types passing 'puts'";
        maro_diagnostics = {maro_original};
        maro_ImproveDiagnostics(maro_request, maro_diagnostics);
        maro_expect(!maro_diagnostics.front().fix, "a Clang type error is not misread as a missing standard header");
    }
    const auto maro_bomRequest = maro_testRequest(L"\xfeffint main(){\r\n\tprintf(\"한글\xD83D\xDE00\");\r\nreturn 0;\r\n}\r\n");
    std::vector<Maro_Diagnostic> maro_bomDiagnostics{maro_headerDiagnostic(maro_bomRequest, L"printf")};
    maro_ImproveDiagnostics(maro_bomRequest, maro_bomDiagnostics);
    maro_expect(maro_testReplacement(maro_bomRequest.sourceText, &maro_bomDiagnostics.front(),
        L"\xfeff#include <stdio.h>\r\nint main(){\r\n\tprintf(\"한글\xD83D\xDE00\");\r\nreturn 0;\r\n}\r\n") &&
        maro_bomDiagnostics.front().range.start.line == 2 && maro_bomDiagnostics.front().range.start.column == 2,
        "header insertion preserves BOM, CRLF, tabs, Korean text and supplementary UTF16 characters");
    for (const auto* maro_source : {
        L"#include \"maro_custom.h\"\nint main(){printf(\"ok\");return 0;}",
        L"#include <maro_custom.h>\nint main(){printf(\"ok\");return 0;}",
        L"#if MARO_TEST\nint main(){printf(\"ok\");return 0;}\n#endif",
        L"#define MARO_NUMBER 3\nint main(){printf(\"ok\");return 0;}",
        L"#define printf maro_custom\nint main(){printf(\"ok\");return 0;}",
        L"int printf(const char* a);int main(){printf(\"ok\");return 0;}",
        L"int main(){int printf;printf(\"ok\");return 0;}",
        L"int main(){int printf(const char* a);printf(\"ok\");return 0;}",
        L"int main(){int (*printf)(const char*);printf(\"ok\");return 0;}",
        L"int main(int printf){printf(\"ok\");return 0;}",
        L"int main(){maro_object.printf(\"ok\");return 0;}",
        L"int main(){std::printf(\"ok\");return 0;}",
        L"namespace maro_space{int main(){printf(\"ok\");return 0;}}",
        L"int main(){printf(\"ok\");return 0;",
        L"int main(){printf(\"ok);return 0;}",
        L"int main(){int printf=1;return printf;}"})
    {
        const auto maro_request = maro_testRequest(maro_source);
        std::vector<Maro_Diagnostic> maro_diagnostics{maro_headerDiagnostic(maro_request, L"printf")};
        maro_ImproveDiagnostics(maro_request, maro_diagnostics);
        maro_expect(!maro_diagnostics.front().fix && maro_diagnostics.front().code == L"C3861",
            "custom headers, macros, declarations, members and incomplete code cannot receive a speculative header insertion");
    }
    const auto maro_sharedHeaderRequest = maro_testRequest(L"int main(){printf(\"a\");puts(\"b\");free(0);return unknown;}");
    std::vector<Maro_Diagnostic> maro_sharedHeaders{
        maro_headerDiagnostic(maro_sharedHeaderRequest, L"printf"),
        maro_headerDiagnostic(maro_sharedHeaderRequest, L"puts"),
        maro_headerDiagnostic(maro_sharedHeaderRequest, L"free"),
        maro_headerDiagnostic(maro_sharedHeaderRequest, L"unknown", L"C2065")};
    maro_ImproveDiagnostics(maro_sharedHeaderRequest, maro_sharedHeaders);
    maro_expect(maro_sharedHeaders.size() == 3 && maro_sharedHeaders.front().maro_relatedCount == 1 &&
        maro_sharedHeaders.front().fix && maro_sharedHeaders.front().fix->edits.size() == 1 &&
        maro_sharedHeaders.front().originalDiagnostic.find(L"'puts'") != std::wstring::npos &&
        maro_VerifyMissingHeaderFix(maro_sharedHeaderRequest, maro_sharedHeaders.front()) &&
        maro_sharedHeaders[1].fix && !maro_sharedHeaders[2].fix,
        "standard calls with the same missing header show one exact insertion root while another header and independent identifier remain");

    const auto maro_returnRequest = maro_testRequest(L"int main(){return 0\n}");
    const auto maro_returnFinding = maro_testAnalyze(maro_returnRequest.sourceText).front();
    auto maro_returnRoot = maro_testCompiler(maro_returnRequest, L"C2143", 1, maro_returnRequest.sourceText.find(L"return 0") + 9);
    maro_returnRoot.severity = Maro_Severity::Error;
    maro_returnRoot.fix = maro_returnFinding.fix;
    maro_returnRoot.friendlyMessage = L"문장 끝에 ';'가 필요합니다.";
    maro_returnRoot.originalDiagnostic = maro_returnRequest.sourcePath + L"(2,1): error C2143: syntax error: missing ';' before '}'";
    auto maro_returnCascade = maro_testCompiler(maro_returnRequest, L"C2059", 2, 1);
    maro_returnCascade.severity = Maro_Severity::Error;
    maro_returnCascade.originalDiagnostic = maro_returnRequest.sourcePath + L"(2,1): error C2059: syntax error: '}'";
    auto maro_returnIdentifier = maro_testCompiler(maro_returnRequest, L"C2065", 2, 1);
    maro_returnIdentifier.severity = Maro_Severity::Error;
    maro_returnIdentifier.originalDiagnostic = maro_returnRequest.sourcePath + L"(2,1): error C2065: 'unknown': undeclared identifier";
    std::vector<Maro_Diagnostic> maro_returnDiagnostics{maro_returnRoot, maro_returnCascade, maro_returnIdentifier};
    maro_ImproveDiagnostics(maro_returnRequest, maro_returnDiagnostics);
    const auto* maro_returnSourceRoot = maro_testFind(maro_returnDiagnostics, L"MARO-MISSING-SEMICOLON");
    maro_expect(maro_returnDiagnostics.size() == 2 && maro_returnSourceRoot && maro_returnSourceRoot->maro_relatedCount == 2 &&
        !maro_testFind(maro_returnDiagnostics, L"C2059") && maro_testFind(maro_returnDiagnostics, L"C2065"),
        "source semicolon root retains the proven compiler syntax cascade and never hides an independent identifier at that position");

    const auto maro_variableDiagnostic = [&](const Maro_SourceRequest& maro_request, std::size_t maro_offset) {
        const auto maro_position = maro_OffsetPosition(maro_request.sourceText, maro_offset);
        auto maro_diagnostic = maro_testCompiler(maro_request, L"C2065", maro_position.line, maro_position.column);
        maro_diagnostic.severity = Maro_Severity::Error;
        maro_diagnostic.originalDiagnostic = L"'value': undeclared identifier";
        return maro_diagnostic;
    };
    for (const auto* maro_declared : {L"vaule", L"valu", L"values", L"valve", L"Value"})
    {
        const auto maro_source = L"int main(){int " + std::wstring(maro_declared) + L"=7;return value;}";
        const auto maro_request = maro_testRequest(maro_source);
        std::vector<Maro_Diagnostic> maro_diagnostics{maro_variableDiagnostic(maro_request, maro_source.find(L"value;"))};
        maro_ImproveDiagnostics(maro_request, maro_diagnostics);
        const auto& maro_finding = maro_diagnostics.front();
        const auto maro_expected = L"int main(){int " + std::wstring(maro_declared) + L"=7;return " + maro_declared + L";}";
        maro_expect(maro_finding.code == L"C2065" && maro_finding.evidence == Maro_Evidence::Conditional &&
            maro_finding.friendlyMessage.starts_with(L"변수명이 다릅니다.") && maro_finding.friendlyMessage.size() <= 60 &&
            maro_testReplacement(maro_source, &maro_finding, maro_expected) && maro_VerifyVariableNameFix(maro_request, maro_finding) &&
            maro_ValidateFix(maro_request, maro_finding, 73, maro_request.sourcePath, maro_source).has_value(),
            "a unique visible local variable with one substitution insertion deletion or transposition gets a precise guarded reference repair");
        if (!maro_finding.fix) continue;
        for (int maro_mutation = 0; maro_mutation < 7; ++maro_mutation)
        {
            auto maro_tampered = maro_finding;
            if (maro_mutation == 0) maro_tampered.fix->edits.front().replacement = L"maro_unrelated";
            if (maro_mutation == 1) maro_tampered.code = L"C2664";
            if (maro_mutation == 2) maro_tampered.originalDiagnostic = L"'other': undeclared identifier";
            if (maro_mutation == 3) maro_tampered.range.start.column = 1;
            if (maro_mutation == 4) maro_tampered.range.generated = true;
            if (maro_mutation == 5) maro_tampered.fix->edits.front().startOffsetUtf16 = maro_source.find(maro_declared);
            if (maro_mutation == 6) maro_tampered.sourceVersion = 72;
            maro_expect(!maro_VerifyVariableNameFix(maro_request, maro_tampered) &&
                !maro_ValidateFix(maro_request, maro_tampered, 73, maro_request.sourcePath, maro_source),
                "altered variable fix cannot rename a declaration or an unrelated token or use stale compiler evidence");
        }
    }
    const auto maro_repeatedNameRequest = maro_testRequest(L"int main(){int vaule=7;value+=1;return value+other;}");
    const auto maro_firstValue = maro_repeatedNameRequest.sourceText.find(L"value");
    const auto maro_secondValue = maro_repeatedNameRequest.sourceText.find(L"value", maro_firstValue + 1);
    auto maro_otherName = maro_variableDiagnostic(maro_repeatedNameRequest, maro_repeatedNameRequest.sourceText.find(L"other"));
    maro_otherName.originalDiagnostic = L"'other': undeclared identifier";
    std::vector<Maro_Diagnostic> maro_repeatedNames{
        maro_variableDiagnostic(maro_repeatedNameRequest, maro_firstValue),
        maro_variableDiagnostic(maro_repeatedNameRequest, maro_secondValue), maro_otherName};
    maro_ImproveDiagnostics(maro_repeatedNameRequest, maro_repeatedNames);
    maro_expect(maro_repeatedNames.size() == 2 && maro_repeatedNames.front().maro_relatedCount == 1 &&
        maro_repeatedNames.front().fix && maro_repeatedNames.front().fix->edits.size() == 2 &&
        maro_VerifyVariableNameFix(maro_repeatedNameRequest, maro_repeatedNames.front()) &&
        maro_testReplacement(maro_repeatedNameRequest.sourceText, &maro_repeatedNames.front(),
            L"int main(){int vaule=7;vaule+=1;return vaule+other;}") && !maro_repeatedNames.back().fix,
        "compiler-confirmed references to the same mismatched local name share one root and one validated multi-edit repair");
    for (const auto* maro_source : {
        L"int main(){int valua=1;int valub=2;return value;}",
        L"int main(){{int vaule=1;}return value;}",
        L"int main(){return value;int vaule=1;}",
        L"int main(){int vaule=value;return 0;}",
        L"int main(){int vaule=7+value;return 0;}",
        L"int vaule=1;int main(){return value;}",
        L"int main(int value){int vaule=1;return value;}",
        L"int value=1;int main(){int vaule=1;return value;}",
        L"int main(){int value=1;int vaule=1;return value;}",
        L"int main(){int vaule=1;return maro_object.value;}",
        L"int main(){int vaule=1;return value();}",
        L"#include \"maro_custom.h\"\nint main(){int vaule=1;return value;}",
        L"#define MARO_NUMBER 7\nint main(){int vaule=1;return value;}",
        L"int main(){typedef int maro_type;int vaule=1;return value;}"})
    {
        const auto maro_request = maro_testRequest(maro_source);
        const auto maro_offset = maro_request.sourceText.rfind(L"value");
        std::vector<Maro_Diagnostic> maro_diagnostics{maro_variableDiagnostic(maro_request, maro_offset)};
        maro_ImproveDiagnostics(maro_request, maro_diagnostics);
        maro_expect(!maro_diagnostics.front().fix && !maro_diagnostics.front().friendlyMessage.starts_with(L"변수명이 다릅니다."),
            "ambiguous candidates, invisible declarations, real bindings, members, functions and preprocessing do not guess a variable rename");
    }
    const auto maro_upperRequest = maro_testRequest(L"int main(){int value=7;return Value;}");
    auto maro_upperDiagnostic = maro_variableDiagnostic(maro_upperRequest, maro_upperRequest.sourceText.find(L"Value"));
    maro_upperDiagnostic.originalDiagnostic = L"'Value': undeclared identifier";
    std::vector<Maro_Diagnostic> maro_upperFindings{maro_upperDiagnostic};
    maro_ImproveDiagnostics(maro_upperRequest, maro_upperFindings);
    maro_expect(maro_upperFindings.front().fix && maro_VerifyVariableNameFix(maro_upperRequest, maro_upperFindings.front()),
        "uppercase reference names are compared exactly rather than lowercased compiler text");
    const auto maro_pseudoRequest = maro_testRequest(L"int main(){return vaule;return value;}");
    std::vector<Maro_Diagnostic> maro_pseudoFindings{
        maro_variableDiagnostic(maro_pseudoRequest, maro_pseudoRequest.sourceText.find(L"value"))};
    maro_ImproveDiagnostics(maro_pseudoRequest, maro_pseudoFindings);
    maro_expect(!maro_pseudoFindings.front().fix, "return expressions cannot be mistaken for variable declarations");
    for (const auto& maro_case : {
        std::pair{L"int main(){int a=7;int* ptr=&a;return *prt;}", L"C2100"},
        std::pair{L"int main(){int array[2]={7,8};return arary[0];}", L"C2109"},
        std::pair{L"int main(){int count=7;return coutn[0];}", L"C2109"}})
    {
        const auto maro_request = maro_testRequest(maro_case.first);
        const auto maro_name = maro_case.second == std::wstring_view(L"C2100") ? L"prt" :
            (maro_request.sourceText.find(L"arary") != std::wstring::npos ? L"arary" : L"coutn");
        const auto maro_offset = maro_request.sourceText.find(maro_name);
        auto maro_primary = maro_variableDiagnostic(maro_request, maro_offset);
        maro_primary.originalDiagnostic = L"'" + std::wstring(maro_name) + L"': undeclared identifier";
        maro_primary.analyzer = L"MSBuild";
        const auto maro_secondaryOffset = maro_case.second == std::wstring_view(L"C2100") ? maro_offset - 1 : maro_offset + std::wstring_view(maro_name).size();
        const auto maro_position = maro_OffsetPosition(maro_request.sourceText, maro_secondaryOffset);
        auto maro_secondary = maro_testCompiler(maro_request, maro_case.second, maro_position.line, maro_position.column);
        maro_secondary.severity = Maro_Severity::Error;
        maro_secondary.analyzer = L"MSBuild";
        std::vector<Maro_Diagnostic> maro_operandFindings{maro_primary, maro_secondary};
        maro_ImproveDiagnostics(maro_request, maro_operandFindings);
        const bool maro_pointer = std::wstring_view(maro_name) != L"coutn";
        maro_expect(maro_operandFindings.front().fix && maro_VerifyVariableNameFix(maro_request, maro_operandFindings.front()) &&
            maro_operandFindings.size() == (maro_pointer ? 1u : 2u),
            "MSBuild variable-name repairs hide only source-proven pointer or array operand cascades and keep independent type errors");
    }
    maro_expect(maro_testAnalyze(std::wstring(262145, L' ')).empty(), "oversized source skips bounded local checks");
    return maro_passed;
}
