// =============================================================
// line_breaker.cpp  —  Post-processing line-break layer for BTL
//
// Scans a flat LaTeX string, identifies candidate breakpoints
// (relations, binary operators, commas), estimates widths, and
// uses a simplified Knuth-Plass DP to find optimal line breaks.
// Emits an \begin{aligned}...\end{aligned} or \begin{gathered}
// environment with \\ separators.
//
// This file has NO dependency on Node.js or the WL parser.
// =============================================================
#include "line_breaker.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace wolfbook {
namespace {

// ----------------------------------------------------------
// Break classification
// ----------------------------------------------------------
enum class BreakClass : uint8_t {
    None,
    Relation,   // =, <, >, \leq, \geq, \neq, \equiv, \sim, ...
    BinOp,      // +, -, \pm, \times, \cdot, ...
    Comma,      // ,
    Open,       // (, [, \{, \langle, ...
    Close,      // ), ], \}, \rangle, ...
};

// ----------------------------------------------------------
// Token — a logical chunk of the LaTeX string
// ----------------------------------------------------------
struct Token {
    size_t     start;        // byte offset in source string
    size_t     len;          // byte length
    double     width;        // estimated width in "em"
    BreakClass breakClass;
    int        delimDepth;   // nesting depth at this token
};

// ----------------------------------------------------------
// Breakpoint candidate for the DP
// ----------------------------------------------------------
struct Breakpoint {
    size_t   tokenIndex;     // index into token array
    double   cumWidth;       // cumulative width up to (and including) this token
    double   penalty;        // break penalty
    int      delimDepth;
    BreakClass breakClass;
};

// ----------------------------------------------------------
// Symbol tables
// ----------------------------------------------------------

// Relation commands: break BEFORE these (low penalty)
static bool isRelationCmd(std::string_view cmd) {
    static const std::string_view rels[] = {
        "=", "<", ">", "\\leq", "\\geq", "\\neq", "\\ne",
        "\\equiv", "\\sim", "\\simeq", "\\approx", "\\cong",
        "\\propto", "\\in", "\\ni", "\\notin",
        "\\subset", "\\supset", "\\subseteq", "\\supseteq",
        "\\vdash", "\\models", "\\to", "\\mapsto",
        "\\coloneq", "\\coloneqq", ":=",
        "\\rightarrow", "\\leftarrow", "\\Rightarrow", "\\Leftarrow",
        "\\leftrightarrow", "\\Leftrightarrow",
        "\\le", "\\ge",
    };
    for (auto& r : rels) if (cmd == r) return true;
    return false;
}

// Binary operator commands: break BEFORE these (moderate penalty)
static bool isBinOpCmd(std::string_view cmd) {
    static const std::string_view ops[] = {
        "+", "-", "\\pm", "\\mp", "\\times", "\\cdot", "\\div",
        "\\cup", "\\cap", "\\wedge", "\\vee",
        "\\oplus", "\\otimes", "\\circ", "\\bullet",
        "\\setminus", "\\star",
    };
    for (auto& o : ops) if (cmd == o) return true;
    return false;
}

// Opening delimiters
static bool isOpenDelim(std::string_view cmd) {
    static const std::string_view opens[] = {
        "(", "[", "\\{", "\\langle", "\\lvert", "\\lVert",
        "\\lfloor", "\\lceil", "\\left(",  "\\left[",
        "\\left\\{", "\\left\\langle", "\\bigl(", "\\Bigl(",
    };
    for (auto& o : opens) if (cmd == o) return true;
    return false;
}

// Closing delimiters
static bool isCloseDelim(std::string_view cmd) {
    static const std::string_view closes[] = {
        ")", "]", "\\}", "\\rangle", "\\rvert", "\\rVert",
        "\\rfloor", "\\rceil", "\\right)", "\\right]",
        "\\right\\}", "\\right\\rangle", "\\bigr)", "\\Bigr)",
    };
    for (auto& c : closes) if (cmd == c) return true;
    return false;
}

// Commands where we must NOT break inside their braced arguments
static bool isAtomicCmd(std::string_view cmd) {
    static const std::string_view atomics[] = {
        "\\frac", "\\dfrac", "\\tfrac", "\\binom",
        "\\sqrt", "\\text", "\\mathrm", "\\mathit", "\\mathbf",
        "\\mathbb", "\\mathcal", "\\mathfrak", "\\mathsf",
        "\\operatorname", "\\hat", "\\bar", "\\vec", "\\tilde",
        "\\dot", "\\ddot", "\\overline", "\\underline",
        "\\overbrace", "\\underbrace", "\\overset", "\\underset",
        "\\stackrel", "\\color", "\\textcolor",
    };
    for (auto& a : atomics) if (cmd == a) return true;
    return false;
}

// ----------------------------------------------------------
// Width estimation
// ----------------------------------------------------------

// Estimated width in "em" for a single LaTeX command
static double cmdWidth(std::string_view cmd) {
    if (cmd.empty()) return 0.0;

    // Single character
    if (cmd.size() == 1) {
        char c = cmd[0];
        if (c >= '0' && c <= '9') return 0.5;
        if (c >= 'a' && c <= 'z') return 0.5;
        if (c >= 'A' && c <= 'Z') return 0.65;
        if (c == '+' || c == '-' || c == '=') return 0.7;
        if (c == '(' || c == ')') return 0.35;
        if (c == '[' || c == ']') return 0.3;
        if (c == ',') return 0.28;
        if (c == ' ') return 0.25;
        return 0.5;
    }

    // Known wide symbols
    if (cmd == "\\sum" || cmd == "\\prod") return 1.2;
    if (cmd == "\\int" || cmd == "\\intop") return 1.0;
    if (cmd == "\\infty") return 1.0;
    if (cmd == "\\partial") return 0.6;

    // Greek letters: ~0.6 em
    if (cmd.size() > 1 && cmd[0] == '\\' &&
        std::islower(static_cast<unsigned char>(cmd[1])))
        return 0.6;

    // Uppercase Greek / operator names
    if (cmd.size() > 1 && cmd[0] == '\\' &&
        std::isupper(static_cast<unsigned char>(cmd[1])))
        return 0.8;

    // \, \; \: \! thin/med/thick/neg space
    if (cmd == "\\," || cmd == "\\:" || cmd == "\\;") return 0.17;
    if (cmd == "\\!") return -0.06;
    if (cmd == "\\quad") return 1.0;
    if (cmd == "\\qquad") return 2.0;

    // Default for unknown commands
    return 0.7;
}

// Width of a braced group {...}: sum of inner token widths
// (Caller handles the recursion; we just need the overhead)
static constexpr double kBraceOverhead = 0.0; // braces are invisible in math mode

// ----------------------------------------------------------
// Tokenizer — breaks LaTeX string into logical tokens
// ----------------------------------------------------------

// Skip a balanced brace group starting at pos (which must be '{').
// Returns the position one past the closing '}'.
static size_t skipBraceGroup(std::string_view s, size_t pos) {
    assert(pos < s.size() && s[pos] == '{');
    int depth = 1;
    size_t i = pos + 1;
    while (i < s.size() && depth > 0) {
        if      (s[i] == '{') ++depth;
        else if (s[i] == '}') --depth;
        ++i;
    }
    return i;
}

// Read a \command sequence starting at pos (which must be '\\').
// Returns the extent: pos..(pos+len).
static size_t readCommand(std::string_view s, size_t pos) {
    assert(pos < s.size() && s[pos] == '\\');
    if (pos + 1 >= s.size()) return pos + 1;
    // Single non-alpha char after backslash: \, \; \{ etc.
    if (!std::isalpha(static_cast<unsigned char>(s[pos + 1]))) {
        return pos + 2;
    }
    // Multi-char alpha command: \alpha, \frac, etc.
    size_t i = pos + 1;
    while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
    return i;
}

struct Tokenizer {
    std::string_view src;
    size_t           pos = 0;

    bool atEnd() const { return pos >= src.size(); }

    // Produce the next token.  Advances pos.
    // Returns false when at end.
    bool next(Token& out) {
        if (pos >= src.size()) return false;

        out.start = pos;
        out.breakClass = BreakClass::None;
        out.delimDepth = 0; // caller sets this
        out.width = 0.0;

        char c = src[pos];

        // ── Case 1: backslash command ──
        if (c == '\\') {
            // Check for \begin or \end (environments are atomic)
            auto remaining = src.substr(pos);
            if ((remaining.size() >= 7 && remaining.substr(0, 7) == "\\begin{") ||
                (remaining.size() >= 5 && remaining.substr(0, 5) == "\\end{")) {
                // Skip through the closing }
                size_t braceStart = src.find('{', pos);
                size_t braceEnd = src.find('}', braceStart);
                if (braceEnd != std::string_view::npos) {
                    pos = braceEnd + 1;
                    out.len = pos - out.start;
                    out.width = 0.0; // environments contribute via content
                    return true;
                }
            }

            size_t cmdEnd = readCommand(src, pos);
            std::string_view cmd = src.substr(pos, cmdEnd - pos);

            // Atomic commands: consume their braced arguments as one token
            if (isAtomicCmd(cmd)) {
                pos = cmdEnd;
                // Consume all immediately following brace groups
                while (pos < src.size() && src[pos] == '{') {
                    pos = skipBraceGroup(src, pos);
                }
                out.len = pos - out.start;
                out.width = estimateAtomicWidth(src.substr(out.start, out.len));
                return true;
            }

            // Left/right with delimiter
            if (cmd == "\\left" || cmd == "\\right" ||
                cmd == "\\bigl" || cmd == "\\bigr" ||
                cmd == "\\Bigl" || cmd == "\\Bigr" ||
                cmd == "\\biggl" || cmd == "\\biggr") {
                // Consume the delimiter that follows
                if (cmdEnd < src.size()) {
                    if (src[cmdEnd] == '\\') {
                        cmdEnd = readCommand(src, cmdEnd);
                    } else {
                        cmdEnd++;
                    }
                }
                std::string_view full = src.substr(pos, cmdEnd - pos);
                pos = cmdEnd;
                out.len = pos - out.start;
                out.width = 0.4;
                if (isOpenDelim(full))  out.breakClass = BreakClass::Open;
                if (isCloseDelim(full)) out.breakClass = BreakClass::Close;
                return true;
            }

            // Regular command
            pos = cmdEnd;
            out.len = pos - out.start;
            out.width = cmdWidth(cmd);

            // Classify
            if (isRelationCmd(cmd))  out.breakClass = BreakClass::Relation;
            else if (isBinOpCmd(cmd)) out.breakClass = BreakClass::BinOp;
            else if (isOpenDelim(cmd))  out.breakClass = BreakClass::Open;
            else if (isCloseDelim(cmd)) out.breakClass = BreakClass::Close;
            return true;
        }

        // ── Case 2: braced group (not preceded by a command) ──
        if (c == '{') {
            pos = skipBraceGroup(src, pos);
            out.len = pos - out.start;
            // Estimate width of group contents
            out.width = estimateBracedWidth(src.substr(out.start + 1,
                                                        out.len - 2));
            return true;
        }

        // ── Case 3: subscript / superscript ──
        if (c == '_' || c == '^') {
            pos++;
            // Consume the argument (single char or brace group)
            if (pos < src.size()) {
                if (src[pos] == '{') {
                    pos = skipBraceGroup(src, pos);
                } else if (src[pos] == '\\') {
                    pos = readCommand(src, pos);
                } else {
                    pos++;
                }
            }
            out.len = pos - out.start;
            // Scripts are rendered at 0.7× scale
            out.width = estimateBracedWidth(src.substr(out.start + 1,
                                                        out.len - 1)) * 0.7;
            return true;
        }

        // ── Case 4: single characters ──
        pos++;
        out.len = 1;
        std::string_view ch = src.substr(out.start, 1);
        out.width = cmdWidth(ch);

        // Classify single chars
        if (isRelationCmd(ch))       out.breakClass = BreakClass::Relation;
        else if (isBinOpCmd(ch))     out.breakClass = BreakClass::BinOp;
        else if (c == ',')           out.breakClass = BreakClass::Comma;
        else if (isOpenDelim(ch))    out.breakClass = BreakClass::Open;
        else if (isCloseDelim(ch))   out.breakClass = BreakClass::Close;

        return true;
    }

    // Rough width estimate for an atomic command with its arguments.
    // e.g. \frac{abc}{de} — width ≈ max(width(abc), width(de))
    double estimateAtomicWidth(std::string_view s) {
        // Find the command name
        size_t cmdEnd = 0;
        if (!s.empty() && s[0] == '\\') {
            cmdEnd = 1;
            while (cmdEnd < s.size() &&
                   std::isalpha(static_cast<unsigned char>(s[cmdEnd])))
                ++cmdEnd;
        }
        std::string_view cmd = s.substr(0, cmdEnd);

        // Fraction: width ≈ max of numerator, denominator
        if (cmd == "\\frac" || cmd == "\\dfrac" || cmd == "\\tfrac" ||
            cmd == "\\binom") {
            double maxArg = 0.0;
            size_t p = cmdEnd;
            while (p < s.size() && s[p] == '{') {
                size_t end = skipBraceGroup(s, p);
                double w = estimateBracedWidth(s.substr(p + 1, end - p - 2));
                maxArg = std::max(maxArg, w);
                p = end;
            }
            return maxArg + 0.5; // small overhead for fraction bar
        }

        // \sqrt: width ≈ content + radical sign
        if (cmd == "\\sqrt") {
            if (cmdEnd < s.size() && s[cmdEnd] == '{') {
                size_t end = skipBraceGroup(s, cmdEnd);
                return estimateBracedWidth(s.substr(cmdEnd + 1, end - cmdEnd - 2)) + 0.5;
            }
        }

        // Text commands: width ≈ char count × 0.5
        if (cmd == "\\text" || cmd == "\\mathrm" || cmd == "\\mathit" ||
            cmd == "\\mathbf" || cmd == "\\operatorname") {
            if (cmdEnd < s.size() && s[cmdEnd] == '{') {
                size_t end = skipBraceGroup(s, cmdEnd);
                double charCount = static_cast<double>(end - cmdEnd - 2);
                return charCount * 0.5;
            }
        }

        // Default: sum of character widths
        double w = 0.0;
        for (size_t i = cmdEnd; i < s.size(); i++) {
            w += 0.5;
        }
        return std::max(0.5, w);
    }

    // Estimate width of content between braces (not including braces)
    double estimateBracedWidth(std::string_view content) {
        double w = 0.0;
        Tokenizer inner{content, 0};
        Token tok;
        while (inner.next(tok)) {
            w += tok.width;
        }
        return w;
    }
};

// ----------------------------------------------------------
// Tokenize and annotate with delimiter depth
// ----------------------------------------------------------
static std::vector<Token> tokenize(std::string_view latex) {
    std::vector<Token> tokens;
    tokens.reserve(64);

    Tokenizer tz{latex, 0};
    Token tok;
    int depth = 0;

    while (tz.next(tok)) {
        // Update depth based on delimiters
        if (tok.breakClass == BreakClass::Close && depth > 0) --depth;
        tok.delimDepth = depth;
        if (tok.breakClass == BreakClass::Open) ++depth;

        tokens.push_back(tok);
    }

    return tokens;
}

// ----------------------------------------------------------
// Extract candidate breakpoints
// ----------------------------------------------------------
static std::vector<Breakpoint> extractBreakpoints(
    const std::vector<Token>& tokens,
    int maxDelimDepth)
{
    std::vector<Breakpoint> bps;
    bps.reserve(tokens.size());

    double cumWidth = 0.0;

    for (size_t i = 0; i < tokens.size(); i++) {
        cumWidth += tokens[i].width;

        BreakClass bc = tokens[i].breakClass;
        if (bc == BreakClass::None) continue;

        int dd = tokens[i].delimDepth;

        // Compute penalty
        double penalty;
        double depthPenalty = 500.0 * dd;

        switch (bc) {
            case BreakClass::Relation: penalty = -100.0 + depthPenalty; break;
            case BreakClass::BinOp:    penalty =   50.0 + depthPenalty; break;
            case BreakClass::Comma:    penalty =   30.0 + depthPenalty; break;
            case BreakClass::Open:     penalty =  200.0 + depthPenalty; break;
            case BreakClass::Close:    penalty =  200.0 + depthPenalty; break;
            default:                   penalty = 1000.0; break;
        }

        // Forbid breaks deep inside delimiters
        if (dd > maxDelimDepth) penalty = 1e9;

        Breakpoint bp;
        bp.tokenIndex = i;
        bp.cumWidth   = cumWidth;
        bp.penalty    = penalty;
        bp.delimDepth = dd;
        bp.breakClass = bc;
        bps.push_back(bp);
    }

    return bps;
}

// ----------------------------------------------------------
// Simplified Knuth-Plass DP
// ----------------------------------------------------------
struct DPNode {
    double totalDemerit;
    int    parent;       // index into breakpoints, or -1 for start
};

static std::vector<size_t> findOptimalBreaks(
    const std::vector<Breakpoint>& bps,
    double pageWidth,
    double indentStep,
    double totalWidth)
{
    if (bps.empty()) return {};

    const int n = static_cast<int>(bps.size());

    // dp[i] = best total demerits when the last line ends at breakpoint i
    std::vector<DPNode> dp(n, {std::numeric_limits<double>::infinity(), -1});

    // Try starting each breakpoint as the first line break
    auto computeDemerit = [&](double lineWidth, double contentWidth,
                              double penalty) -> double {
        double slack = lineWidth - contentWidth;
        double badness;
        if (slack < 0) {
            // Overfull: heavy penalty, but allow slight overflow (10%)
            if (contentWidth > lineWidth * 1.15)
                return 1e15; // way too wide, skip
            badness = 1000.0 + (-slack) * 100.0;
        } else {
            double ratio = slack / std::max(lineWidth, 1.0);
            badness = 100.0 * ratio * ratio * ratio;
        }
        double p = std::max(0.0, penalty);
        return (1.0 + badness + p) * (1.0 + badness + p);
    };

    // First line: from start to breakpoint j
    for (int j = 0; j < n; j++) {
        double contentW = bps[j].cumWidth;
        double demerit = computeDemerit(pageWidth, contentW, bps[j].penalty);
        if (demerit < dp[j].totalDemerit) {
            dp[j] = {demerit, -1};
        }
    }

    // Subsequent lines: from breakpoint i to breakpoint j
    for (int j = 1; j < n; j++) {
        for (int i = 0; i < j; i++) {
            if (std::isinf(dp[i].totalDemerit)) continue;

            double contentW = bps[j].cumWidth - bps[i].cumWidth;
            double lineW = pageWidth - indentStep; // continuation lines indented
            double demerit = computeDemerit(lineW, contentW, bps[j].penalty);
            double total = dp[i].totalDemerit + demerit;

            if (total < dp[j].totalDemerit) {
                dp[j] = {total, i};
            }
        }
    }

    // We also need to account for the last segment (after the last breakpoint).
    // Find the breakpoint with the best total demerit where the remainder fits.
    int bestLast = -1;
    double bestTotal = std::numeric_limits<double>::infinity();

    for (int i = 0; i < n; i++) {
        if (std::isinf(dp[i].totalDemerit)) continue;
        double remainW = totalWidth - bps[i].cumWidth;
        double lineW = pageWidth - indentStep;
        // If remainder fits, no additional penalty
        double demerit = (remainW > lineW * 1.15)
            ? 1e15
            : computeDemerit(lineW, remainW, 0.0);
        double total = dp[i].totalDemerit + demerit;
        if (total < bestTotal) {
            bestTotal = total;
            bestLast = i;
        }
    }

    if (bestLast < 0) return {}; // no feasible solution

    // Trace back
    std::vector<size_t> breaks;
    int cur = bestLast;
    while (cur >= 0) {
        breaks.push_back(static_cast<size_t>(cur));
        cur = dp[cur].parent;
    }
    std::reverse(breaks.begin(), breaks.end());

    return breaks;
}

// ----------------------------------------------------------
// Detect LHS width (everything before first top-level relation)
// ----------------------------------------------------------
static double findLHSWidth(const std::vector<Token>& tokens) {
    double w = 0.0;
    for (auto& t : tokens) {
        if (t.breakClass == BreakClass::Relation && t.delimDepth == 0) {
            return w;
        }
        w += t.width;
    }
    return w; // no relation found — everything is "LHS"
}

// ----------------------------------------------------------
// Emit multi-line LaTeX
// ----------------------------------------------------------

// Trim leading/trailing whitespace from a string_view
static std::string_view trim(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ')  s.remove_suffix(1);
    return s;
}

static std::string emitAligned(
    std::string_view latex,
    const std::vector<Token>& tokens,
    const std::vector<Breakpoint>& bps,
    const std::vector<size_t>& breakIndices)
{
    // Build line segments. Each break occurs BEFORE a relation/binop token,
    // so we break such that the relation/operator starts the next line.

    std::string out;
    out.reserve(latex.size() + breakIndices.size() * 20 + 60);

    out += "\\begin{aligned}\n";

    size_t prevEnd = 0; // byte offset where previous line ended

    for (size_t k = 0; k < breakIndices.size(); k++) {
        size_t bpIdx = breakIndices[k];
        const Breakpoint& bp = bps[bpIdx];
        const Token& tok = tokens[bp.tokenIndex];

        // Current line: from prevEnd to just before this break token
        size_t lineEnd = tok.start;
        std::string_view lineContent = trim(latex.substr(prevEnd, lineEnd - prevEnd));

        if (k == 0) {
            // First line: use & before relation for alignment
            if (bp.breakClass == BreakClass::Relation) {
                out += "  ";
                out += lineContent;
                out += " \\\\\n";
            } else {
                out += "  ";
                out += lineContent;
                out += " \\\\\n";
            }
        } else {
            // Continuation lines: indent with \quad, align at relation with &
            if (bp.breakClass == BreakClass::Relation) {
                out += "  ";
                out += lineContent;
                out += " \\\\\n";
            } else {
                out += "  \\quad ";
                out += lineContent;
                out += " \\\\\n";
            }
        }

        prevEnd = tok.start;
    }

    // Last segment: from the last break to end of string
    std::string_view lastLine = trim(latex.substr(prevEnd));
    if (!lastLine.empty()) {
        // Check if it starts with a relation
        bool startsWithRelation = false;
        if (!breakIndices.empty()) {
            size_t lastBpIdx = breakIndices.back();
            const Breakpoint& lastBp = bps[lastBpIdx];
            startsWithRelation = (lastBp.breakClass == BreakClass::Relation);
        }

        if (startsWithRelation) {
            // Find the relation token and split
            out += "  \\quad ";
        } else if (!breakIndices.empty()) {
            out += "  \\quad ";
        } else {
            out += "  ";
        }
        out += lastLine;
        out += "\n";
    }

    out += "\\end{aligned}";
    return out;
}

// A simpler emission: use & to align at the first relation on each
// continuation line.
static std::string emitAlignedAtRelation(
    std::string_view latex,
    const std::vector<Token>& tokens,
    const std::vector<Breakpoint>& bps,
    const std::vector<size_t>& breakIndices)
{
    std::string out;
    out.reserve(latex.size() + breakIndices.size() * 20 + 60);

    out += "\\begin{aligned}\n";

    size_t prevEnd = 0;
    bool firstLine = true;
    bool hasTopRelation = false;

    // Check if any break is a top-level relation
    for (size_t k = 0; k < breakIndices.size(); k++) {
        const Breakpoint& bp = bps[breakIndices[k]];
        if (bp.breakClass == BreakClass::Relation && bp.delimDepth == 0) {
            hasTopRelation = true;
            break;
        }
    }

    for (size_t k = 0; k < breakIndices.size(); k++) {
        size_t bpIdx = breakIndices[k];
        const Breakpoint& bp = bps[bpIdx];
        const Token& tok = tokens[bp.tokenIndex];
        size_t lineEnd = tok.start;
        std::string_view lineContent = trim(latex.substr(prevEnd, lineEnd - prevEnd));

        if (firstLine) {
            out += "  ";
            out += lineContent;
            out += " \\\\\n";
            firstLine = false;
        } else {
            if (bp.breakClass == BreakClass::Relation && bp.delimDepth == 0 &&
                hasTopRelation) {
                // Align at relation
                out += "  &";
                out += lineContent;
                out += " \\\\\n";
            } else {
                out += "  &\\quad ";
                out += lineContent;
                out += " \\\\\n";
            }
        }

        prevEnd = tok.start;
    }

    // Last line
    std::string_view lastLine = trim(latex.substr(prevEnd));
    if (!lastLine.empty()) {
        if (!firstLine) {
            // Check if last line starts at a relation break
            if (!breakIndices.empty()) {
                const Breakpoint& lastBp = bps[breakIndices.back()];
                if (lastBp.breakClass == BreakClass::Relation &&
                    lastBp.delimDepth == 0 && hasTopRelation) {
                    out += "  &";
                } else {
                    out += "  &\\quad ";
                }
            } else {
                out += "  ";
            }
        } else {
            out += "  ";
        }
        out += lastLine;
        out += "\n";
    }

    out += "\\end{aligned}";
    return out;
}

} // anonymous namespace

// ----------------------------------------------------------
// Public entry point
// ----------------------------------------------------------
std::string lineBreakLatex(std::string_view latex,
                           const LineBreakOptions& opts)
{
    // Skip if already multi-line (contains \\ or is an environment)
    if (latex.find("\\\\") != std::string_view::npos) return std::string(latex);
    if (latex.find("\\begin{") != std::string_view::npos) return std::string(latex);

    // Tokenize
    auto tokens = tokenize(latex);
    if (tokens.empty()) return std::string(latex);

    // Compute total width
    double totalWidth = 0.0;
    for (auto& t : tokens) totalWidth += t.width;

    // If it fits, return unchanged
    if (totalWidth <= opts.pageWidth) return std::string(latex);

    // Extract breakpoints
    auto bps = extractBreakpoints(tokens, opts.maxDelimDepth);
    if (bps.empty()) return std::string(latex); // no break candidates

    // Find optimal breaks
    auto breakIndices = findOptimalBreaks(bps, opts.pageWidth,
                                          opts.indentStep, totalWidth);
    if (breakIndices.empty()) return std::string(latex); // no feasible solution

    // Determine layout: check LHS width for straight ladder vs staggered
    double lhsWidth = findLHSWidth(tokens);
    bool hasTopRelation = false;
    for (auto& bp : bps) {
        if (bp.breakClass == BreakClass::Relation && bp.delimDepth == 0) {
            hasTopRelation = true;
            break;
        }
    }

    // Use aligned-at-relation if we have top-level relations
    // and LHS is not too wide (< 40% of page width)
    if (hasTopRelation && lhsWidth < opts.pageWidth * 0.4) {
        return emitAlignedAtRelation(latex, tokens, bps, breakIndices);
    }

    // Otherwise, use simple aligned with quad indents
    return emitAligned(latex, tokens, bps, breakIndices);
}

} // namespace wolfbook
