// =============================================================
// wl_parser.cpp  —  Wolfbook WL box-expression string parser
// =============================================================
#include "wl_parser.h"

#include <cassert>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <sstream>

namespace wolfbook {

// ----------------------------------------------------------
// ParseResult helpers
// ----------------------------------------------------------
std::string_view ParseResult::headName(const Node& n) const {
    assert(n.kind == NodeKind::Expr);
    // First child of an Expr is the head symbol node
    if (n.childrenCount == 0) return "";
    const auto& headNode = nodes[children[n.childrenStart]];
    return str(headNode);
}

// ----------------------------------------------------------
// WLParser — utilities
// ----------------------------------------------------------

uint32_t WLParser::internString(std::string s) {
    uint32_t idx = static_cast<uint32_t>(out_->strings.size());
    out_->strings.push_back(std::move(s));
    return idx;
}

uint32_t WLParser::appendChildren(const std::vector<uint32_t>& ch) {
    uint32_t start = static_cast<uint32_t>(out_->children.size());
    out_->children.insert(out_->children.end(), ch.begin(), ch.end());
    return start;
}

uint32_t WLParser::allocNode(Node n) {
    uint32_t idx = static_cast<uint32_t>(out_->nodes.size());
    out_->nodes.push_back(n);
    return idx;
}

void WLParser::skipWhitespace() {
    while (pos_ < src_.size() && (src_[pos_] == ' ' || src_[pos_] == '\t' ||
                                   src_[pos_] == '\n' || src_[pos_] == '\r'))
        ++pos_;
}

char WLParser::peek() const {
    return (pos_ < src_.size()) ? src_[pos_] : '\0';
}

char WLParser::peek2() const {
    return (pos_ + 1 < src_.size()) ? src_[pos_ + 1] : '\0';
}

char WLParser::advance() {
    return (pos_ < src_.size()) ? src_[pos_++] : '\0';
}

bool WLParser::atEnd() const { return pos_ >= src_.size(); }

bool WLParser::tryConsume(char c) {
    skipWhitespace();
    if (peek() == c) { advance(); return true; }
    return false;
}

void WLParser::expect(char c) {
    skipWhitespace();
    if (peek() != c) {
        std::ostringstream oss;
        oss << "WLParser: expected '" << c << "' at position " << pos_
            << ", got '" << (char)peek() << "' (context: ...'"
            << src_.substr(pos_ < 20 ? 0 : pos_ - 20, 40) << "')";
        throw std::runtime_error(oss.str());
    }
    advance();
}

// ----------------------------------------------------------
// Main parse entry
// ----------------------------------------------------------
ParseResult WLParser::parse(std::string_view input) {
    ParseResult result;
    result.reset();  // sets initial reserve
    parseInto(input, result);
    return result;
}

void WLParser::parseInto(std::string_view input, ParseResult& result) {
    src_ = input;
    pos_ = 0;
    out_ = &result;

    skipWhitespace();
    result.root = parseExpr();

    skipWhitespace();
    if (!atEnd()) {
        // Non-fatal: we have a valid root
    }

    out_ = nullptr;
}

// ----------------------------------------------------------
// parseExpr — dispatch on first character
// ----------------------------------------------------------
uint32_t WLParser::parseExpr() {
    skipWhitespace();
    uint32_t lhs;

    if (peek() == '"') {
        lhs = parseString();
    } else if (peek() == '{') {
        lhs = parseList();
    } else if (peek() == '-' && std::isdigit(static_cast<unsigned char>(peek2()))) {
        lhs = parseNumber();
    } else if (std::isdigit(static_cast<unsigned char>(peek()))) {
        lhs = parseNumber();
    } else if (std::isalpha(static_cast<unsigned char>(peek())) || peek() == '$') {
        lhs = parseSymbol();
        // Check for [args] → compound expression
        lhs = maybeParseCall(lhs);
    } else if (peek() == '#') {
        // Slot / SlotSequence: #  ##  #n  ##n  — parse as an opaque symbol atom
        lhs = parseSlot();
    } else if (peek() == '_') {
        // Blank / BlankSequence / BlankNullSequence — parse as opaque atom
        lhs = parseBlank();
    } else {
        std::ostringstream oss;
        oss << "WLParser: unexpected character '" << peek()
            << "' at position " << pos_;
        throw std::runtime_error(oss.str());
    }

    // Infix ^ — build Power[base, exp]  (e.g. GoldenRatio^(-1) in InformationData options)
    skipWhitespace();
    if (peek() == '^') {
        advance(); // consume '^'
        skipWhitespace();
        bool hasParen = (peek() == '(');
        if (hasParen) { advance(); skipWhitespace(); }
        const size_t savedNodes    = out_->nodes.size();
        const size_t savedChildren = out_->children.size();
        const size_t savedStrings  = out_->strings.size();
        const size_t savedPos      = pos_;
        try {
            uint32_t expIdx = parseExpr();
            if (hasParen) { skipWhitespace(); if (peek() == ')') advance(); }
            Node headN;
            headN.kind     = NodeKind::Symbol;
            headN.strIndex = internString("Power");
            uint32_t headIdx = allocNode(headN);
            std::vector<uint32_t> ch = {headIdx, lhs, expIdx};
            Node powerN;
            powerN.kind          = NodeKind::Expr;
            powerN.childrenStart = appendChildren(ch);
            powerN.childrenCount = static_cast<uint32_t>(ch.size());
            lhs = allocNode(powerN);
        } catch (...) {
            out_->nodes.resize(savedNodes);
            out_->children.resize(savedChildren);
            out_->strings.resize(savedStrings);
            pos_ = savedPos;
        }
    }

    // Check for rule arrow -> or :>
    lhs = maybeParseRule(lhs);
    return lhs;
}

// ----------------------------------------------------------
// parseAtom — string | symbol | number (no call suffix)
// ----------------------------------------------------------
uint32_t WLParser::parseAtom() {
    skipWhitespace();
    if (peek() == '"')
        return parseString();
    if (peek() == '{')
        return parseList();
    if (std::isdigit(static_cast<unsigned char>(peek())) ||
        (peek() == '-' && std::isdigit(static_cast<unsigned char>(peek2()))))
        return parseNumber();
    return parseSymbol();
}

// ----------------------------------------------------------
// parseString — handles Wolfram named characters \[Name]
// ----------------------------------------------------------
uint32_t WLParser::parseString() {
    expect('"');
    std::string val;
    val.reserve(32);

    while (pos_ < src_.size() && src_[pos_] != '"') {
        if (src_[pos_] == '\\') {
            ++pos_;
            if (pos_ >= src_.size()) break;

            if (src_[pos_] == '\\') {
                val += '\\'; ++pos_;
            } else if (src_[pos_] == '"') {
                val += '"'; ++pos_;
            } else if (src_[pos_] == 'n') {
                val += '\n'; ++pos_;
            } else if (src_[pos_] == 't') {
                val += '\t'; ++pos_;
            } else if (src_[pos_] == '[') {
                // Named character: \[Name] — store the whole token literally
                // so the translator can look it up in the special-char table.
                val += '\\';
                val += '[';
                ++pos_;
                while (pos_ < src_.size() && src_[pos_] != ']' && src_[pos_] != '"') {
                    val += src_[pos_++];
                }
                if (pos_ < src_.size() && src_[pos_] == ']') {
                    val += ']';
                    ++pos_;
                }
            } else {
                val += '\\';
                val += src_[pos_++];
            }
        } else {
            val += src_[pos_++];
        }
    }

    if (pos_ < src_.size()) ++pos_; // consume closing '"'

    Node n;
    n.kind      = NodeKind::String;
    n.strIndex  = internString(std::move(val));
    return allocNode(n);
}

// ----------------------------------------------------------
// parseSymbol — WL identifier, possibly with backtick contexts
// ----------------------------------------------------------
uint32_t WLParser::parseSymbol() {
    size_t start = pos_;
    while (pos_ < src_.size()) {
        char c = src_[pos_];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '$' || c == '`') {
            ++pos_;
        } else {
            break;
        }
    }
    std::string name(src_.substr(start, pos_ - start));
    Node n;
    n.kind     = NodeKind::Symbol;
    n.strIndex = internString(std::move(name));
    return allocNode(n);
}

// ----------------------------------------------------------
// parseNumber — integer or real, optional leading minus
// ----------------------------------------------------------
uint32_t WLParser::parseNumber() {
    size_t start = pos_;
    if (peek() == '-') ++pos_;

    while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_])))
        ++pos_;

    bool isReal = false;
    if (pos_ < src_.size() && src_[pos_] == '.') {
        isReal = true;
        ++pos_;
        while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_])))
            ++pos_;
    }
    // Optional backtick precision marker (e.g. 1.0`15) — consume and ignore
    if (pos_ < src_.size() && src_[pos_] == '`') {
        ++pos_;
        while (pos_ < src_.size() && (std::isdigit(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '.'))
            ++pos_;
    }

    std::string raw(src_.substr(start, pos_ - start));
    Node n;
    if (isReal) {
        n.kind = NodeKind::Real;
        n.dVal = std::stod(raw);
    } else {
        n.kind = NodeKind::Integer;
        n.iVal = static_cast<int64_t>(std::stoll(raw));
    }
    return allocNode(n);
}

// ----------------------------------------------------------
// maybeParseCall — if next non-ws is '[', build Expr node
// ----------------------------------------------------------
uint32_t WLParser::maybeParseCall(uint32_t headIdx) {
    skipWhitespace();
    if (peek() != '[') return headIdx;
    advance(); // consume '['

    std::vector<uint32_t> args = parseArgs(']');
    expect(']');

    // Build Expr node: children = [headIdx, arg0, arg1, …]
    std::vector<uint32_t> all;
    all.reserve(1 + args.size());
    all.push_back(headIdx);
    all.insert(all.end(), args.begin(), args.end());

    Node n;
    n.kind           = NodeKind::Expr;
    n.childrenStart  = appendChildren(all);
    n.childrenCount  = static_cast<uint32_t>(all.size());
    return allocNode(n);
}

// ----------------------------------------------------------
// skipArg — skip one argument (error recovery)
// Advances past balanced bracket content until the next ','  or
// closeChar at depth 0.  Used when parseExpr() throws on bare WL
// infix operators (^, +, -, etc.) that appear as interpretation args.
// ----------------------------------------------------------
void WLParser::skipArg(char closeChar) {
    int depth = 0;
    while (!atEnd()) {
        char c = peek();
        if (c == '[' || c == '{' || c == '(') { ++depth; advance(); }
        else if (c == ']' || c == '}' || c == ')') {
            if (depth == 0) return;  // stop before closing bracket
            --depth; advance();
        }
        else if (c == ',' && depth == 0) return;  // stop before comma
        else advance();
    }
}

// ----------------------------------------------------------
// parseArgs — comma-separated expressions until closeChar
// ----------------------------------------------------------
std::vector<uint32_t> WLParser::parseArgs(char closeChar) {
    std::vector<uint32_t> result;
    result.reserve(8);
    skipWhitespace();
    if (peek() == closeChar) return result;

    // Parse one argument with error recovery:
    // 1. If parseExpr() throws (e.g. on an unrecognised operator), roll back
    //    the arena and skip to the next ',' or closeChar.
    // 2. If parseExpr() succeeds but leaves unrecognised infix content before
    //    the next ',' or closeChar (e.g. "^2" after "x" in "x^2"), also skip
    //    that remainder.  This lets InterpretationBox[display, x^2] parse the
    //    display correctly while silently discarding the interpretation arg.
    auto tryParseArg = [&]() {
        const size_t savedNodes    = out_->nodes.size();
        const size_t savedChildren = out_->children.size();
        const size_t savedStrings  = out_->strings.size();
        const size_t savedPos      = pos_;
        try {
            result.push_back(parseExpr());
        } catch (...) {
            out_->nodes.resize(savedNodes);
            out_->children.resize(savedChildren);
            out_->strings.resize(savedStrings);
            pos_ = savedPos;
            skipArg(closeChar);
            return;
        }
        // parseExpr succeeded but may not have consumed all of this argument
        // (e.g. parsed "x" from "x^2"). Skip any leftover content.
        skipWhitespace();
        if (peek() != ',' && peek() != closeChar && !atEnd())
            skipArg(closeChar);
    };

    tryParseArg();
    skipWhitespace();

    while (peek() == ',') {
        advance();
        skipWhitespace();
        if (peek() == closeChar) break; // trailing comma tolerance
        tryParseArg();
        skipWhitespace();
    }
    return result;
}

// ----------------------------------------------------------
// parseList — { expr, … }
// ----------------------------------------------------------
uint32_t WLParser::parseList() {
    expect('{');
    std::vector<uint32_t> elems = parseArgs('}');
    expect('}');

    Node n;
    n.kind          = NodeKind::List;
    n.childrenStart = appendChildren(elems);
    n.childrenCount = static_cast<uint32_t>(elems.size());
    return allocNode(n);
}

// ----------------------------------------------------------
// maybeParseRule — handle -> and :> after any expression
// ----------------------------------------------------------
uint32_t WLParser::maybeParseRule(uint32_t lhsIdx) {
    skipWhitespace();

    NodeKind ruleKind = NodeKind::Rule;
    bool consumed = false;

    if (peek() == '-' && peek2() == '>') {
        pos_ += 2; consumed = true;
    } else if (peek() == ':' && peek2() == '>') {
        ruleKind = NodeKind::RuleDelayed;
        pos_ += 2; consumed = true;
    }

    // Postfix & (Pure Function) — consume and return lhs unchanged
    if (peek() == '&') { advance(); return lhsIdx; }

    if (!consumed) return lhsIdx;

    skipWhitespace();
    uint32_t rhsIdx = parseExpr();

    std::vector<uint32_t> ch = { lhsIdx, rhsIdx };

    Node n;
    n.kind          = ruleKind;
    n.childrenStart = appendChildren(ch);
    n.childrenCount = 2;
    return allocNode(n);
}

// ----------------------------------------------------------
// parseSlot — #  #n  ##  ##n  (Slot / SlotSequence)
// Stored as a Symbol node; the exact text is preserved.
// ----------------------------------------------------------
uint32_t WLParser::parseSlot() {
    std::string tok;
    tok += advance(); // consume '#'
    if (peek() == '#') tok += advance(); // '##'
    while (std::isdigit(static_cast<unsigned char>(peek())))
        tok += advance();
    // Also allow identifier after # (named slot: #name)
    while (std::isalpha(static_cast<unsigned char>(peek())) ||
           peek() == '$')
        tok += advance();
    Node n;
    n.kind     = NodeKind::Symbol;
    n.strIndex = internString(std::move(tok));
    return allocNode(n);
}

// ----------------------------------------------------------
// parseBlank — _  __  ___  _Name  (pattern objects)
// Stored as a Symbol node.
// ----------------------------------------------------------
uint32_t WLParser::parseBlank() {
    std::string tok;
    while (peek() == '_') tok += advance();
    // Optional head name: _Integer, _List, …
    while (std::isalpha(static_cast<unsigned char>(peek())) ||
           peek() == '$' ||
           std::isdigit(static_cast<unsigned char>(peek())))
        tok += advance();
    Node n;
    n.kind     = NodeKind::Symbol;
    n.strIndex = internString(std::move(tok));
    return allocNode(n);
}

} // namespace wolfbook
