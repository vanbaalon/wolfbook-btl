# BTL Line-Breaking Algorithm Design

> **Status: implemented** — see `src/native/line_breaker.h/.cpp`.
> This document describes the design rationale and algorithm details.
> The public API is `lineBreakLatex(latex, opts?)` exposed via the C++ native addon.

## Credits

The line-breaking algorithm implemented here is inspired by the **`breqn`** LaTeX package, which adapts TeX's Knuth-Plass paragraph-breaking algorithm to displayed mathematical expressions.

**Original authors of `breqn`:**
- **Michael J. Downes** — original design and implementation (1997–2004)
- **Morten Høgholm** — major rewrite and ongoing maintenance (LaTeX3 Project, 2006–2012)

`breqn` is part of the LaTeX3 ecosystem and is available on CTAN: https://ctan.org/pkg/breqn  
License: LaTeX Project Public License (LPPL).

The Knuth-Plass algorithm itself is described in:
> D. E. Knuth and M. F. Plass, "Breaking Paragraphs into Lines", *Software — Practice and Experience*, 11(11):1119–1184, 1981.

## Goal

Add an optional line-breaking post-processing layer to the BTL (Box-to-LaTeX) addon, activated by a `pageWidth` parameter. When `pageWidth` is set, the renderer wraps long mathematical expressions across multiple lines, following the conventions of mathematical typesetting (break at relations first, then binary operators, indent continuation lines).

## Background: How breqn Works

The LaTeX `breqn` package (by Michael Downes / Morten Høgholm) adapts TeX's Knuth-Plass paragraph-breaking algorithm to displayed math. The key ideas are:

### 1. Symbol Classification via flexisym

Before any breaking logic runs, breqn **reclassifies math symbols** into semantic categories. The `flexisym` package intercepts `\DeclareMathSymbol` so that every symbol of mathclass 2–5 (Rel, Bin, Open, Close) gains a hook. Inside a `dmath` environment, these hooks inject **penalty + glue** items into the math list at each potential breakpoint.

The critical penalty values from breqn source:

| Symbol Class | Penalty Name | Default Value | Meaning |
|---|---|---|---|
| Relation (`=`, `<`, `\leq`, …) | `\prerelpenalty` | −10000 (forced) | *Strongly prefer* breaking before relations |
| Binary op (`+`, `−`, `\times`, …) | `\prebinoppenalty` | 888 | Mildly discourage breaking before binops |
| Inside delimiters (depth > 0) | additional penalty | +penalty per depth | Discourage breaks inside `\left..\right` |

The negative relation penalty means TeX will *always* try to break before a `=` sign if needed. The positive binop penalty means breaks at `+`/`−` are a fallback.

### 2. Multi-Pass Layout with Parshape

breqn doesn't just break greedily. It uses TeX's `\parshape` mechanism to run the Knuth-Plass optimal paragraph-breaking algorithm on the math content. The process:

1. **Measure pass**: Typeset the equation in a box of infinite width to get its natural width. Also measure the LHS (everything before the first relation symbol).
2. **Shape decision**: Based on `eq_wdL` (LHS width) vs `eq_linewidth` (available width), choose a layout shape:
   - **Single line**: if natural width ≤ line width, done.
   - **Straight ladder**: LHS fits in first line; continuation lines indented by `indentstep` (default 8pt) from the relation symbol.
   - **Staggered**: LHS is wide (> 50% of line width); first continuation line indented further.
   - **Centered/compact**: with the `compact` option, pack as much as possible per line.
3. **Breaking pass**: Feed the math content to TeX's paragraph builder with the computed `\parshape`. TeX's Knuth-Plass algorithm finds globally optimal breakpoints minimizing total "badness" (deformation of glue).
4. **Failure fallback**: If all lines are overfull, breqn falls back to a less constrained layout.

### 3. The Knuth-Plass Core (for reference)

The algorithm models content as a sequence of **boxes** (fixed-width chunks), **glue** (stretchable/shrinkable space), and **penalties** (breakpoint costs). It uses dynamic programming to find the set of breakpoints minimizing total demerits:

```
demerits(line) = (1 + badness(line) + penalty)²
badness(line) = 100 × |adjustment_ratio|³

adjustment_ratio = (desired_width − actual_width) / available_stretch_or_shrink
```

The algorithm maintains a set of *active nodes* (feasible breakpoints) and prunes nodes whose adjustment ratio exceeds a tolerance. This gives O(n) amortized complexity for typical inputs.

## Design for BTL

### Architecture: Post-Processing Layer

The line-breaking layer is a **separate pass** that runs after BTL has produced a single-line LaTeX string. It operates on the BTL box tree (not raw LaTeX), which already contains semantic structure.

```
Wolfram Boxes  →  BTL Converter  →  Box Tree (IR)  →  Line Breaker  →  Multi-line LaTeX
                                         ↑                                    ↓
                                    (single-line)                    \begin{aligned}...
                                                                     or manual \\ breaks
```

### Activation

```typescript
interface LineBreakOptions {
  pageWidth: number;        // target width in "em" units (e.g., 80)
  measureFn?: (latex: string) => number;  // optional: precise width measurement
  indentstep?: number;      // continuation indent in em (default: 2)
  compact?: boolean;        // prefer fewer lines over relation-aligned breaks
  breakDepth?: number;      // max delimiter nesting depth for breaks (default: 2)
}
```

When `pageWidth` is provided, the line-breaker activates.

### Step 1: Width Estimation

Since BTL runs in VS Code (no TeX engine), we need a width model. Two strategies:

**Strategy A — Character-count heuristic** (simple, fast):
- Assign estimated widths to LaTeX constructs based on character counts
- `\frac{a}{b}` → width ≈ max(width(a), width(b)) + overhead
- Subscripts/superscripts → scaled by 0.7
- Greek letters, `\sum`, `\int` → lookup table of known widths
- Regular characters → ~1.0 em each

**Strategy B — KaTeX measurement** (precise, requires renderer):
- Render to KaTeX in a hidden container, measure the resulting DOM width
- More accurate but requires the KaTeX renderer to be available

Recommendation: Implement Strategy A first; support Strategy B via the optional `measureFn` callback.

### Step 2: Box Tree Annotation

Walk the BTL box tree and **annotate each node** with:

```typescript
interface AnnotatedNode {
  node: BTLNode;
  width: number;              // estimated width
  breakClass: BreakClass;     // NONE | RELATION | BINOP | OPEN | CLOSE | COMMA
  delimDepth: number;         // nesting depth inside delimiters
  isLHS: boolean;             // part of the LHS (before first top-level relation)
  cumulativeWidth: number;    // running total from start of expression
}

enum BreakClass {
  NONE,       // no break possible here
  RELATION,   // =, <, >, \leq, \geq, \sim, \equiv, \approx, ...
  BINOP,      // +, -, \pm, \mp, \times, \cdot, \cup, \cap, \oplus, ...
  COMMA,      // , (in argument lists)
  OPEN,       // after opening delimiter (low priority)
  CLOSE,      // before closing delimiter (low priority)
}
```

The annotation pass recognizes symbols by their BTL node type or the LaTeX command they produce. This mirrors breqn's `flexisym` reclassification but at the tree level rather than TeX's math list level.

### Step 3: Breakpoint Identification

Extract a flat list of **candidate breakpoints** from the annotated tree:

```typescript
interface Breakpoint {
  index: number;              // position in linearized node list
  breakClass: BreakClass;
  delimDepth: number;
  penalty: number;            // computed penalty (lower = more preferred)
  widthBefore: number;        // cumulative width up to this point
  indentAfter: number;        // indent for the continuation line
}
```

**Penalty assignment** (following breqn conventions):

| Break Class | Base Penalty | Depth Modifier |
|---|---|---|
| RELATION | −100 | +500 per delimiter depth |
| BINOP | +50 | +500 per delimiter depth |
| COMMA | +30 | +500 per delimiter depth |
| OPEN | +200 | +500 per delimiter depth |
| CLOSE | +200 | +500 per delimiter depth |

Breaks at delimiter depth > `breakDepth` get penalty +∞ (forbidden).

### Step 4: Optimal Breaking (Simplified Knuth-Plass)

Since math expressions are typically much shorter than text paragraphs (tens of breakpoints, not thousands), we can use a simplified version:

```typescript
function findBreaks(
  breakpoints: Breakpoint[],
  pageWidth: number,
  indentstep: number
): number[] {
  const n = breakpoints.length;

  // dp[i] = minimum total demerits for breaking the expression
  //         such that line j ends at breakpoint i
  const dp: number[] = new Array(n + 1).fill(Infinity);
  const parent: number[] = new Array(n + 1).fill(-1);
  dp[0] = 0; // start of expression

  for (let j = 1; j <= n; j++) {
    // Available width for this line
    const lineWidth = (parent[j] === 0)
      ? pageWidth                    // first line: full width
      : pageWidth - indentstep;      // continuation: indented

    for (let i = 0; i < j; i++) {
      const contentWidth = breakpoints[j].widthBefore
                         - breakpoints[i].widthBefore;

      if (contentWidth > lineWidth * 1.1) continue; // too wide, prune

      const slack = lineWidth - contentWidth;
      const badness = slack < 0
        ? 1000 + (-slack) * 100      // overfull: heavy penalty
        : Math.pow(slack / lineWidth, 3) * 100;  // underfull: cubic

      const penalty = breakpoints[j].penalty;
      const lineDemerit = Math.pow(1 + badness + Math.max(0, penalty), 2);
      const totalDemerit = dp[i] + lineDemerit;

      if (totalDemerit < dp[j]) {
        dp[j] = totalDemerit;
        parent[j] = i;
      }
    }
  }

  // Trace back to find optimal breakpoints
  const result: number[] = [];
  let cur = n;
  while (cur > 0) {
    result.unshift(breakpoints[cur].index);
    cur = parent[cur];
  }
  return result;
}
```

**Key differences from full Knuth-Plass:**
- No glue stretch/shrink model (we don't have elastic spacing in LaTeX output)
- No active-node pruning (expression breakpoint count is small enough for O(n²))
- Parshape is simple: first line full width, all others indented

### Step 5: Layout Shape Selection

Following breqn's layout taxonomy, choose the output format:

**Case 1: Single line** — expression fits within `pageWidth`. No action needed.

**Case 2: Straight ladder** — LHS width < 40% of `pageWidth`.
```latex
\begin{aligned}
  LHS &= RHS_1 \\
      &= RHS_2 \\
      &\quad + RHS_{2b}
\end{aligned}
```
Break before each relation symbol; sub-breaks at binops get extra indent (`\quad`).

**Case 3: Staggered** — LHS width ≥ 40% of `pageWidth`.
```latex
\begin{multline}
  LHS = RHS_{1a} + RHS_{1b} \\
  \quad + RHS_{1c} + RHS_{1d} \\
  \quad = RHS_2
\end{multline}
```
Or equivalently using `aligned` with the first line unindented.

**Case 4: Compact** — `compact: true` option. Pack maximally per line, ignoring the preference for relation-aligned breaks.

### Step 6: LaTeX Emission

The line breaker produces one of:
- **`aligned` environment** (default, for relation-aligned layouts)
- **`multline` environment** (for staggered layouts)
- **Manual `\\` with `\quad` indents** (for simpler contexts)

Each line segment is extracted from the original BTL output by slicing at the chosen breakpoints.

### Integration Point in BTL

```typescript
// In the main BTL rendering pipeline:
function renderToLatex(boxes: WolframBoxes, options?: BTLOptions): string {
  const tree = buildBoxTree(boxes);
  let latex = emitLatex(tree);

  if (options?.pageWidth) {
    latex = lineBreak(tree, {
      pageWidth: options.pageWidth,
      indentstep: options.indentstep ?? 2,
      compact: options.compact ?? false,
      breakDepth: options.breakDepth ?? 2,
    });
  }

  return latex;
}
```

## Symbol Classification Reference

### Relations (break *before*, low penalty)
`=`, `<`, `>`, `\leq`, `\geq`, `\neq`, `\equiv`, `\sim`, `\simeq`, `\approx`, `\cong`, `\propto`, `\in`, `\ni`, `\subset`, `\supset`, `\subseteq`, `\supseteq`, `\vdash`, `\models`, `\to`, `\mapsto`, `\coloneq`, `:=`

### Binary Operators (break *before*, moderate penalty)
`+`, `-`, `\pm`, `\mp`, `\times`, `\cdot`, `\div`, `\cup`, `\cap`, `\wedge`, `\vee`, `\oplus`, `\otimes`, `\circ`, `\bullet`, `\setminus`, `\star`

### Delimiters (track depth, high penalty inside)
**Open:** `(`, `[`, `\{`, `\langle`, `\lvert`, `\lVert`, `\lfloor`, `\lceil`
**Close:** `)`, `]`, `\}`, `\rangle`, `\rvert`, `\rVert`, `\rfloor`, `\rceil`

### Never Break
- Inside `\frac{...}{...}` (numerator/denominator are atomic)
- Inside `\sqrt{...}`
- Inside superscripts/subscripts `^{...}`, `_{...}`
- Inside `\text{...}`, `\mathrm{...}`, etc.

## Complexity

For an expression with *n* candidate breakpoints:
- Annotation pass: O(N) where N is the node count
- Breakpoint extraction: O(N)
- DP optimization: O(n²) — typically n < 50 for real-world expressions
- Total: effectively linear in expression size

## Testing Strategy

1. **Unit tests**: Known expressions with expected break positions
2. **Width validation**: Compare heuristic widths against KaTeX-measured widths
3. **Regression corpus**: Run against a set of expressions from real papers (QSC, fishnet, etc.) and verify output compiles correctly
4. **Visual comparison**: Render broken output in KaTeX at various `pageWidth` values

## Future Extensions

- **User-specified breakpoints**: Allow `\allowbreak`-style hints in the Wolfram input
- **Equation numbers**: Handle `\tag{}` placement for numbered equations
- **Group alignment**: When multiple equations are rendered together, align their relation symbols (breqn's `dgroup` functionality)
- **Shrink-to-fit**: Before breaking, try to fit by using `\!` negative thin spaces (as breqn exploits glue shrink)