'use strict';
// Demonstrates the root cause of the missing matrix and verifies the fix.
// Run:  node tester/_debug_print_form.js
const A = require('../build/Release/wolfbook_btl.node');

function t(label, box) {
  const r = A.boxToLatex(box);
  const out = r.latex || '(empty)';
  console.log('\n' + label);
  console.log('  latex:', out.length > 120 ? out.slice(0, 120) + '…' : out);
  if (r.error) console.log('  error:', r.error);
}

// ── From btl.log: the exact btl input (OutputForm atoms inside GridBox) ──
// "(", "⁠", ")" are BARE (unquoted) → parse error → skipArg eats the GridBox
t('FAILING (OutputForm atoms — from btl.log)',
  'TagBox[RowBox[{(, \u2060, GridBox[{' +
  '{RowBox[{-, 1}], 0, 0, 0},' +
  '{0, SuperscriptBox[RowBox[{a, [, t, ]}], 2], 0, 0},' +
  '{0, 0, RowBox[{SuperscriptBox[r, 2],  , SuperscriptBox[RowBox[{a, [, t, ]}], 2]}], 0},' +
  '{0, 0, 0, RowBox[{SuperscriptBox[r, 2],  , SuperscriptBox[RowBox[{a, [, t, ]}], 2],  , SuperscriptBox[RowBox[{Sin, [, \u03b8, ]}], 2]}]}' +
  '}, RowSpacings -> 1, ColumnSpacings -> 1, RowAlignments -> Baseline, ColumnAlignments -> Center], \u2060, )}],' +
  'Function[BoxForm`e$, BoxForm`e$]]');

// ── The same expression with properly-quoted atoms (InputForm) ──
// This is what ToString[boxes, InputForm] would produce in Mathematica
t('WORKING (InputForm atoms — what the kernel should send)',
  'TagBox[RowBox[{"(","\\[NoBreak]",GridBox[{' +
  '{RowBox[{"-","1"}],"0","0","0"},' +
  '{"0",SuperscriptBox[RowBox[{"a","[","t","]"}],"2"],"0","0"},' +
  '{"0","0",RowBox[{SuperscriptBox["r","2"]," ",SuperscriptBox[RowBox[{"a","[","t","]"}],"2"]}],"0"},' +
  '{"0","0","0",RowBox[{SuperscriptBox["r","2"]," ",SuperscriptBox[RowBox[{"a","[","t","]"}],"2"]," ",SuperscriptBox[RowBox[{"Sin","[","\\[Theta]","]"}],"2"]}]}' +
  '},RowSpacings->1,ColumnSpacings->1,RowAlignments->Baseline,ColumnAlignments->Center],"\\[NoBreak]",")"}],' +
  'Function[BoxForm`e$,BoxForm`e$]]');

// ── Full FRW expression (as close to btl.log as possible, InputForm atoms) ──
t('FULL FRW metric (InputForm atoms)',
  'StyleBox[TemplateBox[{"FRW",":   ",' +
  'TemplateBox[{"\\\\eta",TemplateBox[{"\\[Mu]","\\[Nu]"},RowDefault],TemplateBox[{StyleBox["\\[Mu]",ShowContents->False,StripOnInput->False],StyleBox["\\[Nu]",ShowContents->False,StripOnInput->False]},RowDefault]},Subsuperscript,SyntaxForm->SubsuperscriptBox],' +
  '"(",TemplateBox[{",",",","t","r","\\[Theta]","\\[Phi]"},RowWithSeparators],")"," = ",' +
  'TagBox[RowBox[{"(","\\[NoBreak]",GridBox[{' +
  '{RowBox[{"-","1"}],"0","0","0"},' +
  '{"0",SuperscriptBox[RowBox[{"a","[","t","]"}],"2"],"0","0"},' +
  '{"0","0",RowBox[{SuperscriptBox["r","2"]," ",SuperscriptBox[RowBox[{"a","[","t","]"}],"2"]}],"0"},' +
  '{"0","0","0",RowBox[{SuperscriptBox["r","2"]," ",SuperscriptBox[RowBox[{"a","[","t","]"}],"2"]," ",SuperscriptBox[RowBox[{"Sin","[","\\[Theta]","]"}],"2"]}]}' +
  '},RowSpacings->1,ColumnSpacings->1,RowAlignments->Baseline,ColumnAlignments->Center],"\\[NoBreak]",")"}],' +
  'Function[BoxForm`e$,BoxForm`e$]]' +
  '},RowDefault],DisplayFormula,StripOnInput->False]');
