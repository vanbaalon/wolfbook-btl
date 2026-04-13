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
#include <vector>

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
    // Paging: when > 0 and line-broken output has more lines than this, the
    // result is split into pages of at most maxRows lines each.
    int    maxRows       = 0;
    // Which page to return (0-indexed). Only used when maxRows > 0.
    // Caller requests exactly one page; totalPages in the result tells how
    // many pages exist so the client can build prev/next navigation.
    int    requestedPage = 0;
    // When true and paging is active, compute ALL pages in one call and
    // return them in result.pages[]. Avoids re-tokenizing for each page.
    bool   allPages      = false;
};

// Result returned by lineBreakLatex.
// When maxRows paging is not triggered, totalPages == 1 and result holds
// the full (possibly multi-line aligned) LaTeX string.
// When paging is triggered, result holds the LaTeX for opts.requestedPage
// and totalPages gives the total number of pages available.
struct LineBreakResult {
    std::string result;
    int         totalPages = 1;  // 1 = no paging; >1 = use prev/next navigation
    // When allPages was requested and paging is active, holds all page LaTeX strings.
    // Empty when allPages is false or paging was not triggered.
    std::vector<std::string> pages;
};

// Apply line-breaking to a single-line LaTeX string.
// Returns the original string unchanged if it fits within pageWidth
// or if no suitable breakpoints are found.
// When opts.maxRows > 0 and the broken result has more lines than maxRows,
// the output is split into pages (each a valid aligned environment with
// gray \cdots continuation markers and balanced \left./\right. delimiters).
LineBreakResult lineBreakLatex(std::string_view latex,
                               const LineBreakOptions& opts);

} // namespace wolfbook
