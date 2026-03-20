#!/usr/bin/env wolframscript
(* ================================================================
   bench.wls  --  TeXForm throughput benchmark
   Run with:  wolframscript -file tester/bench.wls
   ================================================================ *)

Print["=== Wolfram TeXForm Benchmark ==="];
Print["Mathematica version: ", $Version];

(* ----------------------------------------------------------------
   Test expressions: pre-evaluate first so we time formatting only,
   not symbolic computation.  Each entry is {label, held-for-texform}.
   ---------------------------------------------------------------- *)
$cases = {
  (* label,  pre-evaluated expression *)
  {"small_symbol",    \[Alpha]},
  {"small_fraction",  5/6},
  {"medium_sum",      Pi^2/6},              (* = Sum[1/n^2,{n,1,Inf}] *)
  {"medium_integral", Sqrt[Pi]},            (* = Integrate[Exp[-x^2],{x,-Inf,Inf}] *)
  {"large_poly",      Expand[(1+x+x^2+x^3)^4]},
  {"large_nested",    {Exp[\[Pi] u] u, Exp[\[Pi] u], Exp[-\[Pi] u] u^2, Exp[-\[Pi] u] u}},
  {"large_matrix",    Table[Binomial[n, k], {n, 0, 5}, {k, 0, 5}]}
};

(* ----------------------------------------------------------------
   Warm up: force JIT, GC, etc.  Run each expr 30 times.
   ---------------------------------------------------------------- *)
Print["\nWarming up kernel (30 passes each)..."];
Do[
  {$skip, expr} = item;
  Do[ToString[TeXForm[expr]], {30}],
  {item, $cases}];
Print["Kernel warm.\n"];

(* ----------------------------------------------------------------
   Benchmark: RepeatedTiming returns {mean_seconds, last_result}
   minTime -> at least this many seconds of sampling
   ---------------------------------------------------------------- *)
minSec = 1.0;  (* sample each case for at least 1 second *)

Print[StringPadRight["case",            20],
      StringPadLeft["mean (us)", 11],
      StringPadLeft["calls/s",   12],
      "  tex-output"];
Print[StringRepeat["-", 72]];

$results = {};
Do[
  {label, expr} = item;
  {meanSec, texStr} = RepeatedTiming[
    ToString[TeXForm[expr]],
    minSec];
  meanUs = meanSec * 1.*^6;
  callsPerSec = Round[1.0 / meanSec];
  preview = If[StringLength[texStr] > 35,
               StringTake[texStr, 35] <> "...",
               texStr];
  Print[StringPadRight[label, 20],
        StringPadLeft[ToString[Round[meanUs, 0.01]], 11], " us",
        StringPadLeft[ToString[callsPerSec], 10], " /s",
        "  ", preview];
  AppendTo[$results, <|"case" -> label, "us" -> meanUs, "cps" -> callsPerSec|>],
  {item, $cases}];

Print["\nDone."];
