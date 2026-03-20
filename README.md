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
  │  BoxTranslator      →  LaTeX string                 │
  │  special_chars      →  O(1) named-char lookup       │
  └─────────────────────────────────────────────────────┘
        │
        │  LaTeX string  e.g. \frac{1}{2}
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
| `src/native/box_to_latex.h/.cpp` | `BoxTranslator` walks the AST by index (no virtual calls). Handles all box heads. `StyleBox` applies colour innermost → bold outermost. Delimiter detection in `RowBox` selects matrix environments. |
| `src/native/addon.cpp` | N-API wrapper. Exports a single JS function: `boxToLatex(str: string): BoxToLatexResult`. Zero DOM dependencies. |

**Performance characteristics:**
- Parse: single-pass, O(n) time, O(n) space in a flat arena
- Special-char lookup: O(1) average hash map
- Translation: one pass over the AST with `std::string::append` into a pre-reserved result buffer
- No allocations after the arena is full; result string is moved out

### 3. TypeScript extension host — `src/`

Runs in the **Node.js extension host process**. No DOM APIs.

| File | Purpose |
|------|---------|
| `src/wolfbook_btl.d.ts` | TypeScript declarations for the C++ addon. |
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
│       ├── addon.cpp            N-API entry point
│       ├── box_to_latex.h/.cpp  AST → LaTeX translator
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
