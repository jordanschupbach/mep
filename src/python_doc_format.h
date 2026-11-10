#pragma once

#include <string>

// Display formatting for Python hover documentation (plain docstrings,
// NumPy/Google/reST sections, and Markdown). Reflows prose and list items,
// retaining code blocks, doctest output, tables, and significant indentation.
// Long code lines remain intact; the popup clips them and focused navigation
// can pan across them. This never edits the source document.
std::string FormatPythonDoc(const std::string &text, int width = 72);

// Same budget for formatting and rendering, allowing 40px screen margin and
// 20px popup padding. Unknown/headless font metrics retain the requested width.
int PythonDocColumns(int requested, int screen_width, float char_width);
