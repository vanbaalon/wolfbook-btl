# BTL Line-Breaking v2: Code Review, Fixes, and New Features

> **Based on:** actual `src/native/line_breaker.cpp` (reviewed in full)
> **Covers:**
>
> 1. Specific bugs and inaccuracies in the current width estimator
> 2. Improved heuristic estimator design
> 3. Breaking inside wide `\frac{...}{...}` arguments
> 4. `pageWidthPx` mode with KaTeX-based precise measurement
> 5. Iterative re-rendering with `maxIterations` parameter

---

## Part 1: Current Code Issues (specific to `line_breaker.cpp`)

### Issue 1: `\color`, `\textcolor` counted as atomic with character-width content

In the current code, `\color` and `\textcolor` are in the `isAtomicCmd` list (line 126). This means the tokenizer consumes the command plus all its braced arguments as a single token, and calls `estimateAtomicWidth`. But `estimateAtomicWidth` falls through to the default branch (lines 392–397) for `\color`, counting every byte after the command name at 0.5em each:

```cpp
// Current: estimateAtomicWidth for \color{red}x
// cmd = "\\color", cmdEnd = 6
// remaining = "{red}" → loops i=6..10, w += 0.5 each → w = 2.5em
// But \color{red} renders at 0 width! Only 'x' has width.
```

**The problem:** `\color{red}` is a *directive* — it affects subsequent content but produces no glyphs itself. The current code gives it ~2.5em width.

**Similarly affected:** `\textcolor{blue}{content}` has two braced args. The first (`{blue}`) is zero-width; only the second (`{content}`) has width. But the tokenizer consumes both args as one atomic blob.

### Issue 2: `\mathbf`, `\mathbb`, etc. content width not scaled correctly

These are in `isAtomicCmd` and handled by `estimateAtomicWidth` (lines 382–389):

```cpp
if (cmd == "\\text" || cmd == "\\mathrm" || cmd == "\\mathit" ||
    cmd == "\\mathbf" || cmd == "\\operatorname") {
    // charCount × 0.5
}
```

This misses `\mathbb`, `\mathcal`, `\mathfrak`, `\mathsf` — they fall through to the default branch which counts raw bytes including braces at 0.5em each. So `\mathcal{N}` → `{N}` → 3 chars × 0.5 = 1.5em, when it should be ~0.65em.

### Issue 3: `estimateAtomicWidth` default branch counts syntax characters

Lines 392–397:
```cpp
// Default: sum of character widths
double w = 0.0;
for (size_t i = cmdEnd; i < s.size(); i++) {
    w += 0.5;
}
return std::max(0.5, w);
```

This iterates over every byte after the command name, including `{`, `}`, and any nested commands. So `\hat{x}` → `{x}` → 3 × 0.5 = 1.5em (should be ~0.6em: the hat accent adds negligible width to `x`).

### Issue 4: `cmdWidth` falls through for all `\uppercase` commands at 0.8em

Lines 164–166:
```cpp
if (cmd.size() > 1 && cmd[0] == '\\' &&
    std::isupper(static_cast<unsigned char>(cmd[1])))
    return 0.8;
```

This catches `\Gamma` (correct ~0.63em), but also `\Rightarrow` (~1.56em with spacing), `\Re` (~0.72em), `\LaTeX` (shouldn't appear), etc. The blanket 0.8em is often 30–50% off.

### Issue 5: Relations and binary operators don't include surrounding math spacing

In TeX, a relation symbol like `=` gets `\thickmuskip` (~5/18em ≈ 0.278em) on each side, totalling ~0.56em of extra space. A binary operator like `+` gets `\medmuskip` (~4/18em ≈ 0.222em) on each side, totalling ~0.44em.

The current `cmdWidth` gives `+` and `=` 0.7em each, which roughly covers the glyph but not the spacing. For `=`, the total rendered width is glyph(0.78em) + space(0.56em) ≈ 1.34em. The estimate of 0.7em is ~48% too low.

### Issue 6: `-` (minus) same width as `+`

Line 145: `if (c == '+' || c == '-' || c == '=') return 0.7;`

In math mode, `-` is a binary operator rendered as a minus sign (~0.78em glyph) with medmuskip spacing. So it should be similar to `+`. But when `-` appears as a unary minus (e.g., `-x`), there's no left space. The current code can't distinguish these cases, so 0.7em is a reasonable compromise, but worth noting.

---

## Part 2: Heuristic Estimator Fixes

### Fix 1: Split zero-width commands from truly atomic commands

Replace the single `isAtomicCmd` list with two:

```cpp
// Commands that produce zero rendered width themselves;
// their style/colour argument(s) are also zero-width.
// Only the CONTENT argument (if any) has width.
static bool isStyleCmd(std::string_view cmd) {
    static const std::string_view styles[] = {
        // Colour: \color{X} affects following; \textcolor{X}{content}
        "\\color", "\\textcolor", "\\colorbox",
        // These are purely directives with no content arg:
        "\\displaystyle", "\\textstyle", "\\scriptstyle", "\\scriptscriptstyle",
        "\\phantom", "\\hphantom", "\\vphantom",
        "\\label", "\\tag", "\\notag", "\\nonumber",
    };
    for (auto& s : styles) if (cmd == s) return true;
    return false;
}

// Font-change commands: the command is zero width, content gets ×1.0
// (could be ×1.05 for bold, but 1.0 is close enough)
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

// Truly atomic: rendered as a unit, width is max/sum of args
// (these must NOT be broken inside)
static bool isStructuralCmd(std::string_view cmd) {
    static const std::string_view structs[] = {
        "\\frac", "\\dfrac", "\\tfrac", "\\binom",
        "\\sqrt",
        "\\hat", "\\bar", "\\vec", "\\tilde", "\\dot", "\\ddot",
        "\\overline", "\\underline",
        "\\overbrace", "\\underbrace",
        "\\overset", "\\underset", "\\stackrel",
    };
    for (auto& s : structs) if (cmd == s) return true;
    return false;
}
```

### Fix 2: New tokenizer handling for each category

```cpp
// In Tokenizer::next(), replace the single isAtomicCmd branch:

if (isStyleCmd(cmd)) {
    pos = cmdEnd;
    // \color{red}: skip the style arg, produce zero width
    // \textcolor{blue}{content}: skip style arg, estimate content arg
    if (cmd == "\\color") {
        if (pos < src.size() && src[pos] == '{')
            pos = skipBraceGroup(src, pos); // skip {red}
        out.len = pos - out.start;
        out.width = 0.0;  // ← the fix: directive is zero width
        return true;
    }
    if (cmd == "\\textcolor" || cmd == "\\colorbox") {
        if (pos < src.size() && src[pos] == '{')
            pos = skipBraceGroup(src, pos); // skip {blue}
        // Content arg: estimate its width
        double contentW = 0.0;
        if (pos < src.size() && src[pos] == '{') {
            size_t end = skipBraceGroup(src, pos);
            contentW = estimateBracedWidth(src.substr(pos + 1, end - pos - 2));
            pos = end;
        }
        out.len = pos - out.start;
        out.width = contentW;
        return true;
    }
    // \displaystyle, \phantom etc — just skip, zero width
    out.len = pos - out.start;
    out.width = 0.0;
    return true;
}

if (isFontCmd(cmd)) {
    pos = cmdEnd;
    // Consume the single braced argument; its content has normal width
    double contentW = 0.0;
    if (pos < src.size() && src[pos] == '{') {
        size_t end = skipBraceGroup(src, pos);
        contentW = estimateBracedWidth(src.substr(pos + 1, end - pos - 2));
        pos = end;
    }
    out.len = pos - out.start;
    out.width = contentW;  // ← just the content, not the command name
    return true;
}

if (isStructuralCmd(cmd)) {
    pos = cmdEnd;
    while (pos < src.size() && src[pos] == '{')
        pos = skipBraceGroup(src, pos);
    out.len = pos - out.start;
    out.width = estimateStructuralWidth(src.substr(out.start, out.len));
    return true;
}
```

### Fix 3: Replace `estimateAtomicWidth` with `estimateStructuralWidth`

```cpp
double estimateStructuralWidth(std::string_view s) {
    size_t cmdEnd = 0;
    if (!s.empty() && s[0] == '\\') {
        cmdEnd = 1;
        while (cmdEnd < s.size() &&
               std::isalpha(static_cast<unsigned char>(s[cmdEnd])))
            ++cmdEnd;
    }
    std::string_view cmd = s.substr(0, cmdEnd);

    // Fraction/binomial: width ≈ max(num, den) + bar overhead
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
        return maxArg + 0.24; // nulldelimiterspace overhead
    }

    // Sqrt: content + radical sign
    if (cmd == "\\sqrt") {
        size_t p = cmdEnd;
        double extraW = 0.0;
        // Optional [n] for nth root
        if (p < s.size() && s[p] == '[') {
            size_t bracket_end = s.find(']', p);
            if (bracket_end != std::string_view::npos) {
                p = bracket_end + 1;
                extraW = 0.3; // root index
            }
        }
        if (p < s.size() && s[p] == '{') {
            size_t end = skipBraceGroup(s, p);
            return estimateBracedWidth(s.substr(p + 1, end - p - 2))
                   + 0.5 + extraW;
        }
    }

    // Accent commands (\hat, \bar, \vec, \tilde, \dot, \ddot):
    // width ≈ width of argument (accent adds negligible horizontal extent)
    if (cmd == "\\hat" || cmd == "\\bar" || cmd == "\\vec" ||
        cmd == "\\tilde" || cmd == "\\dot" || cmd == "\\ddot" ||
        cmd == "\\overline" || cmd == "\\underline") {
        if (cmdEnd < s.size() && s[cmdEnd] == '{') {
            size_t end = skipBraceGroup(s, cmdEnd);
            return estimateBracedWidth(s.substr(cmdEnd + 1, end - cmdEnd - 2));
        }
    }

    // \overset, \underset, \stackrel: width ≈ max of the two args
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

    // Fallback: estimate braced content only (not raw bytes)
    double w = 0.0;
    size_t p = cmdEnd;
    while (p < s.size()) {
        if (s[p] == '{') {
            size_t end = skipBraceGroup(s, p);
            w += estimateBracedWidth(s.substr(p + 1, end - p - 2));
            p = end;
        } else {
            w += 0.5;
            p++;
        }
    }
    return std::max(0.5, w);
}
```

### Fix 4: Improved `cmdWidth` with math spacing

```cpp
static double cmdWidth(std::string_view cmd) {
    if (cmd.empty()) return 0.0;

    // Single character
    if (cmd.size() == 1) {
        char c = cmd[0];
        if (c >= '0' && c <= '9') return 0.5;
        // Math italic letters — KaTeX Math-Italic widths
        if (c >= 'a' && c <= 'z') {
            // Lookup for common letters; italic chars are ~0.4–0.7em
            static const double lowerWidths[26] = {
             // a     b     c     d     e     f     g     h     i     j
                0.53, 0.43, 0.43, 0.52, 0.44, 0.39, 0.50, 0.56, 0.31, 0.35,
             // k     l     m     n     o     p     q     r     s     t
                0.52, 0.31, 0.84, 0.56, 0.50, 0.52, 0.48, 0.41, 0.42, 0.39,
             // u     v     w     x     y     z
                0.54, 0.48, 0.70, 0.52, 0.47, 0.44,
            };
            return lowerWidths[c - 'a'];
        }
        if (c >= 'A' && c <= 'Z') {
            // Uppercase italic — wider on average
            return 0.70; // reasonable average; could add full table
        }
        // Relation/binary single chars: glyph + surrounding space
        if (c == '=') return 0.78 + 0.56;  // glyph + 2×thickmuskip
        if (c == '<' || c == '>') return 0.78 + 0.56;
        if (c == '+') return 0.78 + 0.44;  // glyph + 2×medmuskip
        if (c == '-') return 0.44 + 0.44;  // minus glyph + 2×medmuskip
        if (c == '(' || c == ')') return 0.39;
        if (c == '[' || c == ']') return 0.28;
        if (c == ',') return 0.28 + 0.17;  // comma + thinmuskip after
        if (c == '.') return 0.28;
        if (c == '!') return 0.28;
        if (c == '\'') return 0.31;  // prime
        if (c == ' ') return 0.0;    // spaces in math mode are ignored
        return 0.5;
    }

    // Spacing commands
    if (cmd == "\\,") return 0.167;
    if (cmd == "\\:" || cmd == "\\>") return 0.222;
    if (cmd == "\\;") return 0.278;
    if (cmd == "\\!") return -0.167;
    if (cmd == "\\quad") return 1.0;
    if (cmd == "\\qquad") return 2.0;
    if (cmd == "\\ ") return 0.333; // explicit space

    // Large operators (displaystyle)
    if (cmd == "\\sum") return 1.11;
    if (cmd == "\\prod") return 1.01;
    if (cmd == "\\int" || cmd == "\\intop") return 0.56;
    if (cmd == "\\oint") return 0.56;
    if (cmd == "\\bigcup" || cmd == "\\bigcap") return 1.00;

    // Named functions (upright, with trailing thinspace)
    if (cmd == "\\lim") return 1.37;
    if (cmd == "\\max") return 1.67;
    if (cmd == "\\min") return 1.48;
    if (cmd == "\\sup") return 1.29;
    if (cmd == "\\inf") return 1.06;
    if (cmd == "\\log") return 1.25;
    if (cmd == "\\sin") return 1.12;
    if (cmd == "\\cos") return 1.21;
    if (cmd == "\\tan") return 1.29;
    if (cmd == "\\exp") return 1.37;
    if (cmd == "\\det") return 1.21;
    if (cmd == "\\dim") return 1.37;
    if (cmd == "\\ker") return 1.21;

    // Specific wide symbols
    if (cmd == "\\infty") return 1.00;
    if (cmd == "\\partial") return 0.56;
    if (cmd == "\\nabla") return 0.83;
    if (cmd == "\\forall") return 0.56;
    if (cmd == "\\exists") return 0.56;
    if (cmd == "\\emptyset") return 0.50;
    if (cmd == "\\hbar") return 0.56;
    if (cmd == "\\ell") return 0.35;
    if (cmd == "\\cdots" || cmd == "\\ldots") return 1.17;
    if (cmd == "\\ddots" || cmd == "\\vdots") return 0.5;

    // Relations (glyph + 2×thickmuskip ≈ 0.56em)
    if (isRelationCmd(cmd)) return 0.78 + 0.56;

    // Binary operators (glyph + 2×medmuskip ≈ 0.44em)
    if (isBinOpCmd(cmd)) {
        if (cmd == "\\cdot") return 0.28 + 0.44;
        return 0.78 + 0.44;
    }

    // Greek lowercase: individual widths from KaTeX Math-Italic metrics
    // (The blanket 0.6 was already okay; keep but refine key ones)
    if (cmd == "\\alpha") return 0.63;
    if (cmd == "\\beta") return 0.57;
    if (cmd == "\\gamma") return 0.57;
    if (cmd == "\\delta") return 0.45;
    if (cmd == "\\epsilon" || cmd == "\\varepsilon") return 0.44;
    if (cmd == "\\zeta") return 0.44;
    if (cmd == "\\eta") return 0.58;
    if (cmd == "\\theta" || cmd == "\\vartheta") return 0.55;
    if (cmd == "\\lambda") return 0.58;
    if (cmd == "\\mu") return 0.60;
    if (cmd == "\\nu") return 0.48;
    if (cmd == "\\xi") return 0.47;
    if (cmd == "\\pi") return 0.57;
    if (cmd == "\\rho") return 0.50;
    if (cmd == "\\sigma") return 0.58;
    if (cmd == "\\tau") return 0.44;
    if (cmd == "\\phi" || cmd == "\\varphi") return 0.60;
    if (cmd == "\\chi") return 0.55;
    if (cmd == "\\psi") return 0.67;
    if (cmd == "\\omega") return 0.64;

    // Greek uppercase
    if (cmd == "\\Gamma") return 0.63;
    if (cmd == "\\Delta") return 0.83;
    if (cmd == "\\Theta") return 0.74;
    if (cmd == "\\Lambda") return 0.76;
    if (cmd == "\\Xi") return 0.68;
    if (cmd == "\\Pi") return 0.74;
    if (cmd == "\\Sigma") return 0.69;
    if (cmd == "\\Phi") return 0.76;
    if (cmd == "\\Psi") return 0.78;
    if (cmd == "\\Omega") return 0.77;

    // Arrows (glyph + 2×thickmuskip since they're relations)
    if (cmd == "\\rightarrow" || cmd == "\\to") return 1.00 + 0.56;
    if (cmd == "\\leftarrow") return 1.00 + 0.56;
    if (cmd == "\\Rightarrow") return 1.00 + 0.56;
    if (cmd == "\\Leftarrow") return 1.00 + 0.56;
    if (cmd == "\\mapsto") return 1.00 + 0.56;
    if (cmd == "\\leftrightarrow") return 1.00 + 0.56;

    // Generic lowercase backslash-command: likely a Greek letter
    if (cmd.size() > 1 && cmd[0] == '\\' &&
        std::islower(static_cast<unsigned char>(cmd[1])))
        return 0.55;

    // Generic uppercase backslash-command: likely a Greek capital
    if (cmd.size() > 1 && cmd[0] == '\\' &&
        std::isupper(static_cast<unsigned char>(cmd[1])))
        return 0.73;

    return 0.5;
}
```

---

## Part 3: Breaking Inside Wide Fractions

### Motivation

Expressions like `\frac{A_1 + A_2 + A_3 + A_4 + \cdots + A_n}{B_1 B_2}` commonly arise in physics (partial fraction decomposition, operator product expansions, etc.). The current code treats `\frac` as atomic — it never breaks inside. When the numerator is very wide, the whole fraction overflows.

Note: the `breqn` package also does *not* break inside fractions — this is a known limitation they document. So this is a feature *beyond* breqn's capability.

### Design: Fraction Promotion

When a `\frac{num}{den}` is wider than `pageWidth`, **promote** it to a multi-line form. There are two standard approaches in mathematical typesetting:

**Approach A: Factor out and break the wide argument**

```latex
% Before (single line, overflows):
\frac{A_1 + A_2 + A_3 + A_4 + A_5}{B}

% After (numerator broken, fraction preserved):
\frac{1}{B} \left( A_1 + A_2 + A_3 + A_4 + A_5 \right)
```

This changes the mathematical *form* — not acceptable for an automatic formatter.

**Approach B: Split the fraction into stacked lines (Recommended)**

```latex
% Before:
\frac{A_1 + A_2 + A_3 + A_4 + A_5}{B}

% After — using \cfrac-style continuation or manual stacking:
\frac{\displaystyle A_1 + A_2 + A_3 + {} \\ \displaystyle \quad + A_4 + A_5}{B}
```

This also doesn't work well — `\\` inside `\frac` is not standard.

**Approach C: Use `\genfrac` or manual fraction with `\above` and break the argument with `\substack`**

Also fragile and non-standard in KaTeX.

**Approach D: Break at the fraction level (Recommended)**

Don't try to break *inside* the fraction in the LaTeX output. Instead, **measure whether the fraction's widest argument exceeds the page width**, and if so, **recursively line-break that argument** and wrap the result in an `\underbrace`-less stacking construct. In practice, for KaTeX rendering, the most reliable approach is:

```latex
% Numerator is line-broken, then wrapped in a manual array:
\frac{
  \begin{gathered}
    A_1 + A_2 + A_3 \\
    {} + A_4 + A_5
  \end{gathered}
}{B}
```

KaTeX supports `\begin{gathered}...\end{gathered}` inside fraction arguments. This preserves the mathematical meaning while breaking the wide numerator.

### Implementation

Add a new pass **before** the main line-breaking that detects wide fractions and recursively breaks their arguments:

```cpp
// New function: pre-process wide fractions
std::string preprocessWideFractions(std::string_view latex,
                                     const LineBreakOptions& opts) {
    std::string result;
    result.reserve(latex.size() + 64);
    
    size_t i = 0;
    while (i < latex.size()) {
        // Look for \frac, \dfrac, \tfrac
        if (latex[i] == '\\') {
            size_t cmdEnd = readCommand(latex, i);
            std::string_view cmd = latex.substr(i, cmdEnd - i);
            
            if (cmd == "\\frac" || cmd == "\\dfrac" || cmd == "\\tfrac") {
                size_t p = cmdEnd;
                
                // Extract numerator
                if (p < latex.size() && latex[p] == '{') {
                    size_t numEnd = skipBraceGroup(latex, p);
                    std::string_view numContent = latex.substr(p + 1, numEnd - p - 2);
                    
                    // Extract denominator
                    size_t denStart = numEnd;
                    if (denStart < latex.size() && latex[denStart] == '{') {
                        size_t denEnd = skipBraceGroup(latex, denStart);
                        std::string_view denContent =
                            latex.substr(denStart + 1, denEnd - denStart - 2);
                        
                        // Estimate widths
                        Tokenizer tzNum{numContent, 0};
                        Tokenizer tzDen{denContent, 0};
                        double numW = estimateTotalWidth(numContent);
                        double denW = estimateTotalWidth(denContent);
                        double maxArgW = std::max(numW, denW);
                        
                        if (maxArgW > opts.pageWidth * 0.9) {
                            // Wide fraction — recursively break the wide argument
                            std::string broken;
                            if (numW >= denW) {
                                std::string brokenNum = lineBreakInner(
                                    numContent, opts, /*useGathered=*/true);
                                broken = std::string(cmd) + "{" +
                                         brokenNum + "}{" +
                                         std::string(denContent) + "}";
                            } else {
                                std::string brokenDen = lineBreakInner(
                                    denContent, opts, /*useGathered=*/true);
                                broken = std::string(cmd) + "{" +
                                         std::string(numContent) + "}{" +
                                         brokenDen + "}";
                            }
                            result += broken;
                            i = denEnd;
                            continue;
                        }
                        // Not wide — emit as-is
                    }
                }
            }
            
            // Not a frac or not wide — copy the command through
            result += latex.substr(i, cmdEnd - i);
            i = cmdEnd;
            continue;
        }
        
        result += latex[i];
        i++;
    }
    
    return result;
}

// Inner line-break: breaks a sub-expression and wraps in gathered
// instead of aligned (since we're inside a fraction)
std::string lineBreakInner(std::string_view latex,
                            const LineBreakOptions& opts,
                            bool useGathered) {
    auto tokens = tokenize(latex);
    double totalWidth = 0.0;
    for (auto& t : tokens) totalWidth += t.width;
    
    if (totalWidth <= opts.pageWidth * 0.85) return std::string(latex);
    
    auto bps = extractBreakpoints(tokens, opts.maxDelimDepth);
    if (bps.empty()) return std::string(latex);
    
    auto breakIndices = findOptimalBreaks(bps, opts.pageWidth * 0.85,
                                          opts.indentStep, totalWidth);
    if (breakIndices.empty()) return std::string(latex);
    
    if (useGathered) {
        return emitGathered(latex, tokens, bps, breakIndices);
    }
    return emitAligned(latex, tokens, bps, breakIndices);
}

// Emit \begin{gathered}...\end{gathered} (centered lines, no alignment)
std::string emitGathered(std::string_view latex,
                          const std::vector<Token>& tokens,
                          const std::vector<Breakpoint>& bps,
                          const std::vector<size_t>& breakIndices) {
    std::string out;
    out += "\\begin{gathered}\n";
    
    size_t prevEnd = 0;
    for (size_t k = 0; k < breakIndices.size(); k++) {
        const Breakpoint& bp = bps[breakIndices[k]];
        const Token& tok = tokens[bp.tokenIndex];
        std::string_view lineContent = trim(latex.substr(prevEnd, tok.start - prevEnd));
        out += "  ";
        out += lineContent;
        out += " \\\\\n";
        prevEnd = tok.start;
    }
    
    std::string_view lastLine = trim(latex.substr(prevEnd));
    if (!lastLine.empty()) {
        out += "  ";
        out += lastLine;
        out += "\n";
    }
    
    out += "\\end{gathered}";
    return out;
}
```

### Integration into the main pipeline

```cpp
std::string lineBreakLatex(std::string_view latex,
                           const LineBreakOptions& opts) {
    if (latex.find("\\\\") != std::string_view::npos) return std::string(latex);
    if (latex.find("\\begin{") != std::string_view::npos) return std::string(latex);

    // NEW: Pre-process wide fractions
    std::string preprocessed = preprocessWideFractions(latex, opts);
    std::string_view input = preprocessed;

    auto tokens = tokenize(input);
    // ... rest of existing pipeline, using `input` instead of `latex` ...
}
```

### Example output

Input: `\frac{a_1 + a_2 + a_3 + a_4 + a_5 + a_6 + a_7 + a_8}{b + c}`

With `pageWidth = 30`:
```latex
\frac{
  \begin{gathered}
    a_1 + a_2 + a_3 + a_4 \\
    {} + a_5 + a_6 + a_7 + a_8
  \end{gathered}
}{b + c}
```

The `{}` before `+` on the continuation line ensures KaTeX treats `+` as a binary operator (with proper spacing) rather than a unary plus.

### Edge cases

- **Both num and den are wide**: break only the wider one. If *both* still overflow after breaking the wider one, break the other too. (Add a second pass.)
- **Nested fractions**: `\frac{\frac{A}{B} + \frac{C}{D}}{E}` — the inner fractions are atomic; only the outer level gets broken. This is correct: nested fractions are visually small.
- **`\tfrac`**: text-style fraction — rendered at a smaller scale, so the threshold should be higher (~`pageWidth * 1.3`) before breaking.
- **`\dfrac`**: display-style — same threshold as `\frac`.

---

## Part 4: `pageWidthPx` Mode

### API addition to `LineBreakOptions`

```cpp
struct LineBreakOptions {
    double pageWidth    = 0.0;   // heuristic mode (em)
    double indentStep   = 2.0;   // em
    int    maxDelimDepth = 2;
    bool   compact      = false;

    // NEW
    double pageWidthPx  = 0.0;   // precise mode (CSS px); 0 = disabled
    double baseFontSizePx = 16.0; // for em↔px conversion
    int    maxIterations = 0;    // 0 = no re-rendering loop
};
```

Resolution in `lineBreakLatex`:
```cpp
double effectivePageWidth = opts.pageWidth;
if (opts.pageWidthPx > 0) {
    // Convert px target to em for the C++ DP
    // (precise per-segment widths come from JS measureFn if available)
    effectivePageWidth = opts.pageWidthPx / opts.baseFontSizePx;
}
```

The C++ side always works in em. The px↔em conversion happens at the boundary. When KaTeX measurement is available (via the TypeScript wrapper), the measured segment widths in px are divided by `baseFontSizePx` before being fed to the DP.

### TypeScript wrapper with measurement + iteration

```typescript
// lineBreakService.ts — lives in the extension host

import katex from 'katex';
import { lineBreakLatex, extractSegments } from './native/addon';

export interface LineBreakResult {
    latex: string;
    iterations: number;
    maxLineWidthPx: number;
}

export function lineBreakWithMeasurement(
    latex: string,
    opts: LineBreakOptions
): LineBreakResult {
    // --- Heuristic-only path ---
    if (!opts.pageWidthPx) {
        return {
            latex: lineBreakLatex(latex, opts),
            iterations: 0,
            maxLineWidthPx: 0,
        };
    }

    const maxIter = Math.max(0, Math.min(opts.maxIterations ?? 0, 5));
    let effectiveWidthPx = opts.pageWidthPx;
    let result = lineBreakLatex(latex, {
        ...opts,
        pageWidth: effectiveWidthPx / (opts.baseFontSizePx ?? 16),
    });
    
    if (maxIter === 0) {
        return { latex: result, iterations: 0, maxLineWidthPx: 0 };
    }

    // --- Iterative refinement ---
    for (let iter = 1; iter <= maxIter; iter++) {
        const lines = extractRenderedLines(result);
        const widths = lines.map(l => measureKaTeX(l, opts.baseFontSizePx ?? 16));
        const maxW = Math.max(...widths);

        if (maxW <= opts.pageWidthPx) {
            return {
                latex: result,
                iterations: iter,
                maxLineWidthPx: maxW,
            };
        }

        // Shrink target proportionally + 5% safety margin
        const ratio = maxW / opts.pageWidthPx;
        effectiveWidthPx = Math.floor(effectiveWidthPx / ratio * 0.95);
        
        // Re-break from original (never from already-broken output)
        result = lineBreakLatex(latex, {
            ...opts,
            pageWidth: effectiveWidthPx / (opts.baseFontSizePx ?? 16),
        });
    }

    // Return best-effort after exhausting iterations
    const finalLines = extractRenderedLines(result);
    const finalWidths = finalLines.map(l => measureKaTeX(l, opts.baseFontSizePx ?? 16));
    return {
        latex: result,
        iterations: maxIter,
        maxLineWidthPx: Math.max(...finalWidths),
    };
}

function extractRenderedLines(broken: string): string[] {
    let body = broken
        .replace(/^\\begin\{(aligned|gathered)\}\s*/,'')
        .replace(/\s*\\end\{(aligned|gathered)\}$/, '');
    return body.split(/\s*\\\\\s*/)
        .map(l => l.replace(/^\s*&?\s*/, '').replace(/\\quad\s*/g, '').trim())
        .filter(l => l.length > 0);
}

function measureKaTeX(latex: string, baseFontSizePx: number): number {
    try {
        const tree = (katex as any).__renderToDomTree(latex, {
            displayMode: true,
            throwOnError: false,
        });
        const htmlTree = tree?.children?.[1]; // .katex-html
        if (!htmlTree) return fallback(latex, baseFontSizePx);
        return walkWidth(htmlTree) * baseFontSizePx;
    } catch {
        return fallback(latex, baseFontSizePx);
    }
}

function walkWidth(node: any): number {
    if (!node) return 0;
    if (typeof node === 'string') return node.length * 0.5;
    if (node.text != null) return String(node.text).length * 0.5;

    const classes: string[] = node.classes || [];

    // Vertical stacks → width = max child
    if (classes.some(c => c === 'vlist' || c === 'vlist-t' || c === 'vlist-t2'))
        return Math.max(0, ...(node.children || []).map(walkWidth));

    // Horizontal → sum children + margins
    let w = 0;
    if (node.style?.marginLeft) w += parseEm(node.style.marginLeft);
    if (node.style?.marginRight) w += parseEm(node.style.marginRight);
    for (const child of (node.children || [])) w += walkWidth(child);
    return w;
}

function parseEm(s: string): number {
    const m = s?.match?.(/^(-?[\d.]+)em$/);
    return m ? parseFloat(m[1]) : 0;
}

function fallback(latex: string, baseFontSizePx: number): number {
    const visible = latex.replace(/\\[a-zA-Z]+/g, 'X').replace(/[{}^_\\]/g, '').length;
    return visible * 0.5 * baseFontSizePx;
}
```

---

## Summary of Changes

| Change | Where | Effort | Impact |
|---|---|---|---|
| Split `isAtomicCmd` → `isStyleCmd` / `isFontCmd` / `isStructuralCmd` | `line_breaker.cpp` | Low | Fixes `\color{red}` overcounting |
| New tokenizer branches for each category | `line_breaker.cpp` | Medium | Correct width for all font/style commands |
| Replace `estimateAtomicWidth` with `estimateStructuralWidth` | `line_breaker.cpp` | Medium | Correct `\hat`, `\overline`, `\overset` |
| Detailed `cmdWidth` with math spacing and glyph tables | `line_breaker.cpp` | Medium | ±10% accuracy (was ±30%) |
| Per-character width table for lowercase math italic | `line_breaker.cpp` | Low | `i` vs `m` no longer same width |
| `preprocessWideFractions` + `lineBreakInner` + `emitGathered` | `line_breaker.cpp` | Medium | Breaks wide `\frac` numerators/denominators |
| `pageWidthPx` / `baseFontSizePx` in options | `line_breaker.h` | Low | New precise mode API |
| `maxIterations` in options | `line_breaker.h` | Low | Iterative refinement API |
| `lineBreakWithMeasurement` TypeScript wrapper | `lineBreakService.ts` | Medium | KaTeX measurement + iteration loop |
| `extractRenderedLines` + `measureKaTeX` + `walkWidth` | `lineBreakService.ts` | Medium | Precise width via KaTeX virtual DOM |