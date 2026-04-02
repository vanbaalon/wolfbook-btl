#!/usr/bin/env node
// btl_width_sweep.js
// Sweep lineBreakLatex pageWidth to find the C++ em-estimate threshold for test expressions.
// Usage: node btl_width_sweep.js

const path = require('path');

// Load the prebuilt (newest) addon
const addonPath = path.resolve(
    process.env.HOME, '.vscode/extensions/wolfbook.wolfbook-2.5.11/wllatex-addon/prebuilt/wolfbook_btl-darwin-arm64.node'
);
const { lineBreakLatex, boxToLatex } = require(addonPath);

// ── Test expressions ─────────────────────────────────────────────────────────
const expressions = [
    {
        name: 'Sum n=20  (short)',
        latex: '\\frac{x^{20}}{x^{20}+20}+\\frac{x^{19}}{x^{19}+19}+\\frac{x^{18}}{x^{18}+18}+\\frac{x^{17}}{x^{17}+17}+\\frac{x^{16}}{x^{16}+16}+\\frac{x^{15}}{x^{15}+15}+\\frac{x^{14}}{x^{14}+14}+\\frac{x^{13}}{x^{13}+13}+\\frac{x^{12}}{x^{12}+12}+\\frac{x^{11}}{x^{11}+11}+\\frac{x^{10}}{x^{10}+10}+\\frac{x^9}{x^9+9}+\\frac{x^8}{x^8+8}+\\frac{x^7}{x^7+7}+\\frac{x^6}{x^6+6}+\\frac{x^5}{x^5+5}+\\frac{x^4}{x^4+4}+\\frac{x^3}{x^3+3}+\\frac{x^2}{x^2+2}+\\frac{x}{x+1}',
    },
    {
        name: 'Sum n=100 (long)',
        latex: '\\frac{x^{100}}{x^{100}+100}+\\frac{x^{99}}{x^{99}+99}+\\frac{x^{98}}{x^{98}+98}+\\frac{x^{97}}{x^{97}+97}+\\frac{x^{96}}{x^{96}+96}+\\frac{x^{95}}{x^{95}+95}+\\frac{x^{94}}{x^{94}+94}+\\frac{x^{93}}{x^{93}+93}+\\frac{x^{92}}{x^{92}+92}+\\frac{x^{91}}{x^{91}+91}+\\frac{x^{90}}{x^{90}+90}+\\frac{x^{89}}{x^{89}+89}+\\frac{x^{88}}{x^{88}+88}+\\frac{x^{87}}{x^{87}+87}+\\frac{x^{86}}{x^{86}+86}+\\frac{x^{85}}{x^{85}+85}+\\frac{x^{84}}{x^{84}+84}+\\frac{x^{83}}{x^{83}+83}+\\frac{x^{82}}{x^{82}+82}+\\frac{x^{81}}{x^{81}+81}+\\frac{x^{20}}{x^{20}+20}+\\frac{x^{19}}{x^{19}+19}+\\frac{x^{18}}{x^{18}+18}+\\frac{x^{17}}{x^{17}+17}+\\frac{x^{16}}{x^{16}+16}+\\frac{x^{15}}{x^{15}+15}+\\frac{x^{14}}{x^{14}+14}+\\frac{x^{13}}{x^{13}+13}+\\frac{x^{12}}{x^{12}+12}+\\frac{x^{11}}{x^{11}+11}+\\frac{x^{10}}{x^{10}+10}+\\frac{x^9}{x^9+9}+\\frac{x^8}{x^8+8}+\\frac{x^7}{x^7+7}+\\frac{x^6}{x^6+6}+\\frac{x^5}{x^5+5}+\\frac{x^4}{x^4+4}+\\frac{x^3}{x^3+3}+\\frac{x^2}{x^2+2}+\\frac{x}{x+1}',
    },
    {
        name: 'Simple: x+1',
        latex: 'x+1',
    },
    {
        name: 'Fraction cluster (5 terms)',
        latex: '\\frac{x^5}{x^5+5}+\\frac{x^4}{x^4+4}+\\frac{x^3}{x^3+3}+\\frac{x^2}{x^2+2}+\\frac{x}{x+1}',
    },
];

// ── Sweep function ────────────────────────────────────────────────────────────
// Returns the minimum integer pageWidth at which lineBreakLatex returns the original (no break).
function findThreshold(latex) {
    // First confirm it breaks at a small value
    const test1 = lineBreakLatex(latex, { pageWidth: 1 });
    if (test1 === latex) {
        return null; // expression is too short to ever break
    }

    // Binary search between 1 and 2000
    let lo = 1, hi = 2000;
    while (lo < hi) {
        const mid = Math.floor((lo + hi) / 2);
        const result = lineBreakLatex(latex, { pageWidth: mid });
        if (result === latex) {
            hi = mid; // fits, try smaller
        } else {
            lo = mid + 1; // breaks, try larger
        }
    }
    return lo; // minimum pageWidth that does NOT break
}

// ── Run ───────────────────────────────────────────────────────────────────────
console.log('BTL lineBreakLatex width threshold sweep');
console.log('=========================================\n');

for (const expr of expressions) {
    const threshold = findThreshold(expr.latex);
    if (threshold === null) {
        console.log(`${expr.name}`);
        console.log(`  → Expression too short to ever break (fits at pageWidth=1)`);
    } else {
        console.log(`${expr.name}`);
        console.log(`  → C++ em threshold: ${threshold} em  (breaks at ${threshold-1}, fits at ${threshold})`);

        // Show what the break looks like at threshold-5
        const sampleWidth = Math.max(1, threshold - 5);
        const broken = lineBreakLatex(expr.latex, { pageWidth: sampleWidth });
        const lineCount = (broken.match(/\\\\/g) || []).length + 1;
        console.log(`  → At pageWidth=${sampleWidth}: ${lineCount} line(s) in output`);
    }

    // Also show break at a few specific widths
    for (const w of [20, 30, 40, 50, 60, 80, 100]) {
        const result = lineBreakLatex(expr.latex, { pageWidth: w });
        const broke = result !== expr.latex;
        const lines = broke ? (result.match(/\\\\/g) || []).length + 1 : 1;
        console.log(`  pageWidth=${String(w).padStart(3)}: ${broke ? `BREAK → ${lines} lines` : 'no change (fits)'}`);
    }
    console.log();
}
