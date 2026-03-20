// =============================================================
// box_to_latex.cpp  —  Wolfbook AST → LaTeX translator
//
// Design:
//   BoxTranslator::translate(nodeIdx) is the recursive workhorse.
//   It dispatches on NodeKind first, then on head-name string for
//   Expr nodes.  Every known box head has its own private method.
//   Unknown heads fall through to translateUnknownExpr which recurses
//   over all children and logs a warning to stderr.
//
// Performance:
//   - Result is built into a single std::string via append() calls.
//     A pre-reserved string avoids most reallocations for typical
//     expressions.
//   - No heap allocation per node; the flat arena from WLParser is
//     traversed by index.
//   - String-view comparisons for head dispatch avoid copies.
// =============================================================
#include "box_to_latex.h"
#include "wl_parser.h"
#include "special_chars.h"

#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>

namespace wolfbook {
namespace {

// ----------------------------------------------------------
// Color helpers — RGBColor[r,g,b] → "#rrggbb"
// ----------------------------------------------------------
static char hexChar(int v) {
    return (v < 10) ? ('0' + v) : ('a' + v - 10);
}

static std::string channelHex(double v) {
    int byte = static_cast<int>(v * 255.0 + 0.5);
    if (byte < 0)   byte = 0;
    if (byte > 255) byte = 255;
    std::string result(2, '0');
    result[0] = hexChar(byte >> 4);
    result[1] = hexChar(byte & 0xF);
    return result;
}

// Named colour → hex (matching WL named colour objects)
static const std::unordered_map<std::string_view, std::string_view> kNamedColors {
    {"Red",         "#ff0000"}, {"Green",     "#008000"},
    {"Blue",        "#0000ff"}, {"Yellow",    "#ffff00"},
    {"Cyan",        "#00ffff"}, {"Magenta",   "#ff00ff"},
    {"Black",       "#000000"}, {"White",     "#ffffff"},
    {"Gray",        "#808080"}, {"Orange",    "#ff8800"},
    {"Purple",      "#800080"}, {"Brown",     "#a52a2a"},
    {"Pink",        "#ffc0cb"}, {"LightBlue", "#add8e6"},
    {"LightGreen",  "#90ee90"}, {"LightGray", "#d3d3d3"},
    {"DarkBlue",    "#00008b"}, {"DarkGreen", "#006400"},
    {"DarkRed",     "#8b0000"}, {"DarkGray",  "#404040"},
};

// ----------------------------------------------------------
// String classification (for plain string leaf nodes)
// ----------------------------------------------------------
enum class TokenClass { Number, SingleChar, MultiLetter, Operator };

static TokenClass classifyToken(std::string_view s) {
    if (s.empty()) return TokenClass::Operator;
    if (s.size() == 1) {
        char c = s[0];
        return (std::isdigit(static_cast<unsigned char>(c)) || c == '.')
               ? TokenClass::Number : TokenClass::SingleChar;
    }
    // Single-pass: track whether all chars are digits/dot and whether all are alpha
    bool allDigitOrDot = true;
    bool allAlpha      = true;
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isdigit(uc) && c != '.') allDigitOrDot = false;
        if (!std::isalpha(uc))             allAlpha      = false;
        if (!allDigitOrDot && !allAlpha)   break;  // early exit
    }
    if (allDigitOrDot) return TokenClass::Number;
    if (allAlpha)      return TokenClass::MultiLetter;
    return TokenClass::Operator;
}

// ----------------------------------------------------------
// BoxTranslator — stateful per-call translator
// ----------------------------------------------------------
class BoxTranslator {
public:
    explicit BoxTranslator(const ParseResult& pr) : pr_(pr) {}

    std::string run() {
        // Heuristic: LaTeX output is roughly proportional to string pool size.
        // Reserve early to avoid mid-flight reallocations.
        result_.reserve(std::max<size_t>(512,
            pr_.strings.size() * 8 + pr_.nodes.size() * 4));
        translate(pr_.root);
        return std::move(result_);
    }

private:
    const ParseResult& pr_;
    std::string        result_;

    // ---- main dispatch ----
    void translate(uint32_t idx) {
        const Node& n = pr_.node(idx);
        switch (n.kind) {
            case NodeKind::String:  translateString(n);  break;
            case NodeKind::Symbol:  translateSymbol(n);  break;
            case NodeKind::Integer: result_ += std::to_string(n.iVal); break;
            case NodeKind::Real:    translateReal(n);    break;
            case NodeKind::Expr:    translateExpr(n);    break;
            case NodeKind::List:    translateList(n);    break;
            case NodeKind::Rule:    /* option rule — handled by parent */ break;
            case NodeKind::RuleDelayed: break;
        }
    }

    // ---- leaf: String node ----
    void translateString(const Node& n) {
        std::string_view s = pr_.str(n);

        // Space token: emit one space normally, but two when the previous output
        // ended in a word-command (e.g. \pi).  Mathematica's TeXForm inserts a
        // delimiter space after control sequences, so RowBox[{"\[Pi]"," ","u"}]
        // renders as "\pi  u" (two spaces) to match that convention.
        if (s == " ") {
            if (!result_.empty()) {
                size_t j = result_.size();
                while (j > 0 && std::isalpha(static_cast<unsigned char>(result_[j-1]))) --j;
                result_ += (j > 0 && j < result_.size() && result_[j-1] == '\\')
                           ? "  " : " ";
            } else {
                result_ += ' ';
            }
            return;
        }

        // Strip WL precision annotation from numeric string literals.
        // "3.1415`"  or  "3.1415`15"  → strip backtick+suffix when suffix is
        // all digits (or empty).  Machine-precision literals like "1.`*^-40"
        // have a non-digit suffix and are instead rendered as \text{…} with
        // backtick → $\grave{ }$ and caret → ${}^{\wedge}$ substitutions.
        std::string_view stripped = s;
        auto btPos = s.find('`');
        if (btPos != std::string_view::npos) {
            std::string_view prefix = s.substr(0, btPos);
            std::string_view suffix = s.substr(btPos + 1);
            bool numericPrefix = !prefix.empty();
            for (char c : prefix)
                if (!std::isdigit(static_cast<unsigned char>(c)) &&
                    c != '.' && c != '-' && c != '+' && c != 'e' && c != 'E')
                    { numericPrefix = false; break; }
            bool pureDigitSuffix = true;
            for (char c : suffix)
                if (!std::isdigit(static_cast<unsigned char>(c)))
                    { pureDigitSuffix = false; break; }
            if (numericPrefix && pureDigitSuffix) {
                stripped = prefix;  // strip precision annotation
            } else {
                // Non-standard suffix: render verbatim inside \text{} with
                // substitutions for backtick and caret.
                result_ += "\\text{";
                for (char c : s) {
                    if      (c == '`') result_ += "$\\grave{ }$";
                    else if (c == '^') result_ += "${}^{\\wedge}$";
                    else               result_ += c;
                }
                result_ += '}';
                return;
            }
        }

        // Quoted-text string: WL box strings sometimes contain literal " chars
        // (e.g. the parser sees  "\"x\""  and stores  "x"  with embedded
        // double-quotes).  Strip all " chars and wrap in \text{...}.
        // This handles labels, string values, and Bold/Italic styled text.
        if (stripped.find('"') != std::string_view::npos) {
            std::string inner;
            inner.reserve(stripped.size());
            for (char c : stripped)
                if (c != '"') inner += c;
            if (!inner.empty()) {
                // If the unquoted content is a named WL character (e.g. "\[Alpha]"
                // stored as "\"\[Alpha]\""), translate it properly instead of
                // wrapping in \text{}.
                auto mappedInner = wlCharToLatex(inner);
                if (mappedInner.data() != nullptr) {
                    result_ += mappedInner;
                } else {
                    result_ += "\\text{";
                    for (char c : inner) {
                        if      (c == '_') result_ += "\\_";
                        else if (c == '^') result_ += "\\^{}";
                        else if (c == '$') result_ += "\\$";
                        else if (c == '%') result_ += "\\%";
                        else if (c == '&') result_ += "\\&";
                        else if (c == '#') result_ += "\\#";
                        else if (c == '{') result_ += "\\{";
                        else if (c == '}') result_ += "\\}";
                        else if (c == '~') result_ += "\\textasciitilde{}";
                        else               result_ += c;
                    }
                    result_ += '}';
                }
            }
            return;
        }

        // Named WL character?  wlCharToLatex returns a null string_view when
        // the token is not in the table, and "" (non-null, empty) for explicitly
        // suppressed characters such as \[InvisibleTimes].
        auto mapped = wlCharToLatex(stripped);
        if (mapped.data() != nullptr) {
            result_ += mapped;
            return;
        }
        // String may embed one or more \[Name] sequences mixed with plain text
        // e.g. "\[CapitalSigma]n" or "\[Alpha]\[Beta]".  Scan and translate.
        if (stripped.find("\\[") != std::string_view::npos) {
            size_t i = 0;
            while (i < stripped.size()) {
                if (stripped[i] == '\\' && i + 1 < stripped.size() && stripped[i+1] == '[') {
                    size_t end = stripped.find(']', i + 2);
                    if (end != std::string_view::npos) {
                        std::string_view tok = stripped.substr(i, end - i + 1);
                        auto m = wlCharToLatex(tok);
                        if (m.data() != nullptr) {
                            result_ += m;
                            // If the command ends with a letter and the next
                            // char is also a letter, insert a space so the
                            // control word doesn't merge with following text.
                            if (!m.empty() && std::isalpha(static_cast<unsigned char>(m.back()))
                                && end + 1 < stripped.size()
                                && std::isalpha(static_cast<unsigned char>(stripped[end + 1])))
                                result_ += ' ';
                        } else {
                            result_ += tok; // unknown named char — emit verbatim
                        }
                        i = end + 1;
                        continue;
                    }
                }
                char c = stripped[i++];
                if      (c == '_') result_ += "\\_";
                else if (c == '^') result_ += "\\^{}";
                else               result_ += c;
            }
            return;
        }
        // Mathematica symbol `E` is Euler's number — render as roman e
        if (stripped == "E") { result_ += 'e'; return; }
        // Mathematica symbol `I` is the imaginary unit — render as i
        if (stripped == "I") { result_ += 'i'; return; }
        // WL infix operators that must map to LaTeX commands
        if (stripped == "->")  { result_ += "\\to ";     return; }
        if (stripped == ":>")  { result_ += "\\mapsto "; return; }
        // WL relational operators that map to single LaTeX symbols
        if (stripped == "==")  { result_ += '=';         return; }
        if (stripped == "!=")  { result_ += "\\neq ";    return; }
        if (stripped == ">=")  { result_ += "\\geq ";    return; }
        if (stripped == "<=")  { result_ += "\\leq ";    return; }
        // Bare underscore (WL Blank pattern) — escape it in LaTeX math
        if (stripped == "_")   { result_ += "\\_";       return; }
        // Otherwise classify the (possibly stripped) plain string
        auto cls = classifyToken(stripped);
        switch (cls) {
            case TokenClass::Number:
                result_ += stripped;
                // Trailing decimal point (e.g. "1." from "1.`") gets a thin
                // space so it reads as a number, not punctuation.
                if (!stripped.empty() && stripped.back() == '.')
                    result_ += "\\,";
                break;
            case TokenClass::SingleChar:
            case TokenClass::Operator:
                // If the string contains a space it is display text, not a
                // math operator — emit it inside \text{} so spaces are preserved.
                if (stripped.find(' ') != std::string_view::npos) {
                    result_ += "\\text{";
                    for (char c : stripped) {
                        if      (c == '_') result_ += "\\_";
                        else if (c == '^') result_ += "\\^{}";
                        else if (c == '$') result_ += "\\$";
                        else if (c == '%') result_ += "\\%";
                        else if (c == '&') result_ += "\\&";
                        else if (c == '#') result_ += "\\#";
                        else if (c == '{') result_ += "\\{";
                        else if (c == '}') result_ += "\\}";
                        else if (c == '~') result_ += "\\textasciitilde{}";
                        else if (c == '\\') result_ += "\\textbackslash{}";
                        else               result_ += c;
                    }
                    result_ += '}';
                    break;
                }
                for (char c : stripped) {
                    if (c == '_') result_ += "\\_";
                    else          result_ += c;
                }
                break;
            case TokenClass::MultiLetter:
                result_ += "\\mathrm{";
                result_ += stripped;
                result_ += '}';
                break;
        }
    }

    // ---- leaf: Symbol node (bare symbol in an expression context) ----
    void translateSymbol(const Node& n) {
        std::string_view s = pr_.str(n);
        // True / False are rendered as text
        if (s == "True")  { result_ += "\\text{True}";  return; }
        if (s == "False") { result_ += "\\text{False}"; return; }
        if (s == "None")  { result_ += "\\text{None}";  return; }
        if (s == "Null")  { return; }  // suppress
        // Named colour that appears bare (e.g. FontColor -> Red)
        // — not a translatable leaf in normal flow
        result_ += s;
    }

    // ---- leaf: Real ----
    void translateReal(const Node& n) {
        // Trim unnecessary trailing zeros
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", n.dVal);
        result_ += buf;
    }

    // ---- List: {a, b, c} — shouldn't appear at top level but handle gracefully ----
    void translateList(const Node& n) {
        for (uint32_t i = 0; i < n.childrenCount; ++i) {
            if (i > 0) result_ += ',';
            translate(pr_.children[n.childrenStart + i]);
        }
    }

    // ---- Expr node dispatch on head name ----
    void translateExpr(const Node& n) {
        if (n.childrenCount == 0) return;
        std::string_view head = pr_.headName(n);

        // args are children[1..childrenCount-1]
        uint32_t argStart = n.childrenStart + 1;
        uint32_t argCount = n.childrenCount - 1;

        if      (head == "RowBox")              translateRowBox(argStart, argCount);
        else if (head == "SuperscriptBox")      translateSuperscriptBox(argStart, argCount);
        else if (head == "SubscriptBox")        translateSubscriptBox(argStart, argCount);
        else if (head == "SubsuperscriptBox")   translateSubsuperscriptBox(argStart, argCount);
        else if (head == "FractionBox")         translateFractionBox(argStart, argCount);
        else if (head == "SqrtBox")             translateSqrtBox(argStart, argCount);
        else if (head == "RadicalBox")          translateRadicalBox(argStart, argCount);
        else if (head == "UnderscriptBox")      translateUnderscriptBox(argStart, argCount);
        else if (head == "OverscriptBox")       translateOverscriptBox(argStart, argCount);
        else if (head == "UnderoverscriptBox")  translateUnderoverscriptBox(argStart, argCount);
        else if (head == "StyleBox")            translateStyleBox(argStart, argCount);
        else if (head == "TagBox")              translateTagBox(argStart, argCount);
        else if (head == "InterpretationBox")   translateInterpretationBox(argStart, argCount);
        else if (head == "FormBox")             translateFormBox(argStart, argCount);
        else if (head == "TemplateBox")         translateTemplateBox(argStart, argCount);
        else if (head == "GridBox")             translateGridBox(argStart, argCount, "matrix");
        else if (head == "ButtonBox")           translateButtonBox(argStart, argCount);
        else if (head == "PaneSelectorBox")     translatePaneSelectorBox(argStart, argCount);
        else if (head == "DynamicBox")          { /* suppress */ }
        else if (head == "Dynamic")             { /* suppress */ }
        else if (head == "MouseAppearanceTag")  { /* suppress */ }
        else if (head == "GraphicsBox")         result_ += "[graphics]";
        else if (head == "RGBColor")            { /* colour node — handled by StyleBox */ }
        else                                    translateUnknownExpr(head, argStart, argCount);
    }

    // ================================================================
    // Box handlers
    // ================================================================

    // ---- RowBox[{e1, e2, …}] ----
    // The single argument is always a List node.
    void translateRowBox(uint32_t argStart, uint32_t argCount) {
        if (argCount == 0) return;
        // The sole argument should be a List
        const Node& listNode = pr_.node(pr_.children[argStart]);
        if (listNode.kind != NodeKind::List) {
            // Fallback: translate arg directly
            translate(pr_.children[argStart]);
            return;
        }

        uint32_t nElems = listNode.childrenCount;
        uint32_t lStart = listNode.childrenStart;

        // Detect: Style[expr, opts…] emitted as literal text boxes by the kernel:
        // RowBox[{"Style", "[", body…, ",", opts…, "]"}]
        // Just render the body argument (first arg of Style), drop styling.
        if (nElems >= 4) {
            const Node& sn0   = pr_.node(pr_.children[lStart]);
            const Node& sn1   = pr_.node(pr_.children[lStart + 1]);
            const Node& snLst = pr_.node(pr_.children[lStart + nElems - 1]);
            if (sn0.isString()  && pr_.str(sn0)   == "Style" &&
                sn1.isString()  && pr_.str(sn1)   == "["     &&
                snLst.isString() && pr_.str(snLst) == "]") {
                // Translate children[2 .. first_comma-1] as the first argument
                uint32_t commaAt = nElems - 1;
                for (uint32_t i = 2; i < nElems - 1; ++i) {
                    const Node& ni = pr_.node(pr_.children[lStart + i]);
                    if (ni.isString() && pr_.str(ni) == ",") { commaAt = i; break; }
                }
                for (uint32_t i = 2; i < commaAt; ++i)
                    translate(pr_.children[lStart + i]);
                return;
            }
        }

        // Generalised matrix detection: {open, [empty…], GridBox[…], [empty…], close}
        // Skip empty-string padding — the kernel sometimes inserts "" between
        // the bracket and the GridBox (e.g. "(" "" GridBox[…] "" ")").
        {
            // Collect indices of non-empty-string elements (cap at 4 to bail early).
            // "Empty" means: literal "" OR a WL invisible/zero-width token like
            // \[NoBreak], \[InvisibleTimes], \[InvisibleSpace], \[InvisibleComma].
            uint32_t ne[4]; uint32_t neCnt = 0;
            for (uint32_t i = 0; i < nElems && neCnt <= 3; ++i) {
                const Node& ni = pr_.node(pr_.children[lStart + i]);
                if (ni.isString() || ni.isSymbol()) {
                    std::string_view sv = pr_.str(ni);
                    if (sv.empty()) continue;
                    // Skip WL invisible/no-break chars (they translate to "")
                    std::string_view lx = wlCharToLatex(sv);
                    if (lx.data() != nullptr && lx.empty()) continue;
                }
                if (neCnt < 4) ne[neCnt] = i;
                ++neCnt;
            }
            if (neCnt == 3) {
                const Node& first  = pr_.node(pr_.children[lStart + ne[0]]);
                const Node& middle = pr_.node(pr_.children[lStart + ne[1]]);
                const Node& last   = pr_.node(pr_.children[lStart + ne[2]]);
                if ((first.isString() || first.isSymbol()) && (last.isString() || last.isSymbol()) && middle.isExpr() &&
                    pr_.headName(middle) == "GridBox")
                {
                    std::string_view open  = pr_.str(first);
                    std::string_view close = pr_.str(last);
                    std::string_view env   = delimToEnv(open, close);
                    if (!env.empty()) {
                        uint32_t gArgStart = middle.childrenStart + 1;
                        uint32_t gArgCount = middle.childrenCount - 1;
                        // Pass env as bracket-locked (no ColumnAlignments override)
                        translateGridBox(gArgStart, gArgCount, env, /*bracketLocked=*/true);
                        return;
                    }
                }
            }
        }

        // Wrapping delimiters: RowBox[{"(", …, ")"}] → \left(…\right)
        // Detected AFTER matrix detection so it only fires for non-matrix content.
        if (nElems >= 3) {
            const Node& first = pr_.node(pr_.children[lStart]);
            const Node& last  = pr_.node(pr_.children[lStart + nElems - 1]);
            if ((first.isString() || first.isSymbol()) && (last.isString() || last.isSymbol())) {
                auto [lOpen, lClose] = delimToLeftRight(pr_.str(first), pr_.str(last));
                if (!lOpen.empty()) {
                    result_ += lOpen;
                    for (uint32_t i = 1; i < nElems - 1; ++i)
                        translate(pr_.children[lStart + i]);
                    result_ += lClose;
                    return;
                }
            }
        }

        // Generic: concatenate all children
        for (uint32_t i = 0; i < nElems; ++i)
            translate(pr_.children[lStart + i]);
    }

    // ---- Helper: does a translated LaTeX string need {…} as a script arg?
    // A single ASCII character or a bare word-command (\alpha, \pi, …) needs
    // no grouping; everything else (multi-token, command with arguments, …) does.
    static bool needsBraces(const std::string& s) {
        if (s.empty()) return false;
        if (s.size() == 1) return false;        // single char: ^2 → ^2
        if (s[0] == '\\') {
            size_t i = 1;
            while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
            if (i == s.size()) return false;    // bare cmd: ^\alpha → ^\alpha
        }
        return true;
    }

    // ---- SuperscriptBox[base, exp] ----
    void translateSuperscriptBox(uint32_t a, uint32_t n) {
        if (n < 2) return;
        std::string base = translateToString(pr_.children[a]);
        std::string exp  = translateToString(pr_.children[a + 1]);
        result_ += base;
        result_ += '^';
        if (needsBraces(exp)) { result_ += '{'; result_ += exp; result_ += '}'; }
        else                  { result_ += exp; }
    }

    // ---- SubscriptBox[base, sub] ----
    void translateSubscriptBox(uint32_t a, uint32_t n) {
        if (n < 2) return;
        std::string base = translateToString(pr_.children[a]);
        std::string sub  = translateToString(pr_.children[a + 1]);
        result_ += base;
        result_ += '_';
        if (needsBraces(sub)) { result_ += '{'; result_ += sub; result_ += '}'; }
        else                  { result_ += sub; }
    }

    // ---- SubsuperscriptBox[base, sub, sup] ----
    void translateSubsuperscriptBox(uint32_t a, uint32_t n) {
        if (n < 3) return;
        std::string base = translateToString(pr_.children[a]);
        std::string sub  = translateToString(pr_.children[a + 1]);
        std::string sup  = translateToString(pr_.children[a + 2]);
        result_ += base;
        result_ += '_';
        if (needsBraces(sub)) { result_ += '{'; result_ += sub; result_ += '}'; }
        else                  { result_ += sub; }
        result_ += '^';
        if (needsBraces(sup)) { result_ += '{'; result_ += sup; result_ += '}'; }
        else                  { result_ += sup; }
    }

    // ---- FractionBox[num, den] ----
    void translateFractionBox(uint32_t a, uint32_t n) {
        if (n < 2) return;
        result_ += "\\frac{";  translate(pr_.children[a]);     result_ += "}{";
        translate(pr_.children[a + 1]);
        result_ += '}';
    }

    // ---- SqrtBox[arg] ----
    void translateSqrtBox(uint32_t a, uint32_t n) {
        if (n < 1) return;
        result_ += "\\sqrt{";  translate(pr_.children[a]);  result_ += '}';
    }

    // ---- RadicalBox[arg, index] ----
    void translateRadicalBox(uint32_t a, uint32_t n) {
        if (n < 2) return;
        result_ += "\\sqrt[";  translate(pr_.children[a + 1]);
        result_ += "]{";       translate(pr_.children[a]);
        result_ += '}';
    }

    // ---- Helper: resolve LaTeX for a box node intended to be a base ----
    // Uses a length-mark approach: record cursor position, translate into the
    // main buffer, copy the new suffix out, then truncate back to the mark.
    // This avoids the save/swap/re-allocate dance of the naive approach and
    // keeps the entire translation in one contiguous buffer.
    std::string translateToString(uint32_t idx) {
        const size_t mark = result_.size();
        translate(idx);
        std::string out(result_.data() + mark, result_.size() - mark);
        result_.resize(mark);
        return out;
    }

    // ---- UnderscriptBox[base, under] ----
    void translateUnderscriptBox(uint32_t a, uint32_t n) {
        if (n < 2) return;
        std::string base = translateToString(pr_.children[a]);
        if (isLargeOperator(base)) {
            result_ += base;
            result_ += "_{"; translate(pr_.children[a + 1]); result_ += '}';
        } else {
            result_ += "\\underset{"; translate(pr_.children[a + 1]);
            result_ += "}{"; result_ += base; result_ += '}';
        }
    }

    // ---- OverscriptBox[base, over] ----
    void translateOverscriptBox(uint32_t a, uint32_t n) {
        if (n < 2) return;
        std::string base = translateToString(pr_.children[a]);
        if (isLargeOperator(base)) {
            result_ += base;
            result_ += "^{"; translate(pr_.children[a + 1]); result_ += '}';
        } else {
            result_ += "\\overset{"; translate(pr_.children[a + 1]);
            result_ += "}{"; result_ += base; result_ += '}';
        }
    }

    // ---- UnderoverscriptBox[base, under, over] ----
    void translateUnderoverscriptBox(uint32_t a, uint32_t n) {
        if (n < 3) return;
        std::string base = translateToString(pr_.children[a]);
        if (isLargeOperator(base)) {
            result_ += base;
            result_ += "_{"; translate(pr_.children[a + 1]); result_ += '}';
            result_ += "^{"; translate(pr_.children[a + 2]); result_ += '}';
        } else {
            result_ += "\\underset{"; translate(pr_.children[a + 1]);
            result_ += "}{\\overset{"; translate(pr_.children[a + 2]);
            result_ += "}{"; result_ += base; result_ += "}}";
        }
    }

    // ================================================================
    // Helper: extract (lhs, rhs) from either form of a Wolfram Rule:
    //   NodeKind::Rule / NodeKind::RuleDelayed  (built from -> / :>)
    //     children = [lhs, rhs]
    //   NodeKind::Expr with head "Rule" / "RuleDelayed"  (function call form)
    //     children = [headSymbol, lhs, rhs]
    // Returns false if the node is not a rule in either form.
    // ================================================================
    bool extractRule(const Node& opt, const Node*& lhs, const Node*& rhs) const {
        if (opt.kind == NodeKind::Rule || opt.kind == NodeKind::RuleDelayed) {
            if (opt.childrenCount < 2) return false;
            lhs = &pr_.node(pr_.children[opt.childrenStart]);
            rhs = &pr_.node(pr_.children[opt.childrenStart + 1]);
            return true;
        }
        if (opt.kind == NodeKind::Expr && opt.childrenCount >= 3) {
            std::string_view h = pr_.headName(opt);
            if (h == "Rule" || h == "RuleDelayed") {
                // children[0]=head, children[1]=lhs, children[2]=rhs
                lhs = &pr_.node(pr_.children[opt.childrenStart + 1]);
                rhs = &pr_.node(pr_.children[opt.childrenStart + 2]);
                return true;
            }
        }
        return false;
    }

    // ================================================================
    // StyleBox[expr, opt1, opt2, …]
    //   Options are Rule nodes: FontColor->…, FontWeight->…, FontSlant->…
    // ================================================================
    void translateStyleBox(uint32_t a, uint32_t n) {
        if (n == 0) return;
        // First arg is the expression to style
        std::string inner = translateToString(pr_.children[a]);

        // Remaining args can be in any order; collect option values
        std::string colorHex;
        bool bold        = false;
        bool italic      = false;
        bool underline   = false;
        bool showContents = true;

        for (uint32_t i = 1; i < n; ++i) {
            uint32_t optIdx = pr_.children[a + i];
            const Node& opt = pr_.node(optIdx);

            // Bare positional color argument: StyleBox[expr, RGBColor[…], …]
            // The kernel sometimes passes the color directly rather than via
            // a Rule[FontColor, …] option.
            if (opt.isExpr()) {
                std::string_view h = pr_.headName(opt);
                if (h == "RGBColor" || h == "GrayLevel" || h == "Hue" || h == "CMYKColor") {
                    colorHex = resolveColorNode(opt);
                    continue;
                }
            }

            // Bare positional style symbols: StyleBox[expr, Bold, Italic, …]
            if (opt.isSymbol()) {
                std::string_view sym = pr_.str(opt);
                if (sym == "Bold")      { bold      = true; continue; }
                if (sym == "Italic")    { italic    = true; continue; }
                if (sym == "Underlined"){ underline = true; continue; }
                continue; // ignore other bare symbols (e.g. StripOnInput)
            }

            // Bare integer or real (font size like 20) — ignore
            if (opt.kind == NodeKind::Integer || opt.kind == NodeKind::Real) continue;

            const Node* lhsNp = nullptr;
            const Node* rhsNp = nullptr;
            if (!extractRule(opt, lhsNp, rhsNp)) continue;
            const Node& lhsN = *lhsNp;
            const Node& rhsN = *rhsNp;

            if (!lhsN.isSymbol()) continue;
            std::string_view optName = pr_.str(lhsN);

            if (optName == "FontColor") {
                colorHex = resolveColorNode(rhsN);
            } else if (optName == "FontWeight") {
                std::string_view val = rhsN.isString() ? pr_.str(rhsN) :
                                       rhsN.isSymbol() ? pr_.str(rhsN) : "";
                if (val == "Bold") bold = true;
            } else if (optName == "FontSlant") {
                std::string_view val = rhsN.isString() ? pr_.str(rhsN) :
                                       rhsN.isSymbol() ? pr_.str(rhsN) : "";
                if (val == "Italic") italic = true;
            } else if (optName == "ShowContents") {
                if (rhsN.isSymbol() && pr_.str(rhsN) == "False") showContents = false;
            }
        }

        // ShowContents -> False: suppress the entire cell
        if (!showContents) return;

        // Apply: colour first (innermost), then underline, then italic, then bold.
        if (!colorHex.empty()) {
            inner = "\\textcolor{" + colorHex + "}{" + inner + "}";
        }
        if (underline) {
            inner = "\\underline{" + inner + "}";
        }
        if (italic) {
            inner = "\\mathit{" + inner + "}";
        }
        if (bold) {
            inner = "\\mathbf{" + inner + "}";
        }

        result_ += inner;
    }

    // Resolve a color-valued AST node to "#rrggbb"
    std::string resolveColorNode(const Node& n) {
        // GrayLevel[g]  →  #gggggg
        if (n.isExpr() && pr_.headName(n) == "GrayLevel") {
            uint32_t ns = n.childrenCount - 1;
            if (ns < 1) return "#000000";
            const Node& ch = pr_.node(pr_.children[n.childrenStart + 1]);
            double g = (ch.kind == NodeKind::Real)    ? ch.dVal
                     : (ch.kind == NodeKind::Integer) ? static_cast<double>(ch.iVal)
                     : 0.0;
            return "#" + channelHex(g) + channelHex(g) + channelHex(g);
        }
        // Hue[h]  →  convert to RGB approximately
        if (n.isExpr() && pr_.headName(n) == "Hue") {
            uint32_t ns = n.childrenCount - 1;
            if (ns < 1) return "#ff0000";
            auto getCh = [&](uint32_t off) -> double {
                if (off >= ns) return 1.0;
                const Node& c = pr_.node(pr_.children[n.childrenStart + 1 + off]);
                return (c.kind == NodeKind::Real) ? c.dVal : (c.kind == NodeKind::Integer) ? (double)c.iVal : 0.0;
            };
            double hue = getCh(0) * 360.0;
            double sat = getCh(1);
            double bri = getCh(2);
            // HSB → RGB
            double c2 = bri * sat;
            double x2 = c2 * (1.0 - std::abs(std::fmod(hue / 60.0, 2.0) - 1.0));
            double m  = bri - c2;
            double r1, g1, b1;
            int h6 = static_cast<int>(hue / 60.0) % 6;
            switch (h6) {
                case 0: r1=c2; g1=x2; b1=0;  break;
                case 1: r1=x2; g1=c2; b1=0;  break;
                case 2: r1=0;  g1=c2; b1=x2; break;
                case 3: r1=0;  g1=x2; b1=c2; break;
                case 4: r1=x2; g1=0;  b1=c2; break;
                default:r1=c2; g1=0;  b1=x2; break;
            }
            return "#" + channelHex(r1+m) + channelHex(g1+m) + channelHex(b1+m);
        }
        // CMYKColor[c,m,y,k]
        if (n.isExpr() && pr_.headName(n) == "CMYKColor") {
            uint32_t ns = n.childrenCount - 1;
            auto getCh = [&](uint32_t off) -> double {
                if (off >= ns) return 0.0;
                const Node& c = pr_.node(pr_.children[n.childrenStart + 1 + off]);
                return (c.kind == NodeKind::Real) ? c.dVal : (c.kind == NodeKind::Integer) ? (double)c.iVal : 0.0;
            };
            double cy=getCh(0), mg=getCh(1), ye=getCh(2), k=getCh(3);
            return "#" + channelHex((1-cy)*(1-k)) + channelHex((1-mg)*(1-k)) + channelHex((1-ye)*(1-k));
        }
        // RGBColor[r, g, b]  or  RGBColor[r, g, b, a]
        if (n.isExpr() && pr_.headName(n) == "RGBColor") {
            uint32_t ns = n.childrenCount - 1;  // arg count
            if (ns < 3) return "#000000";
            auto getChannel = [&](uint32_t offset) -> double {
                const Node& ch = pr_.node(pr_.children[n.childrenStart + 1 + offset]);
                if (ch.kind == NodeKind::Real)    return ch.dVal;
                if (ch.kind == NodeKind::Integer) return static_cast<double>(ch.iVal);
                return 0.0;
            };
            return "#" + channelHex(getChannel(0))
                       + channelHex(getChannel(1))
                       + channelHex(getChannel(2));
        }
        // Named colour symbol (bare)
        if (n.isSymbol()) {
            auto it = kNamedColors.find(pr_.str(n));
            if (it != kNamedColors.end()) return std::string(it->second);
        }
        return "#000000";
    }

    // ================================================================
    // TagBox[expr, tag]
    //   Special handling: "Null" → suppress, "Piecewise" → cases env
    // ================================================================
    void translateTagBox(uint32_t a, uint32_t n) {
        if (n < 1) return;
        // Get tag (second arg, if present)
        std::string_view tag;
        if (n >= 2) {
            const Node& tagNode = pr_.node(pr_.children[a + 1]);
            if (tagNode.isString() || tagNode.isSymbol())
                tag = pr_.str(tagNode);
        }

        if (tag == "Null") return;  // suppress

        // Piecewise: the body should be a GridBox
        if (tag == "Piecewise") {
            const Node& body = pr_.node(pr_.children[a]);
            if (body.isExpr() && pr_.headName(body) == "GridBox") {
                uint32_t gArgStart = body.childrenStart + 1;
                uint32_t gArgCount = body.childrenCount - 1;
                translateGridBox(gArgStart, gArgCount, "cases");
                return;
            }
        }

        // Grid: render with background colours and frame borders
        if (tag == "Grid") {
            const Node& body = pr_.node(pr_.children[a]);
            if (body.isExpr() && pr_.headName(body) == "GridBox") {
                translateGridTagBox(body);
                return;
            }
        }

        // Generic: recurse into body
        translate(pr_.children[a]);
    }

    // ================================================================
    // TagBox[GridBox[rows, opts…], "Grid"]
    //   Renders as \begin{array} with optional hline/vline borders.
    // ================================================================
    void translateGridTagBox(const Node& gridBoxNode) {
        uint32_t argStart = gridBoxNode.childrenStart + 1;
        uint32_t argCount = gridBoxNode.childrenCount - 1;
        if (argCount == 0) return;

        const Node& rowsNode = pr_.node(pr_.children[argStart]);

        // ---- Parse options ----
        // GridBoxFrame -> {"Columns" -> {{True/False}}, "Rows" -> {{True/False}}}
        // FrameStyle -> colour  (ignored — KaTeX \hline has no colour param)

        bool frameRows = false;
        bool frameCols = false;

        for (uint32_t i = 1; i < argCount; ++i) {
            const Node& opt = pr_.node(pr_.children[argStart + i]);
            const Node* lhsNp = nullptr;
            const Node* rhsNp = nullptr;
            if (!extractRule(opt, lhsNp, rhsNp)) continue;
            if (!lhsNp->isSymbol()) continue;
            std::string_view optName = pr_.str(*lhsNp);

            if (optName == "GridBoxFrame") {
                const Node& rhs = *rhsNp;
                uint32_t listStart = 0, listCount = 0;
                if (rhs.isList()) {
                    listStart = rhs.childrenStart;
                    listCount = rhs.childrenCount;
                } else if (rhs.isExpr()) {
                    listStart = rhs.childrenStart + 1;
                    listCount = rhs.childrenCount - 1;
                }
                for (uint32_t j = 0; j < listCount; ++j) {
                    const Node* kp = nullptr; const Node* vp = nullptr;
                    if (!extractRule(pr_.node(pr_.children[listStart + j]), kp, vp)) continue;
                    if (!kp->isString()) continue;
                    std::string_view k = pr_.str(*kp);
                    bool hasTrue = nodeContainsTrue(*vp);
                    if (k == "Columns" && hasTrue) frameCols = true;
                    if (k == "Rows"    && hasTrue) frameRows = true;
                }
            }
            // GridBoxBackground / FrameStyle: ignored
        }

        uint32_t rowCount = rowsNode.childrenCount;
        // Determine column count from first row
        uint32_t colCount = 0;
        if (rowCount > 0) {
            const Node& firstRow = pr_.node(pr_.children[rowsNode.childrenStart]);
            if (firstRow.isList()) colCount = firstRow.childrenCount;
        }

        // ---- Build \begin{array}{|c|c|...} or {cc...} ----
        result_ += "\\begin{array}{";
        if (frameCols) result_ += '|';
        for (uint32_t c = 0; c < colCount; ++c) {
            result_ += 'c';
            if (frameCols) result_ += '|';
        }
        result_ += '}';

        if (frameRows) result_ += "\\hline";

        // ---- Rows ----
        for (uint32_t r = 0; r < rowCount; ++r) {
            if (r > 0) {
                result_ += "\\\\";
                if (frameRows) result_ += "\\hline";
            }
            const Node& rowNode = pr_.node(pr_.children[rowsNode.childrenStart + r]);
            if (!rowNode.isList()) {
                translate(pr_.children[rowsNode.childrenStart + r]);
                continue;
            }
            uint32_t cellCount = rowNode.childrenCount;
                for (uint32_t c = 0; c < cellCount; ++c) {
                if (c > 0) result_ += " & ";
                translate(pr_.children[rowNode.childrenStart + c]);
            }
        }

        if (frameRows) result_ += "\\\\\\hline";
        result_ += "\\end{array}";
    }

    // Helper: does a node (or any descendant) equal the symbol True?
    bool nodeContainsTrue(const Node& n) const {
        if (n.isSymbol() && pr_.str(n) == "True") return true;
        if (n.isList() || n.isExpr()) {
            uint32_t start = n.childrenStart + (n.isExpr() ? 1 : 0);
            uint32_t count = n.isExpr() ? n.childrenCount - 1 : n.childrenCount;
            for (uint32_t i = 0; i < count; ++i)
                if (nodeContainsTrue(pr_.node(pr_.children[start + i]))) return true;
        }
        return false;
    }

    // ---- ButtonBox[content, opts…] — render content only, drop interactive opts ----
    void translateButtonBox(uint32_t a, uint32_t n) {
        if (n == 0) return;
        translate(pr_.children[a]);
    }

    // ---- PaneSelectorBox[{False->display, True->hover, …}, Dynamic[…], opts…] ----
    //   Render the False (non-hover) branch — the static display state.
    void translatePaneSelectorBox(uint32_t a, uint32_t n) {
        if (n == 0) return;
        const Node& assocNode = pr_.node(pr_.children[a]);
        if (!assocNode.isList()) { translate(pr_.children[a]); return; }
        // Prefer the False branch
        for (uint32_t i = 0; i < assocNode.childrenCount; ++i) {
            const Node& rule = pr_.node(pr_.children[assocNode.childrenStart + i]);
            if ((rule.kind != NodeKind::Rule && rule.kind != NodeKind::RuleDelayed)
                || rule.childrenCount < 2) continue;
            const Node& lhsNode = pr_.node(pr_.children[rule.childrenStart]);
            if (lhsNode.isSymbol() && pr_.str(lhsNode) == "False") {
                translate(pr_.children[rule.childrenStart + 1]);
                return;
            }
        }
        // Fallback: render first rule's rhs
        for (uint32_t i = 0; i < assocNode.childrenCount; ++i) {
            const Node& rule = pr_.node(pr_.children[assocNode.childrenStart + i]);
            if ((rule.kind == NodeKind::Rule || rule.kind == NodeKind::RuleDelayed)
                && rule.childrenCount >= 2) {
                translate(pr_.children[rule.childrenStart + 1]);
                return;
            }
        }
    }

    // ---- InterpretationBox[display, interpretation, opts…] ----
    void translateInterpretationBox(uint32_t a, uint32_t /*n*/) {
        translate(pr_.children[a]);
    }

    // ---- FormBox[expr, form] ----
    void translateFormBox(uint32_t a, uint32_t /*n*/) {
        translate(pr_.children[a]);
    }

    // ---- TemplateBox[args_List, tag_String] ----
    void translateTemplateBox(uint32_t a, uint32_t n) {
        if (n < 2) {
            if (n == 1) translate(pr_.children[a]);
            return;
        }
        const Node& argListNode = pr_.node(pr_.children[a]);
        const Node& tagNode     = pr_.node(pr_.children[a + 1]);

        std::string_view tag;
        if (tagNode.isString() || tagNode.isSymbol()) tag = pr_.str(tagNode);

        // Mini template registry
        if (tag == "Sqrt" && argListNode.isList() && argListNode.childrenCount >= 1) {
            result_ += "\\sqrt{";
            translate(pr_.children[argListNode.childrenStart]);
            result_ += '}';
            return;
        }
        if (tag == "Abs" && argListNode.isList() && argListNode.childrenCount >= 1) {
            result_ += "\\left|";
            translate(pr_.children[argListNode.childrenStart]);
            result_ += "\\right|";
            return;
        }
        // Subsuperscript: TemplateBox[{base, sub, sup}, Subsuperscript]
        if (tag == "Subsuperscript" && argListNode.isList() &&
            argListNode.childrenCount >= 3) {
            translate(pr_.children[argListNode.childrenStart]);
            result_ += "_{";
            translate(pr_.children[argListNode.childrenStart + 1]);
            result_ += "}^{";
            translate(pr_.children[argListNode.childrenStart + 2]);
            result_ += '}';
            return;
        }
        // HyperlinkURL: TemplateBox[{display, url}, "HyperlinkURL"] → display only
        if (tag == "HyperlinkURL" && argListNode.isList() &&
            argListNode.childrenCount >= 1) {
            translate(pr_.children[argListNode.childrenStart]);
            return;
        }
        // RowDefault: join all items in order (inline row layout)
        if (tag == "RowDefault" && argListNode.isList()) {
            for (uint32_t i = 0; i < argListNode.childrenCount; ++i)
                translate(pr_.children[argListNode.childrenStart + i]);
            return;
        }
        // RowWithSeparators: {sep, quotedSep, item1, item2, …}
        // Used by the kernel for comma-separated subscripts like Q_{1,1}.
        if (tag == "RowWithSeparators" && argListNode.isList() &&
            argListNode.childrenCount >= 3) {
            std::string sep = translateToString(
                pr_.children[argListNode.childrenStart]);
            for (uint32_t i = 2; i < argListNode.childrenCount; ++i) {
                if (i > 2) result_ += sep;
                translate(pr_.children[argListNode.childrenStart + i]);
            }
            return;
        }
        // InactiveD: TemplateBox[{"Inactive", expr, var1, var2, …}, "InactiveD"]
        // Mathematica renders this as ∂_{var1,var2,...} expr (inactive partial derivative).
        if (tag == "InactiveD" && argListNode.isList() &&
            argListNode.childrenCount >= 3) {
            result_ += "\\partial_{";
            for (uint32_t i = 2; i < argListNode.childrenCount; ++i) {
                if (i > 2) result_ += ',';
                translate(pr_.children[argListNode.childrenStart + i]);
            }
            result_ += '}';
            translate(pr_.children[argListNode.childrenStart + 1]);
            return;
        }
        // Superscript / Subscript template boxes (base + script)
        if ((tag == "Superscript" || tag == "SuperscriptBox") &&
            argListNode.isList() && argListNode.childrenCount >= 2) {
            std::string base = translateToString(
                pr_.children[argListNode.childrenStart]);
            std::string exp  = translateToString(
                pr_.children[argListNode.childrenStart + 1]);
            result_ += base;
            result_ += (exp.size() == 1) ? "^" : "^{";
            result_ += exp;
            if (exp.size() != 1) result_ += '}';
            return;
        }
        if ((tag == "Subscript" || tag == "SubscriptBox") &&
            argListNode.isList() && argListNode.childrenCount >= 2) {
            std::string base = translateToString(
                pr_.children[argListNode.childrenStart]);
            std::string sub  = translateToString(
                pr_.children[argListNode.childrenStart + 1]);
            result_ += base + "_{" + sub + "}";
            return;
        }
        // Named-function templates: TemplateBox[{arg}, "Gamma"] → \Gamma\left(arg\right)
        // The kernel generates these for special functions displayed with parens.
        {
            static const std::unordered_map<std::string_view, std::string_view> kFuncCmds {
                {"Gamma",         "\\Gamma"},
                {"Zeta",          "\\zeta"},
                {"EulerGamma",    "\\gamma"},
                {"Beta",          "\\mathrm{B}"},
                {"PolyGamma",     "\\psi"},
                {"LogGamma",      "\\log\\Gamma"},
                {"Erf",           "\\mathrm{erf}"},
                {"Erfc",          "\\mathrm{erfc}"},
                {"Erfi",          "\\mathrm{erfi}"},
                {"FresnelS",      "S"},
                {"FresnelC",      "C"},
                {"SinIntegral",   "\\mathrm{Si}"},
                {"CosIntegral",   "\\mathrm{Ci}"},
                {"ExpIntegralEi", "\\mathrm{Ei}"},
                {"ProductLog",    "W"},
                {"HeavisideTheta","\\theta"},
                {"DiracDelta",    "\\delta"},
                {"KroneckerDelta","\\delta"},
            };
            auto it = kFuncCmds.find(tag);
            if (it != kFuncCmds.end() && argListNode.isList() &&
                argListNode.childrenCount >= 1) {
                result_ += it->second;
                result_ += "\\left(";
                for (uint32_t i = 0; i < argListNode.childrenCount; ++i) {
                    if (i > 0) result_ += ',';
                    translate(pr_.children[argListNode.childrenStart + i]);
                }
                result_ += "\\right)";
                return;
            }
        }

        // Fallback: join args
        std::fprintf(stderr, "[wolfbook] TemplateBox: unknown tag \"%.*s\"\n",
                     static_cast<int>(tag.size()), tag.data());
        if (argListNode.isList()) {
            for (uint32_t i = 0; i < argListNode.childrenCount; ++i)
                translate(pr_.children[argListNode.childrenStart + i]);
        } else {
            translate(pr_.children[a]);
        }
    }

    // ================================================================
    // GridBox[{{r1c1,r1c2,…},{r2c1,…},…}, opts…]
    //   env: "pmatrix", "bmatrix", "vmatrix", "cases", "aligned", …
    // ================================================================
    void translateGridBox(uint32_t a, uint32_t n, std::string_view env,
                           bool bracketLocked = false) {
        if (n == 0) return;

        // Scan opts: look for ColumnAlignments to detect "aligned".
        // Only apply when env was NOT determined by surrounding bracket characters
        // (bracketLocked=true) — otherwise ColumnAlignments->Center on a pmatrix
        // would wrongly convert the matrix to an aligned environment.
        std::string_view resolvedEnv = env;
        if (!bracketLocked) {
            for (uint32_t i = 1; i < n; ++i) {
                const Node& opt = pr_.node(pr_.children[a + i]);
                const Node* lhsNp = nullptr;
                const Node* rhsNp = nullptr;
                if (!extractRule(opt, lhsNp, rhsNp)) continue;
                if (lhsNp->isSymbol() && pr_.str(*lhsNp) == "ColumnAlignments") {
                    // Heuristic: explicit alignment opt on a bare GridBox → aligned env
                    if (resolvedEnv == "matrix") resolvedEnv = "aligned";
                }
            }
        }

        // First arg is the list of rows
        const Node& rowsNode = pr_.node(pr_.children[a]);

        result_ += "\\begin{";
        result_ += resolvedEnv;
        result_ += '}';

        // Iterate rows
        uint32_t rowCount = rowsNode.childrenCount;
        for (uint32_t r = 0; r < rowCount; ++r) {
            if (r > 0) result_ += "\\\\";
            // Each row is a List
            const Node& rowNode = pr_.node(pr_.children[rowsNode.childrenStart + r]);
            if (!rowNode.isList()) { translate(pr_.children[rowsNode.childrenStart + r]); continue; }
            uint32_t cellCount = rowNode.childrenCount;
            for (uint32_t c = 0; c < cellCount; ++c) {
                if (c > 0) result_ += " & ";
                translate(pr_.children[rowNode.childrenStart + c]);
            }
        }

        result_ += "\\end{";
        result_ += resolvedEnv;
        result_ += '}';
    }

    // ---- Fallback for unrecognised box heads ----
    void translateUnknownExpr(std::string_view head, uint32_t argStart, uint32_t argCount) {
        std::fprintf(stderr, "[wolfbook] boxToLatex: unknown box head \"%.*s\"\n",
                     static_cast<int>(head.size()), head.data());
        for (uint32_t i = 0; i < argCount; ++i)
            translate(pr_.children[argStart + i]);
    }

    // ================================================================
    // Delimiter → GridBox environment name
    // ================================================================
    // Map a delimiter pair to \left…\right LaTeX wrappers.
    // Returns empty strings for unrecognised pairs.
    static std::pair<std::string_view, std::string_view>
    delimToLeftRight(std::string_view open, std::string_view close) {
        if (open == "("  && close == ")")  return {"\\left(",       "\\right)"};
        if (open == "["  && close == "]")  return {"\\left[",       "\\right]"};
        if (open == "{"  && close == "}")  return {"\\left\\{",     "\\right\\}"};
        if (open == "|"  && close == "|")  return {"\\left|",       "\\right|"};
        if (open == "\u2016" && close == "\u2016") return {"\\left\\|", "\\right\\|"}; // ‖
        if (open == "\u2308" && close == "\u2309") return {"\\left\\lceil",  "\\right\\rceil"};  // ⌈⌉
        if (open == "\u230a" && close == "\u230b") return {"\\left\\lfloor", "\\right\\rfloor"}; // ⌊⌋
        if (open == "\u27e8" && close == "\u27e9") return {"\\left\\langle", "\\right\\rangle"}; // ⟨⟩
        if (open == "\\[LeftCeiling]"        && close == "\\[RightCeiling]")        return {"\\left\\lceil",  "\\right\\rceil"};
        if (open == "\\[LeftFloor]"           && close == "\\[RightFloor]")          return {"\\left\\lfloor", "\\right\\rfloor"};
        if (open == "\\[LeftAngleBracket]"    && close == "\\[RightAngleBracket]")   return {"\\left\\langle", "\\right\\rangle"};
        if (open == "\\[LeftBracketingBar]"   && close == "\\[RightBracketingBar]")  return {"\\left|",       "\\right|"};
        if (open == "\\[LeftDoubleBracketingBar]" && close == "\\[RightDoubleBracketingBar]") return {"\\left\\|", "\\right\\|"};
        return {"", ""};
    }

    static std::string_view delimToEnv(std::string_view open, std::string_view close) {
        if (open == "(" && close == ")")    return "pmatrix";
        if (open == "[" && close == "]")    return "bmatrix";
        if (open == "{" && close == "}")    return "Bmatrix";
        if (open == "|" && close == "|")    return "vmatrix";
        if (open == "||" && close == "||")  return "Vmatrix";
        if (open == "\\[LeftBracketingBar]"    && close == "\\[RightBracketingBar]")    return "vmatrix";
        if (open == "\\[LeftDoubleBracketingBar]" && close == "\\[RightDoubleBracketingBar]") return "Vmatrix";
        return {};  // not a recognised matrix context
    }
};

} // anonymous namespace

// ----------------------------------------------------------
// Public entry point
// ----------------------------------------------------------
BoxResult boxToLatex(std::string_view wlBoxString) {
    // Thread-local buffers: retain heap capacity across calls so the
    // allocator is only invoked on the first call per thread.
    thread_local std::string  tl_clean;
    thread_local ParseResult  tl_pr;

    try {
        // Strip WL line-continuation sequences (backslash + newline + leading
        // whitespace) that appear when input is pasted from a WL session or
        // copied across multiple lines.  Also collapse bare newlines to spaces.
        tl_clean.clear();
        tl_clean.reserve(wlBoxString.size());
        for (std::size_t i = 0; i < wlBoxString.size(); ) {
            char c = wlBoxString[i];
            if (c == '\\' && i + 1 < wlBoxString.size() &&
                (wlBoxString[i+1] == '\n' || wlBoxString[i+1] == '\r')) {
                // Skip backslash + newline + any leading whitespace on next line
                i += 2;
                if (i < wlBoxString.size() && wlBoxString[i] == '\n') ++i; // \r\n
                while (i < wlBoxString.size() &&
                       (wlBoxString[i] == ' ' || wlBoxString[i] == '\t')) ++i;
                continue;
            }
            if (c == '\n' || c == '\r') {
                // Bare newline (no backslash) — collapse to single space
                tl_clean += ' ';
                ++i;
                if (c == '\r' && i < wlBoxString.size() && wlBoxString[i] == '\n') ++i;
                // skip subsequent whitespace
                while (i < wlBoxString.size() &&
                       (wlBoxString[i] == ' ' || wlBoxString[i] == '\t')) ++i;
                continue;
            }
            tl_clean += c;
            ++i;
        }
        tl_pr.reset();
        WLParser{}.parseInto(tl_clean, tl_pr);
        return { BoxTranslator(tl_pr).run(), "" };
    } catch (const std::exception& ex) {
        std::string msg = ex.what();
        std::fprintf(stderr, "[wolfbook] boxToLatex parse error: %s\n", msg.c_str());
        return { std::string(wlBoxString), std::move(msg) };
    }
}

} // namespace wolfbook
