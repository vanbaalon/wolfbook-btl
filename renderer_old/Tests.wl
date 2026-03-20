(* =========================================================
   Tests.wl  —  Wolfbook box→LaTeX unit tests
   
   Tests for the WL reference implementation (BoxToLatex.wl).
   All inputs are literal box structures — never raw Mathematica
   expressions.  This keeps the tests isolated from ToBoxes
   behaviour and format choices.

   Run with:  wolframscript -file renderer/Tests.wl
   Or from Mathematica:  Get["renderer/Tests.wl"]
   ========================================================= *)

(* ---- Load the reference implementation ---- *)
Get[FileNameJoin[{DirectoryName[$InputFileName], "SpecialChars.wl"}]]
Get[FileNameJoin[{DirectoryName[$InputFileName], "Colors.wl"}]]
Get[FileNameJoin[{DirectoryName[$InputFileName], "BoxToLatex.wl"}]]

(* ============================================================
   Test suite — each entry: {inputBoxes, expectedLatexString}
   ============================================================ *)
$tests = {

  (* --------------------------------------------------------
     Leaf nodes — plain strings
     -------------------------------------------------------- *)
  {"x",       "x",         "single lowercase letter"},
  {"123",     "123",        "integer literal"},
  {"3.14",    "3.14",       "real literal"},
  {"+",       "+",          "plus operator char"},
  {"-",       "-",          "minus operator char"},
  {"=",       "=",          "equals operator char"},

  (* --------------------------------------------------------
     Named WL characters
     -------------------------------------------------------- *)
  {"\[Alpha]",     "\\alpha",  "lowercase alpha"},
  {"\[Beta]",      "\\beta",   "lowercase beta"},
  {"\[Pi]",        "\\pi",     "lowercase pi"},
  {"\[Omega]",     "\\omega",  "lowercase omega"},
  {"\[Infinity]",  "\\infty",  "infinity symbol"},
  {"\[Sum]",       "\\sum",    "summation symbol"},
  {"\[Integral]",  "\\int",    "integral symbol"},
  {"\[CapitalDelta]", "\\Delta", "uppercase Delta"},
  {"\[CapitalSigma]", "\\Sigma", "uppercase Sigma"},
  {"\[CapitalPsi]",   "\\Psi",   "uppercase Psi"},
  {"\[Element]",   "\\in",    "element-of"},
  {"\[Times]",     "\\times", "times"},

  (* --------------------------------------------------------
     RowBox — concatenation
     -------------------------------------------------------- *)  
  {RowBox[{"x", "+", "y"}],                     "x+y",      "simple row"},
  {RowBox[{"(", RowBox[{"x", "+", "1"}], ")"}], "(x+1)",    "parenthesised row"},
  {RowBox[{"\[Alpha]", "+", "\[Beta]"}],         "\\alpha+\\beta", "greek row"},

  (* --------------------------------------------------------
     SuperscriptBox
     -------------------------------------------------------- *)
  {SuperscriptBox["x", "2"],                     "{x}^{2}",  "x squared"},
  {SuperscriptBox["e", RowBox[{"-", "x"}]],      "{e}^{-x}", "e to the minus x"},
  {SuperscriptBox["\[Alpha]", "2"],              "{\\alpha}^{2}", "alpha squared"},

  (* --------------------------------------------------------
     SubscriptBox
     -------------------------------------------------------- *)
  {SubscriptBox["x", "i"],                       "{x}_{i}",  "x sub i"},
  {SubscriptBox["\[Psi]", "n"],                  "{\\psi}_{n}", "psi sub n"},
  {SubscriptBox["\[CapitalDelta]", "\[Mu]"],     "{\\Delta}_{\\mu}", "Delta sub mu"},

  (* --------------------------------------------------------
     SubsuperscriptBox
     -------------------------------------------------------- *)
  {SubsuperscriptBox["x", "i", "2"],             "{x}_{i}^{2}",      "x sub i sup 2"},
  {SubsuperscriptBox["\[Psi]", "n", "2"],        "{\\psi}_{n}^{2}",  "psi sub n sup 2"},

  (* --------------------------------------------------------
     FractionBox
     -------------------------------------------------------- *)
  {FractionBox["1", "2"],                        "\\frac{1}{2}",  "one half"},
  {FractionBox["a", "b"],                        "\\frac{a}{b}",  "a over b"},
  {FractionBox[
     SuperscriptBox["\[Alpha]", "2"],
     RowBox[{"2", "\[Pi]"}]],
   "\\frac{\\alpha^{2}}{2\\pi}",                                  "alpha^2 over 2pi"},

  (* --------------------------------------------------------
     SqrtBox / RadicalBox
     -------------------------------------------------------- *)
  {SqrtBox["x"],                                 "\\sqrt{x}",      "square root x"},
  {SqrtBox[RowBox[{"a", "+", "b"}]],             "\\sqrt{a+b}",    "sqrt a+b"},
  {RadicalBox["x", "3"],                         "\\sqrt[3]{x}",   "cube root"},
  {RadicalBox["x", "n"],                         "\\sqrt[n]{x}",   "nth root"},

  (* --------------------------------------------------------
     UnderscriptBox / OverscriptBox (non-operator base)
     -------------------------------------------------------- *)
  {UnderscriptBox["x", "y"],                     "\\underset{y}{x}", "underset x y"},
  {OverscriptBox["x", "y"],                      "\\overset{y}{x}",  "overset x y"},

  (* --------------------------------------------------------
     UnderoverscriptBox — large operators (display-mode limits)
     -------------------------------------------------------- *)
  {UnderoverscriptBox["\[Sum]",
     RowBox[{"n", "=", "1"}], "\[Infinity]"],
   "\\sum_{n=1}^{\\infty}",                                        "sum n=1 to inf"},

  {UnderoverscriptBox["\[Product]",
     RowBox[{"k", "=", "0"}], "N"],
   "\\prod_{k=0}^{N}",                                             "product k=0 to N"},

  {UnderoverscriptBox["\[Integral]", "0", "1"],
   "\\int_{0}^{1}",                                                "integral 0 to 1"},

  (* Non-operator base falls back to underset/overset nesting *)
  {UnderoverscriptBox["x", "a", "b"],
   "\\underset{a}{\\overset{b}{x}}",                               "underset/overset fallback"},

  (* --------------------------------------------------------
     StyleBox — colour
     -------------------------------------------------------- *)
  {StyleBox["x", FontColor -> RGBColor[1, 0, 0]],
   "\\textcolor{#ff0000}{x}",                                      "red x"},
  {StyleBox["x", FontColor -> RGBColor[0, 0, 1]],
   "\\textcolor{#0000ff}{x}",                                      "blue x"},
  {StyleBox["x", FontColor -> RGBColor[0, 0.502, 0]],
   "\\textcolor{#008000}{x}",                                      "green x (web)"},

  (* StyleBox — weight / slant *)
  {StyleBox["x", FontWeight -> "Bold"],          "\\mathbf{x}",    "bold x"},
  {StyleBox["x", FontSlant -> "Italic"],         "\\mathit{x}",    "italic x"},

  (* StyleBox — combined: bold+colour *)
  {StyleBox["x", FontColor -> RGBColor[1, 0, 0], FontWeight -> "Bold"],
   "\\mathbf{\\textcolor{#ff0000}{x}}",                            "bold red x"},

  (* --------------------------------------------------------
     TagBox / InterpretationBox — pass-through
     -------------------------------------------------------- *)
  {TagBox[SuperscriptBox["x", "2"], "anything"],  "{x}^{2}",       "TagBox passthrough"},
  {TagBox["", "Null"],                            "",               "TagBox Null suppressed"},
  {InterpretationBox[FractionBox["1", "2"], Hold[1/2]], "\\frac{1}{2}", "InterpretationBox"},

  (* --------------------------------------------------------
     GridBox — matrix environments
     -------------------------------------------------------- *)
  (* 2×2 identity *)
  {GridBox[{{"1", "0"}, {"0", "1"}}],
   "\\begin{pmatrix}1 & 0\\\\0 & 1\\end{pmatrix}",                 "2x2 identity"},

  (* 3×3 grid *)
  {GridBox[{{"a", "b", "c"}, {"d", "e", "f"}, {"g", "h", "i"}}],
   "\\begin{pmatrix}a & b & c\\\\d & e & f\\\\g & h & i\\end{pmatrix}", "3x3 matrix"},

  (* 2×3 rectangle *)
  {GridBox[{{"a", "b"}, {"c", "d"}, {"e", "f"}}],
   "\\begin{pmatrix}a & b\\\\c & d\\\\e & f\\end{pmatrix}",         "2x3 matrix"},

  (* Bracket-delimited bmatrix via surrounding RowBox *)
  {RowBox[{"[", GridBox[{{"1", "0"}, {"0", "1"}}], "]"}],
   "\\begin{bmatrix}1 & 0\\\\0 & 1\\end{bmatrix}",                  "bmatrix via []"},

  (* vmatrix via | | *)
  {RowBox[{"|", GridBox[{{"a", "b"}, {"c", "d"}}], "|"}],
   "\\begin{vmatrix}a & b\\\\c & d\\end{vmatrix}",                  "vmatrix"},

  (* --------------------------------------------------------
     Piecewise — TagBox["Piecewise"] wrapping GridBox
     -------------------------------------------------------- *)
  {TagBox[
     GridBox[{
       {RowBox[{"x", "+", "1"}], RowBox[{"x", ">", "0"}]},
       {"0",                     "True"}}],
     "Piecewise"],
   "\\begin{cases}x+1 & x>0\\\\0 & \\text{True}\\end{cases}",      "piecewise"},

  (* --------------------------------------------------------
     Greek letter combinations
     -------------------------------------------------------- *)
  {RowBox[{"\[Alpha]", "+", "\[Beta]", "+", "\[Gamma]"}],
   "\\alpha+\\beta+\\gamma",                                        "alpha+beta+gamma"},

  {FractionBox["\[Pi]", "2"],                    "\\frac{\\pi}{2}", "pi over 2"},

  (* Multiple Greek in subscript *)
  {SubscriptBox["\[CapitalDelta]", "\[Mu]"],
   "{\\Delta}_{\\mu}",                                              "Delta sub mu"}
};

(* ============================================================
   Test runner
   ============================================================ *)
$passed = 0;
$failed = 0;

Scan[
  Function[{entry},
    With[{boxes = entry[[1]], expected = entry[[2]], label = entry[[3]]},
      VerificationTest[
        boxToLatex[boxes],
        expected,
        TestID -> label
      ];
      (* Immediate feedback *)
      With[{result = boxToLatex[boxes]},
        If[result === expected,
          (Print["  PASS  ", label]; $passed++),
          (Print["  FAIL  ", label,
                 "\n      got:      ", result,
                 "\n      expected: ", expected];
           $failed++)
        ]
      ]
    ]
  ],
  $tests
]

Print["\n", $passed, " passed / ", $failed, " failed  (", Length[$tests], " total)"]

If[$failed > 0, Exit[1], Exit[0]]
