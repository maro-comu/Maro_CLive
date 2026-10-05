#pragma once

#include "maro_Models.hpp"

void maro_ImproveDiagnostics(const Maro_SourceRequest& maro_request, std::vector<Maro_Diagnostic>& maro_diagnostics);
bool maro_VerifyMissingHeaderFix(const Maro_SourceRequest& maro_request, const Maro_Diagnostic& maro_diagnostic);
bool maro_VerifyVariableNameFix(const Maro_SourceRequest& maro_request, const Maro_Diagnostic& maro_diagnostic);
bool maro_TestCodeDiagnostics();
