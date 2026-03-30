'use strict';
const btl = require('../build/Release/wolfbook_btl.node');
const tests = [
  ['InterpretationBox[SuperscriptBox["x","2"], x^2]',            'x^2'],
  ['InterpretationBox[RowBox[{"1","+","2"}], 3]',                '1+2'],
  ['InterpretationBox[FractionBox["1","3"], 1/3]',               '\\frac{1}{3}'],
  ['InterpretationBox[SuperscriptBox["e",RowBox[{"i","\\[Pi]"}]], E^(I*Pi)]', 'e^{i\\pi }'],
  // normal boxes must still work after the recovery path
  ['FractionBox["1","2"]',                                       '\\frac{1}{2}'],
  ['SuperscriptBox["x","n"]',                                    'x^n'],
];

let pass = 0, fail = 0;
for (const [box, expected] of tests) {
  const {latex, error} = btl.boxToLatex(box);
  if (error) {
    console.log('FAIL (error)', box.slice(0,50), '->', error);
    fail++;
  } else if (latex !== expected) {
    console.log('FAIL (mismatch)', box.slice(0,50));
    console.log('  got:    ', JSON.stringify(latex));
    console.log('  expect: ', JSON.stringify(expected));
    fail++;
  } else {
    console.log('PASS', box.slice(0,50));
    pass++;
  }
}
console.log(`\n${pass} passed / ${fail} failed`);
