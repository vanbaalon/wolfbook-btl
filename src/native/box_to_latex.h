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

// Style options for boxToLatex — all default to enabled.
// Pass from JS as the second argument to boxToLatex():
//   boxToLatex(wlString, { trigOmitParens: false, trigPowerForm: false })
struct BtlOptions {
    // Rule 1: \sin(\phi) → \sin\phi  (omit parens when arg is a single symbol)
    bool trigOmitParens = true;
    // Rule 2: (\sin\phi)^n → \sin^n\phi  (move exponent onto trig command)
    bool trigPowerForm  = true;
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
