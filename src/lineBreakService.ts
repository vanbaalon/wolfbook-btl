/**
 * lineBreakService.ts
 *
 * TypeScript wrapper around the native lineBreakLatex function.
 * Provides an optional iterative-refinement path that measures
 * actual KaTeX render widths and shrinks the em budget until
 * the longest line fits within pageWidthPx.
 */

import * as path from 'path';

// Lazily loaded native addon
let _addon: { lineBreakLatex: (latex: string, opts?: NativeOpts) => string } | null = null;
function getAddon() {
    if (!_addon) {
        // Support both dev and packaged layouts
        const candidates = [
            path.join(__dirname, '../build/Release/wolfbook_btl.node'),
            path.join(__dirname, 'build/Release/wolfbook_btl.node'),
        ];
        for (const p of candidates) {
            try {
                _addon = require(p) as typeof _addon;
                break;
            } catch { /* try next */ }
        }
        if (!_addon) throw new Error('wolfbook_btl.node not found');
    }
    return _addon!;
}

interface NativeOpts {
    pageWidth?: number;
    pageWidthPx?: number;
    baseFontSizePx?: number;
    indentStep?: number;
    compact?: boolean;
    maxDelimDepth?: number;
    maxIterations?: number;
}

export interface LineBreakOpts {
    /** Heuristic em-width target (default: 80) */
    pageWidth?: number;
    /** CSS pixel width target; when > 0 enables iterative measurement */
    pageWidthPx?: number;
    /** Base font size used for px↔em conversion (default: 16) */
    baseFontSizePx?: number;
    /** Continuation line indent in em (default: 2) */
    indentStep?: number;
    /** Prefer fewer lines over aligned breaks */
    compact?: boolean;
    /** Max iterations for the pixel-based refinement loop (default: 5) */
    maxIterations?: number;
}

// ---------------------------------------------------------------------------
// KaTeX measurement helpers
// ---------------------------------------------------------------------------

/**
 * Recursively sum the rendered em-widths of a KaTeX virtual DOM node.
 * Works with the internal tree returned by katex.__renderToDomTree().
 */
function walkWidth(node: unknown): number {
    if (!node) return 0;

    if (Array.isArray(node)) {
        return (node as unknown[]).reduce((acc: number, n) => acc + walkWidth(n), 0);
    }

    const n = node as Record<string, unknown>;

    // Inline element with children
    if (n['children'] !== undefined) {
        return walkWidth(n['children'] as unknown[]);
    }

    // Leaf text node — approximate as char count × 0.55 em
    if (typeof n['text'] === 'string') {
        return (n['text'] as string).length * 0.55;
    }

    return 0;
}

/**
 * Measure the rendered pixel width of a LaTeX expression using KaTeX's
 * internal DOM-tree builder (zero-cost: no actual DOM required).
 *
 * Returns 0 if KaTeX is unavailable or the expression fails to parse.
 */
function measureKaTeX(latex: string, baseFontSizePx: number = 16): number {
    let katex: Record<string, unknown>;
    try {
        katex = require('katex') as Record<string, unknown>;
    } catch {
        return 0;
    }

    const renderToDomTree = katex['__renderToDomTree'] as
        ((latex: string, opts: unknown) => unknown) | undefined;

    if (typeof renderToDomTree !== 'function') return 0;

    try {
        const tree = renderToDomTree(latex, { throwOnError: false, displayMode: false });
        return walkWidth(tree) * baseFontSizePx;
    } catch {
        return 0;
    }
}

// ---------------------------------------------------------------------------
// Line extraction helpers
// ---------------------------------------------------------------------------

/**
 * Split a broken aligned/gathered output into individual line strings.
 * Handles both \begin{aligned} and \begin{gathered} wrappers.
 */
export function extractRenderedLines(broken: string): string[] {
    const inner = broken
        .replace(/^\\begin\{(?:aligned|gathered)\}\n?/, '')
        .replace(/\\end\{(?:aligned|gathered)\}$/, '');

    return inner
        .split('\\\\')
        .map(l => l.trim().replace(/^&\s*/, '').replace(/^\s*\\quad\s*/, ''));
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------

/**
 * Line-break a LaTeX expression, optionally with pixel-based measurement.
 *
 * - If `pageWidthPx` is 0 (or unset): pure heuristic path — one call to
 *   the native addon using `pageWidth` em units.
 * - If `pageWidthPx > 0`: iterative refinement — measures actual KaTeX
 *   render widths and tightens the em budget until the longest line fits.
 *
 * Always breaks from the **original** input; never re-breaks already-broken
 * output to avoid compounding rounding errors.
 */
export async function lineBreakWithMeasurement(
    latex: string,
    opts: LineBreakOpts = {}
): Promise<string> {
    const addon = getAddon();

    const {
        pageWidth      = 80,
        pageWidthPx    = 0,
        baseFontSizePx = 16,
        indentStep     = 2,
        compact        = false,
        maxIterations  = 5,
    } = opts;

    // ── Heuristic-only path ──────────────────────────────────────────────
    if (!pageWidthPx) {
        return addon.lineBreakLatex(latex, {
            pageWidth,
            indentStep,
            compact,
            maxDelimDepth: 2,
        });
    }

    // ── Iterative pixel-based refinement ─────────────────────────────────
    let currentWidthPx = pageWidthPx;
    let result = latex;

    for (let i = 0; i < maxIterations; i++) {
        result = addon.lineBreakLatex(latex, {
            pageWidthPx: currentWidthPx,
            baseFontSizePx,
            indentStep,
            compact,
            maxDelimDepth: 2,
        });

        const lines = extractRenderedLines(result);
        if (lines.length <= 1) break; // nothing was broken — can't improve

        const linePxWidths = lines.map(l => measureKaTeX(l, baseFontSizePx));
        const maxLineWidthPx = Math.max(...linePxWidths);
        if (maxLineWidthPx <= 0) break; // measurement unavailable

        const ratio = maxLineWidthPx / pageWidthPx;
        if (Math.abs(ratio - 1.0) < 0.05) break; // within 5% — good enough

        // Shrink the em budget proportionally with a 5% safety margin
        currentWidthPx = Math.floor(currentWidthPx / ratio * 0.95);
        if (currentWidthPx < 10) break; // sanity guard
    }

    return result;
}

/**
 * Synchronous convenience wrapper (no pixel measurement).
 * Equivalent to lineBreakLatex from the native addon.
 */
export function lineBreakSync(latex: string, opts: LineBreakOpts = {}): string {
    const addon = getAddon();
    const {
        pageWidth      = 80,
        pageWidthPx    = 0,
        baseFontSizePx = 16,
        indentStep     = 2,
        compact        = false,
    } = opts;

    return addon.lineBreakLatex(latex, {
        pageWidth,
        pageWidthPx,
        baseFontSizePx,
        indentStep,
        compact,
        maxDelimDepth: 2,
    });
}
