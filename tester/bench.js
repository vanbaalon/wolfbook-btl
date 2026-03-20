#!/usr/bin/env node
// ================================================================
// bench.js  --  wolfbook_btl native addon throughput benchmark
// Run with:  node tester/bench.js
// ================================================================
'use strict';

const { performance } = require('perf_hooks');
const btl = require('../build/Release/wolfbook_btl.node');

// ----------------------------------------------------------------
// Test cases: box strings at various expression sizes
// ----------------------------------------------------------------
const CASES = [
  {
    label: 'small_symbol',
    box: '"\\[Alpha]"',
  },
  {
    label: 'small_fraction',
    box: 'FractionBox["1","2"]',
  },
  {
    label: 'medium_sum',
    box: 'RowBox[{UnderoverscriptBox["\\[Sum]",RowBox[{"n","=","1"}],"\\[Infinity]"],FractionBox["1",SuperscriptBox["n","2"]]}]',
  },
  {
    label: 'medium_integral',
    box: 'RowBox[{SubsuperscriptBox["\\[Integral]",RowBox[{"-","\\[Infinity]"}],"\\[Infinity]"],SuperscriptBox["\\[ExponentialE]",RowBox[{"-",SuperscriptBox["x","2"]}]],"\\[DifferentialD]","x"}]',
  },
  {
    label: 'large_set',
    // {E^(π u) u, E^(π u), E^(-π u) u², E^(-π u) u}  from examples.csv
    box: 'RowBox[{"{",' +
         'RowBox[{' +
           'RowBox[{SuperscriptBox["E",RowBox[{"\\[Pi]"," ","u"}]]," ","u"}],' +
           '",",' +
           'SuperscriptBox["E",RowBox[{"\\[Pi]"," ","u"}]],' +
           '",",' +
           'RowBox[{SuperscriptBox["E",RowBox[{RowBox[{"-","\\[Pi]"}]," ","u"}]]," ",SuperscriptBox["u","2"]}],' +
           '",",' +
           'RowBox[{SuperscriptBox["E",RowBox[{RowBox[{"-","\\[Pi]"}]," ","u"}]]," ","u"}]' +
         '}],' +
         '"}"}]',
  },
  {
    label: 'large_poly',
    // Σ_{k=0}^{8} c_k x^k  (manually expanded (1+x)^8 coefficients)
    box: (() => {
      const terms = [
        'SuperscriptBox["x","8"]',
        'RowBox[{"8",SuperscriptBox["x","7"]}]',
        'RowBox[{"28",SuperscriptBox["x","6"]}]',
        'RowBox[{"56",SuperscriptBox["x","5"]}]',
        'RowBox[{"70",SuperscriptBox["x","4"]}]',
        'RowBox[{"56",SuperscriptBox["x","3"]}]',
        'RowBox[{"28",SuperscriptBox["x","2"]}]',
        'RowBox[{"8","x"}]',
        '"1"',
      ];
      const items = terms.flatMap((t, i) => (i < terms.length - 1 ? [t, '"+"'] : [t]));
      return `RowBox[{${items.join(',')}}]`;
    })(),
  },
  {
    label: 'large_matrix',
    // 4x4 binomial matrix  C(n,k) for n,k in 0..3
    box: (() => {
      const rows = [];
      for (let n = 0; n <= 3; n++) {
        const cells = [];
        for (let k = 0; k <= 3; k++) {
          const bn = [[1],[1,1],[1,2,1],[1,3,3,1]];
          const val = k <= n ? bn[n][k] : 0;
          cells.push(`"${val}"`);
        }
        rows.push(`{${cells.join(',')}}`);
      }
      return `GridBox[{${rows.join(',')}}]`;
    })(),
  },
];

// ----------------------------------------------------------------
// Benchmark runner
// ----------------------------------------------------------------
const WARMUP = 5000;          // iterations to discard
const MIN_ITER = 10000;       // minimum timed iterations
const MIN_MS   = 500;         // min wall-clock ms per case

function bench(box) {
  // warmup
  for (let i = 0; i < WARMUP; i++) btl.boxToLatex(box);

  // timed loop: auto-extend until MIN_MS elapsed
  let iter = 0;
  let totalNs = 0;
  do {
    const t0 = performance.now();
    for (let i = 0; i < MIN_ITER; i++) btl.boxToLatex(box);
    totalNs += (performance.now() - t0) * 1e6;
    iter += MIN_ITER;
  } while (totalNs / 1e6 < MIN_MS);

  const meanUs = totalNs / iter / 1000;
  const callsPerSec = Math.round(1e9 / (totalNs / iter));
  return { meanUs, callsPerSec };
}

// ----------------------------------------------------------------
// Main
// ----------------------------------------------------------------
const PAD_LABEL = 20;
const PAD_US    = 11;
const PAD_CPS   = 12;

console.log('=== wolfbook_btl native addon benchmark ===\n');
console.log(`Warmup: ${WARMUP.toLocaleString()} iterations, then sample for ≥${MIN_MS} ms\n`);

console.log(
  'case'.padEnd(PAD_LABEL) +
  'mean (us)'.padStart(PAD_US) +
  'calls/s'.padStart(PAD_CPS) +
  '  tex-output'
);
console.log('-'.repeat(76));

const results = [];
for (const { label, box } of CASES) {
  const { latex } = btl.boxToLatex(box);
  const preview = (latex || '').length > 35
    ? (latex || '').slice(0, 35) + '...'
    : (latex || '');

  const { meanUs, callsPerSec } = bench(box);

  console.log(
    label.padEnd(PAD_LABEL) +
    meanUs.toFixed(3).padStart(PAD_US) + ' us' +
    String(callsPerSec).padStart(PAD_CPS - 2) + ' /s' +
    '  ' + preview
  );
  results.push({ label, meanUs, callsPerSec });
}

console.log('\nDone.');
