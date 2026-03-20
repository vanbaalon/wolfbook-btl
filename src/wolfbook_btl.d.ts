// =============================================================
// wolfbook_btl.d.ts  —  type declaration for the C++ native addon
// Generated at build time by node-gyp; this file documents the
// public surface so TypeScript consumers get proper types.
// =============================================================

/** Result returned by `boxToLatex`. */
export interface BoxToLatexResult {
  /**
   * The translated LaTeX string.
   * On success this is the rendered LaTeX (e.g. `\\frac{1}{2}`).
   * On error this is the raw WL box string passed in (verbatim pass-through),
   * so the caller can still display something meaningful.
   */
  latex: string;

  /**
   * Diagnostic message if translation failed, `null` on success.
   *
   * When non-null the `latex` field contains the verbatim input rather
   * than valid LaTeX.  Callers should log or surface this message and
   * optionally fall back to plain-text display.
   *
   * Typical causes:
   *  - WL InputForm parse error (unexpected syntax)
   *  - Unsupported box head
   *  - Input is not a string
   */
  error: string | null;
}

/**
 * Translate a Wolfram Language box-expression string
 * (as produced by `ToString[ToBoxes[expr, TraditionalForm], InputForm]`)
 * to a LaTeX string suitable for KaTeX.
 *
 * Implemented in C++ (src/native/box_to_latex.cpp).
 * **Never throws** — on any parse or translation error it returns a result
 * object with `error` set to a diagnostic string and `latex` set to the
 * raw input for display fall-back.
 *
 * @example
 * ```ts
 * const { latex, error } = btl.boxToLatex('FractionBox["1","2"]');
 * if (error) console.warn('[wolfbook] boxToLatex error:', error);
 * katex.render(latex, container);
 * ```
 *
 * @param wlBoxString  InputForm box string, e.g. `FractionBox["1","2"]`
 * @returns `{ latex, error }` — `error` is `null` on success
 */
export declare function boxToLatex(wlBoxString: string): BoxToLatexResult;
