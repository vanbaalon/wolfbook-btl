// =============================================================
// box_to_latex.h  —  Wolfbook AST → LaTeX translator
//
// Entry point:
//   std::string boxToLatex(std::string_view wlBoxString);
//
// The function parses the WL InputForm string with WLParser,
// then walks the resulting AST and emits a LaTeX string.
//
// All translation state is kept in a BoxTranslator object so
// the free function is reentrant (one object per call).
// =============================================================
#pragma once
#include <string>
#include <string_view>

namespace wolfbook {

// Result returned by boxToLatex.
// On success: latex contains the LaTeX string, error is empty.
// On failure: latex contains the raw input (verbatim pass-through),
//             error contains the diagnostic message.
struct BoxResult {
    std::string latex;
    std::string error;  // empty string means no error
};

// Translate a Wolfram Language box-expression string (as produced
// by  ToString[ToBoxes[expr, TraditionalForm], InputForm]  on the
// Mathematica kernel) to a LaTeX string suitable for KaTeX.
//
// Never throws on bad input — returns a best-effort string.
// Diagnostic messages go to stderr AND are available in result.error.
BoxResult boxToLatex(std::string_view wlBoxString);

} // namespace wolfbook
