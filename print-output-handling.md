# Handling Mathematica `Print` Output — Developer Guide

## Status

| Component | Status |
|-----------|--------|
| **btl addon** | ✅ Fixed — handles raw Unicode chars + invisible chars |
| **Extension (kernel side)** | ✅ Fixed — uses `ToString[…, InputForm]` |
| **Extension (escaping bug)** | ❌ **Still broken** — double-escaping backslashes |

---

## What btl Fixed (this commit)

1. **Raw Unicode → LaTeX** — Added 50+ direct Unicode character mappings to
   `special_chars.cpp`.  Characters like `θ`, `μ`, `η`, `ϕ` now translate to
   `\theta`, `\mu`, `\eta`, `\varphi` etc. even when they appear as raw UTF-8
   instead of `\[Name]` escapes.

2. **Invisible Unicode chars** — `U+2060` (Word Joiner), `U+200B`
   (Zero-Width Space), `U+2061–U+2063` (Function Application, Invisible Times,
   Invisible Separator) and `U+00A0` (NBSP) are now in the lookup table.
   They are suppressed in output and correctly recognized as "empty" in the
   RowBox matrix-detection algorithm, so `RowBox[{"(", "⁠", GridBox[…], "⁠", ")"}]`
   now produces `\begin{pmatrix}…\end{pmatrix}` instead of failing.

---

## The Remaining Extension-Side Bug: Double-Escaped Backslashes

### Symptom

The btl input logged in `btl copy.log` shows strings like:

```
"\\"FRW\\"" "\\":   \\"" "\\"\\\\\\\\eta\\""
```

These should be:

```
"\"FRW\"" "\":   \"" "\"\\[Eta]\""
```

Every `\"` (InputForm escaped quote) has been turned into `\\"`, and every
`\\` has been doubled to `\\\\` — creating a cascade of misparses.

### Impact

| Original InputForm | After double-escaping | Parser sees |
|---|---|---|
| `"\"FRW\""` | `"\\"FRW\\""`  | string `\`, bare symbol `FRW`, string `\`, empty string |
| `"\"\\[Eta]\""` | `"\\"\\\\[Eta]\\""`  | string `\`, then `\\[Eta]` garbles |
| `"\",\""` | `"\\",\\""`  | string `\`, comma, string `\`, empty string |

This corrupts any string that contains embedded double-quotes (which is
nearly all text strings in InputForm) — labels, RowWithSeparators delimiters,
Subsuperscript arguments, etc.

### Root Cause

The extension is performing an extra escaping pass on the `ToString[…, InputForm]`
result string. Somewhere in the pipeline, all `\` characters are being doubled
to `\\`. This may be:

- An explicit `.replace(/\\/g, '\\\\')` call
- Double JSON-encoding (JSON.stringify applied twice)
- A string interpolation inside a template literal or regex that adds a layer

### The Fix

The string returned by `ToString[boxes, InputForm]` on the Mathematica kernel
side is already in the exact format that the btl C++ parser expects. It must be
passed through **verbatim** — no additional escaping, no JSON encoding beyond
what the transport layer requires.

**Check for these common mistakes:**

```typescript
// ❌ WRONG — double-escapes all backslashes
const btlInput = inputFormStr.replace(/\\/g, '\\\\');

// ❌ WRONG — double JSON encoding
const payload = JSON.stringify(JSON.stringify(inputFormStr));

// ❌ WRONG — escaping for string interpolation
const btlInput = inputFormStr.replace(/"/g, '\\"');

// ✅ CORRECT — pass the string directly
const { latex, error } = btl.boxToLatex(inputFormStr);
```

**Debug verification:** After the fix, logging the btl input should show
`"\"FRW\""` (single backslash before each quote), not `"\\"FRW\\""`.

### Quick Diagnostic

Check the logged btl input for these patterns:

| Pattern in log | Means | Correct form |
|---|---|---|
| `\\"` before/after a word | double-escaped quote | `\"` |
| `\\\\[Name]` | double-escaped named char | `\\[Name]` |
| `\\\\\\\\` (4+ backslashes) | cascading double-escape | `\\` |

If any of these appear, the escaping bug is still present.

---

## Expected Data Flow

```
Mathematica kernel
    │
    │  ToString[boxes, InputForm]
    │  → produces:  TemplateBox[{"\"FRW\"", ...}, "RowDefault"]
    │                            ─────────
    │                            \" = embedded quote (correct)
    ▼
Extension host (JavaScript string)
    │
    │  The string arrives via WSTP / JSON.
    │  After JSON.parse(), it should contain
    │  exactly the text that ToString produced.
    │
    │  DO NOT escape, encode, or transform it further.
    │
    ▼
btl.boxToLatex(inputFormString)   ← pass verbatim
    │
    ▼
LaTeX string: \text{FRW}\text{:   }\eta_{\mu\nu}^{}...
```

---

## Test Results (after btl fixes, simulating correct escaping)

```
Test 5 — FRW metric (proper InputForm):
  \text{FRW}\text{:   }\eta_{\mu\nu}^{}\text{(}t,r,\theta,\phi
  \text{)}\text{ = }\begin{pmatrix}-1 & 0 & 0 & 0\\0 & a[t]^2 & ...
  \end{pmatrix}

Test 6 — FRW metric (ACTUAL btl input from log, double-escaped):
  \text{FRW}\text{:   }\text{\\η}_{\mu\nu}^{}\text{(}t,r,\theta,\varphi
  \text{)}\text{ = }\begin{pmatrix}-1 & 0 & 0 & 0\\0 & a[t]^2 & ...
  \end{pmatrix}
```

Note: Test 6 (the actual log input) now renders the matrix correctly thanks to
the U+2060 and Unicode fixes, but the label shows `\text{\\η}` instead of
`\eta` due to the double-escaped backslash.  Once the extension-side escaping
is fixed, test 5's output will be produced.

```
Test 7 — OGRe header (proper InputForm):
  \begin{matrix}\mathbf{\text{OGRe: An }\underline{\text{O}}\text{bject-Oriented }
  \underline{\text{G}}\text{eneral }\underline{\text{Re}}
  \text{lativity Package for Mathematica}}\\...
  \end{matrix}
```

---

## Summary of All Fixes

| Fix | Where | Status |
|-----|-------|--------|
| `ToString[boxes, InputForm]` for Print | Kernel / extension | ✅ Done |
| Raw Unicode Greek → LaTeX (`θ`→`\theta`, etc.) | btl `special_chars.cpp` | ✅ Done |
| U+2060 Word Joiner → invisible | btl `special_chars.cpp` | ✅ Done |
| U+200B, U+2061–U+2063 → invisible | btl `special_chars.cpp` | ✅ Done |
| Matrix detection with U+2060 separators | btl (via table lookup) | ✅ Done |
| DynamicBox suppression | btl (already existed) | ✅ Already done |
| **Stop double-escaping backslashes** | **Extension host** | **❌ TODO** |
