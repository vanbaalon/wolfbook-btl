// =============================================================
// line_breaker.h  —  Post-processing line-break layer for BTL
//
// Takes a single-line LaTeX string (as produced by boxToLatex)
// and, if it exceeds pageWidth, breaks it into a multi-line
// aligned/multline environment following mathematical
// typesetting conventions (break at relations, then binops).
//
// This is a pure string-to-string transformation — it does not
// need the parsed box tree, only the flat LaTeX output.
// =============================================================
#pragma once
#include <string>
#include <string_view>

namespace wolfbook {

struct LineBreakOptions {
    double pageWidth     = 80.0;  // target width in approximate em units
    double indentStep    = 2.0;   // continuation line indent (em)
    bool   compact       = false; // prefer fewer lines over aligned breaks
    int    maxDelimDepth = 2;     // max delimiter nesting for breaks
    // Pixel-based width (takes priority over pageWidth when > 0)
    double pageWidthPx    = 0.0;  // CSS pixel target (0 = disabled)
    double baseFontSizePx = 16.0; // em↔px: effectivePageWidth = pageWidthPx / baseFontSizePx
    int    maxIterations  = 0;    // for TypeScript iterative wrapper
};

// Apply line-breaking to a single-line LaTeX string.
// Returns the original string unchanged if it fits within pageWidth
// or if no suitable breakpoints are found.
std::string lineBreakLatex(std::string_view latex,
                           const LineBreakOptions& opts);

} // namespace wolfbook
