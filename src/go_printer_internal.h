#pragma once

// Internal to the go_printer*.cpp files: go/build/constraint and the
// //go:build line fixer (go_printer_build.cpp).

#include <string>
#include <string_view>
#include <vector>

namespace goprint {

/** @brief constraint.IsGoBuild. */
bool IsGoBuild(std::string_view line);
/** @brief constraint.IsPlusBuild. */
bool IsPlusBuild(std::string_view line);
/** @brief printer.fixGoBuildLines over the tabwriter input in `output`. */
void FixGoBuildLines(std::string &output, const std::vector<int> &go_build, const std::vector<int> &plus_build);

}  // namespace goprint
