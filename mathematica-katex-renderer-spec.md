# Mathematica Box → KaTeX Renderer

## Overview

This module converts Mathematica's internal box representation (as produced by `ToBoxes[expr, TraditionalForm]`) into rendered output for display inside a VSCode Webview. It is a core rendering component of the **Wolfbook** extension.

It supports two output modes, selectable per-output or globally via user preference:

```
Mathematica expression
        ↓  ToBoxes[expr, TraditionalForm]       (kernel side, Wolfram Language)
   Box tree (WL symbolic expression)
        ↓  boxToLatex[boxes]                    (kernel side, Wolfram Language)
   LaTeX string
        ↓  (transmitted via WSTP/JSON to extension host)
        │
        ├── Mode A: "latex"
        │       ↓  postMessage to Webview
        │   LaTeX string in Webview
        │       ↓  katex.render(latex, container)
        │   HTML injected into output cell
        │
        └── Mode B: "prerendered"
                ↓  katex.renderToString(latex)  (extension host, Node.js)
            HTML+CSS string
                ↓  postMessage to Webview
            container.innerHTML = html           (Webview does no rendering)
```

**Mode A** (lazy): LaTeX string is sent to the Webview and rendered there. KaTeX must be bundled in the Webview.

**Mode B** (pre-rendered): KaTeX runs in the extension host (Node.js process) synchronously, producing a self-contained HTML+CSS string. The Webview does nothing but set `innerHTML`. No KaTeX bundle needed in the Webview at all. This is faster for large outputs because the Webview's rendering thread is not blocked, and the extension host can pipeline rendering across multiple output cells concurrently.

---

## Architecture

### Responsibilities by layer

| Layer | Responsibility |
|-------|---------------|
| **Wolfram Language (kernel)** | Box → LaTeX translation. Always produces a LaTeX string. Knows nothing about rendering. |
| **Extension host (Node.js TypeScript)** | Decides output mode. In Mode B, calls `katex.renderToString()` synchronously and sends HTML. In Mode A, forwards the LaTeX string. |
| **Webview** | In Mode A, calls `katex.render()` on the received LaTeX string. In Mode B, sets `container.innerHTML` directly. |

The Wolfram Language side is identical in both modes — it always outputs a LaTeX string. The mode decision lives entirely in the extension host, which means it can be changed at runtime (e.g. per user setting, or automatically switched based on expression size) without touching WL code.

### Why pre-rendering in the extension host (Mode B) is preferable for large outputs

The extension host is a Node.js process where KaTeX runs natively and synchronously with no DOM overhead. `katex.renderToString()` produces a self-contained HTML string — KaTeX inlines all glyph paths as SVG data, so no font files need to be loaded in the Webview. For a large QSC output cell, the Webview's rendering thread just does a single `innerHTML` assignment rather than running a layout-intensive JS render pass.

The downside of Mode B is that the HTML strings are larger than LaTeX strings (~10–50× depending on expression complexity), so there is more data to transmit over the Webview message channel. For most expressions this is negligible. A configurable size threshold can be used to auto-select the mode.

---

## Component 1: Wolfram Language box translator (`boxToLatex`)

**File:** `renderer/BoxToLatex.wl`

### Entry point

```mathematica
boxToLatex[expr_] := boxToLatex[ToBoxes[expr, TraditionalForm]]
```

The function is defined by pattern matching over every known box head. The output is always a `String`.

---

### Box → LaTeX mapping

#### Leaf nodes

| Input | Output |
|-------|--------|
| `"x"` (plain string) | `"x"` — classified as `\mathrm`, `\mathit`, or operator depending on content |
| A string of digits `"123"` | `"123"` |
| A Greek letter string `"\[Alpha]"` | `"\\alpha"` |
| A named WL character `"\[Infinity]"` | `"\\infty"` |

String classification heuristic:
- All digits → number, output as-is
- Single letter or Greek → identifier, output as `\mathit{x}` or `\alpha`
- Multi-letter (e.g. `"Sin"`, `"Cos"`) → operator name, output as `\mathrm{Sin}`
- Operator characters `+`, `-`, `=`, `\[Element]`, etc. → map to LaTeX operators

#### Structural boxes

| Box | LaTeX output | Notes |
|-----|-------------|-------|
| `RowBox[{e1, e2, ...}]` | `e1 e2 ...` (concatenated) | Main grouping construct |
| `SuperscriptBox[x, y]` | `{X}^{Y}` | |
| `SubscriptBox[x, y]` | `{X}_{Y}` | |
| `SubsuperscriptBox[x, y, z]` | `{X}_{Y}^{Z}` | |
| `FractionBox[num, den]` | `\frac{N}{D}` | |
| `SqrtBox[x]` | `\sqrt{X}` | |
| `RadicalBox[x, n]` | `\sqrt[N]{X}` | |
| `UnderscriptBox[x, y]` | `\underset{Y}{X}` | |
| `OverscriptBox[x, y]` | `\overset{Y}{X}` | |
| `UnderoverscriptBox[x, y, z]` | `\underset{Y}{\overset{Z}{X}}` or `\sum`-style | See note below |
| `GridBox[{{r1c1, r1c2,...},...}]` | `\begin{pmatrix}...\end{pmatrix}` | Environment depends on context |

**Note on `UnderoverscriptBox`:** When the base is a known large operator (`\[Sum]`, `\[Integral]`, `\[Product]`, etc.), use `\sum_{Y}^{Z}` display-style rather than `\underset`. Detect this by checking if the base string maps to a large operator.

#### Style box

`StyleBox[expr_, opts___]` is the key box for colour and font. Extract options and wrap:

```mathematica
boxToLatex[StyleBox[expr_, opts___]] :=
  Module[{latex = boxToLatex[expr], col, bold, italic},
    col   = FontColor /. {opts} /. FontColor -> None;
    bold  = FontWeight /. {opts} /. FontWeight -> "Plain";
    italic = FontSlant /. {opts} /. FontSlant -> "Plain";
    latex = If[bold === "Bold",   "\\mathbf{" <> latex <> "}", latex];
    latex = If[italic === "Italic", "\\mathit{" <> latex <> "}", latex];
    latex = If[col =!= None, "\\textcolor{" <> colorToHex[col] <> "}{" <> latex <> "}", latex];
    latex
  ]
```

#### Colour conversion

```mathematica
colorToHex[RGBColor[r_, g_, b_]] :=
  "#" <> StringJoin[IntegerString[Round[255 #], 16, 2] & /@ {r, g, b}]

colorToHex[RGBColor[r_, g_, b_, _]] := colorToHex[RGBColor[r, g, b]]

(* Named colours *)
colorToHex[Red]   = "#ff0000";
colorToHex[Blue]  = "#0000ff";
colorToHex[Green] = "#008000";
(* ... extend as needed *)
```

#### Semantic / interpretation boxes

| Box | Behaviour |
|-----|-----------|
| `TagBox[expr_, _]` | Recurse into `expr`, ignore tag |
| `InterpretationBox[display_, _]` | Recurse into `display` |
| `FormBox[expr_, _]` | Recurse into `expr` |
| `TemplateBox[args_, tag_]` | Look up tag in template registry; fall back to joining args |

#### Fallback

Any unrecognised box head: recurse over all arguments and concatenate. Log a warning.

```mathematica
boxToLatex[head_[args___]] :=
  (Message[boxToLatex::unknown, head];
   StringJoin[boxToLatex /@ {args}])
```

---

### Special character table

A lookup association mapping Wolfram named characters to LaTeX commands. Key entries:

```mathematica
$WLtoLaTeX = <|
  "\[Alpha]"   -> "\\alpha",
  "\[Beta]"    -> "\\beta",
  "\[Gamma]"   -> "\\gamma",
  "\[Delta]"   -> "\\delta",
  "\[Epsilon]" -> "\\epsilon",
  "\[Zeta]"    -> "\\zeta",
  "\[Eta]"     -> "\\eta",
  "\[Theta]"   -> "\\theta",
  "\[Iota]"    -> "\\iota",
  "\[Kappa]"   -> "\\kappa",
  "\[Lambda]"  -> "\\lambda",
  "\[Mu]"      -> "\\mu",
  "\[Nu]"      -> "\\nu",
  "\[Xi]"      -> "\\xi",
  "\[Pi]"      -> "\\pi",
  "\[Rho]"     -> "\\rho",
  "\[Sigma]"   -> "\\sigma",
  "\[Tau]"     -> "\\tau",
  "\[Upsilon]" -> "\\upsilon",
  "\[Phi]"     -> "\\phi",
  "\[Chi]"     -> "\\chi",
  "\[Psi]"     -> "\\psi",
  "\[Omega]"   -> "\\omega",
  (* Uppercase *)
  "\[CapitalGamma]"  -> "\\Gamma",
  "\[CapitalDelta]"  -> "\\Delta",
  "\[CapitalTheta]"  -> "\\Theta",
  "\[CapitalLambda]" -> "\\Lambda",
  "\[CapitalXi]"     -> "\\Xi",
  "\[CapitalPi]"     -> "\\Pi",
  "\[CapitalSigma]"  -> "\\Sigma",
  "\[CapitalUpsilon]" -> "\\Upsilon",
  "\[CapitalPhi]"    -> "\\Phi",
  "\[CapitalPsi]"    -> "\\Psi",
  "\[CapitalOmega]"  -> "\\Omega",
  (* Operators / symbols *)
  "\[Infinity]"      -> "\\infty",
  "\[PlusMinus]"     -> "\\pm",
  "\[Times]"         -> "\\times",
  "\[Divide]"        -> "\\div",
  "\[NotEqual]"      -> "\\neq",
  "\[LessEqual]"     -> "\\leq",
  "\[GreaterEqual]"  -> "\\geq",
  "\[Element]"       -> "\\in",
  "\[NotElement]"    -> "\\notin",
  "\[Subset]"        -> "\\subset",
  "\[Superset]"      -> "\\supset",
  "\[Union]"         -> "\\cup",
  "\[Intersection]"  -> "\\cap",
  "\[ForAll]"        -> "\\forall",
  "\[Exists]"        -> "\\exists",
  "\[Partial]"       -> "\\partial",
  "\[Nabla]"         -> "\\nabla",
  "\[Sum]"           -> "\\sum",
  "\[Product]"       -> "\\prod",
  "\[Integral]"      -> "\\int",
  "\[Square]"        -> "\\square",
  "\[LeftArrow]"     -> "\\leftarrow",
  "\[RightArrow]"    -> "\\rightarrow",
  "\[LeftRightArrow]" -> "\\leftrightarrow",
  "\[Rule]"          -> "\\rightarrow",
  "\[InvisibleTimes]" -> "",          (* suppress *)
  "\[InvisibleSpace]" -> "",          (* suppress *)
  "\[ThinSpace]"     -> "\\,",
  "\[MediumSpace]"   -> "\\:",
  "\[ThickSpace]"    -> "\\;",
  "\[NonBreakingSpace]" -> "~"
|>
```

---

### Matrix / GridBox handling

`GridBox` is used for matrices, piecewise functions, and general tables. Detect context:

```mathematica
gridBoxToLatex[GridBox[rows_, opts___]] :=
  Module[{env, body},
    env = detectGridEnvironment[rows, opts];
    body = StringRiffle[
      StringRiffle[boxToLatex /@ #, " & "] & /@ rows,
      " \\\\ "
    ];
    "\\begin{" <> env <> "}" <> body <> "\\end{" <> env <> "}"
  ]

detectGridEnvironment[rows_, opts___] :=
  Which[
    (* bracketing from enclosing RowBox *)
    True, "pmatrix"  (* default; caller can override *)
  ]
```

Bracket type (pmatrix, bmatrix, vmatrix, etc.) is inferred from the surrounding `RowBox` delimiters.

---

## Component 2: TypeScript rendering layer

The rendering layer lives in **two separate files** depending on the mode.

---

### Mode A — Webview-side renderer

**File:** `webview/renderer.ts`

KaTeX is bundled in the Webview (do not load from CDN — VSCode CSP blocks it).

```typescript
import katex from 'katex';
import 'katex/dist/katex.min.css';

export function renderLatexOutput(
  container: HTMLElement,
  latex: string,
  displayMode: boolean = true
): void {
  try {
    katex.render(latex, container, {
      displayMode,
      throwOnError: false,
      errorColor: '#cc0000',
      trust: false,
      strict: false,     // required for \textcolor
      macros: {
        "\\dd": "\\mathrm{d}",
      }
    });
  } catch (err) {
    container.textContent = latex;
    container.classList.add('render-error');
  }
}
```

---

### Mode B — Extension host pre-renderer

**File:** `src/katexPrerender.ts`

This runs in the extension host (Node.js), not in the Webview. KaTeX is imported as a plain Node.js module — no DOM required.

```typescript
import katex from 'katex';

const KATEX_OPTIONS: katex.KatexOptions = {
  displayMode: true,
  throwOnError: false,
  errorColor: '#cc0000',
  trust: false,
  strict: false,
  macros: {
    "\\dd": "\\mathrm{d}",
  }
};

export function prerenderLatex(
  latex: string,
  displayMode: boolean = true
): string {
  try {
    return katex.renderToString(latex, { ...KATEX_OPTIONS, displayMode });
  } catch (err) {
    // Return a styled error span — Webview just does innerHTML, so this is safe
    return `<span class="render-error" style="color:#cc0000;font-family:monospace">${escapeHtml(latex)}</span>`;
  }
}

function escapeHtml(s: string): string {
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}
```

The returned HTML string is a fully self-contained fragment. KaTeX inlines all font glyph data as SVG `<path>` elements, so no external font files need to be loadable from the Webview.

---

### Output mode dispatcher

**File:** `src/outputRenderer.ts`

This is the single point where the mode decision is made.

```typescript
import { prerenderLatex } from './katexPrerender';

export type OutputMode = 'latex' | 'prerendered';

export interface RenderConfig {
  mode: OutputMode;
  /** Auto-switch to prerendered if latex string exceeds this length. Default: 2000 chars. */
  prerenderedThreshold: number;
}

const DEFAULT_CONFIG: RenderConfig = {
  mode: 'prerendered',       // default: pre-render in extension host
  prerenderedThreshold: 2000
};

export function prepareOutput(
  latex: string,
  displayMode: boolean,
  config: RenderConfig = DEFAULT_CONFIG
): { mode: OutputMode; payload: string } {
  const effectiveMode =
    config.mode === 'prerendered' || latex.length > config.prerenderedThreshold
      ? 'prerendered'
      : 'latex';

  return {
    mode: effectiveMode,
    payload: effectiveMode === 'prerendered'
      ? prerenderLatex(latex, displayMode)
      : latex
  };
}
```

---

### Webview message handler

The Webview handles both message types with a single handler:

```typescript
window.addEventListener('message', (event) => {
  const msg = event.data;
  if (msg.type !== 'output') return;

  const container = document.getElementById(msg.cellId);
  if (!container) return;

  if (msg.mode === 'prerendered') {
    container.innerHTML = msg.payload;          // Mode B: just inject HTML
  } else {
    renderLatexOutput(container, msg.payload, msg.displayMode);  // Mode A: KaTeX in Webview
  }
});
```

### KaTeX CSS in pre-render mode

In Mode B the Webview still needs the **KaTeX CSS** loaded in its `<head>` — without it the pre-rendered HTML will be unstyled. The CSS file is served as a Webview resource URI:

```typescript
// In the Webview HTML template (extension host side)
const katexCssUri = webview.asWebviewUri(
  vscode.Uri.joinPath(context.extensionUri, 'node_modules', 'katex', 'dist', 'katex.min.css')
);
// Inject: <link rel="stylesheet" href="${katexCssUri}">
```

Note: the JS bundle (`katex.min.js`) is only needed in the Webview for Mode A. In Mode B it can be omitted entirely, reducing Webview payload.

### Colour support

KaTeX supports `\textcolor{#rrggbb}{expr}` natively in both modes when `strict: false`. No additional configuration needed.

---

## Component 3: WSTP communication layer

### Kernel → extension host message

The Mathematica kernel always sends a **LaTeX string** — it is not aware of output modes. The `mode` and `payload` fields are filled in by the extension host after calling `prepareOutput`.

Kernel output JSON (extension host receives this):

```json
{
  "type": "output",
  "cellId": "uuid",
  "latex": "\\frac{1}{2} \\alpha^{2}",
  "displayMode": true,
  "rawBoxes": "FractionBox[\"1\",\"2\"]"
}
```

The WL side:

```mathematica
outputMessage[boxes_, cellId_String] :=
  ExportString[<|
    "type"        -> "output",
    "cellId"      -> cellId,
    "latex"       -> boxToLatex[boxes],
    "displayMode" -> True
  |>, "JSON"]
```

### Extension host → Webview message

After `prepareOutput`, the extension host sends:

```json
{
  "type": "output",
  "cellId": "uuid",
  "mode": "prerendered",
  "payload": "<span class=\"katex-display\">...</span>",
  "displayMode": true
}
```

or in Mode A:

```json
{
  "type": "output",
  "cellId": "uuid",
  "mode": "latex",
  "payload": "\\frac{1}{2} \\alpha^{2}",
  "displayMode": true
}
```

---

## Edge cases and known issues

| Case | Handling |
|------|----------|
| Very large expressions (QSC outputs, long sums) | KaTeX handles large trees well; no fallback needed unless nesting depth exceeds ~200 |
| `TemplateBox` with unknown tag | Log tag name, render args joined |
| `DynamicBox` | Evaluate the expression first, then render the static result |
| `GraphicsBox` | Do not attempt to render; pass expression to a separate SVG/PNG export pipeline |
| `TagBox[expr, "Null"]` | Suppress (renders as empty) |
| Multi-line expressions (`GridBox` used as layout, not matrix) | Detect via `ColumnAlignments` option; render as `\begin{aligned}...\end{aligned}` |
| Piecewise functions | `GridBox` inside `TagBox[_, "Piecewise"]` → `\begin{cases}...\end{cases}` |

---

## Testing strategy

### Principle

Tests pass **literal box structures** directly to `boxToLatex`, never raw Mathematica expressions. This keeps the unit tests isolated from `ToBoxes` behaviour and format choices — the renderer is not responsible for how Mathematica decides to typeset an expression, only for correctly translating whatever boxes it receives.

### Unit tests (WL side)

```mathematica
(* Each test: {inputBoxes, expectedLatexString} *)
tests = {

  (* --- Leaf nodes --- *)
  {"x",                                          "x"},
  {"123",                                        "123"},
  {"\[Alpha]",                                   "\\alpha"},
  {"\[Infinity]",                                "\\infty"},

  (* --- Structural boxes --- *)
  {SuperscriptBox["x", "2"],                     "{x}^{2}"},
  {SubscriptBox["x", "i"],                       "{x}_{i}"},
  {SubsuperscriptBox["x", "i", "2"],             "{x}_{i}^{2}"},
  {FractionBox["1", "2"],                        "\\frac{1}{2}"},
  {SqrtBox["x"],                                 "\\sqrt{x}"},
  {RadicalBox["x", "3"],                         "\\sqrt[3]{x}"},
  {UnderscriptBox["x", "y"],                     "\\underset{y}{x}"},
  {OverscriptBox["x", "y"],                      "\\overset{y}{x}"},

  (* --- RowBox --- *)
  {RowBox[{"x", "+", "y"}],                      "x+y"},
  {RowBox[{"(", RowBox[{"x", "+", "1"}], ")"}],  "(x+1)"},

  (* --- Large operators via UnderoverscriptBox --- *)
  {UnderoverscriptBox["\[Sum]",
     RowBox[{"n", "=", "1"}], "\[Infinity]"],    "\\sum_{n=1}^{\\infty}"},
  {UnderoverscriptBox["\[Product]",
     RowBox[{"k", "=", "0"}], "N"],              "\\prod_{k=0}^{N}"},
  {UnderoverscriptBox["\[Integral]", "0", "1"],  "\\int_{0}^{1}"},

  (* Non-operator base: falls back to \underset/\overset *)
  {UnderoverscriptBox["x", "a", "b"],
     "\\underset{a}{\\overset{b}{x}}"},

  (* --- Fractions and nested structures --- *)
  {FractionBox[SuperscriptBox["\[Alpha]", "2"],
               RowBox[{"2", "\[Pi]"}]],          "\\frac{\\alpha^{2}}{2\\pi}"},

  (* --- StyleBox: colour --- *)
  {StyleBox["x", FontColor -> RGBColor[1, 0, 0]],
     "\\textcolor{#ff0000}{x}"},
  {StyleBox["x", FontColor -> RGBColor[0, 0, 1]],
     "\\textcolor{#0000ff}{x}"},

  (* --- StyleBox: weight and slant --- *)
  {StyleBox["x", FontWeight -> "Bold"],          "\\mathbf{x}"},
  {StyleBox["x", FontSlant -> "Italic"],         "\\mathit{x}"},

  (* --- StyleBox: combined --- *)
  {StyleBox["x",
     FontColor -> RGBColor[1, 0, 0],
     FontWeight -> "Bold"],
     "\\mathbf{\\textcolor{#ff0000}{x}}"},

  (* --- TagBox / InterpretationBox: pass-through --- *)
  {TagBox[SuperscriptBox["x", "2"], "anything"], "{x}^{2}"},
  {InterpretationBox[FractionBox["1","2"], Hold[1/2]], "\\frac{1}{2}"},

  (* --- GridBox: matrix --- *)
  {GridBox[{{"1", "0"}, {"0", "1"}}],
     "\\begin{pmatrix}1 & 0\\\\0 & 1\\end{pmatrix}"},
  {GridBox[{{"a", "b"}, {"c", "d"}, {"e", "f"}}],
     "\\begin{pmatrix}a & b\\\\c & d\\\\e & f\\end{pmatrix}"},

  (* --- TagBox["Piecewise"] wrapping GridBox --- *)
  {TagBox[GridBox[{
       {RowBox[{"x", "+", "1"}], RowBox[{"x", ">", "0"}]},
       {"0",                     "True"}}],
     "Piecewise"],
     "\\begin{cases}x+1 & x>0\\\\0 & \\text{True}\\end{cases}"},

  (* --- TagBox["Null"]: suppress --- *)
  {TagBox["", "Null"],                           ""},

  (* --- Greek letters --- *)
  {RowBox[{"\[Alpha]", "+", "\[Beta]"}],         "\\alpha+\\beta"},
  {SubsuperscriptBox["\[Psi]", "n", "2"],        "{\\psi}_{n}^{2}"},
  {SubscriptBox["\[CapitalDelta]", "\[Mu]"],     "{\\Delta}_{\\mu}"}
};

(* Runner *)
Scan[
  Function[{test},
    With[{boxes = test[[1]], expected = test[[2]]},
      VerificationTest[
        boxToLatex[boxes],
        expected,
        TestID -> ToString[boxes, InputForm]
      ]
    ]
  ],
  tests
]
```

### Visual regression (Webview side)

Render a fixed set of LaTeX strings and screenshot-compare against reference images.

---

## Files summary

```
renderer/
  BoxToLatex.wl          ← Main WL translator (always produces LaTeX string)
  SpecialChars.wl        ← $WLtoLaTeX association
  GridBox.wl             ← Matrix/table handling
  Colors.wl              ← colorToHex utilities
  Tests.wl               ← Unit tests (box inputs only)

src/
  katexPrerender.ts      ← Mode B: katex.renderToString() in extension host
  outputRenderer.ts      ← Mode dispatcher + auto-threshold logic

webview/
  renderer.ts            ← Mode A: katex.render() in Webview
  renderer.css           ← Output cell styles (shared by both modes)
```

---

## Dependencies

| Dependency | Version | Used in | Purpose |
|------------|---------|---------|---------|
| `katex` | `^0.16` | Extension host + Webview | Math rendering |
| `katex/dist/katex.min.css` | same | Webview `<head>` | KaTeX styles (required in both modes) |

KaTeX must be **bundled** (not CDN-loaded) due to VSCode Webview CSP restrictions.

In Mode B the `katex.min.js` bundle does not need to be loaded in the Webview, only the CSS. This reduces the Webview's initial JS payload.
