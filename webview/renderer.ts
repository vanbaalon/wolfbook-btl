// =============================================================
// webview/renderer.ts  —  Mode A: Webview-side KaTeX renderer
//
// Runs inside the VSCode Webview (browser-like environment).
// DOM APIs are fine here.
//
// KaTeX is imported as a bundled npm package — NEVER loaded from
// CDN (VSCode Webview CSP blocks external network requests).
//
// Two roles:
//   1. renderLatexOutput() — Mode A: call katex.render() on a
//      LaTeX string and inject the result into a container.
//   2. Window message handler — dispatches incoming output
//      messages to either renderLatexOutput (Mode A) or a direct
//      innerHTML assignment (Mode B / pre-rendered).
// =============================================================

import katex from 'katex';
// The CSS must be imported so the bundler (webpack / esbuild) includes
// it in the Webview bundle.
import 'katex/dist/katex.min.css';

// ----------------------------------------------------------
// KaTeX options for Mode A (Webview-side rendering)
// Must match the options used in katexPrerender.ts so that
// Mode A and Mode B produce visually identical output.
// ----------------------------------------------------------
const KATEX_OPTIONS: katex.KatexOptions = {
    throwOnError: false,
    errorColor: '#cc0000',
    trust: false,
    strict: false,    // required for \textcolor
    maxExpand: 5000,   // default is 1000; increase for complex Mathematica output
    macros: {
        '\\dd': '\\mathrm{d}',
        '\\R':  '\\mathbb{R}',
        '\\C':  '\\mathbb{C}',
        '\\N':  '\\mathbb{N}',
    },
};

// ----------------------------------------------------------
// renderLatexOutput  (Mode A entry point)
//   container  — the HTMLElement to render into
//   latex      — LaTeX string from the extension host
//   displayMode — true = display math (centred block)
// ----------------------------------------------------------
export function renderLatexOutput(
    container: HTMLElement,
    latex: string,
    displayMode: boolean = true
): void {
    try {
        container.classList.remove('wolfbook-render-error');
        katex.render(latex, container, { ...KATEX_OPTIONS, displayMode });
    } catch (err: unknown) {
        // Degrade gracefully: show the raw LaTeX with an error marker
        const msg = err instanceof Error ? err.message : String(err);
        container.textContent = latex;
        container.title = msg;
        container.classList.add('wolfbook-render-error');
    }
}

// ----------------------------------------------------------
// WebviewOutputMessage — mirrors the type in outputRenderer.ts
// (duplicated here to keep the Webview bundle self-contained)
// ----------------------------------------------------------
interface WebviewOutputMessage {
    type: 'output';
    cellId: string;
    mode: 'latex' | 'prerendered';
    payload: string;
    displayMode: boolean;
}

// ----------------------------------------------------------
// handleOutputMessage  —  process a single decoded message
// Exported for testing; also called by the window listener.
// ----------------------------------------------------------
export function handleOutputMessage(msg: WebviewOutputMessage): void {
    // Validate message shape
    if (msg.type !== 'output') return;
    if (typeof msg.cellId !== 'string') return;

    const container = document.getElementById(msg.cellId);
    if (!container) {
        console.warn(`[wolfbook] output cell "${msg.cellId}" not found in DOM`);
        return;
    }

    if (msg.mode === 'prerendered') {
        // Mode B: pre-rendered HTML from extension host — just inject it.
        // KaTeX CSS is already loaded in <head>; no JS render needed.
        container.innerHTML = msg.payload;
    } else {
        // Mode A: render the LaTeX string here in the Webview
        renderLatexOutput(container, msg.payload, msg.displayMode ?? true);
    }
}

// ----------------------------------------------------------
// Window-level message listener
// Call installMessageHandler() once on Webview startup.
// ----------------------------------------------------------
export function installMessageHandler(): void {
    window.addEventListener('message', (event: MessageEvent) => {
        // VSCode posts messages whose `data` is the payload object
        const msg = event.data as WebviewOutputMessage;
        try {
            handleOutputMessage(msg);
        } catch (err) {
            console.error('[wolfbook] message handler error:', err);
        }
    });
}

// ----------------------------------------------------------
// Auto-install when this module is the Webview entry point.
// The bundler typically sets a global flag; fall back to always
// installing when loaded.
// ----------------------------------------------------------
installMessageHandler();
