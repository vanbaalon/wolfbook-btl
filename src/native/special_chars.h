// =============================================================
// special_chars.h  —  Wolfbook WL named-char → LaTeX lookup
// =============================================================
#pragma once
#include <string_view>

namespace wolfbook {

// Returns the LaTeX string for a Wolfram named-character token
// such as "\\[Alpha]" or "\\[Infinity]".
// Returns an empty string_view if the token is not in the table.
std::string_view wlCharToLatex(std::string_view wlToken);

// Returns true if the given LaTeX command is a large operator
// (\\sum, \\prod, \\int, …) that should take limits via
// subscript/superscript syntax in display mode.
bool isLargeOperator(std::string_view latexCmd);

} // namespace wolfbook
