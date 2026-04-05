# Wolfbook — Mathematica Box → KaTeX Renderer

A high-performance rendering engine for the **Wolfbook** VSCode extension. Converts Mathematica's internal box representation into LaTeX, then renders it via KaTeX inside the VSCode Webview.

---

## Architecture

```
Mathematica kernel
        │
        │  ToBoxes[expr, TraditionalForm]
        ▼
   Box tree  (WL symbolic expression)
        │
        │  ToString[boxes, InputForm]  → JSON field "rawBoxes"
        ▼
  WL InputForm string  e.g. FractionBox["1","2"]
        │
        │  WSTP / JSON  →  Extension host (Node.js)
        ▼
  ┌─────────────────────────────────────────────────────┐
  │  C++ native addon  (build/Release/wolfbook_btl.node) │
  │                                                     │
  │  WLParser           →  flat arena AST               │
  │  BoxTranslator      →  single-line LaTeX string     │
  │  LineBreaker        →  multi-line LaTeX (optional)  │
  │  special_chars      →  O(1) named-char lookup       │
  └─────────────────────────────────────────────────────┘
        │
        │  LaTeX string  e.g. \frac{1}{2}
        │  or \begin{aligned}…\end{aligned} (if line-broken)
        ▼
  outputRenderer.ts
  (mode decision)
        │
        ├── Mode A  "latex"  ──────────────────────────────────►  Webview
        │                                                        katex.render()
        │
        └── Mode B  "prerendered"  ──►  katexPrerender.ts
                                        katex.renderToString()
                                              │
                                              ▼
                                        HTML string  ─────────►  Webview
                                                               container.innerHTML
```

---

## Layers

### 1. Wolfram Language (kernel side) — `renderer/`

The WL files are the **reference implementation** of the translator. They run on the Mathematica/Wolfram Engine kernel and are also used as the ground truth for the unit tests.

| File | Purpose |
|------|---------|
| `renderer/BoxToLatex.wl` | Main pattern-matching translator. Exports `boxToLatex`. |
| `renderer/SpecialChars.wl` | `$WLtoLaTeX` Association: named WL chars → LaTeX commands. |
| `renderer/Colors.wl` | `colorToHex` utility: `RGBColor`, `GrayLevel`, named colours → `#rrggbb`. |
| `renderer/GridBox.wl` | `gridBoxToLatex`: matrix environments, piecewise, aligned. |
| `renderer/Tests.wl` | `VerificationTest` suite (43 cases, literal box inputs only). |

The WL layer always produces a **LaTeX string**. It knows nothing about rendering modes, KaTeX, or VSCode.

### 2. C++ native addon — `src/native/`

The production fast path. Compiled to a Node.js native module via `node-gyp`. All translation logic is in pure C++17 with no Node.js API beyond the thin N-API wrapper.

| File | Purpose |
|------|---------|
| `src/native/wl_parser.h/.cpp` | Recursive-descent parser for WL InputForm strings. Produces a flat arena-allocated AST (no per-node heap allocation). Handles strings (including `\[Named]` escapes), symbols, numbers, compound expressions `Head[…]`, lists `{…}`, and rules `lhs -> rhs`. |
| `src/native/special_chars.h/.cpp` | `wlCharToLatex(token)` via `unordered_map` (O(1) avg). `isLargeOperator(latex)` via `unordered_set`. Covers all Greek, operators, arrows, blackboard-bold, spacing/invisible characters. |
| `src/native/box_to_latex.h/.cpp` | `BoxTranslator` walks the AST by index (no virtual calls). Handles all box heads. `StyleBox` applies colour innermost → bold outermost. Delimiter detection in `RowBox` selects matrix environments. Accepts a `BtlOptions` struct to enable/disable typographic rules (trig paren omission, trig power form). |
| `src/native/line_breaker.h/.cpp` | Post-processing line-break layer. Takes a single-line LaTeX string and wraps it in `\begin{aligned}…\end{aligned}` when it exceeds a target `pageWidth`. Uses symbol classification (relations, binary operators, delimiters), delimiter-depth tracking, and a simplified Knuth-Plass DP to find optimal breakpoints. Inspired by the algorithm in the `breqn` LaTeX package (see [Credits](#credits)). |
| `src/native/addon.cpp` | N-API wrapper. Exports two JS functions: `boxToLatex(str, opts?)` and `lineBreakLatex(latex, opts?)`. Zero DOM dependencies. |

**Performance characteristics:**
- Parse: single-pass, O(n) time, O(n) space in a flat arena
- Special-char lookup: O(1) average hash map
- Translation: one pass over the AST with `std::string::append` into a pre-reserved result buffer
- No allocations after the arena is full; result string is moved out

### 3. TypeScript extension host — `src/`

Runs in the **Node.js extension host process**. No DOM APIs.

| File | Purpose |
|------|---------|
| `src/wolfbook_btl.d.ts` | TypeScript declarations for the C++ addon (`boxToLatex` and `lineBreakLatex`). |
| `src/katexPrerender.ts` | **Mode B**: calls `katex.renderToString()` synchronously and returns a self-contained HTML+SVG string. HTML-escape fallback on KaTeX error. `strict: false` enables `\textcolor`. |
| `src/outputRenderer.ts` | **Mode dispatcher**: `prepareOutput(latex, displayMode, config)` → `{ mode, payload }`. Auto-upgrades to pre-rendered when `latex.length > prerenderedThreshold` (default 2 000 chars). Zero VSCode API imports — fully unit-testable in plain Node.js. |

### 4. TypeScript Webview — `webview/`

Runs in the **Webview renderer process** (browser-like sandbox). All DOM APIs available.

| File | Purpose |
|------|---------|
| `webview/renderer.ts` | **Mode A**: `renderLatexOutput(container, latex, displayMode)` calls `katex.render()`. `handleOutputMessage(msg)` dispatches on `msg.mode`: injects HTML directly (Mode B) or calls `renderLatexOutput` (Mode A). `installMessageHandler()` wires the `window.message` listener. |

**KaTeX CSS is always needed** in the Webview `<head>` (both modes). The KaTeX JS bundle is only needed in the Webview for Mode A — in Mode B it can be omitted, reducing Webview JS payload.

---

## Output modes

| | Mode A — `"latex"` | Mode B — `"prerendered"` |
|---|---|---|
| **Where KaTeX runs** | Webview JS thread | Extension host (Node.js) |
| **Payload size** | Small (LaTeX string) | Larger (HTML+SVG, ~10–50×) |
| **Webview work** | `katex.render()` | `container.innerHTML = …` |
| **When preferred** | Small expressions, interactive edits | Large outputs, batch rendering |
| **Auto-selected when** | `config.mode = 'latex'` AND `len ≤ 2000` | `config.mode = 'prerendered'` OR `len > 2000` |

---

## Data flow (full pipeline)

```
Kernel:
  ToBoxes[expr, TraditionalForm]
  → ToString[boxes, InputForm]          "FractionBox[\"1\",\"2\"]"
  → ExportString to JSON                { type:"output", latex:"...", rawBoxes:"..." }

Extension host receives JSON:
  boxToLatex(rawBoxes)   [C++ addon]    { latex: "\\frac{1}{2}", error: null }
  if (result.error) → log warning, use verbatim fallback
  prepareOutput(result.latex, true, config)
    if prerendered:
      prerenderLatex(latex)             "<span class=\"katex-display\">…</span>"
      → postMessage { mode:"prerendered", payload: html }
    else:
      → postMessage { mode:"latex",       payload: latex }

Webview receives postMessage:
  mode="prerendered" → container.innerHTML = payload
  mode="latex"       → katex.render(payload, container, opts)
```

---

## Error reporting

`boxToLatex` **never throws**. Instead it always returns an object with two fields:

```ts
interface BoxToLatexResult {
  latex: string;         // LaTeX on success; verbatim WL input on failure
  error: string | null;  // null on success; diagnostic message on failure
}
```

### `lineBreakLatex` — optional line-breaking pass

```ts
import * as btl from './build/Release/wolfbook_btl.node';

const { latex } = btl.boxToLatex(rawBoxes);

// Wrap long expressions in \begin{aligned}...\end{aligned}.
// No-op when the expression fits within pageWidth, or when the
// input is already a multi-line environment.
const broken = btl.lineBreakLatex(latex, {
  pageWidth:     60,   // target width in em (default: 80)
  indentStep:     2,   // continuation indent in em (default: 2)
  compact:     false,  // true: pack lines; false: prefer relation-aligned breaks
  maxDelimDepth:  2,   // max nesting depth for breakpoints (default: 2)
});

katex.render(broken, container, { displayMode: true });
```

### Style options (`BtlOptions`)

`boxToLatex` accepts an optional second argument to control typographic style rules.
Both flags default to `true` (enabled). Set to `false` to revert to plain TeX output.

```ts
const { latex } = btl.boxToLatex(rawBoxes, {
  trigOmitParens: true,   // \sin(\phi) → \sin\phi   (single-symbol arg only)
  trigPowerForm:  true,   // (\sin\phi)^2 → \sin^2\phi
});
```

| Flag | Default | `true` | `false` |
|---|---|---|---|
| `trigOmitParens` | `true` | `\sin\phi` | `\sin(\phi)` |
| `trigPowerForm` | `true` | `\sin^2\phi` | `(\sin\phi)^2` |

**Scope:** both rules apply to all standard trig/hyperbolic names (`sin`, `cos`, `tan`, `cot`, `sec`, `csc`, `arcsin`, `arccos`, `arctan`, `arccot`, `arcsec`, `arccsc`, `sinh`, `cosh`, `tanh`, `coth`, `arcsinh`, `arccosh`, `arctanh`), including their WL-capitalised variants (`Sin`, `Cos`, etc.).

**`trigOmitParens`** fires only when the argument inside the parentheses is a single symbol (a single letter, a bare LaTeX command like `\phi`, or a single Unicode character). Multi-token arguments are left unchanged: `\sin(x+y)` stays as-is.

**`trigPowerForm`** fires only when the base of a superscript is a parenthesised trig expression whose own argument is a single symbol. The pattern `(\sin\phi)^n` → `\sin^n\phi` is applied at the C++ AST level, so it handles all exponent forms reliably including multi-character powers like `^{2}` or `^{-1}`.

These rules are also exposed as VS Code settings:
- `wolfbook.notebook.rendering.trigOmitParens` (default: `true`)
- `wolfbook.notebook.rendering.trigPowerForm` (default: `true`)

**Break priority** (lower penalty = preferred):

| Symbol class | Examples | Penalty |
|---|---|---|
| Relation | `=`, `<`, `\leq`, `\to`, `:=` | −100 (strongest — always try here first) |
| Comma | `,` | +30 |
| Binary op | `+`, `−`, `\times`, `\cup` | +50 |
| Delimiter | `(`, `)`, `[`, `]` | +200 |
| Inside delimiters | any of the above at depth *d* | base + 500 × *d* |

Breaks deeper than `maxDelimDepth` are forbidden entirely.

---

### Checking for errors

```ts
import * as btl from './build/Release/wolfbook_btl.node';

const { latex, error } = btl.boxToLatex(rawBoxes);

if (error) {
  // Translation failed.
  // `latex` contains the raw WL box string — display it as plain text
  // or show a fallback placeholder.
  console.warn('[wolfbook] boxToLatex error:', error);
  showFallback(latex);
} else {
  katex.render(latex, container, { displayMode: true });
}
```

### What triggers an error

| Situation | `latex` field | `error` field |
|---|---|---|
| Parse error (unexpected syntax, `#`, `&`, etc.) | verbatim input | parser message |
| Valid parse, all boxes rendered | LaTeX string | `null` |
| Unsupported box head encountered | partial LaTeX + head name | currently `null` (unknown heads emit a warning to `stderr` but translation continues) |

### stderr vs. the error field

Both channels convey the same message:

| Channel | Audience | Format |
|---|---|---|
| `stderr` | Developer / log files | `[wolfbook] boxToLatex parse error: <message>` |
| `result.error` | Calling code | Plain string, suitable for UI display or structured logging |

The `stderr` line is always emitted regardless of whether the caller checks `result.error`. This means parse failures are always visible in the VS Code **Output** panel (extension host channel) even if the caller ignores the error field.

### Verbatim pass-through

On failure the `latex` field contains the **original input string**, not an empty string. This is intentional — it ensures the UI always has something to display (e.g. the raw WL expression) rather than showing nothing. Callers can distinguish success from failure by checking `error !== null`.

---

## Build

```bash
# First time
npm install --ignore-scripts
./build.sh            # native + TypeScript + smoke test

# Incremental
./build.sh            # only rebuilds what changed

# Force full rebuild
./build.sh rebuild

# Individual steps
./build.sh native     # C++ addon only
./build.sh ts         # TypeScript only
./build.sh smoke      # 8-case Node.js smoke test
./build.sh test       # 43-case WL unit tests (needs wolframscript)
./build.sh clean      # remove build/ and out/
```

---

## Interactive tester

A local dev server lets you paste WL box expression strings and see the LaTeX output and KaTeX rendering side-by-side.

```bash
npm run tester
# → http://localhost:3141
```

The tester calls the C++ addon directly via a `/api/translate` REST endpoint. It is completely separate from VSCode — useful for debugging the translator in isolation.

---

## File tree

```
.
├── binding.gyp                  node-gyp build descriptor
├── package.json
├── tsconfig.json
├── build.sh                     build / rebuild / test / clean script
│
├── renderer/                    Wolfram Language reference implementation
│   ├── BoxToLatex.wl            main translator  (exports boxToLatex)
│   ├── SpecialChars.wl          $WLtoLaTeX association
│   ├── Colors.wl                colorToHex utility
│   ├── GridBox.wl               matrix / piecewise / aligned environments
│   └── Tests.wl                 VerificationTest suite (43 tests)
│
├── src/
│   ├── wolfbook_btl.d.ts        TS declarations for C++ addon
│   ├── katexPrerender.ts        Mode B: katex.renderToString() in ext host
│   ├── outputRenderer.ts        mode dispatcher (no VSCode API imports)
│   └── native/                  C++ native addon
│       ├── addon.cpp            N-API entry point (boxToLatex + lineBreakLatex)
│       ├── box_to_latex.h/.cpp  AST → LaTeX translator
│       ├── line_breaker.h/.cpp  post-processing line-break layer
│       ├── wl_parser.h/.cpp     WL InputForm string parser
│       └── special_chars.h/.cpp named-char lookup table
│
├── webview/
│   └── renderer.ts              Mode A: katex.render() in Webview
│
└── tester/                      standalone local dev tester
    ├── index.html               interactive UI (KaTeX from CDN)
    └── server.js                Node.js HTTP server + /api/translate
```

---

## Dependencies

| Package | Version | Used in | Purpose |
|---------|---------|---------|---------|
| `katex` | `^0.16` | Extension host + Webview | Math rendering |
| `node-addon-api` | `^8` | Build only | C++ N-API wrapper |
| `node-gyp` | `^10` | Build only | Native addon build tool |
| `typescript` | `^5.4` | Build only | TS compilation |

KaTeX **must be bundled** (not CDN-loaded) when running inside a VSCode Webview due to the extension host Content Security Policy. The tester page uses the CDN for convenience.

---

## Credits

### breqn — Automatic line breaking for displayed math

The line-breaking algorithm in `src/native/line_breaker.cpp` is inspired by the **`breqn`** LaTeX package. The core ideas adopted here are:

- **Symbol reclassification**: math symbols assigned to semantic break-classes (relation, binary operator, open/close delimiter) to determine preferred break positions — mirroring `breqn`'s use of `flexisym`.
- **Penalty model**: negative penalty at relation symbols (strongly preferred break), positive penalty at binary operators (fallback), and increasing penalty at deeper delimiter nesting — following `breqn`'s `\prerelpenalty` / `\prebinoppenalty` conventions.
- **Optimal breaking via dynamic programming**: a simplified version of the Knuth-Plass paragraph-breaking algorithm (box/glue/penalty model), adapted for short math expressions where O(n²) DP is sufficient.
- **Layout shapes**: the distinction between *straight ladder* (LHS < 40% of line width, relation-aligned continuation) and *staggered* (`\quad`-indented continuation) layouts is drawn from `breqn`'s parshape taxonomy.

**Original authors of `breqn`:**
- **Michael J. Downes** — original design and implementation (1997–2004)
- **Morten Høgholm** — major rewrite and maintenance (LaTeX3 Project, 2006–2012)

The `breqn` package is part of the LaTeX3 Project and is distributed under the LaTeX Project Public License (LPPL). Source: https://ctan.org/pkg/breqn

### Knuth-Plass line-breaking algorithm

The underlying DP model is the **Knuth-Plass optimal paragraph-breaking algorithm**, originally described in:

> D. E. Knuth and M. F. Plass, "Breaking Paragraphs into Lines", *Software — Practice and Experience*, 11(11):1119–1184, 1981.

### KaTeX

Math rendering is provided by **KaTeX**, developed by Khan Academy. https://katex.org
