'use strict';
const A = require('../build/Release/wolfbook_btl.node');

function t(label, box) {
  const r = A.boxToLatex(box);
  console.log(label + ':', JSON.stringify(r.latex), r.error ? '  ERR:'+r.error : '');
}

// 1. SubsuperscriptBox with empty superscript
t('subsup empty sup',
  'SubsuperscriptBox["\\[Eta]","\\[Mu]",""]');

// 2. The FRW label part (no matrix)
t('FRW label',
  'RowBox[{"FRW",":   ",SubsuperscriptBox["\\[Eta]",RowBox[{"\\[Mu]","\\[Nu]"}],""],"(","r",")"," = "}]');

// 3. The matrix alone (string parens + NoBreak)
t('matrix alone',
  'TagBox[RowBox[{"(","\\[NoBreak]",GridBox[{{"a","b"},{"c","d"}}],"\\[NoBreak]",")"}],Function[BoxForm`e$,BoxForm`e$]]');

// 4. Full expression (label + matrix in outer RowBox)
t('full (outer RowBox)',
  'RowBox[{"FRW",":   ",SubsuperscriptBox["\\[Eta]",RowBox[{"\\[Mu]","\\[Nu]"}],""],"(","r",")"," = ",TagBox[RowBox[{"(","\\[NoBreak]",GridBox[{{"a","b"},{"c","d"}}],"\\[NoBreak]",")"}],Function[BoxForm`e$,BoxForm`e$]]}]');

// 5. With FormBox wrapper
t('FormBox outer',
  'FormBox[RowBox[{"FRW",":   ",SubsuperscriptBox["\\[Eta]",RowBox[{"\\[Mu]","\\[Nu]"}],""],"(","r",")"," = ",TagBox[RowBox[{"(","\\[NoBreak]",GridBox[{{"a","b"},{"c","d"}}],"\\[NoBreak]",")"}],Function[BoxForm`e$,BoxForm`e$]]}],StandardForm]');

// 6. With StyleBox wrappers as OGRe might produce
t('StyleBox label',
  'RowBox[{StyleBox["FRW",Bold],":   ",SubsuperscriptBox[StyleBox["\\[Eta]",Italic],RowBox[{"\\[Mu]","\\[Nu]"}],""],"(","r",")"," = ",TagBox[RowBox[{"(","\\[NoBreak]",GridBox[{{"a","b"},{"c","d"}}],"\\[NoBreak]",")"}],Function[BoxForm`e$,BoxForm`e$]]}]');

// 7. What if the matrix is in an AdjustmentBox?
t('AdjustmentBox wrapper',
  'AdjustmentBox[TagBox[RowBox[{"(","\\[NoBreak]",GridBox[{{"a","b"},{"c","d"}}],"\\[NoBreak]",")"}],Function[BoxForm`e$,BoxForm`e$]],BoxMargins->{{0,0},{0,0}}]');

// 8. What if there's a TooltipBox or ActionMenuBox over the matrix?
t('TooltipBox wrapper',
  'TooltipBox[TagBox[RowBox[{"(","\\[NoBreak]",GridBox[{{"a","b"},{"c","d"}}],"\\[NoBreak]",")"}],Function[BoxForm`e$,BoxForm`e$]],"tooltip"]');
