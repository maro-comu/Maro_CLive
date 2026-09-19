#include "maro_DiagnosticFilter.hpp"

#include <functional>
#include <limits>
#include <string_view>

namespace
{
Maro_Diagnostic maro_DiagnosticFixture(std::wstring maro_code, std::wstring maro_message,
    std::size_t maro_line = 4, std::size_t maro_column = 5, std::wstring maro_path = L"C:\\project\\maro_main.c")
{
    Maro_Diagnostic maro_result;
    maro_result.sourceVersion = 7;
    maro_result.sourcePath = maro_path;
    maro_result.analyzer = L"MSVC";
    maro_result.analyzerVersion = L"19.50";
    maro_result.code = std::move(maro_code);
    maro_result.severity = Maro_Severity::Error;
    maro_result.range.start = {maro_line, maro_column};
    maro_result.range.end = maro_result.range.start;
    maro_result.originalDiagnostic = maro_path + L"(" + std::to_wstring(maro_line) + L"," +
        std::to_wstring(maro_column) + L"): error " + maro_result.code + L": " + maro_message;
    maro_result.friendlyMessage = std::move(maro_message);
    return maro_result;
}
}

void maro_TestDiagnosticFilter(const std::function<void(bool, std::string_view)>& maro_expect)
{
    auto maro_root = maro_DiagnosticFixture(L"C2143", L"syntax error: missing ';' before 'return'");
    maro_root.range.start = {3, 20};
    maro_root.range.end = maro_root.range.start;
    maro_root.friendlyMessage = L"문장 끝에 ';'가 필요합니다.\r\n" + maro_root.friendlyMessage;
    const auto maro_cascade = maro_DiagnosticFixture(L"C2059", L"syntax error: 'return'");
    const auto maro_secondRoot = maro_DiagnosticFixture(L"C2146", L"syntax error: missing ';' before identifier 'return'");
    const auto maro_grouped = maro_GroupDiagnostics({maro_root, maro_cascade, maro_secondRoot, maro_cascade});
    maro_expect(maro_grouped.size() == 1 && maro_grouped[0].maro_relatedCount == 3 &&
        maro_grouped[0].range.start.line == 3 && maro_grouped[0].originalDiagnostic == maro_root.originalDiagnostic,
        "diagnostic grouping retains precise semicolon root and folds same-position syntax cascades");
    maro_expect(maro_GroupDiagnostics(maro_grouped)[0].maro_relatedCount == 3,
        "diagnostic grouping is idempotent and preserves related count");
    const auto maro_reordered = maro_GroupDiagnostics({maro_cascade, maro_root});
    maro_expect(maro_reordered.size() == 1 && maro_reordered[0].code == L"C2143" && maro_reordered[0].range.start.line == 3,
        "diagnostic root can follow its secondary syntax diagnostic");

    const auto maro_unknownA = maro_DiagnosticFixture(L"C2065", L"'maro_a': undeclared identifier");
    const auto maro_unknownB = maro_DiagnosticFixture(L"C2065", L"'maro_b': undeclared identifier");
    maro_expect(maro_GroupDiagnostics({maro_root, maro_cascade, maro_unknownA, maro_unknownB}).size() == 3,
        "diagnostic grouping keeps independent identifiers at the same compiler position");
    const auto maro_nearby = maro_DiagnosticFixture(L"C2059", L"syntax error: 'return'", 5);
    const auto maro_otherColumn = maro_DiagnosticFixture(L"C2059", L"syntax error: 'return'", 4, 25);
    const auto maro_otherToken = maro_DiagnosticFixture(L"C2059", L"syntax error: 'else'");
    maro_expect(maro_GroupDiagnostics({maro_root, maro_nearby, maro_otherColumn, maro_otherToken}).size() == 4,
        "diagnostic grouping never merges merely nearby lines columns or different tokens");
    const auto maro_otherFile = maro_DiagnosticFixture(L"C2059", L"syntax error: 'return'", 4, 5, L"C:\\project\\maro_header.h");
    maro_expect(maro_GroupDiagnostics({maro_root, maro_otherFile}).size() == 2,
        "diagnostic grouping preserves independent header errors");
    auto maro_otherVersion = maro_cascade;
    ++maro_otherVersion.sourceVersion;
    auto maro_otherAnalyzer = maro_cascade;
    maro_otherAnalyzer.analyzer = L"Other compiler";
    maro_expect(maro_GroupDiagnostics({maro_root, maro_otherVersion, maro_otherAnalyzer}).size() == 3,
        "diagnostic grouping does not combine versions or analyzers");
    auto maro_warning = maro_cascade;
    maro_warning.severity = Maro_Severity::Warning;
    auto maro_information = maro_cascade;
    maro_information.severity = Maro_Severity::Info;
    maro_expect(maro_GroupDiagnostics({maro_root, maro_warning, maro_warning, maro_information}).size() == 4,
        "diagnostic grouping retains warnings and contextual notes unchanged");
    auto maro_linkA = maro_DiagnosticFixture(L"LNK2019", L"unresolved external symbol maro_a");
    auto maro_linkB = maro_DiagnosticFixture(L"LNK2019", L"unresolved external symbol maro_b");
    maro_expect(maro_GroupDiagnostics({maro_root, maro_linkA, maro_linkB}).size() == 3,
        "diagnostic grouping keeps distinct linker failures");
    const auto maro_ambiguous = maro_DiagnosticFixture(L"C2143", L"syntax error: missing ')' before 'return'");
    maro_expect(maro_GroupDiagnostics({maro_root, maro_ambiguous, maro_cascade}).size() == 3,
        "diagnostic grouping preserves ambiguous competing missing-token causes");
    auto maro_noLocation = maro_cascade;
    maro_noLocation.originalDiagnostic = L"cl : error C2059: syntax error: 'return'";
    maro_expect(maro_GroupDiagnostics({maro_root, maro_noLocation}).size() == 2,
        "diagnostic grouping requires an original compiler source location");
    auto maro_wrongOriginal = maro_cascade;
    maro_wrongOriginal.originalDiagnostic = L"C:\\other\\maro_main.c(4,5): error C2059: syntax error: 'return'";
    maro_expect(maro_GroupDiagnostics({maro_root, maro_wrongOriginal}).size() == 2,
        "diagnostic grouping retains distinct original snapshot paths despite remapped locations");
    const auto maro_koreanRoot = maro_DiagnosticFixture(L"C2143", L"구문 오류: ';'이(가) 'return' 앞에 없습니다.");
    const auto maro_koreanCascade = maro_DiagnosticFixture(L"C2059", L"구문 오류: 'return'");
    maro_expect(maro_GroupDiagnostics({maro_koreanRoot, maro_koreanCascade}).size() == 1,
        "diagnostic grouping supports Korean compiler messages without English-only matching");
    auto maro_identifierRoot = maro_DiagnosticFixture(L"C2146", L"syntax error: missing ';' before identifier 'maro_value'");
    const auto maro_identifierCascade = maro_DiagnosticFixture(L"C2061", L"syntax error: identifier 'maro_value'");
    maro_expect(maro_GroupDiagnostics({maro_identifierRoot, maro_identifierCascade}).size() == 2,
        "diagnostic grouping keeps unproven missing-type versus identifier errors separate");
    maro_identifierRoot.friendlyMessage = L"문장 끝에 ';'가 필요합니다.\r\n" + maro_identifierRoot.friendlyMessage;
    maro_identifierRoot.range.start = {3, 20};
    maro_expect(maro_GroupDiagnostics({maro_identifierRoot, maro_identifierCascade}).size() == 1,
        "diagnostic grouping folds identifier syntax only after source-confirmed semicolon remapping");
    const auto maro_missingType = maro_DiagnosticFixture(L"C4430", L"missing type specifier - int assumed");
    maro_expect(maro_GroupDiagnostics({maro_identifierRoot, maro_missingType}).size() == 2,
        "diagnostic grouping preserves unanchored missing-type errors");
    const auto maro_braceRoot = maro_DiagnosticFixture(L"C2143", L"syntax error: missing ';' before '{'");
    const auto maro_braceCascade = maro_DiagnosticFixture(L"C2447", L"'{': missing function header (old-style formal list?)");
    maro_expect(maro_GroupDiagnostics({maro_braceRoot, maro_braceCascade}).size() == 1,
        "diagnostic grouping folds a matching missing-semicolon function-header cascade");
    auto maro_generated = maro_cascade;
    maro_generated.range.generated = true;
    maro_expect(maro_GroupDiagnostics({maro_root, maro_generated}).size() == 2,
        "diagnostic grouping keeps generated-only errors distinct from user source");
    auto maro_saturated = maro_root;
    maro_saturated.maro_relatedCount = (std::numeric_limits<std::size_t>::max)();
    const auto maro_saturatedGroup = maro_GroupDiagnostics({maro_saturated, maro_root});
    maro_expect(maro_saturatedGroup.size() == 1 && maro_saturatedGroup[0].maro_relatedCount == maro_saturated.maro_relatedCount,
        "diagnostic related count saturates without integer overflow");
    auto maro_malformed = maro_cascade;
    maro_malformed.originalDiagnostic = L"C:\\main.c(999999999999999999999999999999,5): error C2059: syntax error: 'return'";
    maro_expect(maro_GroupDiagnostics({maro_root, maro_malformed}).size() == 2,
        "diagnostic grouping rejects overflowing compiler coordinates safely");
    const std::vector<Maro_Diagnostic> maro_repeated(4096, maro_root);
    const auto maro_compacted = maro_GroupDiagnostics(maro_repeated);
    maro_expect(maro_compacted.size() == 1 && maro_compacted[0].maro_relatedCount == 4095,
        "diagnostic grouping compacts repeated compiler errors with bounded per-item lookup work");
    maro_expect(maro_GroupDiagnostics({}).empty(), "diagnostic grouping accepts an empty result");
}
