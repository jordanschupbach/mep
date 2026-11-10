#pragma once

#include "r_lsp.h"

// Optional wire-server fallback. Reads installed Rd documentation with Rscript;
// neither sources the user's script nor attaches the documented package.
// Returns empty on missing R/package/topic, subprocess failure, or timeout.
std::string RLspInstalledHelp(const RLspHoverInfo &info, const std::string &cwd);
