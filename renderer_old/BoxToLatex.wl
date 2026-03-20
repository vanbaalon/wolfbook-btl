(* =========================================================
   BoxToLatex.wl  —  Wolfbook Mathematica → LaTeX renderer
   Main translator: box tree → LaTeX string.

   Exported symbol: boxToLatex

   Usage:
     Get["renderer/SpecialChars.wl"]
     Get["renderer/Colors.wl"]
     Get["renderer/BoxToLatex.wl"]   (* loads GridBox.wl internally *)
     boxToLatex[FractionBox["1","2"]]   (* => "\\frac{1}{2}" *)

   Architecture note:
     This WL file is the reference / fallback implementation.
     The primary fast path in the extension host is the C++ native
     addon (src/native/).  Both must produce identical output.

   StyleBox option ordering:
     Colour is the innermost wrap, bold/italic are outermost.
     This matches the unit tests:
       StyleBox["x", FontColor->Red, FontWeight->"Bold"]
       => \\mathbf{\\textcolor{#ff0000}{x}}
   ========================================================= *)

BeginPackage["Wolfbook`BoxToLatex`",
  {"Wolfbook`SpecialChars`", "Wolfbook`Colors`"}]

boxToLatex::usage = "boxToLatex[boxes] converts a Mathematica box expression \
(as produced by ToBoxes[expr, TraditionalForm]) to a LaTeX string suitable \
for KaTeX rendering."

boxToLatex::unknown         = "boxToLatex: unrecognised box head `1`; recursing."
boxToLatex::unknownTemplate = "boxToLatex: unknown TemplateBox tag \"`1`\"; joining args."
boxToLatex::dynamic         = "boxToLatex: DynamicBox suppressed."
boxToLatex::graphics        = "boxToLatex: GraphicsBox → [graphics]."

Begin["`Private`"]

(* ============================================================
   1. LARGE-OPERATOR DETECTION
   ============================================================ *)
$largeOperatorLatex = {
  "\\sum", "\\prod", "\\int", "\\oint",
  "\\bigcup", "\\bigcap", "\\bigoplus", "\\bigotimes",
  "\\bigodot", "\\bigsqcup", "\\coprod", "\\biguplus"
}

isLargeOperator[l_String] := MemberQ[$largeOperatorLatex, l]
isLargeOperator[_]        := False

(* ============================================================
   2. LEAF STRING CLASSIFICATION
   ============================================================ *)
classifyString[s_String] :=
  Which[
    KeyExistsQ[$WLtoLaTeX, s],
      $WLtoLaTeX[s],
    StringMatchQ[s, (DigitCharacter | ".")..],
      s,
    StringLength[s] === 1,
      s,
    StringMatchQ[s, LetterCharacter..],
      "\\mathrm{" <> s <> "}",
    True,
      s
  ]

(* ============================================================
   3. LEAF: PLAIN STRING
   ============================================================ *)
boxToLatex[s_String] := classifyString[s]

(* ============================================================
   4. STRUCTURAL BOXES
   ============================================================ *)

(* ---- RowBox ---- *)
(* Special case: {open, GridBox[…], close} with known delimiters → matrix *)
boxToLatex[RowBox[{open_String, g : GridBox[__], close_String}]] :=
  Module[{env = delimiterToGridEnv[open, close]},
    If[env =!= None,
      gridBoxToLatex[g, env],
      StringJoin[boxToLatex /@ {open, g, close}]
    ]
  ]

boxToLatex[RowBox[children_List]] :=
  StringJoin[boxToLatex /@ children]

(* ---- Superscript / Subscript / SubSuperscript ---- *)
boxToLatex[SuperscriptBox[b_, e_]] :=
  "{" <> boxToLatex[b] <> "}^{" <> boxToLatex[e] <> "}"

boxToLatex[SubscriptBox[b_, s_]] :=
  "{" <> boxToLatex[b] <> "}_{" <> boxToLatex[s] <> "}"

boxToLatex[SubsuperscriptBox[b_, s_, e_]] :=
  "{" <> boxToLatex[b] <> "}_{" <> boxToLatex[s] <> "}^{" <> boxToLatex[e] <> "}"

(* ---- Fraction / Sqrt / Radical ---- *)
boxToLatex[FractionBox[n_, d_]] :=
  "\\frac{" <> boxToLatex[n] <> "}{" <> boxToLatex[d] <> "}"

boxToLatex[SqrtBox[a_]] :=
  "\\sqrt{" <> boxToLatex[a] <> "}"

boxToLatex[RadicalBox[a_, idx_]] :=
  "\\sqrt[" <> boxToLatex[idx] <> "]{" <> boxToLatex[a] <> "}"

(* ---- Under / Over ---- *)
boxToLatex[UnderscriptBox[base_, under_]] :=
  Module[{lb = boxToLatex[base]},
    If[isLargeOperator[lb],
      lb <> "_{" <> boxToLatex[under] <> "}",
      "\\underset{" <> boxToLatex[under] <> "}{" <> lb <> "}"
    ]
  ]

boxToLatex[OverscriptBox[base_, over_]] :=
  Module[{lb = boxToLatex[base]},
    If[isLargeOperator[lb],
      lb <> "^{" <> boxToLatex[over] <> "}",
      "\\overset{" <> boxToLatex[over] <> "}{" <> lb <> "}"
    ]
  ]

boxToLatex[UnderoverscriptBox[base_, under_, over_]] :=
  Module[{lb = boxToLatex[base]},
    If[isLargeOperator[lb],
      lb <> "_{" <> boxToLatex[under] <> "}^{" <> boxToLatex[over] <> "}",
      "\\underset{" <> boxToLatex[under] <> "}{\\overset{" <>
        boxToLatex[over] <> "}{" <> lb <> "}}"
    ]
  ]

(* ============================================================
   5. STYLE BOX
   Colour is applied FIRST (innermost), bold/italic LAST (outer).
   This matches the spec test:
     StyleBox["x", FontColor->Red, FontWeight->"Bold"]
     => \\mathbf{\\textcolor{#ff0000}{x}}
   ============================================================ *)
boxToLatex[StyleBox[expr_, opts___]] :=
  Module[{latex, col, bold, italic},
    latex  = boxToLatex[expr];
    col    = FontColor  /. {opts} /. FontColor  -> None;
    bold   = FontWeight /. {opts} /. FontWeight -> "Plain";
    italic = FontSlant  /. {opts} /. FontSlant  -> "Plain";

    (* Colour — innermost wrap *)
    If[col =!= None,
      latex = "\\textcolor{" <> colorToHex[col] <> "}{" <> latex <> "}"];
    (* Italic *)
    If[italic === "Italic",
      latex = "\\mathit{" <> latex <> "}"];
    (* Bold — outermost wrap *)
    If[bold === "Bold",
      latex = "\\mathbf{" <> latex <> "}"];
    latex
  ]

(* ============================================================
   6. SEMANTIC / INTERPRETATION BOXES
   ============================================================ *)
(* TagBox: suppress "Null", handle "Piecewise", "Grid", pass-through otherwise *)
boxToLatex[TagBox[_, "Null"]]              := ""
boxToLatex[TagBox[g : GridBox[__], "Piecewise"]] :=
  gridBoxToLatex[g, "cases"]
boxToLatex[TagBox[g : GridBox[__], "Grid"]] :=
  gridTagBoxToLatex[g]
boxToLatex[TagBox[expr_, _]]               := boxToLatex[expr]

(* InterpretationBox / FormBox: render display form *)
boxToLatex[InterpretationBox[display_, _]] := boxToLatex[display]
boxToLatex[FormBox[expr_, _]]              := boxToLatex[expr]

(* TemplateBox: lightweight registry, fallback to joining args *)
boxToLatex[TemplateBox[args_List, tag_]] :=
  Module[{mapped = Lookup[$templateRegistry, tag, Missing[]]},
    If[!MissingQ[mapped],
      mapped @@ (boxToLatex /@ args),
      (Message[boxToLatex::unknownTemplate, tag];
       StringJoin[boxToLatex /@ args])
    ]
  ]

$templateRegistry = <|
  "Sqrt"        -> Function[{x},    "\\sqrt{" <> x <> "}"],
  "Abs"         -> Function[{x},    "\\left|" <> x <> "\\right|"],
  "Norm"        -> Function[{x},    "\\left\\|" <> x <> "\\right\\|"],
  "Superscript" -> Function[{b, e}, "{" <> b <> "}^{" <> e <> "}"],
  "Subscript"   -> Function[{b, i}, "{" <> b <> "}_{" <> i <> "}"],
  "Fraction"    -> Function[{n, d}, "\\frac{" <> n <> "}{" <> d <> "}"]
|>

(* ============================================================
   7. GRIDBOX (free-standing — no surrounding delimiter context)
   ============================================================ *)
boxToLatex[g : GridBox[__]] := gridBoxToLatex[g]

(* ============================================================
   8. INTENTIONALLY-SKIPPED HEADS
   ============================================================ *)
boxToLatex[DynamicBox[__]]  := (Message[boxToLatex::dynamic];  "")
boxToLatex[GraphicsBox[__]] := (Message[boxToLatex::graphics]; "[graphics]")

(* ============================================================
   9. FALLBACK
   ============================================================ *)
boxToLatex[head_[args___]] :=
  (Message[boxToLatex::unknown, head];
   StringJoin[boxToLatex /@ {args}])

(* ============================================================
   10. DELIMITER → GRID ENVIRONMENT HELPER
   ============================================================ *)
delimiterToGridEnv[open_String, close_String] :=
  Switch[{open, close},
    {"(", ")"},   "pmatrix",
    {"[", "]"},   "bmatrix",
    {"{", "}"},   "Bmatrix",
    {"|", "|"},   "vmatrix",
    {"||","||"},  "Vmatrix",
    {"\[LeftBracketingBar]",       "\[RightBracketingBar]"},           "vmatrix",
    {"\[LeftDoubleBracketingBar]", "\[RightDoubleBracketingBar]"},     "Vmatrix",
    _,            None
  ]

End[]  (* `Private` *)

(* ============================================================
   11. LOAD GRIDBOX (after boxToLatex is defined so it can be
       called from gridBoxToLatex cell-level translation)
   ============================================================ *)
If[!TrueQ[Wolfbook`BoxToLatex`GridBoxLoaded],
  Get[FileNameJoin[{DirectoryName[$InputFileName], "GridBox.wl"}]];
  Wolfbook`BoxToLatex`GridBoxLoaded = True
]

EndPackage[]
