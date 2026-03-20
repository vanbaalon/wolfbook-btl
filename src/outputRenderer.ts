// =============================================================
// outputRenderer.ts  —  Output mode dispatcher
//
// Pure TypeScript — zero VSCode API imports so this module is
// testable in isolation with plain Node.js / Jest.
//
// Responsibilities:
//   1. Decide whether this output should be pre-rendered (Mode B)
//      or forwarded as a raw LaTeX string (Mode A).
//   2. In Mode B, call prerenderLatex() synchronously.
//   3. Return a { mode, payload } object ready for postMessage.
//
// Decision logic:
//   Pre-render if:
//     config.mode === 'prerendered'          (user preference)
//     OR latex.length > prerenderedThreshold (auto-threshold)
//   Otherwise pass through the LaTeX string.
// =============================================================

import { prerenderLatex } from './katexPrerender';

// ----------------------------------------------------------
// Types
// ----------------------------------------------------------

/** Rendering mode for a single output cell. */
export type OutputMode = 'latex' | 'prerendered';

/**
 * Runtime configuration for the renderer.
 * Intended to be read from the VSCode workspace settings; the
 * defaults here are used when no config is supplied.
 */
export interface RenderConfig {
    /**
     * Base mode preference.
     * - `'prerendered'` (default) — always pre-render in the extension host.
     * - `'latex'`                 — send raw LaTeX to the Webview for Mode A.
     */
    mode: OutputMode;

    /**
     * Auto-upgrade threshold (characters).
     * If the LaTeX string exceeds this length the output is
     * pre-rendered regardless of `mode`.
     * Default: 2000.
     */
    prerenderedThreshold: number;
}

export const DEFAULT_CONFIG: RenderConfig = {
    mode: 'prerendered',
    prerenderedThreshold: 2000,
};

/**
 * The object that the extension host includes in the postMessage
 * payload when forwarding an output to the Webview.
 */
export interface OutputPayload {
    mode: OutputMode;
    /** Either a KaTeX-rendered HTML string (Mode B) or a LaTeX string (Mode A). */
    payload: string;
}

// ----------------------------------------------------------
// prepareOutput
// ----------------------------------------------------------

/**
 * Decide mode and produce the payload for a single output cell.
 *
 * @param latex       LaTeX string from the C++ boxToLatex addon.
 * @param displayMode true for display math, false for inline.
 * @param config      Rendering config; falls back to DEFAULT_CONFIG.
 * @returns { mode, payload } ready to JSON-serialize and postMessage.
 */
export function prepareOutput(
    latex: string,
    displayMode: boolean,
    config: RenderConfig = DEFAULT_CONFIG
): OutputPayload {
    const effectiveMode: OutputMode =
        config.mode === 'prerendered' || latex.length > config.prerenderedThreshold
            ? 'prerendered'
            : 'latex';

    const payload =
        effectiveMode === 'prerendered'
            ? prerenderLatex(latex, displayMode)
            : latex;

    return { mode: effectiveMode, payload };
}

// ----------------------------------------------------------
// Convenience: build the full postMessage object
// (includes fields the Webview uses beyond mode+payload)
// ----------------------------------------------------------
export interface WebviewOutputMessage {
    type: 'output';
    cellId: string;
    mode: OutputMode;
    payload: string;
    displayMode: boolean;
}

export function buildOutputMessage(
    cellId: string,
    latex: string,
    displayMode: boolean,
    config?: RenderConfig
): WebviewOutputMessage {
    const { mode, payload } = prepareOutput(latex, displayMode, config);
    return { type: 'output', cellId, mode, payload, displayMode };
}
