#pragma once

#include "maro_Models.hpp"

void maro_ImproveDiagnostics(const Maro_SourceRequest& maro_request, std::vector<Maro_Diagnostic>& maro_diagnostics);
bool maro_TestCodeDiagnostics();
