// ci_smoke.js — Quick CI smoke test for the native addon
const addon = require('../build/Release/wolfbook_btl.node');

const cases = [
  ['FractionBox["1","2"]',  '\\frac{1}{2}'],
  ['SqrtBox["x"]',          '\\sqrt{x}'],
  ['SuperscriptBox["x","2"]', 'x^2'],
  ['RowBox[{"x","+","y"}]', 'x+y'],
];

let passed = 0, failed = 0;
for (const [input, expected] of cases) {
  const result = addon.boxToLatex(input);
  if (result.error) {
    console.error(`  FAIL  ${input}  error: ${result.error}`);
    failed++;
  } else if (result.latex === expected) {
    console.log(`  PASS  ${input}`);
    passed++;
  } else {
    console.error(`  FAIL  ${input}  got: ${result.latex}  expected: ${expected}`);
    failed++;
  }
}
console.log(`\n${passed} passed / ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
