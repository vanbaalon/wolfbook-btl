'use strict';
const btl = require('../build/Release/wolfbook_btl.node');

function show(label, box) {
  const {latex, error} = btl.boxToLatex(box);
  console.log(label + ':', error ? 'ERROR: '+error : JSON.stringify(latex));
}

function test(label, box, expected) {
  const {latex, error} = btl.boxToLatex(box);
  const got = error ? ('ERROR: ' + error) : latex;
  const ok = got === expected;
  console.log((ok ? 'PASS' : 'FAIL'), label);
  if (!ok) {
    console.log('  got:    ', JSON.stringify(got));
    console.log('  expect: ', JSON.stringify(expected));
  }
  return ok;
}

let pass = 0, fail = 0;
function t(label, box, expected) {
  if (test(label, box, expected)) pass++; else fail++;
}

// ---- Space handling ----
show('"a b c" (should have visible spaces)', '"a b c"');
show('"abc" (no spaces)',                     '"abc"');

t('space-separated text in \\text{}',
  '"a b c"',
  '\\text{a b c}');

t('no-space multi-letter in \\mathrm{}',
  '"abc"',
  '\\mathrm{abc}');

t('RowBox with space tokens',
  'RowBox[{"a"," ","b"," ","c"}]',
  'a b c');

// ---- StyleBox "TI" (TraditionalForm Italic) ----
t('StyleBox min TI -> mathit',
  'StyleBox["min", "TI", StripOnInput -> False]',
  '\\mathit{min}');

t('StyleBox max TI -> mathit',
  'StyleBox["max", "TI", StripOnInput -> False]',
  '\\mathit{max}');

t('StyleBox f TI -> mathit (single char)',
  'StyleBox["f", "TI", StripOnInput -> False]',
  '\\mathit{f}');

t('StyleBox reg TI -> mathit',
  'StyleBox["reg", "TI", StripOnInput -> False]',
  '\\mathit{reg}');

// ---- SubscriptBox with TI-styled min/max ----
t('x_{min} with TI style',
  'SubscriptBox[StyleBox["x", "TI"], StyleBox["min", "TI"]]',
  '\\mathit{x}_{\\mathit{min}}');

t('x_{max} with TI style',
  'SubscriptBox[StyleBox["x", "TI"], StyleBox["max", "TI"]]',
  '\\mathit{x}_{\\mathit{max}}');

// ---- Integral guard space ----
t('integral f dx - no merge',
  'RowBox[{"\\[Integral]",RowBox[{"f"," ","d","","x"}]}]',
  '\\intop f dx');

// ---- SubsuperscriptBox with TI-styled subscripts ----
t('definite integral bounds',
  'SubsuperscriptBox["\\[Integral]", SubscriptBox[StyleBox["x", "TI"], StyleBox["min", "TI"]], SubscriptBox[StyleBox["x", "TI"], StyleBox["max", "TI"]]]',
  '\\intop _{\\mathit{x}_{\\mathit{min}}}^{\\mathit{x}_{\\mathit{max}}}');

// ---- Full Information[Integrate] Usage line test (simplified) ----
t('Usage line: Integrate[f,x]',
  'RowBox[{"Integrate", "[", RowBox[{StyleBox["f", "TI"], ",", StyleBox["x", "TI"]}], "]"}]',
  '\\mathrm{Integrate}[\\mathit{f},\\mathit{x}]');

console.log('\n' + pass + ' passed / ' + fail + ' failed');
process.exit(fail > 0 ? 1 : 0);
