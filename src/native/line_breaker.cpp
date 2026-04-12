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

// Zero-width rendering commands (no glyph, no space)
static bool isStyleCmd(std::string_view cmd) {
    static const std::string_view styles[] = {
        "\\color", "\\textcolor", "\\colorbox",
        "\\displaystyle", "\\textstyle", "\\scriptstyle", "\\scriptscriptstyle",
        "\\phantom", "\\hphantom", "\\vphantom",
        "\\label", "\\tag", "\\notag", "\\nonumber",
    };
    for (auto& s : styles) if (cmd == s) return true;
    return false;
}

// Font/style-change commands: content retains normal width
static bool isFontCmd(std::string_view cmd) {
    static const std::string_view fonts[] = {
        "\\mathbf", "\\mathit", "\\mathrm", "\\mathsf", "\\mathtt",
        "\\mathcal", "\\mathbb", "\\mathfrak", "\\mathscr",
        "\\boldsymbol", "\\bm",
        "\\text", "\\textbf", "\\textit", "\\textrm", "\\textsf", "\\texttt",
        "\\operatorname",
    };
    for (auto& f : fonts) if (cmd == f) return true;
    return false;
}

// Structural commands: atomic, width from estimateStructuralWidth
static bool isStructuralCmd(std::string_view cmd) {
    static const std::string_view structs[] = {
        "\\frac", "\\dfrac", "\\tfrac", "\\binom",
        "\\sqrt",
        "\\hat", "\\bar", "\\vec", "\\tilde", "\\dot", "\\ddot",
        "\\overline", "\\underline", "\\overbrace", "\\underbrace",
        "\\overset", "\\underset", "\\stackrel",
    };
    for (auto& s : structs) if (cmd == s) return true;
    return false;
}

// ----------------------------------------------------------
// Width estimation
// ----------------------------------------------------------

// Math italic glyph widths for a-z
static const double kMathItalicWidths[26] = {
    0.53, 0.43, 0.43, 0.52, 0.44, 0.39, 0.50, 0.56, 0.31, 0.35,
    0.52, 0.31, 0.84, 0.56, 0.50, 0.52, 0.48, 0.41, 0.42, 0.39,
    0.54, 0.48, 0.70, 0.52, 0.47, 0.44
};

// Estimated width in "em" for a single LaTeX command (or character).
// For relations/binops the surrounding math spacing is baked in.
static double cmdWidth(std::string_view cmd) {
    if (cmd.empty()) return 0.0;

    // Single character
    if (cmd.size() == 1) {
        char c = cmd[0];
        if (c >= '0' && c <= '9') return 0.50;
        if (c >= 'a' && c <= 'z') return kMathItalicWidths[c - 'a'];
        if (c >= 'A' && c <= 'Z') return 0.65;
        // Relations: glyph + 2×thickmuskip (≈0.56 em total spacing)
        if (c == '=' || c == '<' || c == '>') return 0.78 + 0.56;
        // Binops: glyph + 2×medmuskip (≈0.44 em total spacing)
        if (c == '+') return 0.78 + 0.44;
        if (c == '-') return 0.44 + 0.44;
        if (c == '(' || c == ')') return 0.35;
        if (c == '[' || c == ']') return 0.30;
        if (c == ',') return 0.28;
        if (c == ' ') return 0.25;
        if (c == ';') return 0.35;
        if (c == ':') return 0.30;
        if (c == '|') return 0.28;
        return 0.50;
    }

    if (cmd[0] != '\\') return 0.50;

    // Math spacing
    if (cmd == "\\," || cmd == "\\:") return 0.17;
    if (cmd == "\\;") return 0.22;
    if (cmd == "\\!") return -0.06;
    if (cmd == "\\quad") return 1.00;
    if (cmd == "\\qquad") return 2.00;

    // Named operators with precise widths
    if (cmd == "\\lim")  return 1.37;
    if (cmd == "\\max")  return 1.67;
    if (cmd == "\\min")  return 1.37;
    if (cmd == "\\sup")  return 1.12;
    if (cmd == "\\inf")  return 0.84;
    if (cmd == "\\sin")  return 1.12;
    if (cmd == "\\cos")  return 1.21;
    if (cmd == "\\tan")  return 1.12;
    if (cmd == "\\exp")  return 1.21;
    if (cmd == "\\log")  return 1.12;
    if (cmd == "\\ln")   return 0.84;
    if (cmd == "\\det")  return 1.12;
    if (cmd == "\\dim")  return 1.12;
    if (cmd == "\\ker")  return 1.12;
    if (cmd == "\\arg")  return 1.12;
    if (cmd == "\\deg")  return 1.12;
    if (cmd == "\\gcd")  return 1.21;
    if (cmd == "\\Pr")   return 0.84;

    // Big operators
    if (cmd == "\\sum" || cmd == "\\prod") return 1.20;
    if (cmd == "\\int" || cmd == "\\intop" || cmd == "\\oint") return 1.00;
    if (cmd == "\\bigcup" || cmd == "\\bigcap") return 1.10;
    if (cmd == "\\bigoplus" || cmd == "\\bigotimes") return 1.10;

    // Common symbols
    if (cmd == "\\infty")   return 1.00;
    if (cmd == "\\partial") return 0.61;
    if (cmd == "\\nabla")   return 0.71;
    if (cmd == "\\forall")  return 0.80;
    if (cmd == "\\exists")  return 0.70;
    if (cmd == "\\emptyset" || cmd == "\\varnothing") return 0.70;
    if (cmd == "\\cdot")    return 0.39 + 0.44;
    if (cmd == "\\cdots" || cmd == "\\ldots") return 1.02;
    if (cmd == "\\vdots")   return 0.28;
    if (cmd == "\\ddots")   return 1.02;
    if (cmd == "\\pm" || cmd == "\\mp")   return 0.78 + 0.44;
    if (cmd == "\\times")   return 0.78 + 0.44;
    if (cmd == "\\div")     return 0.78 + 0.44;
    if (cmd == "\\circ")    return 0.44 + 0.44;
    if (cmd == "\\bullet")  return 0.44 + 0.44;
    if (cmd == "\\cap" || cmd == "\\cup") return 0.78 + 0.44;
    if (cmd == "\\wedge" || cmd == "\\vee") return 0.78 + 0.44;
    if (cmd == "\\oplus" || cmd == "\\otimes") return 0.78 + 0.44;

    // Relations (glyph + 2×thickmuskip ≈ 0.56 em)
    if (cmd == "\\leq" || cmd == "\\geq" || cmd == "\\neq") return 0.78 + 0.56;
    if (cmd == "\\approx" || cmd == "\\sim" || cmd == "\\cong") return 0.78 + 0.56;
    if (cmd == "\\equiv")   return 0.78 + 0.56;
    if (cmd == "\\propto")  return 0.78 + 0.56;
    if (cmd == "\\subset" || cmd == "\\supset") return 0.78 + 0.56;
    if (cmd == "\\subseteq" || cmd == "\\supseteq") return 0.78 + 0.56;
    if (cmd == "\\in" || cmd == "\\notin" || cmd == "\\ni") return 0.70 + 0.56;
    if (cmd == "\\ll" || cmd == "\\gg") return 0.78 + 0.56;
    if (cmd == "\\perp" || cmd == "\\parallel") return 0.78 + 0.56;

    // Arrows (relations: glyph + 0.56 em spacing)
    if (cmd == "\\rightarrow" || cmd == "\\to") return 1.00 + 0.56;
    if (cmd == "\\leftarrow"  || cmd == "\\gets") return 1.00 + 0.56;
    if (cmd == "\\Rightarrow" || cmd == "\\Leftarrow") return 1.11 + 0.56;
    if (cmd == "\\leftrightarrow") return 1.56 + 0.56;
    if (cmd == "\\Leftrightarrow") return 1.56 + 0.56;
    if (cmd == "\\mapsto")      return 1.22 + 0.56;
    if (cmd == "\\longrightarrow" || cmd == "\\longleftarrow") return 2.00 + 0.56;
    if (cmd == "\\iff")         return 2.56 + 0.56;
    if (cmd == "\\implies")     return 1.56 + 0.56;

    // Delimiters
    if (cmd == "\\{" || cmd == "\\}") return 0.37;
    if (cmd == "\\langle" || cmd == "\\rangle") return 0.37;
    if (cmd == "\\lvert"  || cmd == "\\rvert")  return 0.28;
    if (cmd == "\\lVert"  || cmd == "\\rVert")  return 0.28;
    if (cmd == "\\lfloor" || cmd == "\\rfloor") return 0.44;
    if (cmd == "\\lceil"  || cmd == "\\rceil")  return 0.44;

    // Lowercase Greek letters (per-symbol)
    if (cmd == "\\alpha")   return 0.63;
    if (cmd == "\\beta")    return 0.57;
    if (cmd == "\\gamma")   return 0.57;
    if (cmd == "\\delta")   return 0.56;
    if (cmd == "\\epsilon" || cmd == "\\varepsilon") return 0.52;
    if (cmd == "\\zeta")    return 0.52;
    if (cmd == "\\eta")     return 0.56;
    if (cmd == "\\theta"  || cmd == "\\vartheta")  return 0.55;
    if (cmd == "\\iota")    return 0.30;
    if (cmd == "\\kappa"  || cmd == "\\varkappa")  return 0.57;
    if (cmd == "\\lambda")  return 0.63;
    if (cmd == "\\mu")      return 0.70;
    if (cmd == "\\nu")      return 0.52;
    if (cmd == "\\xi")      return 0.52;
    if (cmd == "\\pi" || cmd == "\\varpi") return 0.57;
    if (cmd == "\\rho" || cmd == "\\varrho") return 0.50;
    if (cmd == "\\sigma" || cmd == "\\varsigma") return 0.52;
    if (cmd == "\\tau")     return 0.44;
    if (cmd == "\\upsilon") return 0.56;
    if (cmd == "\\phi" || cmd == "\\varphi") return 0.63;
    if (cmd == "\\chi")     return 0.63;
    if (cmd == "\\psi")     return 0.63;
    if (cmd == "\\omega")   return 0.70;

    // Uppercase Greek letters (per-symbol)
    if (cmd == "\\Gamma")   return 0.63;
    if (cmd == "\\Delta")   return 0.83;
    if (cmd == "\\Theta")   return 0.83;
    if (cmd == "\\Lambda")  return 0.83;
    if (cmd == "\\Xi")      return 0.72;
    if (cmd == "\\Pi")      return 0.83;
    if (cmd == "\\Sigma")   return 0.83;
    if (cmd == "\\Upsilon") return 0.83;
    if (cmd == "\\Phi")     return 0.83;
    if (cmd == "\\Psi")     return 0.83;
    if (cmd == "\\Omega")   return 0.83;

    // Fallback by case
    if (std::islower(static_cast<unsigned char>(cmd[1]))) return 0.55;
    if (std::isupper(static_cast<unsigned char>(cmd[1]))) return 0.73;
    return 0.55;
}

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

static std::string_view trimSpaces(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ')  s.remove_suffix(1);
    return s;
}

static bool readEnvironment(std::string_view s,
                            size_t pos,
                            std::string_view& envName,
                            size_t& beginTagEnd,
                            size_t& fullEnd)
{
    if (s.substr(pos, 7) != "\\begin{") return false;

    size_t nameStart = pos + 7;
    size_t nameEnd = s.find('}', nameStart);
    if (nameEnd == std::string_view::npos) return false;

    envName = s.substr(nameStart, nameEnd - nameStart);
    beginTagEnd = nameEnd + 1;

    std::string beginTag = "\\begin{" + std::string(envName) + "}";
    std::string endTag   = "\\end{"   + std::string(envName) + "}";

    size_t searchPos = beginTagEnd;
    int depth = 1;
    while (searchPos < s.size()) {
        size_t nextBegin = s.find(beginTag, searchPos);
        size_t nextEnd   = s.find(endTag, searchPos);

        if (nextEnd == std::string_view::npos) return false;

        if (nextBegin != std::string_view::npos && nextBegin < nextEnd) {
            ++depth;
            searchPos = nextBegin + beginTag.size();
            continue;
        }

        --depth;
        searchPos = nextEnd + endTag.size();
        if (depth == 0) {
            fullEnd = searchPos;
            return true;
        }
    }

    return false;
}

static double estimateEnvironmentWidth(std::string_view envName,
                                       std::string_view body,
                                       double (*estimateTotalWidthFn)(std::string_view));

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
            auto remaining = src.substr(pos);
            if (remaining.size() >= 7 && remaining.substr(0, 7) == "\\begin{") {
                std::string_view envName;
                size_t beginTagEnd = 0;
                size_t envEnd = 0;
                if (readEnvironment(src, pos, envName, beginTagEnd, envEnd)) {
                    pos = envEnd;
                    out.len = pos - out.start;
                    std::string endTag = "\\end{" + std::string(envName) + "}";
                    size_t bodyEnd = envEnd - endTag.size();
                    std::string_view body = src.substr(beginTagEnd, bodyEnd - beginTagEnd);
                    out.width = estimateEnvironmentWidth(envName, body,
                                                         [](std::string_view latex) {
                                                             Tokenizer tz{latex, 0};
                                                             double w = 0.0;
                                                             Token tok;
                                                             while (tz.next(tok)) w += tok.width;
                                                             return w;
                                                         });
                    return true;
                }
            }
            if (remaining.size() >= 5 && remaining.substr(0, 5) == "\\end{") {
                size_t braceStart = src.find('{', pos);
                size_t braceEnd = src.find('}', braceStart);
                if (braceEnd != std::string_view::npos) {
                    pos = braceEnd + 1;
                    out.len = pos - out.start;
                    out.width = 0.0;
                    return true;
                }
            }

            size_t cmdEnd = readCommand(src, pos);
            std::string_view cmd = src.substr(pos, cmdEnd - pos);

            // Style commands: zero-width directives
            if (isStyleCmd(cmd)) {
                pos = cmdEnd;
                if (cmd == "\\color") {
                    // \color{name} — skip the color brace group
                    if (pos < src.size() && src[pos] == '{')
                        pos = skipBraceGroup(src, pos);
                    out.len = pos - out.start; out.width = 0.0; return true;
                }
                if (cmd == "\\textcolor" || cmd == "\\colorbox") {
                    // skip {color}, then measure {content}
                    if (pos < src.size() && src[pos] == '{')
                        pos = skipBraceGroup(src, pos);
                    double contentW = 0.0;
                    if (pos < src.size() && src[pos] == '{') {
                        size_t end = skipBraceGroup(src, pos);
                        contentW = estimateBracedWidth(src.substr(pos + 1, end - pos - 2));
                        pos = end;
                    }
                    out.len = pos - out.start; out.width = contentW; return true;
                }
                // All other directives (\displaystyle, \phantom, etc.): zero width
                out.len = pos - out.start; out.width = 0.0; return true;
            }

            // Font commands: content has normal width, no additional overhead
            if (isFontCmd(cmd)) {
                pos = cmdEnd;
                double contentW = 0.0;
                if (pos < src.size() && src[pos] == '{') {
                    size_t end = skipBraceGroup(src, pos);
                    contentW = estimateBracedWidth(src.substr(pos + 1, end - pos - 2));
                    pos = end;
                }
                out.len = pos - out.start; out.width = contentW; return true;
            }

            // Structural commands: atomic token, width from estimateStructuralWidth
            if (isStructuralCmd(cmd)) {
                pos = cmdEnd;
                // \sqrt may have an optional [n] index
                if (cmd == "\\sqrt" && pos < src.size() && src[pos] == '[') {
                    size_t closeIdx = src.find(']', pos + 1);
                    if (closeIdx != std::string_view::npos) pos = closeIdx + 1;
                }
                // Consume all immediately following brace groups
                while (pos < src.size() && src[pos] == '{') {
                    pos = skipBraceGroup(src, pos);
                }
                out.len = pos - out.start;
                out.width = estimateStructuralWidth(src.substr(out.start, out.len));
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

    // Rough width estimate for a structural command with its arguments.
    // e.g. \frac{abc}{de} — width ≈ max(width(abc), width(de)) + overhead
    double estimateStructuralWidth(std::string_view s) {
        // Find the command name
        size_t cmdEnd = 0;
        if (!s.empty() && s[0] == '\\') {
            cmdEnd = 1;
            while (cmdEnd < s.size() &&
                   std::isalpha(static_cast<unsigned char>(s[cmdEnd])))
                ++cmdEnd;
        }
        std::string_view cmd = s.substr(0, cmdEnd);

        // Fraction/binom: width ≈ max(num, den) + overhead
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
            // \tfrac is smaller, so less overhead
            double overhead = (cmd == "\\tfrac") ? 0.15 : 0.24;
            return maxArg + overhead;
        }

        // \sqrt[n]{content}: handle optional index
        if (cmd == "\\sqrt") {
            size_t p = cmdEnd;
            double extraW = 0.0;
            if (p < s.size() && s[p] == '[') {
                size_t closeIdx = s.find(']', p + 1);
                if (closeIdx != std::string_view::npos) {
                    extraW = 0.3;
                    p = closeIdx + 1;
                }
            }
            if (p < s.size() && s[p] == '{') {
                size_t end = skipBraceGroup(s, p);
                return estimateBracedWidth(s.substr(p + 1, end - p - 2)) + 0.5 + extraW;
            }
            return 0.5 + extraW;
        }

        // Accent commands: pure content width (accents add no horizontal extent)
        if (cmd == "\\hat"  || cmd == "\\bar"  || cmd == "\\vec" ||
            cmd == "\\tilde" || cmd == "\\dot"  || cmd == "\\ddot" ||
            cmd == "\\overline" || cmd == "\\underline") {
            if (cmdEnd < s.size() && s[cmdEnd] == '{') {
                size_t end = skipBraceGroup(s, cmdEnd);
                return estimateBracedWidth(s.substr(cmdEnd + 1, end - cmdEnd - 2));
            }
            return 0.5;
        }

        // Overbrace/underbrace: just the content width
        if (cmd == "\\overbrace" || cmd == "\\underbrace") {
            if (cmdEnd < s.size() && s[cmdEnd] == '{') {
                size_t end = skipBraceGroup(s, cmdEnd);
                return estimateBracedWidth(s.substr(cmdEnd + 1, end - cmdEnd - 2));
            }
            return 1.0;
        }

        // \overset{top}{base}, \underset, \stackrel — max of two args
        if (cmd == "\\overset" || cmd == "\\underset" || cmd == "\\stackrel") {
            double maxArg = 0.0;
            size_t p = cmdEnd;
            while (p < s.size() && s[p] == '{') {
                size_t end = skipBraceGroup(s, p);
                double w = estimateBracedWidth(s.substr(p + 1, end - p - 2));
                maxArg = std::max(maxArg, w);
                p = end;
            }
            return maxArg;
        }

        // Default: count non-brace characters
        double w = 0.0;
        for (size_t i = cmdEnd; i < s.size(); i++) {
            char c = s[i];
            if (c != '{' && c != '}') w += 0.5;
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

// Free function: sum of all token widths for a latex fragment
static double estimateTotalWidth(std::string_view latex) {
    Tokenizer tz{latex, 0};
    double w = 0.0;
    Token tok;
    while (tz.next(tok)) w += tok.width;
    return w;
}

static double estimateEnvironmentWidth(std::string_view envName,
                                       std::string_view body,
                                       double (*estimateTotalWidthFn)(std::string_view))
{
    if (envName == "gathered" || envName == "aligned") {
        double maxLineWidth = 0.0;
        size_t start = 0;
        while (start <= body.size()) {
            size_t sep = body.find("\\\\", start);
            size_t end = (sep == std::string_view::npos) ? body.size() : sep;
            std::string_view line = trimSpaces(body.substr(start, end - start));
            while (!line.empty() && line.front() == '&') {
                line.remove_prefix(1);
                line = trimSpaces(line);
            }
            if (line.substr(0, 5) == "\\quad") {
                line.remove_prefix(5);
                line = trimSpaces(line);
            }
            maxLineWidth = std::max(maxLineWidth, estimateTotalWidthFn(line));
            if (sep == std::string_view::npos) break;
            start = sep + 2;
        }
        return maxLineWidth;
    }

    return estimateTotalWidthFn(body);
}

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

        // Never break before a closing delimiter. In particular this avoids
        // emitting "\\" right before "\right...", which can produce
        // invalid LaTeX in gathered/aligned output.
        if (bc == BreakClass::Close) continue;

        int dd = tokens[i].delimDepth;

        // Compute penalty
        double penalty;
        double depthPenalty = 500.0 * dd;

        switch (bc) {
            // At depth 0 (top-level), relations (=, \to, …) are the preferred break
            // point. Inside delimiters (depth > 0) prefer commas over relations so
            // that list items like "lhs \to rhs" are not split at the arrow.
            case BreakClass::Relation: penalty = (dd > 0 ? 50.0 : -100.0) + depthPenalty; break;
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
            // Overfull: keep it expensive, but never impossible.
            // This ensures we still pick the closest-fit layout when no
            // perfect solution exists, instead of collapsing to a nearly
            // unbroken expression because the remainder became "infeasible".
            double overflowRatio = (-slack) / std::max(lineWidth, 1.0);
            badness = 1000.0 + 4000.0 * overflowRatio * overflowRatio * overflowRatio;
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
        double demerit = computeDemerit(lineW, remainW, 0.0);
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

static std::vector<size_t> appendTailRescueBreaks(
    const std::vector<Breakpoint>& bps,
    std::vector<size_t> breakIndices,
    double pageWidth,
    double indentStep,
    double totalWidth)
{
    if (bps.empty()) return breakIndices;

    const double lineWidth = pageWidth - indentStep;
    if (lineWidth <= 0.0) return breakIndices;

    while (true) {
        double segmentStartWidth = breakIndices.empty()
            ? 0.0
            : bps[breakIndices.back()].cumWidth;
        double tailWidth = totalWidth - segmentStartWidth;

        // Tail already acceptable.
        if (tailWidth <= lineWidth * 1.02) break;

        size_t candidateStart = breakIndices.empty() ? 0 : (breakIndices.back() + 1);
        int best = -1;
        double bestScore = std::numeric_limits<double>::infinity();

        for (size_t j = candidateStart; j < bps.size(); j++) {
            double firstWidth = bps[j].cumWidth - segmentStartWidth;
            double secondWidth = totalWidth - bps[j].cumWidth;
            if (secondWidth <= 0.0) continue;

            double over1 = std::max(0.0, firstWidth - lineWidth);
            double over2 = std::max(0.0, secondWidth - lineWidth);
            double tinyTailPenalty = (secondWidth < lineWidth * 0.12)
                ? (lineWidth * 0.12 - secondWidth) * 200.0
                : 0.0;
            double score = std::max(over1, over2) * 10000.0
                + over1 * 100.0
                + over2 * 100.0
                + std::max(0.0, bps[j].penalty)
                + tinyTailPenalty;

            if (score < bestScore) {
                bestScore = score;
                best = static_cast<int>(j);
            }
        }

        if (best < 0) break;
        breakIndices.push_back(static_cast<size_t>(best));
    }

    return breakIndices;
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

static bool startsWithCloseDelimiterCmd(std::string_view s) {
    static const std::string_view closers[] = {
        "\\right", "\\bigr", "\\Bigr", "\\biggr", "\\Biggr",
        "\\rangle", "\\rvert", "\\rVert", "\\rfloor", "\\rceil", "\\}",
    };
    for (auto c : closers) {
        if (s.size() >= c.size() && s.substr(0, c.size()) == c) return true;
    }
    return false;
}

// Count net unmatched \left opens in a LaTeX string segment.
// Returns positive when there are more \left than \right (opens left unclosed).
// Returns negative when there are more \right than \left (closes from a prior segment).
// Does NOT clamp at zero — callers use std::max(0, pendingOpens + delta).
static int leftRightNetDepth(std::string_view s) {
    int depth = 0;
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] == '\\') {
            // \left — 5 chars, must be followed by non-alpha (delimiter)
            if (i + 5 <= s.size() && s.substr(i, 5) == "\\left" &&
                (i + 5 == s.size() || !std::isalpha((unsigned char)s[i + 5]))) {
                ++depth;
                i += 5;
                continue;
            }
            // \right — 6 chars, must be followed by non-alpha (delimiter)
            if (i + 6 <= s.size() && s.substr(i, 6) == "\\right" &&
                (i + 6 == s.size() || !std::isalpha((unsigned char)s[i + 6]))) {
                --depth;
                i += 6;
                continue;
            }
        }
        ++i;
    }
    return depth;
}

// Remove invalid line breaks that appear immediately before a closing delimiter.
// Example fix: "... \\\n+//   \\right) ..."  -> "... \\right) ..."
static void sanitizeBreaksBeforeClosers(std::string& latex) {
    size_t pos = 0;
    const std::string br = "\\\\\n";
    while ((pos = latex.find(br, pos)) != std::string::npos) {
        size_t q = pos + br.size();
        while (q < latex.size() && latex[q] == ' ') ++q;
        if (q < latex.size() && startsWithCloseDelimiterCmd(std::string_view(latex).substr(q))) {
            // Replace line break + indentation with a single space.
            latex.replace(pos, q - pos, " ");
            if (pos > 0) --pos;
            continue;
        }
        pos = q;
    }
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
    int pendingOpens = 0; // unmatched \left opens carried from previous line

    for (size_t k = 0; k < breakIndices.size(); k++) {
        size_t bpIdx = breakIndices[k];
        const Breakpoint& bp = bps[bpIdx];
        const Token& tok = tokens[bp.tokenIndex];

        // Current line: from prevEnd to just before this break token
        size_t lineEnd = tok.start;
        if (bp.breakClass == BreakClass::Close) {
            // Never emit "\\" immediately before a closing delimiter.
            // If a breakpoint lands at a close token, treat it as a break
            // right after the token.
            lineEnd = tok.start + tok.len;
        }
        std::string_view lineContent = trim(latex.substr(prevEnd, lineEnd - prevEnd));

        out += "  &";
        for (int i = 0; i < pendingOpens; i++) out += "\\left. ";
        out += lineContent;
        // Close any \left opens that are unmatched at this break point to
        // avoid "missing \right" errors when \\ splits a \left...\right pair.
        int lineDepth = std::max(0, pendingOpens + leftRightNetDepth(lineContent));
        for (int i = 0; i < lineDepth; i++) out += " \\right.";
        out += " \\\\\n";
        pendingOpens = lineDepth;

        prevEnd = lineEnd;
    }

    // Last segment: from the last break to end of string
    std::string_view lastLine = trim(latex.substr(prevEnd));
    if (!lastLine.empty()) {
        out += "  &";
        for (int i = 0; i < pendingOpens; i++) out += "\\left. ";
        out += lastLine;
        out += "\n";
    }

    out += "\\end{aligned}";
    sanitizeBreaksBeforeClosers(out);
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
    int pendingOpens = 0; // unmatched \left opens carried from previous line

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
        if (bp.breakClass == BreakClass::Close) {
            // Keep closing delimiters on the same line as their content.
            lineEnd = tok.start + tok.len;
        }
        std::string_view lineContent = trim(latex.substr(prevEnd, lineEnd - prevEnd));

        out += "  &";
        for (int i = 0; i < pendingOpens; i++) out += "\\left. ";
        out += lineContent;
        // Close any \left opens that are unmatched at this break point.
        int lineDepth = std::max(0, pendingOpens + leftRightNetDepth(lineContent));
        for (int i = 0; i < lineDepth; i++) out += " \\right.";
        out += " \\\\\n";
        pendingOpens = lineDepth;
        firstLine = false;

        prevEnd = lineEnd;
    }

    // Last line
    std::string_view lastLine = trim(latex.substr(prevEnd));
    if (!lastLine.empty()) {
        if (!firstLine) {
            out += "  &";
        } else {
            out += "  ";
        }
        for (int i = 0; i < pendingOpens; i++) out += "\\left. ";
        out += lastLine;
        out += "\n";
    }

    out += "\\end{aligned}";
    sanitizeBreaksBeforeClosers(out);
    return out;
}

// Emit a \begin{gathered}...\end{gathered} environment (no & column)
static std::string emitGathered(
    std::string_view latex,
    const std::vector<Token>& tokens,
    const std::vector<Breakpoint>& bps,
    const std::vector<size_t>& breakIndices)
{
    std::string out;
    out.reserve(latex.size() + breakIndices.size() * 10 + 50);
    out += "\\begin{gathered}\n";

    size_t prevEnd = 0;
    int pendingOpens = 0; // unmatched \left opens carried from previous line

    for (size_t k = 0; k < breakIndices.size(); k++) {
        size_t bpIdx = breakIndices[k];
        const Breakpoint& bp = bps[bpIdx];
        const Token& tok = tokens[bp.tokenIndex];
        size_t lineEnd = tok.start;
        if (bp.breakClass == BreakClass::Close) {
            // Avoid invalid "\\" before \right-like closers.
            lineEnd = tok.start + tok.len;
        }
        std::string_view lineContent = trim(latex.substr(prevEnd, lineEnd - prevEnd));
        out += "  ";
        for (int i = 0; i < pendingOpens; i++) out += "\\left. ";
        out += lineContent;
        // Close any \left opens that are unmatched at this break point.
        int lineDepth = std::max(0, pendingOpens + leftRightNetDepth(lineContent));
        for (int i = 0; i < lineDepth; i++) out += " \\right.";
        out += " \\\\\n";
        pendingOpens = lineDepth;
        prevEnd = lineEnd;
    }

    std::string_view lastLine = trim(latex.substr(prevEnd));
    if (!lastLine.empty()) {
        out += "  ";
        for (int i = 0; i < pendingOpens; i++) out += "\\left. ";
        out += lastLine;
        out += "\n";
    }

    out += "\\end{gathered}";
    sanitizeBreaksBeforeClosers(out);
    // Wrap with gray scaling brackets so multi-line numerators are visually
    // scoped.  \textcolor{gray}{\left[…\right]} keeps \left/\right matched
    // inside one group; the inner \textcolor{black}{…} restores content colour.
    return "\\textcolor{gray}{\\left[\\textcolor{black}{" + out + "}\\right]}";
}

// Break a sub-expression (e.g. a numerator) with 85% of the page budget.
// Returns a gathered/aligned multi-line string if needed, otherwise original.
static std::string lineBreakInner(std::string_view latex,
                                  const LineBreakOptions& opts,
                                  bool useGathered = true)
{
    double budget = opts.pageWidth * 0.85;
    auto tokens = tokenize(latex);
    if (tokens.empty()) return std::string(latex);

    double totalWidth = 0.0;
    for (auto& t : tokens) totalWidth += t.width;
    if (totalWidth <= budget) return std::string(latex);

    auto bps = extractBreakpoints(tokens, opts.maxDelimDepth);
    if (bps.empty()) return std::string(latex);

    auto breakIndices = findOptimalBreaks(bps, budget, opts.indentStep, totalWidth);
    if (breakIndices.empty()) return std::string(latex);
    breakIndices = appendTailRescueBreaks(bps, std::move(breakIndices),
                                          budget, opts.indentStep, totalWidth);

    if (useGathered)
        return emitGathered(latex, tokens, bps, breakIndices);
    return emitAligned(latex, tokens, bps, breakIndices);
}

// Scan for wide fractions and recursively break their wide arguments.
static std::string preprocessWideFractions(std::string_view latex,
                                           const LineBreakOptions& opts)
{
    std::string result;
    result.reserve(latex.size());

    size_t pos = 0;
    while (pos < latex.size()) {
        if (latex[pos] != '\\') {
            result += latex[pos++];
            continue;
        }

        size_t cmdEnd = readCommand(latex, pos);
        std::string_view cmd = latex.substr(pos, cmdEnd - pos);

        bool isFrac = (cmd == "\\frac" || cmd == "\\dfrac" || cmd == "\\tfrac");
        if (!isFrac) {
            result += latex.substr(pos, cmdEnd - pos);
            pos = cmdEnd;
            continue;
        }

        // Threshold: promote when an argument is too wide for the actual
        // continuation-line budget, not the full first-line width.
        // This is important for long sums of fractions where most terms land
        // on indented continuation lines.
        double continuationWidth = std::max(1.0, opts.pageWidth - opts.indentStep);
        double threshold = (cmd == "\\tfrac")
            ? continuationWidth * 1.3
            : continuationWidth * 0.9;

        // Emit the \frac/\dfrac command
        result += latex.substr(pos, cmdEnd - pos);
        pos = cmdEnd;

        // Process up to 2 brace groups (numerator, denominator)
        for (int argIdx = 0; argIdx < 2 && pos < latex.size() && latex[pos] == '{'; argIdx++) {
            size_t end = skipBraceGroup(latex, pos);
            std::string_view argContent = latex.substr(pos + 1, end - pos - 2);
            double argWidth = estimateTotalWidth(argContent);
            if (argWidth > threshold) {
                std::string broken = lineBreakInner(argContent, opts, true);
                result += '{';
                result += broken;
                result += '}';
            } else {
                result += latex.substr(pos, end - pos);
            }
            pos = end;
        }
    }

    return result;
}

} // anonymous namespace

// ----------------------------------------------------------
// Paging helpers (used by the public entry point)
// ----------------------------------------------------------
namespace {

// Build a vector of balanced line-segment strings from a set of break indices.
// Each string is the content of one \begin{aligned} row, with \left. / \right.
// added to balance any unmatched \left...\right pairs that span line boundaries.
// This replicates the per-line loop logic from emitAligned/emitAlignedAtRelation.
static std::vector<std::string> buildLineSegs(
    std::string_view latex,
    const std::vector<Token>& tokens,
    const std::vector<Breakpoint>& bps,
    const std::vector<size_t>& breakIndices)
{
    std::vector<std::string> segs;
    segs.reserve(breakIndices.size() + 1);
    size_t prevEnd   = 0;
    int pendingOpens = 0;

    auto addSeg = [&](std::string_view raw) {
        std::string seg;
        for (int i = 0; i < pendingOpens; i++) seg += "\\left. ";
        seg += raw;
        int lineDepth = std::max(0, pendingOpens + leftRightNetDepth(raw));
        for (int i = 0; i < lineDepth; i++) seg += " \\right.";
        pendingOpens = lineDepth;
        segs.push_back(std::move(seg));
    };

    for (size_t k = 0; k < breakIndices.size(); k++) {
        const Breakpoint& bp  = bps[breakIndices[k]];
        const Token&      tok = tokens[bp.tokenIndex];
        size_t lineEnd = tok.start;
        if (bp.breakClass == BreakClass::Close)
            lineEnd = tok.start + tok.len;
        addSeg(trim(latex.substr(prevEnd, lineEnd - prevEnd)));
        prevEnd = lineEnd;
    }

    auto lastLine = trim(latex.substr(prevEnd));
    if (!lastLine.empty()) addSeg(lastLine);

    return segs;
}

// Emit the requested page from a list of balanced line segments.
// Each page is a complete \begin{aligned}...\end{aligned} block.
// Continuation boundaries are marked with \textcolor{gray}{\cdots} (gray \cdots).
// maxRows == 0 means no paging (single page always).
// Returns {result: latex for requestedPage, totalPages}.
static wolfbook::LineBreakResult emitSegsAsPaged(
    const std::vector<std::string>& segs,
    int maxRows,
    int requestedPage)
{
    const size_t N = segs.size();
    if (N == 0) return { "", 1 };

    const size_t pSize = (maxRows > 0) ? (size_t)maxRows : N;

    // Continuation markers: gray \cdots to signal "expression continues"
    static const char kCont[] = "\\;\\textcolor{gray}{\\cdots}";
    static const char kResm[] = "\\textcolor{gray}{\\cdots}\\;";

    auto buildPage = [&](size_t start, size_t end, bool hasBefore, bool hasAfter) -> std::string {
        std::string out;
        out += "\\begin{aligned}\n";
        for (size_t i = start; i < end; i++) {
            out += "  &";
            if (i == start  && hasBefore) out += kResm;
            out += segs[i];
            if (i == end - 1 && hasAfter)  out += kCont;
            if (i + 1 < end) out += " \\\\\n";
            else             out += "\n";
        }
        out += "\\end{aligned}";
        return out;
    };

    // Single page (no paging triggered)
    if (maxRows <= 0 || (int)N <= maxRows)
        return { buildPage(0, N, false, false), 1 };

    // Multi-page: compute total pages, clamp requestedPage, emit only that page.
    const int totalPages = (int)((N + pSize - 1) / pSize);
    int pg = requestedPage;
    if (pg < 0) pg = 0;
    if (pg >= totalPages) pg = totalPages - 1;

    const size_t start = (size_t)pg * pSize;
    const size_t end   = std::min(start + pSize, N);
    return { buildPage(start, end, pg > 0, end < N), totalPages };
}

} // anonymous namespace (paging helpers)

// ----------------------------------------------------------
// Public entry point
// ----------------------------------------------------------
LineBreakResult lineBreakLatex(std::string_view latex,
                               const LineBreakOptions& opts)
{
    // Skip if already multi-line (contains \\ or is an environment)
    if (latex.find("\\\\") != std::string_view::npos) return { std::string(latex), 1 };
    if (latex.find("\\begin{") != std::string_view::npos) return { std::string(latex), 1 };

    // Compute effective page width (pixel-based takes priority over em-based)
    double effectivePageWidth = opts.pageWidth;
    if (opts.pageWidthPx > 0.0)
        effectivePageWidth = opts.pageWidthPx / opts.baseFontSizePx;

    // Build inner options with the resolved page width
    LineBreakOptions innerOpts = opts;
    innerOpts.pageWidth = effectivePageWidth;

    // Pre-process wide fractions (may inject \begin{gathered} inside args)
    std::string preprocessed = preprocessWideFractions(latex, innerOpts);
    std::string_view input = preprocessed;

    // Tokenize
    auto tokens = tokenize(input);
    if (tokens.empty()) return { preprocessed, 1 };

    // Compute total width
    double totalWidth = 0.0;
    for (auto& t : tokens) totalWidth += t.width;

    // If it fits, return preprocessed (may have inner line-breaks)
    if (totalWidth <= effectivePageWidth) return { preprocessed, 1 };

    // Extract breakpoints
    auto bps = extractBreakpoints(tokens, opts.maxDelimDepth);
    if (bps.empty()) return { preprocessed, 1 };

    // Find optimal breaks
    auto breakIndices = findOptimalBreaks(bps, effectivePageWidth,
                                          opts.indentStep, totalWidth);
    if (breakIndices.empty()) return { preprocessed, 1 };
    breakIndices = appendTailRescueBreaks(bps, std::move(breakIndices),
                                          effectivePageWidth, opts.indentStep, totalWidth);

    // When paging is requested, use segment-based path (works for all layout types)
    if (opts.maxRows > 0) {
        auto segs = buildLineSegs(input, tokens, bps, breakIndices);
        return emitSegsAsPaged(segs, opts.maxRows, opts.requestedPage);
    }

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
    if (hasTopRelation && lhsWidth < effectivePageWidth * 0.4) {
        return { emitAlignedAtRelation(input, tokens, bps, breakIndices), 1 };
    }

    // Otherwise, use simple aligned with quad indents
    return { emitAligned(input, tokens, bps, breakIndices), 1 };
}

} // namespace wolfbook
