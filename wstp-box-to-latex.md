# WSTP Box-to-LaTeX: C++ Parsing Reference

## Key Documentation

| Topic | URL |
|---|---|
| Reading/writing expressions | https://reference.wolfram.com/language/tutorial/ManipulatingExpressionsInExternalPrograms.html |
| `WSGetType` + token constants | https://reference.wolfram.com/language/ref/c/WSGetType.html |
| C API function index | https://reference.wolfram.com/language/guide/WSTPCLanguageFunctions.html |
| Exchange functions (`WSGetFunction`, `WSGetArgCount`) | https://reference.wolfram.com/language/guide/WSTPCFunctionsForExchangingExpressions.html |
| String exchange (`WSGetUTF8String`, `WSReleaseString`) | https://reference.wolfram.com/language/guide/WSTPCFunctionsForExchangingStrings.html |
| Packet protocol | https://reference.wolfram.com/language/guide/WSTPPackets.html |
| LLU C++ wrapper (`WSStream`, `<<`/`>>`) | https://wolframresearch.github.io/LibraryLinkUtilities/modules/wstp.html |

---

## Architecture

```
Kernel ──WSTP──► readExpr()  ──►  Expr tree  ──►  boxToLatex()  ──►  LaTeX string
                 (type-dispatch)   (vector nodes)  (recursive visitor)
```

Ask the kernel for boxes, not strings:
```mathematica
MakeBoxes[your_expr, TraditionalForm]
```
The result arrives wrapped in `ReturnPacket[...]` — peel it with `WSNextPacket` / `WSTestHead`, then call `readExpr` on the payload.

---

## Token Dispatch Loop

WSTP serialises every expression as a depth-first pre-order tree. The token type tells you what to read next:

```cpp
Expr readExpr(WSLINK lp) {
    switch (WSGetType(lp)) {

    case WSTKFUNC: {          // compound: f[a, b, ...]
        int argc;
        WSGetArgCount(lp, &argc);
        Expr head = readExpr(lp);      // head is itself an expression
        Expr node{Compound, head.atom};
        for (int i = 0; i < argc; i++)
            node.args.push_back(readExpr(lp));
        return node;
    }
    case WSTKSYM: {
        const char* s; WSGetSymbol(lp, &s);
        Expr n{Symbol, s}; WSReleaseSymbol(lp, s); return n;
    }
    case WSTKSTR: {
        const char* s; WSGetUTF8String(lp, &s, nullptr, nullptr);
        Expr n{String, s}; WSReleaseUTF8String(lp, s); return n;
    }
    case WSTKINT: {
        int i; WSGetInteger(lp, &i);
        return {Integer, std::to_string(i)};
    }
    case WSTKREAL: {
        double d; WSGetReal(lp, &d);
        return {Real, std::to_string(d)};
    }
    }
}
```

> **String encoding:** use `WSGetUTF8String` / `WSReleaseUTF8String` so that `\[Alpha]` arrives as `U+03B1` rather than Wolfram's internal encoding.

---

## Box-to-LaTeX Visitor

```cpp
std::string boxToLatex(const Expr& e) {
    if (e.kind != Compound) return translateAtom(e.atom);

    if (e.head == "RowBox")
        // args[0] is a List — concatenate children
        return joinChildren(e.args[0].args);

    if (e.head == "FractionBox")
        return "\\frac{" + boxToLatex(e.args[0]) + "}{" + boxToLatex(e.args[1]) + "}";

    if (e.head == "SqrtBox")
        return "\\sqrt{" + boxToLatex(e.args[0]) + "}";

    if (e.head == "SuperscriptBox")
        return "{" + boxToLatex(e.args[0]) + "}^{" + boxToLatex(e.args[1]) + "}";

    if (e.head == "SubscriptBox")
        return "{" + boxToLatex(e.args[0]) + "}_{" + boxToLatex(e.args[1]) + "}";

    if (e.head == "StyleBox")   return translateStyleBox(e);
    if (e.head == "GridBox")    return translateGridBox(e);

    return fallback(e);
}
```

---

## Handling Options (StyleBox, GridBox)

Options arrive as extra `Rule[lhs, rhs]` arguments after the main content. Iterate from index 1:

```cpp
std::string translateStyleBox(const Expr& e) {
    std::string inner = boxToLatex(e.args[0]);
    for (size_t i = 1; i < e.args.size(); i++) {
        if (e.args[i].head != "Rule") continue;
        const auto& key = e.args[i].args[0].atom;
        const auto& val = e.args[i].args[1];
        if (key == "FontColor")
            inner = "\\textcolor{" + rgbToHex(val) + "}{" + inner + "}";
        else if (key == "FontWeight" && val.atom == "Bold")
            inner = "\\mathbf{" + inner + "}";
        else if (key == "FontSlant" && val.atom == "Italic")
            inner = "\\mathit{" + inner + "}";
    }
    return inner;
}
```

`RGBColor[r, g, b]` arrives as `Compound{"RGBColor", {Real r, Real g, Real b}}` — convert each channel to a two-digit hex byte.

---

## Atom Translation

Build a `std::unordered_map<std::string, std::string>` keyed on UTF-8 codepoints:

```cpp
static const std::unordered_map<std::string, std::string> WL_TO_LATEX = {
    {"\xce\xb1",           "\\alpha"},
    {"\xce\x93",           "\\Gamma"},
    {"\xe2\x88\xab",       "\\int"},
    {"\xf0\x9d\x91\x91",   "\\,\\mathrm{d}"},  // DifferentialD
    {"\xe2\x88\x9e",       "\\infty"},
    // ...
};
```

Strip numeric precision marks before emitting numbers:

```cpp
std::string cleanNumeric(const std::string& s) {
    auto pos = s.find('`');
    return pos == std::string::npos ? s : s.substr(0, pos);
}
// "3.1415`" → "3.1415"
```

---

## GridBox Environment Detection

`GridBox` bracket context is encoded in the surrounding `RowBox`:

| Surrounding `RowBox` delimiters | Environment |
|---|---|
| `(` … `)` | `pmatrix` |
| `[` … `]` | `bmatrix` |
| `{` … `}` | `Bmatrix` |
| No brackets | `matrix` |
| `TagBox[…,"Piecewise"]` ancestor | `cases` |
| `ColumnAlignments` option present | `aligned` |

Match on the parent node when dispatching, not inside `translateGridBox` itself, so the delimiters are **absorbed** and not emitted twice.

---

## Common Pitfalls

| Symptom | Cause | Fix |
|---|---|---|
| `Style[2,Red]` emitted literally | Non-font options bypass `StyleBox` conversion in kernel | Detect `RowBox[{"Style","[",…,"]"}]` fallback pattern |
| Double brackets around matrix | `GridBox` emits environment *and* parent `RowBox` emits `(` `)` | Absorb delimiters in parent dispatch |
| `\[DifferentialD]` literal | Wrong string API; Wolfram encoding ≠ UTF-8 | Use `WSGetUTF8String`; add DifferentialD entry to map |
| `3.1415\`` in output | `InputForm` precision annotation leaks through | Strip from backtick in `cleanNumeric` |
