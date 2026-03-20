// =============================================================
// wl_parser.h  —  Wolfbook fast box-string parser
//
// Parses the subset of Wolfram Language InputForm that covers
// box expressions emitted by ToString[boxes, InputForm]:
//
//   String literals  "text"  (including \[NamedChar] escapes)
//   Symbol names     FractionBox, FontColor, True, …
//   Number literals  1  3.14  -2
//   Compound expr    Head[arg1, arg2, …]
//   List             {el1, el2, …}
//   Rule             lhs -> rhs  (also :>)
//
// The parser produces a lightweight tagged-union AST whose
// nodes are owned by an arena allocator so the entire tree
// can be freed in O(1).
//
// Thread-safety: each WLParser instance is independent.
// =============================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <stdexcept>

namespace wolfbook {

// ----------------------------------------------------------
// AST node kinds
// ----------------------------------------------------------
enum class NodeKind : uint8_t {
    String,   // "text"
    Symbol,   // identifier (box head or option name or named constant)
    Integer,  // in box trees these are rare but do appear in e.g. GridBox opts
    Real,
    Expr,     // Head[arg, …]  — head is always a Symbol node index
    List,     // {el, …}
    Rule,     // lhs -> rhs
    RuleDelayed // lhs :> rhs
};

// ----------------------------------------------------------
// Node — all fields fit in 32 bytes (cache-line friendly)
// ----------------------------------------------------------
struct Node {
    NodeKind kind;

    // For String / Symbol: index into the string pool
    uint32_t strIndex { 0 };

    // For Expr / List / Rule / RuleDelayed: index into the
    // children array (children are stored contiguously)
    uint32_t childrenStart { 0 };
    uint32_t childrenCount { 0 };

    // For Integer / Real: stored inline
    union {
        int64_t  iVal;
        double   dVal;
    };

    // Convenience accessors (used by translator)
    bool isString() const { return kind == NodeKind::String; }
    bool isSymbol() const { return kind == NodeKind::Symbol; }
    bool isExpr()   const { return kind == NodeKind::Expr;   }
    bool isList()   const { return kind == NodeKind::List;   }
    bool isRule()   const { return kind == NodeKind::Rule || kind == NodeKind::RuleDelayed; }
};

// ----------------------------------------------------------
// ParseResult — owns all arena memory
// ----------------------------------------------------------
struct ParseResult {
    std::vector<Node>        nodes;        // flat node pool
    std::vector<uint32_t>    children;     // flat children index array
    std::vector<std::string> strings;      // string / symbol literals
    uint32_t                 root { 0 };   // index of the root node

    // Clear all content but retain allocated capacity so the same
    // ParseResult can be reused (e.g. via thread_local) without
    // paying for repeated vector heap allocations.
    void reset() {
        nodes.clear();
        children.clear();
        strings.clear();
        root = 0;
        // Ensure a sensible minimum on first use
        if (nodes.capacity()    < 256) nodes.reserve(256);
        if (children.capacity() < 512) children.reserve(512);
        if (strings.capacity()  < 128) strings.reserve(128);
    }

    // Convenience: dereference a node by index
    const Node& node(uint32_t idx) const { return nodes[idx]; }
    Node& node(uint32_t idx)             { return nodes[idx]; }

    // Get the string value of a String or Symbol node
    std::string_view str(const Node& n) const {
        return strings[n.strIndex];
    }
    std::string_view str(uint32_t nodeIdx) const {
        return str(nodes[nodeIdx]);
    }

    // Get children indices for an Expr or List node
    std::vector<uint32_t> getChildren(const Node& n) const {
        return std::vector<uint32_t>(
            children.begin() + n.childrenStart,
            children.begin() + n.childrenStart + n.childrenCount);
    }

    // For an Expr node, head symbol name
    std::string_view headName(const Node& n) const;
};

// ----------------------------------------------------------
// WLParser
// ----------------------------------------------------------
class WLParser {
public:
    // Parse the entire input string.  Throws std::runtime_error
    // on syntax error.
    ParseResult parse(std::string_view input);

    // Like parse() but fills an existing ParseResult (which should have
    // been reset() by the caller).  Avoids allocation when reusing a
    // thread_local ParseResult across calls.
    void parseInto(std::string_view input, ParseResult& out);

private:
    // Input buffer (pointed at caller-owned memory)
    std::string_view src_;
    size_t           pos_ { 0 };

    // Output being built
    ParseResult*     out_ { nullptr };

    // ---- helpers ----
    void  skipWhitespace();
    char  peek() const;
    char  peek2() const;
    char  advance();
    bool  atEnd() const;
    bool  tryConsume(char c);
    void  expect(char c);

    // ---- recursive-descent production rules ----
    uint32_t parseExpr();    // top-level dispatch
    uint32_t parseAtom();    // string | symbol | number
    uint32_t parseString();  // "..."
    uint32_t parseSymbol();  // [A-Za-z$][A-Za-z0-9$`]*
    uint32_t parseNumber();  // [0-9]+(\.[0-9]*)?  or  - prefixed
    uint32_t parseSlot();    // #  #n  ##  ##n  (Slot / SlotSequence)
    uint32_t parseBlank();   // _  __  ___  _Name  (Blank patterns)

    // After parseSymbol returns a symbol node we check for '[':
    // if found, parse it as a compound expression
    uint32_t maybeParseCall(uint32_t headIdx);

    // Parse argument list: arg, arg, …  until ']'
    std::vector<uint32_t> parseArgs(char closeChar);

    // Parse list: { expr, … }
    uint32_t parseList();

    // Parse rule tail after lhs has been parsed:  -> rhs  or  :> rhs
    uint32_t maybeParseRule(uint32_t lhsIdx);

    // Intern a string into out_->strings, return index
    uint32_t internString(std::string s);
    // Append children block, return start index
    uint32_t appendChildren(const std::vector<uint32_t>& ch);

    // Allocate a new node, return index
    uint32_t allocNode(Node n);

    // Skip one argument (used for error recovery): advances past any
    // balanced bracket content until the next ',' or closeChar at depth 0.
    void skipArg(char closeChar);
};

// ----------------------------------------------------------
// Convenience free function
// ----------------------------------------------------------
inline ParseResult parseWL(std::string_view input) {
    return WLParser{}.parse(input);
}

} // namespace wolfbook
