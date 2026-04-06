#!/usr/bin/env node
// tester/run_tests.js
// Loads test cases from tester/cases.txt and runs them through the BTL addon.
//
// Case file format (cases.txt):
//   # comment lines (ignored)
//   IN: <WL box expression>
//   EX: <expected LaTeX>           ← blank lines between cases are fine
//
// Optional per-case opts (appended after the IN: expression, separated by |):
//   IN: RowBox[{"sin","(","t",")"}] | {trigOmitParens: false}
//
// Usage:
//   node tester/run_tests.js [cases-file]       (default: tester/cases.txt)
//   node tester/run_tests.js --filter=trig       (run only cases whose label contains "trig")

'use strict';

const fs   = require('fs');
const path = require('path');

// ── Load addon ────────────────────────────────────────────────────────────────
const addonPath = path.resolve(__dirname, '../build/Release/wolfbook_btl.node');
let addon;
try {
  addon = require(addonPath);
} catch (e) {
  console.error(`ERROR: could not load addon from ${addonPath}`);
  console.error(e.message);
  process.exit(1);
}

// ── Args ──────────────────────────────────────────────────────────────────────
const args       = process.argv.slice(2);
let   casesFile  = path.resolve(__dirname, 'cases.txt');
let   filterRe   = null;

for (const arg of args) {
  if (arg.startsWith('--filter=')) {
    filterRe = new RegExp(arg.slice('--filter='.length), 'i');
  } else if (!arg.startsWith('--')) {
    casesFile = path.resolve(arg);
  }
}

// ── Parse cases.txt ──────────────────────────────────────────────────────────
function parseCasesFile(filePath) {
  const text   = fs.readFileSync(filePath, 'utf8');
  const lines  = text.split(/\r?\n/);
  const cases  = [];
  let label    = null;
  let input    = null;
  let expected = null;
  let opts     = null;

  function flush() {
    if (input !== null && expected !== null) {
      cases.push({ label: label || `case #${cases.length + 1}`, input, expected, opts });
    }
    label = null; input = null; expected = null; opts = null;
  }

  for (const raw of lines) {
    const line = raw.trimEnd();

    if (line === '' || line === '---') {
      // blank separator — flush current case if complete
      if (input !== null && expected !== null) flush();
      else if (input !== null || expected !== null) {
        // incomplete case; reset silently (probably mid-comment block)
        input = expected = opts = null;
      }
      continue;
    }

    if (line.startsWith('#')) {
      // comment line — also acts as a label for the NEXT case
      const text = line.slice(1).trim();
      if (text) label = text;
      continue;
    }

    if (line.startsWith('IN:')) {
      if (input !== null && expected !== null) flush();
      const rest = line.slice(3).trim();
      const pipeIdx = rest.indexOf(' | ');
      if (pipeIdx !== -1) {
        input = rest.slice(0, pipeIdx).trim();
        opts  = rest.slice(pipeIdx + 3).trim();
      } else {
        input = rest;
        opts  = null;
      }
      continue;
    }

    if (line.startsWith('EX:')) {
      expected = line.slice(3).trim();
      continue;
    }
  }

  // flush last case
  if (input !== null && expected !== null) flush();

  return cases;
}

// ── Run ───────────────────────────────────────────────────────────────────────
const allCases = parseCasesFile(casesFile);
const cases    = filterRe ? allCases.filter(c => filterRe.test(c.label)) : allCases;

if (cases.length === 0) {
  console.error('No test cases found' + (filterRe ? ` matching /${filterRe.source}/` : ''));
  process.exit(1);
}

let passed = 0;
let failed = 0;

for (const { label, input, expected, opts } of cases) {
  let got;
  try {
    const result = opts
      ? addon.boxToLatex(input, JSON.parse(opts))
      : addon.boxToLatex(input);
    if (result && result.error) throw new Error(result.error);
    got = result && result.latex !== undefined ? result.latex : result;
  } catch (e) {
    console.error(`ERRR  [${label}]`);
    console.error(`      IN:  ${input}`);
    console.error(`      ERR: ${e.message}`);
    failed++;
    continue;
  }

  if (got === expected) {
    console.log(`PASS  [${label}]`);
    passed++;
  } else {
    console.error(`FAIL  [${label}]`);
    console.error(`      IN:  ${input}`);
    console.error(`      EX:  ${expected}`);
    console.error(`      GOT: ${got}`);
    failed++;
  }
}

console.log(`\n${passed}/${passed + failed} passed`);
if (failed > 0) process.exit(1);
