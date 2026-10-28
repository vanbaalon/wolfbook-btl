"use strict";
const {test}=require('node:test');
const assert=require('node:assert/strict');
const addon=require('../build/Release/wolfbook_btl.node');
const katex=require('katex');
const render=latex=>katex.renderToString(latex,{throwOnError:true,strict:'error'});
test('narrow product breaks keep zeta and powered log attached to their arguments',()=>{
 const factor=String.raw`\zeta \left(15\right)\,\log^{2} \left(x\right)`;
 const source=Array(6).fill(factor).join(String.raw`\,`);
 for(const pageWidth of [6,12,18,24]){
  const {result}=addon.lineBreakLatex(source,{pageWidth});
  assert.match(result,/begin\{aligned\}/);
  assert.equal(result.split(String.raw`\zeta \left(15\right)`).length-1,6);
  assert.equal(result.split(String.raw`\log^{2} \left(x\right)`).length-1,6);
  render(result);
 }
});
test('uniform numeric Column row spacing survives translation and pagination',()=>{
 for(const spacing of [0,0.5,3]){
  const box=`GridBox[{{"a"},{"b"},{"c"},{"d"}},GridBoxSpacings->{"Rows"->{{${spacing}}}}]`;
  const {pages,error}=addon.boxToLatex(box,{maxRows:2,allPages:true});
  assert.equal(error,null);assert.equal(pages.length,2);
  for(const page of pages){assert.ok(page.includes(`\\\\[${spacing}em]`));render(page);}
 }
});
test('ShowContents False preserves layout with a styled phantom',()=>{
 const {latex,error}=addon.boxToLatex('StyleBox[FractionBox["long","x"],ShowContents->False,FontWeight->Bold]');
 assert.equal(error,null);assert.match(latex,/^\\phantom\{/);assert.ok(latex.includes(String.raw`\frac{`));
 const html=render(latex);assert.match(html,/<mphantom>/);assert.match(html,/color:transparent/);
});
