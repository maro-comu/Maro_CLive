#pragma once

#include <string>
#include <string_view>

std::wstring maro_CompilerGuidance(std::wstring_view maro_code, std::wstring_view maro_raw);
bool maro_TestDiagnosticGuidance();
