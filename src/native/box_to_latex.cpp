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

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace wolfbook {
namespace {

// ----------------------------------------------------------
// Trig style helpers (for BtlOptions rules)
// ----------------------------------------------------------

// Returns the LaTeX operator name for a WL or lowercase trig string,
// or an empty string_view if the name is not a recognised trig function.
static std::string_view getTrigLatex(std::string_view s) {
    using sv = std::string_view;
    static const std::pair<sv,sv> kTrig[] = {
        // lowercase (plain TraditionalForm output)
        {"sin","\\sin"},{"cos","\\cos"},{"tan","\\tan"},
        {"cot","\\cot"},{"sec","\\sec"},{"csc","\\csc"},
        {"arcsin","\\arcsin"},{"arccos","\\arccos"},{"arctan","\\arctan"},
        {"arccot","\\operatorname{arccot}"},{"arcsec","\\operatorname{arcsec}"},
        {"arccsc","\\operatorname{arccsc}"},
        {"sinh","\\sinh"},{"cosh","\\cosh"},{"tanh","\\tanh"},{"coth","\\coth"},
        {"arcsinh","\\operatorname{arcsinh}"},{"arccosh","\\operatorname{arccosh}"},
        {"arctanh","\\operatorname{arctanh}"},
        // WL capitalised (StandardForm / InputForm function names)
        {"Sin","\\sin"},{"Cos","\\cos"},{"Tan","\\tan"},
        {"Cot","\\cot"},{"Sec","\\sec"},{"Csc","\\csc"},
        {"ArcSin","\\arcsin"},{"ArcCos","\\arccos"},{"ArcTan","\\arctan"},
        {"ArcCot","\\operatorname{arccot}"},{"ArcSec","\\operatorname{arcsec}"},
        {"ArcCsc","\\operatorname{arccsc}"},
        {"Sinh","\\sinh"},{"Cosh","\\cosh"},{"Tanh","\\tanh"},{"Coth","\\coth"},
        {"ArcSinh","\\operatorname{arcsinh}"},{"ArcCosh","\\operatorname{arccosh}"},
        {"ArcTanh","\\operatorname{arctanh}"},
    };
    for (auto& [k, v] : kTrig)
        if (k == s) return v;
    return {};
}

// Returns true when the translated LaTeX string represents a single symbol:
//   • a single ASCII letter (e.g. "x")
//   • a bare LaTeX command with no arguments (e.g. "\phi ", "\alpha")
//   • a single multi-byte UTF-8 codepoint (e.g. "φ" encoded as 2–4 bytes)
static bool isSingleSymbolLatex(const std::string& s) {
    if (s.empty()) return false;
    // Single ASCII letter
    if (s.size() == 1) return std::isalpha(static_cast<unsigned char>(s[0])) != 0;
    // Bare LaTeX command: \alpha, \phi, \sigma, etc.
    // Pattern: \ [a-zA-Z]+ optionally followed by one trailing guard space
    if (s[0] == '\\') {
        size_t i = 1;
        while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
        if (i == s.size()) return true;
        if (i == s.size() - 1 && s[i] == ' ') return true;
        return false;
    }
    // Single multi-byte UTF-8 codepoint (no backslash, no braces)
    auto b = static_cast<unsigned char>(s[0]);
    size_t byteLen = (b & 0xE0) == 0xC0 ? 2 :
                     (b & 0xF0) == 0xE0 ? 3 :
                     (b & 0xF8) == 0xF0 ? 4 : 0;
    if (byteLen != 0 && byteLen == s.size()) return true;
    return false;
}

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

// Truncate a decimal number string (digits + optional '.', no sign) to
// nSig significant figures, rounding the last kept digit.
// Returns the input unmodified when it already has ≤ nSig sig figs.
static std::string truncateToSigFigs(std::string_view sv, int nSig) {
    if (nSig <= 0 || sv.empty()) return std::string(sv);

    // Build pure-digit string; remember where the decimal point sits.
    std::string digits;
    digits.reserve(sv.size());
    int dotIdx = -1;   // index in `digits` after which '.' falls
    for (char c : sv) {
        if (c == '.') { dotIdx = (int)digits.size(); }
        else          { digits += c; }
    }
    if (dotIdx < 0) dotIdx = (int)digits.size();  // integer input

    // First significant (non-zero) digit.
    int firstSig = -1;
    for (int i = 0; i < (int)digits.size(); ++i)
        if (digits[i] != '0') { firstSig = i; break; }
    if (firstSig < 0) return std::string(sv);  // all zeros

    int truncPos = firstSig + nSig;
    if (truncPos >= (int)digits.size())
        return std::string(sv);  // fewer digits than requested — keep as-is

    // Round: inspect the first discarded digit.
    std::string d = digits.substr(0, truncPos);
    if (digits[truncPos] >= '5') {
        int carry = 1;
        for (int i = (int)d.size() - 1; i >= 0 && carry; --i) {
            int val = (d[i] - '0') + carry;
            d[i]   = (char)('0' + val % 10);
            carry  = val / 10;
        }
        if (carry) { d = "1" + d; ++dotIdx; }
    }

    // Re-insert the decimal point.
    int dLen = (int)d.size();
    std::string result;
    if (dotIdx <= 0) {
        result = "0.";
        for (int i = 0; i < -dotIdx; ++i) result += '0';
        result += d;
    } else if (dotIdx >= dLen) {
        result = d;
        for (int i = dLen; i < dotIdx; ++i) result += '0';
    } else {
        result = d.substr(0, dotIdx) + '.' + d.substr(dotIdx);
    }
    return result;
}

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
// WL string escape helpers — for strings in both text and math contexts
// ----------------------------------------------------------

// Encode a Unicode codepoint (U+0000..U+10FFFF) as UTF-8 and append to out.
static void appendCodepointUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Parse n hex digits from sv[pos] and return the codepoint, or -1 on failure.
static int32_t parseHexN(std::string_view sv, size_t pos, size_t n) {
    if (pos + n > sv.size()) return -1;
    uint32_t cp = 0;
    for (size_t i = 0; i < n; ++i) {
        char h = sv[pos + i];
        if      (h >= '0' && h <= '9') cp = cp * 16 + static_cast<uint32_t>(h - '0');
        else if (h >= 'a' && h <= 'f') cp = cp * 16 + static_cast<uint32_t>(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') cp = cp * 16 + static_cast<uint32_t>(h - 'A' + 10);
        else return -1;
    }
    return static_cast<int32_t>(cp);
}

// Append sv to out as text-mode LaTeX (inside \text{...}), handling:
//   \[Name]   → wlCharToLatex; \command wrapped in $...$; \unicode{X} → UTF-8
//   \:XXXX    → UTF-8 decode (4-digit hex codepoint)
//   \.XX      → UTF-8 decode (2-digit hex, Latin-1 range)
//   high bytes (>0x7F) → skip (garbled Mathematica-internal encoding)
//   LaTeX special chars → escaped
static void appendWlTextContent(std::string& out, std::string_view sv) {
    size_t i = 0;
    while (i < sv.size()) {
        unsigned char c = static_cast<unsigned char>(sv[i]);

        // Non-ASCII byte: start of a multi-byte UTF-8 sequence (or an isolated
        // continuation byte from malformed UTF-8 which we skip).
        // Valid UTF-8 sequences are passed verbatim — KaTeX \text{} accepts them.
        if (c >= 0xC0) {
            size_t seqLen = (c >= 0xF0) ? 4u : (c >= 0xE0) ? 3u : 2u;
            size_t seqEnd = std::min(i + seqLen, sv.size());
            while (i < seqEnd) { out += static_cast<char>(sv[i++]); }
            continue;
        }
        if (c >= 0x80) { ++i; continue; }  // isolated continuation byte — skip

        // WL escape sequences starting with backslash
        if (c == '\\' && i + 1 < sv.size()) {
            char next = sv[i + 1];

            // \[Name] — named WL character
            if (next == '[') {
                size_t end = sv.find(']', i + 2);
                if (end != std::string_view::npos) {
                    std::string_view tok = sv.substr(i, end - i + 1);
                    auto m = wlCharToLatex(tok);
                    if (m.data() != nullptr && !m.empty()) {
                        if (m[0] == '\\') {
                            // Check for \unicode{XXXX} — decode to UTF-8
                            if (m.size() > 9 && m.substr(0, 9) == "\\unicode{") {
                                size_t hexEnd = m.find('}', 9);
                                if (hexEnd != std::string_view::npos) {
                                    int32_t cp = parseHexN(m, 9, hexEnd - 9);
                                    if (cp >= 0) appendCodepointUtf8(out, static_cast<uint32_t>(cp));
                                    else { out += '$'; out += m; if (!out.empty() && out.back() == ' ') out.pop_back(); out += '$'; }
                                }
                            } else if (m.size() >= 7 && m.substr(0, 6) == "\\text{" && m.back() == '}') {
                                // Already a text-mode construct (e.g. \text{\' e} for é).
                                // We're already inside \text{} context, so strip the wrapper
                                // and emit the inner content directly — avoids $\text{...}$.
                                out += m.substr(6, m.size() - 7);
                            } else {
                                // Math-mode command (e.g. \pi, \Omega) — wrap in $...$
                                out += '$';
                                out += m;
                                if (!out.empty() && out.back() == ' ') out.pop_back();  // strip guard space
                                out += '$';
                            }
                        } else {
                            out += m;  // plain UTF-8 char or operator
                        }
                    }
                    // Unknown named char → silently skip (avoids garbage in labels)
                    i = end + 1;
                    continue;
                }
            }

            // \:XXXX — 4-digit Unicode hex escape
            if (next == ':') {
                int32_t cp = parseHexN(sv, i + 2, 4);
                if (cp >= 0) {
                    appendCodepointUtf8(out, static_cast<uint32_t>(cp));
                    i += 6;
                    continue;
                }
            }

            // \.XX — 2-digit hex escape (Latin-1 range U+00..U+FF)
            if (next == '.') {
                int32_t cp = parseHexN(sv, i + 2, 2);
                if (cp >= 0) {
                    appendCodepointUtf8(out, static_cast<uint32_t>(cp));
                    i += 4;
                    continue;
                }
            }

            // WL string newline/tab/CR escapes — render as spaces in \text{}
            if (next == 'n' || next == 'r') { i += 2; out += ' '; continue; }
            if (next == 't')               { i += 2; out += ' '; continue; }
        }

        // LaTeX special characters that must be escaped inside \text{}
        switch (c) {
            case '_': out += "\\_";               break;
            case '^': out += "\\^{}";             break;
            case '$': out += "\\$";               break;
            case '%': out += "\\%";               break;
            case '&': out += "\\&";               break;
            case '#': out += "\\#";               break;
            case '{': out += "\\{";               break;
            case '}': out += "\\}";               break;
            case '~': out += "\\textasciitilde{}"; break;
            default:  out += static_cast<char>(c); break;
        }
        ++i;
    }
}

// ----------------------------------------------------------
// BoxTranslator — stateful per-call translator
// ----------------------------------------------------------
class BoxTranslator {
public:
    explicit BoxTranslator(const ParseResult& pr, const BtlOptions& opts = BtlOptions{})
        : pr_(pr), opts_(opts) {}

    BoxResult run() {
        // Heuristic: LaTeX output is roughly proportional to string pool size.
        // Reserve early to avoid mid-flight reallocations.
        result_.reserve(std::max<size_t>(512,
            pr_.strings.size() * 8 + pr_.nodes.size() * 4));
        translate(pr_.root);
        postProcess();
        BoxResult r;
        r.latex = std::move(result_);
        r.pages = std::move(pages_);
        // When paging produced multiple pages, latex already holds the first page
        // (written there by translateGridBox).  Nothing more to do.
        return r;
    }

private:
    const ParseResult& pr_;
    BtlOptions         opts_;
    std::string        result_;
    std::vector<std::string> pages_;      // populated when paging is triggered
    int                gridBoxDepth_ = 0; // nesting depth of translateGridBox calls

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
                if (j > 0 && j < result_.size() && result_[j-1] == '\\') {
                    result_ += "  ";
                } else {
                    // For RowBox[{"ab", " ", "cd"}], ensure a visual gap.
                    // KaTeX ignores single spaces between \mathrm{} blocks.
                    result_ += "\\,";
                }
            } else {
                result_ += "\\,";
            }
            return;
        }

        // Strip WL precision annotation from numeric string literals.
        //
        //   "3.1415`"           →  "3.1415"   (empty suffix)
        //   "3.1415`15"         →  "3.1415"   (integer/empty suffix, no trunc)
        //   "3.1415`15.3"       →  "3.1415"   (prec > digits, no trunc)
        //   "2.43…`20.15…"      →  "2.4320…"  (truncated to 20 sig figs)
        //   "0``19.76…"         →  0.×10^{-20} (accuracy notation for zero)
        //   "n``accuracy"       →  "n"         (non-zero accuracy: strip)
        //   "1.`*^-40"          →  \text{…}   (machine-precision tiny, keep)
        //   "Global`x"          →  \text{…}   (WL context sep, not a number)

        std::string      ownedStripped;   // owns buffer when truncation is used
        std::string_view stripped = s;
        auto btPos = s.find('`');
        if (btPos != std::string_view::npos) {
            std::string_view prefix  = s.substr(0, btPos);
            std::string_view afterBt = s.substr(btPos + 1);

            // A numeric prefix contains only digits, '.', '+', '-', 'e', 'E'.
            bool numericPrefix = !prefix.empty();
            for (char c : prefix)
                if (!std::isdigit(static_cast<unsigned char>(c)) &&
                    c != '.' && c != '-' && c != '+' && c != 'e' && c != 'E')
                    { numericPrefix = false; break; }

            if (!numericPrefix) {
                // WL context separator (e.g. "Global`x") or other non-number:
                // render verbatim with backtick → grave substitution.
                result_ += "\\text{";
                for (char c : s) {
                    if      (c == '`') result_ += "$\\grave{ }$";
                    else if (c == '^') result_ += "${}^{\\wedge}$";
                    else if (c == '$') result_ += "\\$";
                    else               result_ += c;
                }
                result_ += '}';
                return;
            }

            // Helper: parse a string_view as a double (returns 0.0 on failure).
            auto parseDouble = [](std::string_view sv) -> double {
                if (sv.empty()) return 0.0;
                std::string tmp(sv);
                char* end = nullptr;
                double v = std::strtod(tmp.c_str(), &end);
                return (end > tmp.c_str()) ? v : 0.0;
            };

            if (!afterBt.empty() && afterBt[0] == '`') {
                // ── Double backtick: accuracy notation  "n``accuracy" ──────────
                std::string_view accSv = afterBt.substr(1);
                bool validAcc = !accSv.empty();
                for (char c : accSv)
                    if (!std::isdigit(static_cast<unsigned char>(c)) &&
                        c != '.' && c != '-' && c != '+')
                        { validAcc = false; break; }

                // Check whether the prefix is zero (e.g. "0").
                bool isZero = !prefix.empty();
                for (char c : prefix)
                    if (c != '0' && c != '.' && c != '+' && c != '-')
                        { isZero = false; break; }

                if (isZero && validAcc) {
                    // "0``<accuracy>" → 0.×10^{-round(accuracy)}
                    double acc = parseDouble(accSv);
                    int    exp = static_cast<int>(std::round(acc));
                    result_ += "0.{\\times}10^{-";
                    result_ += std::to_string(exp);
                    result_ += '}';
                    return;
                }
                // Non-zero with accuracy or unparseable: strip the annotation.
                stripped = prefix;

            } else {
                // ── Single backtick: precision notation  "n`precision" ──────────
                // Suffix may be empty, all-digits, or decimal ("20.15…").
                // Also handle machine-precision form like "*^-40".
                bool numericSuffix = true;  // empty suffix counts as numeric
                for (char c : afterBt)
                    if (!std::isdigit(static_cast<unsigned char>(c)) &&
                        c != '.' && c != '-' && c != '+')
                        { numericSuffix = false; break; }

                if (numericSuffix) {
                    // Parse the precision value and truncate the number to
                    // that many significant digits.  Empty suffix → just strip.
                    double prec = afterBt.empty() ? 0.0 : parseDouble(afterBt);
                    int nSig = (prec > 0.5) ? static_cast<int>(std::round(prec)) : 0;
                    if (nSig > 0) {
                        ownedStripped = truncateToSigFigs(prefix, nSig);
                        stripped = ownedStripped;
                    } else {
                        stripped = prefix;
                    }
                } else {
                    // Non-numeric suffix — check for "prec*^exp" scientific
                    // notation form first, e.g. afterBt = "10.*^-23".
                    auto starPos = afterBt.find("*^");
                    bool handled = false;
                    if (starPos != std::string_view::npos) {
                        std::string_view precSv = afterBt.substr(0, starPos);
                        std::string_view expSv  = afterBt.substr(starPos + 2);
                        bool validPrec = !precSv.empty();
                        for (char c : precSv)
                            if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.')
                                { validPrec = false; break; }
                        size_t expStart = (!expSv.empty() &&
                            (expSv[0] == '-' || expSv[0] == '+')) ? 1u : 0u;
                        bool validExp = expSv.size() > expStart;
                        for (size_t i = expStart; i < expSv.size(); ++i)
                            if (!std::isdigit(static_cast<unsigned char>(expSv[i])))
                                { validExp = false; break; }
                        if (validPrec && validExp) {
                            // Truncate mantissa to stated precision sig figs.
                            double prec = parseDouble(precSv);
                            int nSig = (prec > 0.5) ? static_cast<int>(std::round(prec)) : 0;
                            std::string mantissa = (nSig > 0)
                                ? truncateToSigFigs(prefix, nSig)
                                : std::string(prefix);
                            // Strip trailing '.' before \times: "1." → "1"
                            while (!mantissa.empty() && mantissa.back() == '.')
                                mantissa.pop_back();
                            result_ += mantissa;
                            result_ += "\\times 10^{";
                            result_ += expSv;
                            result_ += '}';
                            handled = true;
                        }
                    }
                    if (!handled) {
                        // Unrecognised suffix (e.g. bare "*^-40"): render as
                        // \text{} with backtick → grave, caret → wedge.
                        result_ += "\\text{";
                        for (char c : s) {
                            if      (c == '`') result_ += "$\\grave{ }$";
                            else if (c == '^') result_ += "${}^{\\wedge}$";
                            else if (c == '$') result_ += "\\$";
                            else               result_ += c;
                        }
                        result_ += '}';
                    }
                    return;
                }
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
                    // Guard space: prevent command merging with next sibling
                    if (!mappedInner.empty() && std::isalpha(static_cast<unsigned char>(mappedInner.back())))
                        result_ += ' ';
                } else {
                    result_ += "\\text{";
                    appendWlTextContent(result_, inner);
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
            // If the command ends with a letter (e.g. \intop, \alpha) ensure
            // it cannot merge with the next RowBox sibling — insert a delimiter
            // space so "\intop" + "f" doesn't become the undefined "\intopf".
            if (!mapped.empty() && std::isalpha(static_cast<unsigned char>(mapped.back())))
                result_ += ' ';
            return;
        }
        // String may embed one or more \[Name] or \:XXXX sequences mixed with plain text
        // e.g. "\[CapitalSigma]n" or "\:03a9" or "\[Alpha]\[Beta]".  Scan and translate.
        if (stripped.find("\\[") != std::string_view::npos ||
            stripped.find("\\:") != std::string_view::npos ||
            stripped.find("\\.") != std::string_view::npos) {
            size_t i = 0;
            while (i < stripped.size()) {
                if (stripped[i] == '\\' && i + 1 < stripped.size()) {
                    char next = stripped[i + 1];
                    if (next == '[') {
                        size_t end = stripped.find(']', i + 2);
                        if (end != std::string_view::npos) {
                            std::string_view tok = stripped.substr(i, end - i + 1);
                            auto m = wlCharToLatex(tok);
                            if (m.data() != nullptr) {
                                result_ += m;
                                // If the command ends with a letter, insert a
                                // guard space when the next char in this string
                                // is also a letter OR when we're at the end of
                                // the string (the next RowBox sibling may start
                                // with a letter, e.g. \intop + f → \intop f).
                                if (!m.empty() && std::isalpha(static_cast<unsigned char>(m.back()))
                                    && (end + 1 >= stripped.size()
                                        || std::isalpha(static_cast<unsigned char>(stripped[end + 1]))))
                                    result_ += ' ';
                            } else {
                                result_ += "{\\square}"; // unknown WL char — □ placeholder
                            }
                            i = end + 1;
                            continue;
                        }
                    }
                    // \:XXXX — Unicode hex escape; emit as UTF-8 (works in KaTeX math mode)
                    if (next == ':') {
                        int32_t cp = parseHexN(stripped, i + 2, 4);
                        if (cp >= 0) {
                            appendCodepointUtf8(result_, static_cast<uint32_t>(cp));
                            i += 6;
                            continue;
                        }
                    }
                    // \.XX — 2-digit hex escape
                    if (next == '.') {
                        int32_t cp = parseHexN(stripped, i + 2, 2);
                        if (cp >= 0) {
                            appendCodepointUtf8(result_, static_cast<uint32_t>(cp));
                            i += 4;
                            continue;
                        }
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
        if (stripped == "|->") { result_ += "\\mapsto "; return; } // Function[{x},body] mapsto arrow
        if (stripped == "&")   { result_ += "\\& ";      return; } // pure-function ampersand
        // WL logical/bitwise infix operators
        if (stripped == "&&")  { result_ += "\\land ";   return; } // And
        if (stripped == "||")  { result_ += "\\lor ";    return; } // Or
        if (stripped == "!")   { result_ += "\\lnot ";   return; } // Not
        // WL Association brackets
        if (stripped == "<|")  { result_ += "\\langle|"; return; }
        if (stripped == "|>")  { result_ += "|\\rangle"; return; }
        // Big-O order symbol (appears in SeriesData output as the bare string "O")
        if (stripped == "O")   { result_ += "\\mathcal{O}"; return; }
        // WL relational operators that map to single LaTeX symbols
        if (stripped == "==")  { result_ += '=';         return; }
        if (stripped == "!=")  { result_ += "\\neq ";    return; }
        if (stripped == ">=")  { result_ += "\\geq ";    return; }
        if (stripped == "<=")  { result_ += "\\leq ";    return; }
        // Raw multiplication sign U+00D7 (UTF-8 \xc3\x97) — appears in
        // ScientificForm output as \[Times]; emit as \times in math mode.
        if (stripped == "\xc3\x97" || stripped == "\xd7") { result_ += "\\times "; return; }
        // Bare underscore (WL Blank pattern) — escape it in LaTeX math
        if (stripped == "_")   { result_ += "\\_";       return; }
        // WL Slot: #, #1, #2, ... — pure function argument placeholder
        if (!stripped.empty() && stripped[0] == '#') {
            result_ += "\\# ";
            result_ += stripped.substr(1);  // trailing digit(s) or empty
            return;
        }
        // WL system variable names starting with '$' (e.g. $Aborted, $Failed,
        // $Version).  Must be wrapped in \text{\$...} — `$` is a math delimiter
        // and cannot appear bare or inside \mathrm{} in KaTeX/LaTeX math mode.
        if (!stripped.empty() && stripped[0] == '$') {
            result_ += "\\text{\\$";
            for (size_t i = 1; i < stripped.size(); ++i) {
                char c = stripped[i];
                if      (c == '_') result_ += "\\_";
                else if (c == '^') result_ += "\\^{}";
                else if (c == '%') result_ += "\\%";
                else if (c == '&') result_ += "\\&";
                else if (c == '#') result_ += "\\#";
                else if (c == '{') result_ += "\\{";
                else if (c == '}') result_ += "\\}";
                else               result_ += c;
            }
            result_ += '}';
            return;
        }
        // Safety net: Wolfram BMP Private-Use-Area chars not in kTable.
        // BMP PUA: U+E000-U+EFFF → UTF-8 starts with 0xEE.
        //          U+F000-U+F8FF → UTF-8 starts with 0xEF, second byte 0x80-0xA3.
        // Standard Unicode (emoji U+1F..., math symbols, Greek, etc.) is NOT blocked
        // and passes through as raw UTF-8 for KaTeX to render natively.
        if (!stripped.empty()) {
            unsigned char _c0 = static_cast<unsigned char>(stripped[0]);
            bool _isPUA = (_c0 == 0xEE);
            if (!_isPUA && _c0 == 0xEF && stripped.size() >= 2) {
                unsigned char _c1 = static_cast<unsigned char>(stripped[1]);
                _isPUA = (_c1 >= 0x80 && _c1 <= 0xA3);
            }
            if (_isPUA) {
                result_ += "{\\square}";
                return;
            }
        }
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
                    if      (c == '_') result_ += "\\_";
                    else if (c == '$') result_ += "\\$";
                    else if (c == '%') result_ += "\\%";
                    else if (c == '&') result_ += "\\&";
                    else if (c == '#') result_ += "\\#";
                    else if (c == '{') result_ += "\\{";
                    else if (c == '}') result_ += "\\}";
                    else               result_ += c;
                }
                break;
            case TokenClass::MultiLetter: {
                // Standard LaTeX math operators → use \cmd directly (no \mathrm).
                // These have proper spacing built into LaTeX's operator font.
                static const std::unordered_map<std::string_view, std::string_view> kMathOps {
                    // Trig
                    {"sin","\\sin"},{"cos","\\cos"},{"tan","\\tan"},
                    {"cot","\\cot"},{"sec","\\sec"},{"csc","\\csc"},
                    // Inverse trig
                    {"arcsin","\\arcsin"},{"arccos","\\arccos"},{"arctan","\\arctan"},
                    // Hyperbolic
                    {"sinh","\\sinh"},{"cosh","\\cosh"},{"tanh","\\tanh"},{"coth","\\coth"},
                    // Exp / log
                    {"exp","\\exp"},{"log","\\log"},{"ln","\\ln"},{"lg","\\lg"},
                    // Limits / bounds
                    {"lim","\\lim"},{"limsup","\\limsup"},{"liminf","\\liminf"},
                    {"max","\\max"},{"min","\\min"},{"sup","\\sup"},{"inf","\\inf"},
                    // Algebra / misc
                    {"det","\\det"},{"dim","\\dim"},{"gcd","\\gcd"},{"lcm","\\operatorname{lcm}"},
                    {"ker","\\ker"},{"deg","\\deg"},{"arg","\\arg"},
                    {"hom","\\hom"},{"Pr","\\Pr"},{"Re","\\operatorname{Re}"},{"Im","\\operatorname{Im}"},
                    {"tr","\\operatorname{tr}"},{"Tr","\\operatorname{Tr}"},
                    {"diag","\\operatorname{diag}"},{"rank","\\operatorname{rank}"},
                    {"sgn","\\operatorname{sgn}"},{"sign","\\operatorname{sign}"},
                    {"mod","\\operatorname{mod}"},
                    // WL capitalised function names (StandardForm / InputForm output)
                    {"Sin","\\sin"},{"Cos","\\cos"},{"Tan","\\tan"},
                    {"Cot","\\cot"},{"Sec","\\sec"},{"Csc","\\csc"},
                    {"Sinh","\\sinh"},{"Cosh","\\cosh"},{"Tanh","\\tanh"},{"Coth","\\coth"},
                    {"ArcSin","\\arcsin"},{"ArcCos","\\arccos"},{"ArcTan","\\arctan"},
                    {"ArcCot","\\operatorname{arccot}"},{"ArcSec","\\operatorname{arcsec}"},{"ArcCsc","\\operatorname{arccsc}"},
                    {"ArcSinh","\\operatorname{arcsinh}"},{"ArcCosh","\\operatorname{arccosh}"},{"ArcTanh","\\operatorname{arctanh}"},
                    {"Exp","\\exp"},{"Log","\\log"},{"Log2","\\log_2"},{"Log10","\\log_{10}"},
                    {"Max","\\max"},{"Min","\\min"},{"Det","\\det"},{"GCD","\\gcd"},{"LCM","\\operatorname{lcm}"},
                    {"Abs","\\left|\\cdot\\right|"}, // rarely appears as bare string but map defensively
                };
                auto oit = kMathOps.find(stripped);
                if (oit != kMathOps.end()) {
                    result_ += oit->second;
                } else {
                    result_ += "\\mathrm{";
                    for (char c : stripped) {
                        if      (c == '$') result_ += "\\$";
                        else if (c == '_') result_ += "\\_";
                        else if (c == '%') result_ += "\\%";
                        else if (c == '&') result_ += "\\&";
                        else if (c == '#') result_ += "\\#";
                        else if (c == '{') result_ += "\\{";
                        else if (c == '}') result_ += "\\}";
                        else               result_ += c;
                    }
                    result_ += '}';
                }
                break;
            }
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
        else if (head == "DynamicModuleBox")    { /* complex dynamic UI widget — suppress */ }
        else if (head == "MouseAppearanceTag")  { /* suppress */ }
        else if (head == "GraphicsBox")         result_ += "[graphics]";
        else if (head == "RGBColor")            { /* colour node — handled by StyleBox */ }
        else if (head == "InformationData")     translateInformationData(n);
        else                                    translateUnknownExpr(head, argStart, argCount);
    }

    // ================================================================
    // Box handlers
    // ================================================================

    // ---- RowBox[{e1, e2, …}] ----
    // The single argument is always a List node.
    // ---- Trig style helper: tryExtractTrigCall ----
    // Checks if a node is RowBox[{trig, "(", singleSymbol, ")"}].
    // On success fills trigCmd (e.g. "\\sin") and argStr (translated arg)
    // and returns true.  Does not modify result_.
    bool tryExtractTrigCall(uint32_t nodeIdx, std::string& trigCmd, std::string& argStr) {
        const Node& node = pr_.node(nodeIdx);
        if (!node.isExpr()) return false;
        if (pr_.headName(node) != "RowBox") return false;
        if (node.childrenCount < 2) return false;
        const Node& listNode = pr_.node(pr_.children[node.childrenStart + 1]);
        if (listNode.kind != NodeKind::List || listNode.childrenCount != 4) return false;
        const Node& n0 = pr_.node(pr_.children[listNode.childrenStart]);      // trig name
        const Node& n1 = pr_.node(pr_.children[listNode.childrenStart + 1]);  // "("
        const Node& n3 = pr_.node(pr_.children[listNode.childrenStart + 3]);  // ")"
        if (!(n0.isString() || n0.isSymbol())) return false;
        if (!(n1.isString() && pr_.str(n1) == "(")) return false;
        if (!(n3.isString() && pr_.str(n3) == ")")) return false;
        std::string_view cmd = getTrigLatex(pr_.str(n0));
        if (cmd.empty()) return false;
        std::string arg = translateToString(pr_.children[listNode.childrenStart + 2]);
        if (!isSingleSymbolLatex(arg)) return false;
        trigCmd = std::string(cmd);
        argStr  = std::move(arg);
        return true;
    }

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

        // Rule 1 (trigOmitParens): RowBox[{trig, "(", singleSymbol, ")"}]
        // → \trig symbol  (no outer parens)
        if (opts_.trigOmitParens && nElems == 4) {
            const Node& rn0 = pr_.node(pr_.children[lStart]);
            const Node& rn1 = pr_.node(pr_.children[lStart + 1]);
            const Node& rn3 = pr_.node(pr_.children[lStart + 3]);
            if ((rn0.isString() || rn0.isSymbol()) &&
                (rn1.isString() && pr_.str(rn1) == "(") &&
                (rn3.isString() && pr_.str(rn3) == ")")) {
                std::string_view cmd = getTrigLatex(pr_.str(rn0));
                if (!cmd.empty()) {
                    std::string arg = translateToString(pr_.children[lStart + 2]);
                    if (isSingleSymbolLatex(arg)) {
                        result_ += cmd;
                        // need a space separator when arg is a bare letter/name
                        // (no backslash) to avoid e.g. \cost instead of \cos t
                        if (!arg.empty() && arg[0] != '\\') result_ += ' ';
                        result_ += arg;
                        return;
                    }
                }
            }
        }

        // Function-call style: RowBox[{head, "(", args..., ")"}]
        // The wrapping-delimiter check above only fires when the FIRST element
        // is itself an open delimiter.  Here we catch cases where an operator
        // or function name precedes the opening paren, e.g.:
        //   O(\frac{1}{x})  →  O\left(\frac{1}{x}\right)
        //   am(σ|\frac{…})  →  \mathrm{am}\left(σ|\frac{…}\right)
        //   csc^2(χ)        →  \csc^2\left(\chi\right)
        // This ensures tall content (fractions, nested \left\right) auto-sizes
        // the outer delimiters correctly.
        if (nElems >= 3) {
            const Node& snd  = pr_.node(pr_.children[lStart + 1]);
            const Node& llst = pr_.node(pr_.children[lStart + nElems - 1]);
            if ((snd.isString() || snd.isSymbol()) && (llst.isString() || llst.isSymbol())) {
                auto [lOpen, lClose] = delimToLeftRight(pr_.str(snd), pr_.str(llst));
                if (!lOpen.empty()) {
                    translate(pr_.children[lStart]);          // head / name
                    result_ += lOpen;
                    for (uint32_t i = 2; i + 1 < nElems; ++i)
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

    // ---- Helper: does the base expression contain an unbraced ^ or _?
    // If so it must be wrapped in {} when used as a super/subscript base to
    // prevent the "double superscript / double subscript" LaTeX error.
    static bool baseNeedsBracing(const std::string& s) {
        int depth = 0;
        for (char c : s) {
            if      (c == '{') ++depth;
            else if (c == '}') { if (depth > 0) --depth; }
            else if ((c == '^' || c == '_') && depth == 0) return true;
        }
        return false;
    }

    // ---- Post-processing atom scanners ----
    // Walk backwards from `end` (exclusive) over one LaTeX atom:
    //   {balanced block}  |  \command[guard-space]  |  single char
    // Returns the start index of that atom.
    static size_t scanAtomBackward(const std::string& s, size_t end) {
        if (end == 0) return 0;
        size_t i = end;
        if (i > 0 && s[i-1] == ' ') --i;   // skip guard space
        if (i == 0) return 0;
        if (s[i-1] == '}') {
            int d = 0;
            while (i > 0) { --i; if (s[i]=='}') ++d; else if (s[i]=='{') { if (--d==0) return i; } }
            return 0;
        }
        if (std::isalpha(static_cast<unsigned char>(s[i-1])) ||
            std::isdigit(static_cast<unsigned char>(s[i-1]))) {
            while (i > 0 && (std::isalnum(static_cast<unsigned char>(s[i-1])))) --i;
            if (i > 0 && s[i-1] == '\\') return i - 1;
            return i;
        }
        return i - 1;
    }

    // Walk forward from `pos` over one LaTeX atom. Returns position after atom.
    static size_t skipAtomForward(const std::string& s, size_t pos) {
        if (pos >= s.size()) return pos;
        if (s[pos] == '{') {
            int d = 0;
            while (pos < s.size()) {
                if (s[pos]=='{') ++d; else if (s[pos]=='}') { if (--d==0) return pos+1; }
                ++pos;
            }
            return pos;
        }
        if (s[pos] == '\\') {
            ++pos;
            while (pos < s.size() && std::isalpha(static_cast<unsigned char>(s[pos]))) ++pos;
            if (pos < s.size() && s[pos] == ' ') ++pos;  // skip guard space
            return pos;
        }
        return pos + 1;
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

    // ---- Helper: count derivative primes in a box-expression node ----
    // Returns the number of primes if the node is purely prime characters
    // (single \[Prime], \[DoublePrime], or a RowBox concatenation of them),
    // and 0 if the node contains anything else.
    int countPrimes(uint32_t idx) {
        const Node& n = pr_.node(idx);
        if (n.kind == NodeKind::String) {
            std::string_view s = pr_.str(n);
            if (s == "\\[Prime]" || s == "\u2032") return 1;
            if (s == "\\[DoublePrime]" || s == "\u2033") return 2;
            if (s == "'") return 1;
            // Comma notation: "," = 1 prime, ",," = 2, ",,," = 3, etc.
            if (!s.empty() && s.find_first_not_of(',') == std::string_view::npos) return (int)s.size();
            return 0;
        }
        if (n.kind == NodeKind::Expr) {
            std::string_view head = pr_.str(pr_.node(pr_.children[n.childrenStart]));
            if (head != "RowBox") return 0;
            // RowBox[{children...}] — check the List argument
            if (n.childrenCount < 2) return 0;
            const Node& listNode = pr_.node(pr_.children[n.childrenStart + 1]);
            if (listNode.kind != NodeKind::List) return 0;
            int total = 0;
            for (uint32_t i = 0; i < listNode.childrenCount; ++i) {
                int p = countPrimes(pr_.children[listNode.childrenStart + i]);
                if (p == 0) return 0;
                total += p;
            }
            return total;
        }
        return 0;
    }

    // ---- Helper: detect dot/ddot OverscriptBox patterns ----
    // Returns "dot" or "ddot" if the over-content is a single- or double-dot,
    // empty string otherwise.
    std::string detectDotOver(uint32_t idx) {
        const Node& n = pr_.node(idx);
        if (n.kind == NodeKind::String) {
            std::string_view s = pr_.str(n);
            if (s == "." || s == "\u02D9" || s == "\\[Dot]") return "dot";
            if (s == ".." || s == "\u00A8" || s == "\\[DoubleDot]") return "ddot";
        }
        // The over content might translate to \dot{} or \ddot{} via special_chars
        std::string translated = translateToString(idx);
        if (translated == "\\dot{ }" || translated == "\\dot{}") return "dot";
        if (translated == "\\ddot{ }" || translated == "\\ddot{}") return "ddot";
        return "";
    }

    // ---- SuperscriptBox[base, exp] ----
    // Special handling for derivative primes: SuperscriptBox["f", "\[Prime]"]
    // should produce f' not f^{^{\prime}} (double superscript).
    void translateSuperscriptBox(uint32_t a, uint32_t n) {
        if (n < 2) return;

        // Check if the exponent is a derivative prime marker before translating
        int primeCount = countPrimes(pr_.children[a + 1]);
        if (primeCount > 0) {
            std::string base = translateToString(pr_.children[a]);
            if (baseNeedsBracing(base)) { result_ += '{'; result_ += base; result_ += '}'; }
            else result_ += base;
            for (int i = 0; i < primeCount; ++i)
                result_ += '\'';
            return;
        }

        // Rule 2 (trigPowerForm): SuperscriptBox[RowBox[{"(", trigCall, ")"}], exp]
        // → \trig^{exp} symbol   where trigCall = RowBox[{trig,"(",sym,")"}]
        if (opts_.trigPowerForm) {
            const Node& baseNode = pr_.node(pr_.children[a]);
            if (baseNode.isExpr() && pr_.headName(baseNode) == "RowBox" &&
                baseNode.childrenCount >= 2) {
                const Node& listNode = pr_.node(pr_.children[baseNode.childrenStart + 1]);
                if (listNode.kind == NodeKind::List && listNode.childrenCount == 3) {
                    const Node& first = pr_.node(pr_.children[listNode.childrenStart]);
                    const Node& last  = pr_.node(pr_.children[listNode.childrenStart + 2]);
                    if ((first.isString() || first.isSymbol()) && pr_.str(first) == "(" &&
                        (last.isString()  || last.isSymbol())  && pr_.str(last)  == ")") {
                        std::string trigCmd, argStr;
                        if (tryExtractTrigCall(pr_.children[listNode.childrenStart + 1],
                                               trigCmd, argStr)) {
                            std::string exp = translateToString(pr_.children[a + 1]);
                            result_ += trigCmd;
                            result_ += '^';
                            if (needsBraces(exp)) { result_ += '{'; result_ += exp; result_ += '}'; }
                            else                  { result_ += exp; }
                            if (!argStr.empty() && argStr[0] != '\\') result_ += ' ';
                            result_ += argStr;
                            return;
                        }
                    }
                }
            }
        }

        std::string base = translateToString(pr_.children[a]);
        std::string exp  = translateToString(pr_.children[a + 1]);
        if (baseNeedsBracing(base)) { result_ += '{'; result_ += base; result_ += '}'; }
        else result_ += base;
        result_ += '^';
        if (needsBraces(exp)) { result_ += '{'; result_ += exp; result_ += '}'; }
        else                  { result_ += exp; }
    }

    // ---- SubscriptBox[base, sub] ----
    void translateSubscriptBox(uint32_t a, uint32_t n) {
        if (n < 2) return;
        std::string base = translateToString(pr_.children[a]);
        std::string sub  = translateToString(pr_.children[a + 1]);
        if      (base.empty())            result_ += "{}";
        else if (baseNeedsBracing(base)) { result_ += '{'; result_ += base; result_ += '}'; }
        else                              result_ += base;
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
        if      (base.empty())            result_ += "{}";
        else if (baseNeedsBracing(base)) { result_ += '{'; result_ += base; result_ += '}'; }
        else                              result_ += base;
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
        // Detect dot/ddot notation for time derivatives
        std::string dotKind = detectDotOver(pr_.children[a + 1]);
        if (!dotKind.empty()) {
            result_ += '\\';
            result_ += dotKind;
            result_ += '{';
            translate(pr_.children[a]);
            result_ += '}';
            return;
        }
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

            // Named WL style strings: "TI" = TraditionalForm Italic,
            // "TR" = TraditionalForm Roman.  These appear as positional
            // string arguments in boxes from Information[], Usage text, etc.
            if (opt.isString()) {
                std::string_view sv = pr_.str(opt);
                if (sv == "TI") italic = true;
                // "TR" is the default (roman) for multi-letter tokens — no flag needed
                continue;
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

        // When a text-styling flag is active and the body was a plain
        // multi-letter string that kMathOps mapped to an operator command
        // (e.g. "min" → \min), override with the raw text so the font
        // wrapper produces correct output (\mathit{min} not \mathit{\min}).
        if (italic || bold) {
            const Node& bodyNode = pr_.node(pr_.children[a]);
            if (bodyNode.isString()) {
                std::string_view raw = pr_.str(bodyNode);
                if (classifyToken(raw) == TokenClass::MultiLetter) {
                    inner = std::string(raw);
                }
            }
        }

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
    void translateInterpretationBox(uint32_t a, uint32_t n) {
        // When the interpretation is InformationData (from ?Symbol / Information[]),
        // render a structured usage block instead of the complex UI display form.
        if (n >= 2) {
            const Node& interpNode = pr_.node(pr_.children[a + 1]);
            if (interpNode.kind == NodeKind::Expr &&
                pr_.headName(interpNode) == "InformationData") {
                translateInformationData(interpNode);
                return;
            }
        }
        // If the display arg is a string containing \!\(\* embedded box expressions
        // (e.g. StringForm output like "level \!\(\*FormBox[\"0\", TraditionalForm]\)"),
        // render it inline — extract the plain text prefix/suffix and recursively
        // translate each embedded box sub-expression.
        const Node& dispNode = pr_.node(pr_.children[a]);
        if (dispNode.kind == NodeKind::String) {
            std::string_view sv = pr_.str(dispNode);
            InfoBox ib = findNextInfoBox(sv, 0);
            if (ib.patStart != std::string_view::npos) {
                // Has at least one embedded box — render the whole string inline.
                renderInterpBoxString(sv);
                return;
            }
        }
        translate(pr_.children[a]);
    }

    // Render a string that may contain \!\(\*BoxExpr\) sequences inline (no array context).
    // Plain text segments → \text{…}; embedded boxes → translated recursively.
    void renderInterpBoxString(std::string_view s) {
        size_t pos = 0;
        while (pos < s.size()) {
            InfoBox ib = findNextInfoBox(s, pos);
            if (ib.patStart == std::string_view::npos) {
                // No more embedded boxes — emit remaining plain text
                emitInlineText(s.substr(pos));
                break;
            }
            if (ib.patStart > pos) emitInlineText(s.substr(pos, ib.patStart - pos));
            // Pre-process box expression: unescape \" → "
            std::string_view rawExpr = s.substr(ib.cStart, ib.cEnd - ib.cStart);
            std::string boxExpr;
            boxExpr.reserve(rawExpr.size());
            for (size_t i = 0; i < rawExpr.size(); ++i) {
                if (rawExpr[i] == '\\' && i + 1 < rawExpr.size() && rawExpr[i+1] == '"') {
                    boxExpr += '"'; ++i;
                } else {
                    boxExpr += rawExpr[i];
                }
            }
            try {
                ParseResult innerPr;
                WLParser{}.parseInto(boxExpr, innerPr);
                result_ += BoxTranslator(innerPr, opts_).run().latex;
            } catch (...) {
                result_ += "\\cdots";
            }
            pos = (ib.cEnd + 2 <= s.size()) ? ib.cEnd + 2 : s.size(); // skip past \)
        }
    }

    // Emit a plain-text segment inline (in math mode), escaping special chars.
    // Strips surrounding WL quote characters and ignores empty segments.
    void emitInlineText(std::string_view text) {
        // Strip leading/trailing " that WL wraps string values in
        while (!text.empty() && text.front() == '"') text.remove_prefix(1);
        while (!text.empty() && text.back()  == '"') text.remove_suffix(1);
        if (text.empty()) return;
        result_ += "\\text{";
        for (char c : text) {
            switch (c) {
                case '#':  result_ += "\\#"; break;
                case '%':  result_ += "\\%"; break;
                case '&':  result_ += "\\&"; break;
                case '_':  result_ += "\\_"; break;
                case '{':  result_ += "\\{"; break;
                case '}':  result_ += "\\}"; break;
                case '$':  result_ += "\\$"; break;
                case '\\': result_ += "\\textbackslash{}"; break;
                default:   result_ += c;
            }
        }
        result_ += '}';
    }

    // ---- InformationData[<|"FullName"->…, "Usage"->…, …|>, False] ----
    // Renders the symbol name, usage text, attributes and options extracted from
    // the parsed Rule children.
    // (The <|…|> association is partially parsed by the WL parser — Rule nodes
    //  whose values are plain strings/symbols/lists survive; nested associations
    //  don't, so "Documentation"'s local key is lost, but "Web" leaks through.)

    // Helper: emit a single \text{…} atom with LaTeX special-char escaping.
    void infoTextAtom(std::string_view sv) {
        result_ += "\\text{";
        for (char c : sv) {
            switch (c) {
                case '"':  break;
                case '#':  result_ += "\\#"; break;
                case '%':  result_ += "\\%"; break;
                case '&':  result_ += "\\&"; break;
                case '_':  result_ += "\\_"; break;
                case '{':  result_ += "\\{"; break;
                case '}':  result_ += "\\}"; break;
                case '$':  result_ += "\\$"; break;
                case '\\': result_ += "\\textbackslash{}"; break;
                default:   result_ += c;
            }
        }
        result_ += "}";
    }

    // Render Attributes list: {Protected, ReadProtected, …}
    void renderInfoAttributes(uint32_t listIdx) {
        const Node& list = pr_.node(listIdx);
        result_ += "\\text{Attributes: }\\{";
        for (uint32_t i = 0; i < list.childrenCount; ++i) {
            if (i > 0) result_ += "\\text{, }";
            infoTextAtom(pr_.str(pr_.node(pr_.children[list.childrenStart + i])));
        }
        result_ += "\\}";
    }

    // Render a single option value: symbols/strings as \text{}, lists as \{…\},
    // numbers as numerals, and Power[x,-1] as \frac{1}{x}.
    void renderInfoOptValue(uint32_t idx) {
        const Node& n = pr_.node(idx);
        if (n.isSymbol() || n.isString()) {
            infoTextAtom(pr_.str(n));
        } else if (n.kind == NodeKind::Integer) {
            result_ += std::to_string(n.iVal);
        } else if (n.kind == NodeKind::Real) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%g", n.dVal);
            result_ += buf;
        } else if (n.isList()) {
            result_ += "\\{";
            for (uint32_t i = 0; i < n.childrenCount; ++i) {
                if (i > 0) result_ += "\\text{,}\\,";
                renderInfoOptValue(pr_.children[n.childrenStart + i]);
            }
            result_ += "\\}";
        } else if (n.isExpr()) {
            // Power[base, -1] → \frac{1}{\text{base}}
            if (pr_.headName(n) == "Power" && n.childrenCount == 3) {
                const Node& base = pr_.node(pr_.children[n.childrenStart + 1]);
                const Node& exp  = pr_.node(pr_.children[n.childrenStart + 2]);
                if (exp.kind == NodeKind::Integer && exp.iVal == -1 &&
                    (base.isSymbol() || base.isString())) {
                    result_ += "\\frac{1}{";
                    infoTextAtom(pr_.str(base));
                    result_ += "}";
                    return;
                }
            }
            translate(idx);  // fallback for other Expr types
        }
    }

    // Render Options list: {Opt1 -> val1, Opt2 :> val2, …}
    void renderInfoOptions(uint32_t listIdx) {
        const Node& list = pr_.node(listIdx);
        result_ += "\\text{Options: }";
        bool first = true;
        for (uint32_t i = 0; i < list.childrenCount; ++i) {
            const Node& opt = pr_.node(pr_.children[list.childrenStart + i]);
            if ((opt.kind != NodeKind::Rule && opt.kind != NodeKind::RuleDelayed)
                || opt.childrenCount < 2) continue;
            if (!first) result_ += "\\text{, }";
            first = false;
            const Node& lhs = pr_.node(pr_.children[opt.childrenStart]);
            infoTextAtom(pr_.str(lhs));
            result_ += (opt.kind == NodeKind::RuleDelayed) ? "\\mapsto " : "\\to ";
            renderInfoOptValue(pr_.children[opt.childrenStart + 1]);
        }
    }

    void translateInformationData(const Node& infoNode) {
        std::string_view fullName, usageStr;
        uint32_t attrsIdx = UINT32_MAX;
        uint32_t optsIdx  = UINT32_MAX;
        for (uint32_t i = 1; i < infoNode.childrenCount; ++i) {
            const Node& ch = pr_.node(pr_.children[infoNode.childrenStart + i]);
            if (ch.kind != NodeKind::Rule && ch.kind != NodeKind::RuleDelayed) continue;
            if (ch.childrenCount < 2) continue;
            const Node& k = pr_.node(pr_.children[ch.childrenStart]);
            const Node& v = pr_.node(pr_.children[ch.childrenStart + 1]);
            if (!k.isString()) continue;
            std::string_view ks = pr_.str(k);
            if (ks == "FullName"   && v.isString()) fullName = pr_.str(v);
            if (ks == "Usage"      && v.isString()) usageStr = pr_.str(v);
            if (ks == "Attributes" && v.isList())   attrsIdx = pr_.children[ch.childrenStart + 1];
            if (ks == "Options"    && v.isList())   optsIdx  = pr_.children[ch.childrenStart + 1];
        }

        // Derive short name: strip WL context prefix ("System`Integrate" → "Integrate")
        std::string_view shortName = fullName;
        {
            auto p = shortName.rfind('`');
            if (p != std::string_view::npos) shortName = shortName.substr(p + 1);
        }

        // The Usage value may be wrapped in an extra layer of WL string quoting
        // (starts and ends with literal ") — strip if present.
        if (usageStr.size() >= 2 && usageStr.front() == '"' && usageStr.back() == '"')
            usageStr = usageStr.substr(1, usageStr.size() - 2);

        result_ += "\\begin{array}{l}";
        if (!shortName.empty()) {
            result_ += "\\textbf{";
            result_ += shortName;
            result_ += "}";
        }
        if (!usageStr.empty()) {
            result_ += "\\\\ ";
            renderInfoUsageString(usageStr);
        }
        if (attrsIdx != UINT32_MAX) {
            result_ += "\\\\ ";
            renderInfoAttributes(attrsIdx);
        }
        if (optsIdx != UINT32_MAX) {
            result_ += "\\\\ ";
            renderInfoOptions(optsIdx);
        }
        result_ += "\\end{array}";
    }

    // Locate the next \!\(\* … \) embedded StandardForm box in a parsed usage string.
    // After WL string parsing, the sequence appears as literal bytes:
    //   0x5c 0x21 0x5c 0x28 0x5c 0x2a  (\!\(\*)
    // Returns {patStart, contentStart, contentEnd}: s[contentStart..contentEnd) is
    // the raw box expression.  All three fields are npos when nothing is found.
    struct InfoBox { size_t patStart, cStart, cEnd; };
    static InfoBox findNextInfoBox(std::string_view s, size_t from) {
        const size_t npos = std::string_view::npos;
        for (size_t i = from; i + 5 < s.size(); ++i) {
            if (s[i]   == '\\' && s[i+1] == '!' &&
                s[i+2] == '\\' && s[i+3] == '(' &&
                s[i+4] == '\\' && s[i+5] == '*') {
                size_t cs = i + 6;
                size_t j  = cs;
                int depth = 0;
                while (j + 1 < s.size()) {
                    if (s[j] == '\\' && s[j+1] == '(') { depth++; j += 2; continue; }
                    if (s[j] == '\\' && s[j+1] == ')') {
                        if (depth == 0) return { i, cs, j };
                        depth--; j += 2; continue;
                    }
                    ++j;
                }
                return { i, cs, j }; // no \) found — return to end
            }
        }
        return { npos, npos, npos };
    }

    // Render a Usage string containing \!\(\*BoxExpr\) embedded StandardForm.
    // We are inside \begin{array}{l}, so no outer $ delimiters are needed.
    void renderInfoUsageString(std::string_view s) {
        size_t pos = 0;
        while (pos < s.size()) {
            InfoBox ib = findNextInfoBox(s, pos);
            if (ib.patStart == std::string_view::npos) {
                appendInfoText(s.substr(pos));
                break;
            }
            if (ib.patStart > pos) appendInfoText(s.substr(pos, ib.patStart - pos));

            // Pre-process box expression: replace \" with " so WLParser can
            // handle string literals.  (Needed when usage comes from the visual
            // display form; a no-op when coming from InformationData directly.)
            std::string_view rawExpr = s.substr(ib.cStart, ib.cEnd - ib.cStart);
            std::string boxExpr;
            boxExpr.reserve(rawExpr.size());
            for (size_t i = 0; i < rawExpr.size(); ++i) {
                if (rawExpr[i] == '\\' && i + 1 < rawExpr.size() && rawExpr[i+1] == '"') {
                    boxExpr += '"'; ++i;
                } else {
                    boxExpr += rawExpr[i];
                }
            }
            try {
                ParseResult innerPr;
                WLParser{}.parseInto(boxExpr, innerPr);
                result_ += BoxTranslator(innerPr, opts_).run().latex;
            } catch (...) {
                result_ += "\\cdots";
            }
            pos = (ib.cEnd + 2 <= s.size()) ? ib.cEnd + 2 : s.size(); // skip past \)
        }
    }

    // Emit a run of plain text characters (no ^ or _) inside math mode.
    // Wraps in \text{…}, escaping LaTeX special characters.
    void emitInfoPlainText(std::string_view seg) {
        if (seg.empty()) return;
        result_ += "\\text{";
        for (char c : seg) {
            switch (c) {
                case '"':  break;   // strip stray WL string quotes
                case '#':  result_ += "\\#"; break;
                case '%':  result_ += "\\%"; break;
                case '&':  result_ += "\\&"; break;
                case '{':  result_ += "\\{"; break;
                case '}':  result_ += "\\}"; break;
                case '$':  result_ += "\\$"; break;
                case '\\': result_ += "\\textbackslash{}"; break;
                default:   result_ += c;
            }
        }
        result_ += '}';
    }

    // Emit a plain-text segment (inside \begin{array}{l}, i.e. in math mode),
    // treating ^ and _ followed by {…} or a single char as math super/subscript
    // operators so that e.g. q^{B,up} and theta_k render correctly in KaTeX.
    // Newline (0x0a) → \\ row break.
    void appendInfoText(std::string_view text) {
        size_t i = 0;
        while (i < text.size()) {
            if (text[i] == '\n') {
                result_ += "\\\\ ";
                ++i;
                continue;
            }
            // Gather plain segment up to next \n, ^ or _
            size_t j = i;
            while (j < text.size() && text[j] != '\n' &&
                   text[j] != '^' && text[j] != '_') {
                ++j;
            }
            if (j > i) emitInfoPlainText(text.substr(i, j - i));
            if (j >= text.size() || text[j] == '\n') { i = j; continue; }

            // At ^ or _: emit as math super/subscript operator
            char op = text[j++];
            result_ += op;
            if (j < text.size() && text[j] == '{') {
                // grouped argument — find matching }
                size_t argStart = j + 1;
                int depth = 1;
                size_t k = argStart;
                while (k < text.size() && depth > 0) {
                    if      (text[k] == '{') { ++depth; ++k; }
                    else if (text[k] == '}') { --depth; ++k; }
                    else ++k;
                }
                result_ += '{';
                emitInfoPlainText(text.substr(argStart, (k - 1) - argStart));
                result_ += '}';
                i = k;
            } else if (j < text.size() && text[j] != ' ' && text[j] != '\n') {
                // single-character argument
                result_ += '{';
                emitInfoPlainText(text.substr(j, 1));
                result_ += '}';
                i = j + 1;
            } else {
                // ^ or _ not followed by an argument — emit literally in text
                result_ += "\\text{";
                result_ += op;
                result_ += '}';
                i = j;
            }
        }
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
    // ── Helper: translate one row of a GridBox into `result_` ──────────────────
    void translateGridRow(const Node& rowsNode, uint32_t r) {
        const Node& rowNode = pr_.node(pr_.children[rowsNode.childrenStart + r]);
        if (!rowNode.isList()) { translate(pr_.children[rowsNode.childrenStart + r]); return; }
        uint32_t cellCount = rowNode.childrenCount;
        for (uint32_t c = 0; c < cellCount; ++c) {
            if (c > 0) result_ += " & ";
            translate(pr_.children[rowNode.childrenStart + c]);
        }
    }

    // GridBox[{{r1c1,r1c2,…},{r2c1,…},…}, opts…]
    //   env: "pmatrix", "bmatrix", "vmatrix", "cases", "aligned", …
    // ================================================================
    void translateGridBox(uint32_t a, uint32_t n, std::string_view env,
                           bool bracketLocked = false) {
        if (n == 0) return;

        // Track nesting depth so we only page the outermost matrix.
        const int myDepth = gridBoxDepth_++;

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
        uint32_t rowCount = rowsNode.childrenCount;

        // ── Paged output (outermost matrix only) ─────────────────────────────
        if (myDepth == 0 && opts_.maxRows > 0 && rowCount > (uint32_t)opts_.maxRows) {
            const uint32_t pageSize = (uint32_t)opts_.maxRows;
            for (uint32_t pageStart = 0; pageStart < rowCount; pageStart += pageSize) {
                const uint32_t pageEnd = std::min(pageStart + pageSize, rowCount);

                // Save result_ (may have content from surrounding context),
                // build page into a fresh string, then push to pages_.
                std::string saved = std::move(result_);
                result_.clear();
                result_.reserve(pageSize * 80);

                result_ += "\\begin{";
                result_ += resolvedEnv;
                result_ += '}';
                for (uint32_t r = pageStart; r < pageEnd; ++r) {
                    if (r > pageStart) result_ += "\\\\";
                    translateGridRow(rowsNode, r);
                }
                result_ += "\\end{";
                result_ += resolvedEnv;
                result_ += '}';

                pages_.push_back(std::move(result_));
                result_ = std::move(saved);
            }
            // Emit first page into result_ for single-string callers.
            result_ += pages_[0];
            --gridBoxDepth_;
            return;
        }

        // ── Normal (non-paged) output ─────────────────────────────────────────
        result_ += "\\begin{";
        result_ += resolvedEnv;
        result_ += '}';

        for (uint32_t r = 0; r < rowCount; ++r) {
            if (r > 0) result_ += "\\\\";
            translateGridRow(rowsNode, r);
        }

        result_ += "\\end{";
        result_ += resolvedEnv;
        result_ += '}';

        --gridBoxDepth_;
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

    // ================================================================
    // Post-processing stage — runs on result_ after full tree translation.
    // Fixes string-level LaTeX issues that are hard to prevent at parse time.
    // ================================================================

    // Fix 1: Trim trailing guard spaces (cosmetic — the spaces are harmless in
    // math mode but make displayed LaTeX strings neater).
    // Fix 2: Double superscript / subscript safety net.
    //   Even though translateSuperscriptBox/etc wrap bases that already carry
    //   ^/_ (via baseNeedsBracing), unexpected parse paths could still produce
    //   sequences like `f^{A}^{B}`.  Detect and fix them here.
    void postProcess() {
        // ── Fix 1: trailing space ───────────────────────────────────────────
        while (!result_.empty() && result_.back() == ' ')
            result_.pop_back();

        // ── Fix 2: double scripts ───────────────────────────────────────────
        fixDoubleScripts();
    }

    // Repeatedly scan depth-0 ^ and _ positions; when two consecutive script
    // operators share no base between them (argEnd[k-1] == scriptPos[k]),
    // wrap [baseStart[k-1], scriptPos[k]) in braces so the second operator
    // attaches to the group instead of directly to the base.
    void fixDoubleScripts() {
        for (;;) {
            struct Script { size_t baseStart, scriptPos, argEnd; char kind; };
            std::vector<Script> ss;

            int depth = 0;
            for (size_t i = 0; i < result_.size(); ++i) {
                char c = result_[i];
                if      (c == '{') { ++depth; continue; }
                else if (c == '}') { if (depth > 0) --depth; continue; }
                if (depth != 0 || (c != '^' && c != '_')) continue;

                Script s;
                s.scriptPos = i;
                s.kind      = c;
                s.baseStart = scanAtomBackward(result_, i);
                s.argEnd    = skipAtomForward(result_, i + 1);
                ss.push_back(s);
            }

            bool fixed = false;
            for (size_t k = 1; k < ss.size() && !fixed; ++k) {
                // Only a double-script error when the SAME operator repeats
                // with no base in between (^A^B or _A_B).
                // Mixed _A^B or ^A_B is valid LaTeX (sub + super on same atom).
                if (ss[k].kind == ss[k-1].kind &&
                    ss[k].scriptPos == ss[k-1].argEnd) {
                    std::string out;
                    out.reserve(result_.size() + 2);
                    out.append(result_, 0, ss[k-1].baseStart);
                    out += '{';
                    out.append(result_, ss[k-1].baseStart,
                               ss[k].scriptPos - ss[k-1].baseStart);
                    out += '}';
                    out.append(result_, ss[k].scriptPos, std::string::npos);
                    result_ = std::move(out);
                    fixed = true;
                }
            }
            if (!fixed) break;
        }
    }
};

} // anonymous namespace

// ----------------------------------------------------------
// Public entry point
// ----------------------------------------------------------
BoxResult boxToLatex(std::string_view wlBoxString, const BtlOptions& opts) {
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
        return BoxTranslator(tl_pr, opts).run();
    } catch (const std::exception& ex) {
        std::string msg = ex.what();
        std::fprintf(stderr, "[wolfbook] boxToLatex parse error: %s\n", msg.c_str());
        return { std::string(wlBoxString), std::move(msg), {} };
    }
}

} // namespace wolfbook
