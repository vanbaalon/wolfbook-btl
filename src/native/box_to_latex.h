// =============================================================
// box_to_latex.h  —  Wolfbook AST → LaTeX translator
//
// Entry point:
//   BoxResult boxToLatex(std::string_view wlBoxString);
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
#include <vector>

namespace wolfbook {

// Result returned by boxToLatex.
// On success: latex contains the LaTeX string, error is empty.
// On failure: latex contains the raw input (verbatim pass-through),
//             error contains the diagnostic message.
// When maxRows paging is active and the top-level matrix has more rows
// than maxRows, pages contains one complete LaTeX string per page
// (each wrapped in the correct \begin{env}…\end{env}).  In that case
// `latex` holds the first page for backward-compat single-string callers.
struct BoxResult {
    std::string latex;
    std::string error;  // empty string means no error
    std::vector<std::string> pages;  // non-empty only when paging was triggered
};

// Style options for boxToLatex — all default to enabled.
// Pass from JS as the second argument to boxToLatex():
//   boxToLatex(wlString, { trigOmitParens: false, trigPowerForm: false })
struct BtlOptions {
    // Rule 1: \sin(\phi) → \sin\phi  (omit parens when arg is a single symbol)
    bool trigOmitParens = true;
    // Rule 2: (\sin\phi)^n → \sin^n\phi  (move exponent onto trig command)
    bool trigPowerForm  = true;
    // Paging: when > 0, the outermost matrix GridBox is split into pages of
    // at most maxRows rows each.  0 = no paging (default).
    int  maxRows = 0;
};

// Translate a Wolfram Language box-expression string (as produced
// by  ToString[ToBoxes[expr, TraditionalForm], InputForm]  on the
// Mathematica kernel) to a LaTeX string suitable for KaTeX.
//
// Never throws on bad input — returns a best-effort string.
// Diagnostic messages go to stderr AND are available in result.error.
BoxResult boxToLatex(std::string_view wlBoxString,
                     const BtlOptions& opts = BtlOptions{});

} // namespace wolfbook
