(* =========================================================
   GenerateCharTable.wls -- Wolfbook developer tool
   -------------------------------------------------------
   One-time / refresh script.  Requires WolframScript (any
   desktop Mathematica / Wolfram Engine installation).

   Algorithm
   ---------
   1.  Parse  $InstallationDirectory/.../UnicodeCharacters.tr
       -- the canonical WL character table -- to get every
       \[Name] and its LaTeX hint ($\cmd$ tokens).
   2.  Scan all palette .nb files for extra \[Name] tokens
       not covered by the .tr file.
   3.  For entries without a direct $\cmd$ hint (e.g.
       \[ScriptCapitalA], \[DoubleStruckCapitalR]) fall back
       to TeXForm.
   4.  Apply a manual-override table (highest priority) to
       fix TeXForm quirks and set invisible/spacing chars.
   5.  Write the complete replacement files:
         src/native/special_chars.cpp   (C++ table for addon)
         renderer/SpecialChars.wl       (WL reference table)

   Usage
     wolframscript -file renderer/GenerateCharTable.wls

   The output files are committed to git; this script is
   gitignored (it requires a Mathematica installation).
   ========================================================= *)

$scriptDir = DirectoryName @ $InputFileName;
$rootDir   = ParentDirectory @ $scriptDir;

$trFile  = FileNameJoin[{$InstallationDirectory, "SystemFiles",
                          "FrontEnd", "TextResources", "UnicodeCharacters.tr"}];
$palDir  = FileNameJoin[{$InstallationDirectory, "SystemFiles",
                          "FrontEnd", "Palettes"}];
$cppOut  = FileNameJoin[{$rootDir, "src", "native", "special_chars.cpp"}];
$wlOut   = FileNameJoin[{$rootDir, "renderer", "SpecialChars.wl"}];

(* ================================================================
   Helper: join a list of strings, each followed by a newline
   ================================================================ *)
joinLines[lines_List] := StringJoin @ Map[# <> "\n" &, lines];

(* ================================================================
   SS1  Parse UnicodeCharacters.tr
   ================================================================ *)
Print["[1/5] Reading UnicodeCharacters.tr..."];
$trText  = Import[$trFile, "Text"];
$trLines = StringSplit[$trText, "\n"];

(* Extract  name -> {latexHint, isLargeOp}  from each line.
   Line format:  0xHEX  \[Name]  ($alias1$  $\cmd$  ...)  Class  ... *)
parseOneLine[line_String] := Module[
  {nameM, name, hintsM, hints, candidates, bestLatex, isLarge},
  nameM = StringCases[line,
    "\\[" ~~ n : Except["]"] .. ~~ "]" :> n, 1];
  If[nameM === {}, Return[Nothing]];
  name = First[nameM];
  (* Skip raw ASCII aliases -- they are just keyboard shortcuts *)
  If[StringStartsQ[name, "Raw"], Return[Nothing]];

  hintsM = StringCases[line,
    "(" ~~ h : Except["("] .. ~~ ")" :> h, 1];
  hints  = If[hintsM === {}, "", First[hintsM]];
  (* Collect $\cmd$ tokens (LaTeX commands start with backslash) *)
  candidates = StringCases[hints,
    "$\\" ~~ cmd : Except["$"] .. ~~ "$" :> "\\" <> cmd];
  (* Exclude self-references like $\[Alpha]$ *)
  candidates = Select[candidates, ! StringStartsQ[#, "\\["] &];
  bestLatex  = If[candidates === {}, "", Last[candidates]];

  isLarge = StringContainsQ[line, "LargeOp"];
  name -> <|"latex" -> bestLatex, "large" -> isLarge|>
];

$trMap = Association @ (parseOneLine /@ $trLines);
Print["  Parsed ", Length[$trMap], " named characters from .tr file."];
Print["  Of those, ", Length @ Select[$trMap, #["latex"] =!= "" &],
      " already have a LaTeX hint."];

(* ================================================================
   SS2  Collect extra names from palette files
   ================================================================ *)
Print["[2/5] Scanning palette .nb files..."];
$palFiles = FileNames["*.nb", $palDir, Infinity];
$palText  = StringJoin[Import[#, "Text"] & /@ $palFiles];
$palNames = DeleteDuplicates @
  StringCases[$palText, "\\[" ~~ n : Except["]"] .. ~~ "]" :> n];
$palNames = Select[$palNames,
  ! StringStartsQ[#, "Raw"] && ! KeyExistsQ[$trMap, #] &];
Print["  ", Length[$palNames], " extra names found only in palettes."];
(* Add them with empty latex -- TeXForm / overrides will fill them *)
$trMap = Join[$trMap,
  Association[# -> <|"latex" -> "", "large" -> False|> & /@ $palNames]];

(* ================================================================
   SS3  TeXForm fallback for entries with empty latex hint
   ================================================================ *)
Print["[3/5] Computing TeXForm fallback for un-hinted entries..."];
safeTeX[name_String] := Quiet @ Check[
  Module[{expr, tex},
    expr = ToExpression["\\" <> "[" <> name <> "]"];
    tex  = ToString[TeXForm[expr]];
    (* Reject obviously bad output *)
    If[StringContainsQ[tex, "Failed" | "\\text{$" | "\\$"],
      Return[""]];
    tex
  ], ""];

$trMap = Association @ KeyValueMap[Function[{name, info},
  If[info["latex"] =!= "",
    name -> info,                          (* already has hint *)
    Module[{fb = safeTeX[name]},
      name -> <|"latex" -> fb, "large" -> info["large"]|>
    ]
  ]
], $trMap];

$filled = Length @ Select[$trMap, #["latex"] =!= "" &];
Print["  After TeXForm pass: ", $filled, " / ", Length[$trMap],
      " entries have a LaTeX mapping."];

(* ================================================================
   SS4  Manual overrides  (highest priority)
   ================================================================
   Keys are WL \[Name] tokens (without backslash-bracket).
   Values: "" = explicitly suppress (invisible/spacing char);
           string = LaTeX command (single backslash in WL string).
   ================================================================ *)
Print["[4/5] Applying manual overrides..."];

$overrides = <|
  (* ---- Invisible / no-width ---- *)
  "InvisibleTimes"    -> "",
  "InvisibleSpace"    -> "",
  "InvisibleComma"    -> "",
  "NoBreak"           -> "",
  "InvisibleApplication"        -> "",
  "InvisiblePostfixScriptBase"  -> "",
  "InvisiblePrefixScriptBase"   -> "",
  "AliasIndicator"    -> "",
  "AlignmentMarker"   -> "",
  "Piecewise"         -> "",

  (* ---- Spacing ---- *)
  "ThinSpace"         -> "\\,",
  "MediumSpace"       -> "\\:",
  "ThickSpace"        -> "\\;",
  "NegativeThinSpace" -> "\\!",
  "NonBreakingSpace"  -> "~",
  "VeryThinSpace"     -> "\\,",
  "NegativeVeryThinSpace" -> "\\!",
  "NegativeMediumSpace"   -> "\\!",
  "NegativeThickSpace"    -> "\\!",

  (* ---- Physics / calculus constants ---- *)
  "DifferentialD"     -> "\\,\\mathrm{d}",
  "CapitalDifferentialD" -> "\\mathrm{D}",
  "ExponentialE"      -> "\\mathrm{e}",
  "ImaginaryI"        -> "\\mathrm{i}",
  "ImaginaryJ"        -> "\\mathrm{j}",
  "HBar"              -> "\\hbar",
  "Laplacian"         -> "\\nabla^2",

  (* ---- Script lowercase: TeXForm gives \mathit, we want \mathscr ---- *)
  "ScriptA" -> "\\mathscr{a}", "ScriptB" -> "\\mathscr{b}",
  "ScriptC" -> "\\mathscr{c}", "ScriptD" -> "\\mathscr{d}",
  "ScriptE" -> "\\mathscr{e}", "ScriptF" -> "\\mathscr{f}",
  "ScriptG" -> "\\mathscr{g}", "ScriptH" -> "\\mathscr{h}",
  "ScriptI" -> "\\mathscr{i}", "ScriptJ" -> "\\mathscr{j}",
  "ScriptK" -> "\\mathscr{k}", "ScriptL" -> "\\mathscr{l}",
  "ScriptM" -> "\\mathscr{m}", "ScriptN" -> "\\mathscr{n}",
  "ScriptO" -> "\\mathscr{o}", "ScriptP" -> "\\mathscr{p}",
  "ScriptQ" -> "\\mathscr{q}", "ScriptR" -> "\\mathscr{r}",
  "ScriptS" -> "\\mathscr{s}", "ScriptT" -> "\\mathscr{t}",
  "ScriptU" -> "\\mathscr{u}", "ScriptV" -> "\\mathscr{v}",
  "ScriptW" -> "\\mathscr{w}", "ScriptX" -> "\\mathscr{x}",
  "ScriptY" -> "\\mathscr{y}", "ScriptZ" -> "\\mathscr{z}",

  (* ---- Gothic full alphabet ---- *)
  "GothicCapitalA" -> "\\mathfrak{A}", "GothicCapitalB" -> "\\mathfrak{B}",
  "GothicCapitalC" -> "\\mathfrak{C}", "GothicCapitalD" -> "\\mathfrak{D}",
  "GothicCapitalE" -> "\\mathfrak{E}", "GothicCapitalF" -> "\\mathfrak{F}",
  "GothicCapitalG" -> "\\mathfrak{G}", "GothicCapitalH" -> "\\mathfrak{H}",
  "GothicCapitalI" -> "\\Im",          "GothicCapitalJ" -> "\\mathfrak{J}",
  "GothicCapitalK" -> "\\mathfrak{K}", "GothicCapitalL" -> "\\mathfrak{L}",
  "GothicCapitalM" -> "\\mathfrak{M}", "GothicCapitalN" -> "\\mathfrak{N}",
  "GothicCapitalO" -> "\\mathfrak{O}", "GothicCapitalP" -> "\\mathfrak{P}",
  "GothicCapitalQ" -> "\\mathfrak{Q}", "GothicCapitalR" -> "\\Re",
  "GothicCapitalS" -> "\\mathfrak{S}", "GothicCapitalT" -> "\\mathfrak{T}",
  "GothicCapitalU" -> "\\mathfrak{U}", "GothicCapitalV" -> "\\mathfrak{V}",
  "GothicCapitalW" -> "\\mathfrak{W}", "GothicCapitalX" -> "\\mathfrak{X}",
  "GothicCapitalY" -> "\\mathfrak{Y}", "GothicCapitalZ" -> "\\mathfrak{Z}",
  "GothicA" -> "\\mathfrak{a}", "GothicB" -> "\\mathfrak{b}",
  "GothicC" -> "\\mathfrak{c}", "GothicD" -> "\\mathfrak{d}",
  "GothicE" -> "\\mathfrak{e}", "GothicF" -> "\\mathfrak{f}",
  "GothicG" -> "\\mathfrak{g}", "GothicH" -> "\\mathfrak{h}",
  "GothicI" -> "\\mathfrak{i}", "GothicJ" -> "\\mathfrak{j}",
  "GothicK" -> "\\mathfrak{k}", "GothicL" -> "\\mathfrak{l}",
  "GothicM" -> "\\mathfrak{m}", "GothicN" -> "\\mathfrak{n}",
  "GothicO" -> "\\mathfrak{o}", "GothicP" -> "\\mathfrak{p}",
  "GothicQ" -> "\\mathfrak{q}", "GothicR" -> "\\mathfrak{r}",
  "GothicS" -> "\\mathfrak{s}", "GothicT" -> "\\mathfrak{t}",
  "GothicU" -> "\\mathfrak{u}", "GothicV" -> "\\mathfrak{v}",
  "GothicW" -> "\\mathfrak{w}", "GothicX" -> "\\mathfrak{x}",
  "GothicY" -> "\\mathfrak{y}", "GothicZ" -> "\\mathfrak{z}",

  (* ---- Operator/symbol corrections ---- *)
  "Prime"             -> "^{\\prime}",
  "DoublePrime"       -> "^{\\prime\\prime}",
  "ReversePrime"      -> "{}^{\\backprime}",
  "HermitianConjugate" -> "^{\\dagger}",
  "Transpose"         -> "\\intercal",
  "Cross"             -> "\\times",
  "CenterDot"         -> "\\cdot",
  "SmallCircle"       -> "\\circ",
  "Degree"            -> "^\\circ",
  "Ellipsis"          -> "\\ldots",
  "CenterEllipsis"    -> "\\cdots",
  "VerticalEllipsis"  -> "\\vdots",
  "DescendingEllipsis" -> "\\ddots",
  "AscendingEllipsis" -> "\\iddots",
  "VerticalSeparator" -> "\\mid",
  (* Bracket chars are handled as delimiters at box level; suppress here *)
  "LeftBracketingBar"      -> "",
  "RightBracketingBar"     -> "",
  "LeftDoubleBracketingBar"  -> "",
  "RightDoubleBracketingBar" -> "",

  (* ---- Placeholders ---- *)
  "Placeholder"       -> "\\square",
  "SelectionPlaceholder" -> "\\square",
  "EmptySmallSquare"  -> "\\square",
  "FilledSmallSquare" -> "\\blacksquare",
  "FilledSquare"      -> "\\blacksquare",
  "EmptyCircle"       -> "\\circ",
  "FilledCircle"      -> "\\bullet",
  "Checkmark"         -> "\\checkmark"
|>;

(* Merge: overrides win *)
$finalMap = Association @ KeyValueMap[Function[{name, info},
  Module[{ov = Lookup[$overrides, name, Missing[]]},
    If[! MissingQ[ov],
      name -> <|"latex" -> ov, "large" -> info["large"]|>,
      name -> info
    ]
  ]
], $trMap];
(* Also add override-only entries not yet in the map *)
$finalMap = Join[$finalMap,
  Association @ KeyValueMap[Function[{name, tex},
    If[! KeyExistsQ[$trMap, name],
      name -> <|"latex" -> tex, "large" -> False|>,
      Nothing
    ]
  ], $overrides]];

(* Retain: entries with a non-empty mapping, OR explicitly suppressed to "" *)
$finalMap = Select[$finalMap,
  #["latex"] =!= "" || KeyExistsQ[$overrides, First[#]] &];

(* Stable sort *)
$sortedNames = Sort @ Keys[$finalMap];

Print["  Final table: ", Length[$finalMap], " entries (",
      Length @ Select[$finalMap, #["latex"] === "" &], " suppressed)."];

(* ================================================================
   SS5a  Write  src/native/special_chars.cpp
   ================================================================ *)
Print["[5/5] Writing output files..."];

(* Escape a LaTeX string for use as a C++ string literal:
   single backslash -> double backslash, double-quote -> \" *)
toCpp[s_String] :=
  "\"" <> StringReplace[s, {"\\" -> "\\\\", "\"" -> "\\\""}] <> "\"";

(* Build C++ key like  "\\[Alpha]"  from name "Alpha" *)
toKey[name_String] := "\"\\\\[" <> name <> "]\"";

(* Build table entry lines *)
tableLines = Function[name,
  Module[{info = $finalMap[name], key, val, pad},
    key = toKey[name];
    val = toCpp[info["latex"]];
    pad = Max[1, 46 - StringLength[key]];
    "    {" <> key <> "," <> StringRepeat[" ", pad] <> val <> "},"
  ]
] /@ $sortedNames;

(* LargeOp LaTeX values for kLargeOps *)
$largeOpLatex = DeleteDuplicates @
  Select[
    Values @ Map[#["latex"] &, Select[$finalMap, #["large"] &]],
    StringLength[#] > 0 &];
$largeOpLatex = DeleteDuplicates @ Join[$largeOpLatex,
  {"\\sum", "\\prod", "\\int", "\\oint", "\\iint", "\\iiint", "\\iiiint",
   "\\bigcup", "\\bigcap", "\\bigoplus", "\\bigotimes",
   "\\bigodot", "\\bigsqcup", "\\coprod", "\\biguplus"}];
largeOpLines = ("    " <> toCpp[#] <> ",") & /@ Sort[$largeOpLatex];

$now = DateString[{"Year", "-", "Month", "-", "Day"}];

$cppParts = {
  "// =============================================================\n",
  "// special_chars.cpp  --  Wolfbook WL named-char -> LaTeX\n",
  "//\n",
  "// AUTO-GENERATED by renderer/GenerateCharTable.wls on ", $now, "\n",
  "// Do NOT edit by hand.  Regenerate with:\n",
  "//   wolframscript -file renderer/GenerateCharTable.wls\n",
  "//\n",
  "// Table initialised once at startup; lookups are O(1) average\n",
  "// via std::unordered_map.  Keys are the literal strings that\n",
  "// appear inside a WL InputForm string, e.g. \"\\\\[Alpha]\".\n",
  "// =============================================================\n",
  "#include \"special_chars.h\"\n",
  "\n",
  "#include <unordered_map>\n",
  "#include <unordered_set>\n",
  "#include <string>\n",
  "\n",
  "namespace wolfbook {\n",
  "namespace {\n",
  "\n",
  "// ----------------------------------------------------------\n",
  "// Main lookup table: WL token -> LaTeX command\n",
  "// ----------------------------------------------------------\n",
  "const std::unordered_map<std::string_view, std::string_view> kTable {\n",
  joinLines[tableLines],
  "}; // end kTable\n",
  "\n",
  "// ----------------------------------------------------------\n",
  "// Large-operator set (LaTeX side)\n",
  "// These operators take limits as subscript/superscript in\n",
  "// display math rather than using \\\\underset/\\\\overset.\n",
  "// ----------------------------------------------------------\n",
  "const std::unordered_set<std::string_view> kLargeOps {\n",
  joinLines[largeOpLines],
  "};\n",
  "\n",
  "} // anonymous namespace\n",
  "\n",
  "// ----------------------------------------------------------\n",
  "// Public API\n",
  "// ----------------------------------------------------------\n",
  "std::string_view wlCharToLatex(std::string_view wlToken) {\n",
  "    auto it = kTable.find(wlToken);\n",
  "    if (it == kTable.end()) return {};\n",
  "    return it->second;\n",
  "}\n",
  "\n",
  "bool isLargeOperator(std::string_view latexCmd) {\n",
  "    return kLargeOps.count(latexCmd) != 0;\n",
  "}\n",
  "\n",
  "} // namespace wolfbook\n"
};

$cppContent = StringJoin[$cppParts];

Export[$cppOut, $cppContent, "Text", CharacterEncoding -> "ASCII"];
Print["  Wrote ", $cppOut];
Print["  Lines: ", Length[StringSplit[$cppContent, "\n"]]];

(* ================================================================
   SS5b  Write  renderer/SpecialChars.wl  (WL reference parity)
   ================================================================ *)
wlMapLines = Function[name,
  Module[{info = $finalMap[name], key, val},
    key = "\"\\[" <> name <> "]\"";
    val = "\"" <> StringReplace[info["latex"], "\"" -> "\\\""] <> "\"";
    "  " <> key <> " -> " <> val
  ]
] /@ $sortedNames;

$wlParts = {
  "(* =========================================================\n",
  "   SpecialChars.wl  --  Wolfbook Mathematica -> LaTeX renderer\n",
  "   Mapping from Wolfram named characters to LaTeX commands.\n",
  "   Loaded by BoxToLatex.wl; exports only $WLtoLaTeX.\n",
  "\n",
  "   AUTO-GENERATED by renderer/GenerateCharTable.wls on ", $now, "\n",
  "   ========================================================= *)\n",
  "\n",
  "BeginPackage[\"Wolfbook`SpecialChars`\"]\n",
  "\n",
  "$WLtoLaTeX::usage = \"Association mapping Wolfram named-character strings to LaTeX command strings.\"\n",
  "\n",
  "Begin[\"`Private`\"]\n",
  "\n",
  "$WLtoLaTeX = <|\n",
  StringJoin[Riffle[wlMapLines, ",\n"]],
  "\n|>\n",
  "\n",
  "End[]  (* `Private` *)\n",
  "\n",
  "EndPackage[]\n"
};

$wlContent = StringJoin[$wlParts];

Export[$wlOut, $wlContent, "Text", CharacterEncoding -> "UTF-8"];
Print["  Wrote ", $wlOut];

Print[""];
Print["Done. Run  ./build.sh native  to recompile."];

   -------------------------------------------------------
   One-time / refresh script.  Requires WolframScript (any
   desktop Mathematica / Wolfram Engine installation).

   Algorithm
   ---------
   1.  Parse  $InstallationDirectory/.../UnicodeCharacters.tr
       — the canonical WL character table — to get every
       \[Name] and its LaTeX hint ($\cmd$ tokens).
   2.  Scan all palette .nb files for extra \[Name] tokens
       not covered by the .tr file.
   3.  For entries without a direct $\cmd$ hint (e.g.
       \[ScriptCapitalA], \[DoubleStruckCapitalR]) fall back
       to TeXForm.
   4.  Apply a manual-override table (highest priority) to
       fix TeXForm quirks and set invisible/spacing chars.
   5.  Write the complete replacement files:
         src/native/special_chars.cpp   (C++ table for addon)
         renderer/SpecialChars.wl       (WL reference table)

   Usage
     wolframscript -file renderer/GenerateCharTable.wls

   The output files are committed to git; this script is
   gitignored (it requires a Mathematica installation).
   ========================================================= *)

$scriptDir = DirectoryName @ $InputFileName;
$rootDir   = ParentDirectory @ $scriptDir;

$trFile  = FileNameJoin[{$InstallationDirectory, "SystemFiles",
                          "FrontEnd", "TextResources", "UnicodeCharacters.tr"}];
$palDir  = FileNameJoin[{$InstallationDirectory, "SystemFiles",
                          "FrontEnd", "Palettes"}];
$cppOut  = FileNameJoin[{$rootDir, "src", "native", "special_chars.cpp"}];
$wlOut   = FileNameJoin[{$rootDir, "renderer", "SpecialChars.wl"}];

(* ================================================================
   §1  Parse UnicodeCharacters.tr
   ================================================================ *)
Print["[1/5] Reading UnicodeCharacters.tr…"];
$trText  = Import[$trFile, "Text"];
$trLines = StringSplit[$trText, "\n"];

(* Extract  name -> {latexHint, isLargeOp}  from each line.
   Line format:  0xHEX  \[Name]  ($alias1$  $\cmd$  …)  Class  … *)
parseOneLine[line_String] := Module[
  {nameM, name, hintsM, hints, candidates, bestLatex, isLarge},
  nameM = StringCases[line,
    "\\[" ~~ n : Except["]"] .. ~~ "]" :> n, 1];
  If[nameM === {}, Return[Nothing]];
  name = First[nameM];
  (* Skip raw ASCII aliases — they're just keyboard shortcuts *)
  If[StringStartsQ[name, "Raw"], Return[Nothing]];

  hintsM = StringCases[line,
    "(" ~~ h : Except["("] .. ~~ ")" :> h, 1];
  hints  = If[hintsM === {}, "", First[hintsM]];
  (* Collect $\cmd$ tokens (LaTeX commands start with backslash) *)
  candidates = StringCases[hints,
    "$\\" ~~ cmd : Except["$"] .. ~~ "$" :> "\\" <> cmd];
  (* Exclude self-references like $\[Alpha]$ *)
  candidates = Select[candidates,
    !StringStartsQ[#, "\\["] &];
  bestLatex  = If[candidates === {}, "", Last[candidates]];

  isLarge = StringContainsQ[line, "LargeOp"];
  name -> <|"latex" -> bestLatex, "large" -> isLarge|>
];

$trMap = Association @ (parseOneLine /@ $trLines);
Print["  Parsed ", Length[$trMap], " named characters from .tr file."];
Print["  Of those, ", Length@Select[$trMap, #["latex"] =!= "" &],
      " already have a LaTeX hint."];

(* ================================================================
   §2  Collect extra names from palette files
   ================================================================ *)
Print["[2/5] Scanning palette .nb files…"];
$palFiles = FileNames["*.nb", $palDir, Infinity];
$palText  = StringJoin[Import[#, "Text"] & /@ $palFiles];
$palNames = DeleteDuplicates @
  StringCases[$palText, "\\[" ~~ n : Except["]"] .. ~~ "]" :> n];
$palNames = Select[$palNames,
  !StringStartsQ[#, "Raw"] && !KeyExistsQ[$trMap, #] &];
Print["  ", Length[$palNames], " extra names found only in palettes."];
(* Add them with empty latex — TeXForm / overrides will fill them *)
$trMap = Join[$trMap,
  Association[# -> <|"latex" -> "", "large" -> False|> & /@ $palNames]];

(* ================================================================
   §3  TeXForm fallback for entries with empty latex hint
   ================================================================ *)
Print["[3/5] Computing TeXForm fallback for un-hinted entries…"];
safeTeX[name_String] := Quiet @ Check[
  Module[{expr, tex},
    expr = ToExpression["\\" <> "[" <> name <> "]"];
    tex  = ToString[TeXForm[expr]];
    (* Reject obviously bad output *)
    If[StringContainsQ[tex, "Failed" | "\\text{$" | "\\$"],
      Return[""]];
    tex
  ], ""];

$trMap = Association @ KeyValueMap[Function[{name, info},
  If[info["latex"] =!= "",
    name -> info,                          (* already has hint *)
    Module[{fb = safeTeX[name]},
      name -> <|"latex" -> fb, "large" -> info["large"]|>
    ]
  ]
], $trMap];

$filled = Length @ Select[$trMap, #["latex"] =!= "" &];
Print["  After TeXForm pass: ", $filled, " / ", Length[$trMap],
      " entries have a LaTeX mapping."];

(* ================================================================
   §4  Manual overrides  (highest priority)
   ================================================================
   Keys are WL \[Name] tokens (without backslash-bracket).
   Values: "" = explicitly suppress (invisible/spacing char);
           string = LaTeX command (single backslash in WL string).
   ================================================================ *)
Print["[4/5] Applying manual overrides…"];

$overrides = <|
  (* ---- Invisible / no-width ---- *)
  "InvisibleTimes"    -> "",
  "InvisibleSpace"    -> "",
  "InvisibleComma"    -> "",
  "NoBreak"           -> "",
  "InvisibleApplication"        -> "",
  "InvisiblePostfixScriptBase"  -> "",
  "InvisiblePrefixScriptBase"   -> "",
  "AliasIndicator"    -> "",
  "AlignmentMarker"   -> "",
  "Piecewise"         -> "",

  (* ---- Spacing ---- *)
  "ThinSpace"         -> "\\,",
  "MediumSpace"       -> "\\:",
  "ThickSpace"        -> "\\;",
  "NegativeThinSpace" -> "\\!",
  "NonBreakingSpace"  -> "~",
  "VeryThinSpace"     -> "\\,",
  "NegativeVeryThinSpace" -> "\\!",
  "NegativeMediumSpace"   -> "\\!",
  "NegativeThickSpace"    -> "\\!",

  (* ---- Physics / calculus constants ---- *)
  "DifferentialD"     -> "\\,\\mathrm{d}",
  "CapitalDifferentialD" -> "\\mathrm{D}",
  "ExponentialE"      -> "\\mathrm{e}",
  "ImaginaryI"        -> "\\mathrm{i}",
  "ImaginaryJ"        -> "\\mathrm{j}",
  "HBar"              -> "\\hbar",
  "Laplacian"         -> "\\nabla^2",

  (* ---- Script lowercase → \mathscr (TeXForm gives \mathit) ---- *)
  "ScriptA" -> "\\mathscr{a}", "ScriptB" -> "\\mathscr{b}",
  "ScriptC" -> "\\mathscr{c}", "ScriptD" -> "\\mathscr{d}",
  "ScriptE" -> "\\mathscr{e}", "ScriptF" -> "\\mathscr{f}",
  "ScriptG" -> "\\mathscr{g}", "ScriptH" -> "\\mathscr{h}",
  "ScriptI" -> "\\mathscr{i}", "ScriptJ" -> "\\mathscr{j}",
  "ScriptK" -> "\\mathscr{k}", "ScriptL" -> "\\mathscr{l}",
  "ScriptM" -> "\\mathscr{m}", "ScriptN" -> "\\mathscr{n}",
  "ScriptO" -> "\\mathscr{o}", "ScriptP" -> "\\mathscr{p}",
  "ScriptQ" -> "\\mathscr{q}", "ScriptR" -> "\\mathscr{r}",
  "ScriptS" -> "\\mathscr{s}", "ScriptT" -> "\\mathscr{t}",
  "ScriptU" -> "\\mathscr{u}", "ScriptV" -> "\\mathscr{v}",
  "ScriptW" -> "\\mathscr{w}", "ScriptX" -> "\\mathscr{x}",
  "ScriptY" -> "\\mathscr{y}", "ScriptZ" -> "\\mathscr{z}",

  (* ---- Gothic full alphabet ---- *)
  "GothicCapitalA" -> "\\mathfrak{A}", "GothicCapitalB" -> "\\mathfrak{B}",
  "GothicCapitalC" -> "\\mathfrak{C}", "GothicCapitalD" -> "\\mathfrak{D}",
  "GothicCapitalE" -> "\\mathfrak{E}", "GothicCapitalF" -> "\\mathfrak{F}",
  "GothicCapitalG" -> "\\mathfrak{G}", "GothicCapitalH" -> "\\mathfrak{H}",
  "GothicCapitalI" -> "\\mathfrak{I}", "GothicCapitalJ" -> "\\mathfrak{J}",
  "GothicCapitalK" -> "\\mathfrak{K}", "GothicCapitalL" -> "\\mathfrak{L}",
  "GothicCapitalM" -> "\\mathfrak{M}", "GothicCapitalN" -> "\\mathfrak{N}",
  "GothicCapitalO" -> "\\mathfrak{O}", "GothicCapitalP" -> "\\mathfrak{P}",
  "GothicCapitalQ" -> "\\mathfrak{Q}", "GothicCapitalR" -> "\\mathfrak{R}",
  "GothicCapitalS" -> "\\mathfrak{S}", "GothicCapitalT" -> "\\mathfrak{T}",
  "GothicCapitalU" -> "\\mathfrak{U}", "GothicCapitalV" -> "\\mathfrak{V}",
  "GothicCapitalW" -> "\\mathfrak{W}", "GothicCapitalX" -> "\\mathfrak{X}",
  "GothicCapitalY" -> "\\mathfrak{Y}", "GothicCapitalZ" -> "\\mathfrak{Z}",
  "GothicA" -> "\\mathfrak{a}", "GothicB" -> "\\mathfrak{b}",
  "GothicC" -> "\\mathfrak{c}", "GothicD" -> "\\mathfrak{d}",
  "GothicE" -> "\\mathfrak{e}", "GothicF" -> "\\mathfrak{f}",
  "GothicG" -> "\\mathfrak{g}", "GothicH" -> "\\mathfrak{h}",
  "GothicI" -> "\\mathfrak{i}", "GothicJ" -> "\\mathfrak{j}",
  "GothicK" -> "\\mathfrak{k}", "GothicL" -> "\\mathfrak{l}",
  "GothicM" -> "\\mathfrak{m}", "GothicN" -> "\\mathfrak{n}",
  "GothicO" -> "\\mathfrak{o}", "GothicP" -> "\\mathfrak{p}",
  "GothicQ" -> "\\mathfrak{q}", "GothicR" -> "\\mathfrak{r}",
  "GothicS" -> "\\mathfrak{s}", "GothicT" -> "\\mathfrak{t}",
  "GothicU" -> "\\mathfrak{u}", "GothicV" -> "\\mathfrak{v}",
  "GothicW" -> "\\mathfrak{w}", "GothicX" -> "\\mathfrak{x}",
  "GothicY" -> "\\mathfrak{y}", "GothicZ" -> "\\mathfrak{z}",
  (* Common Gothic capitals already present — consolidate *)
  "GothicCapitalR" -> "\\Re",
  "GothicCapitalI" -> "\\Im",

  (* ---- Operator corrections (TeXForm gives wrong/missing) ---- *)
  "Prime"             -> "^{\\prime}",
  "DoublePrime"       -> "^{\\prime\\prime}",
  "ReversePrime"      -> "{}^{\\backprime}",
  "HermitianConjugate" -> "^{\\dagger}",
  "Transpose"         -> "\\intercal",
  "Cross"             -> "\\times",
  "CenterDot"         -> "\\cdot",
  "SmallCircle"       -> "\\circ",
  "Degree"            -> "^\\circ",
  "Ellipsis"          -> "\\ldots",
  "CenterEllipsis"    -> "\\cdots",
  "VerticalEllipsis"  -> "\\vdots",
  "DescendingEllipsis" -> "\\ddots",
  "AscendingEllipsis" -> "\\iddots",
  "VerticalSeparator" -> "\\mid",
  "LeftBracketingBar" -> "",       (* handled as delimiter by RowBox logic *)
  "RightBracketingBar" -> "",
  "LeftDoubleBracketingBar" -> "",
  "RightDoubleBracketingBar" -> "",

  (* ---- Suppress extras ---- *)
  "Placeholder"       -> "\\square",
  "SelectionPlaceholder" -> "\\square",
  "EmptySmallSquare"  -> "\\square",
  "FilledSmallSquare" -> "\\blacksquare",
  "FilledSquare"      -> "\\blacksquare",
  "EmptyCircle"       -> "\\circ",
  "FilledCircle"      -> "\\bullet",
  "Checkmark"         -> "\\checkmark"
|>;

(* Merge: overrides win *)
$finalMap = Association @ KeyValueMap[Function[{name, info},
  Module[{ov = Lookup[$overrides, name, Missing[]]},
    If[!MissingQ[ov],
      name -> <|"latex" -> ov, "large" -> info["large"]|>,
      name -> info
    ]
  ]
], $trMap];
(* Also add override-only entries not yet in the map *)
$overrideOnly = Select[$overrides, !KeyExistsQ[$trMap, First[#]] &];
$finalMap = Join[$finalMap,
  Association @ KeyValueMap[Function[{name, tex},
    name -> <|"latex" -> tex, "large" -> False|>
  ], $overrideOnly]];

(* Keep only entries with a non-empty mapping OR explicitly suppressed "" *)
$finalMap = Select[$finalMap,
  #["latex"] =!= "" || KeyExistsQ[$overrides, First[#]] &];

(* Sort keys to produce stable diffs *)
$sortedNames = Sort @ Keys[$finalMap];

Print["  Final table: ", Length[$finalMap], " entries (",
      Length@Select[$finalMap, #["latex"] === "" &], " suppressed)."];

(* ================================================================
   §5a  Write  src/native/special_chars.cpp
   ================================================================ *)
Print["[5/5] Writing output files…"];

(* Convert a WL LaTeX string to its C++ string-literal representation:
   backslash → \\, quote → \" *)
toCpp[s_String] :=
  "\"" <> StringReplace[s, {"\\" -> "\\\\", "\"" -> "\\\""}] <> "\"";

(* WL key:  name  →  C++ key like  "\\[Alpha]"  *)
toKey[name_String] :=
  "\"\\\\[" <> name <> "]\"";

(* Build the kTable entries as a list of strings *)
tableLines = Function[name,
  Module[{info = $finalMap[name], key, val, pad},
    key = toKey[name];
    val = toCpp[info["latex"]];
    (* Align value column at column 44 of the entry *)
    pad = Max[1, 44 - StringLength[key]];
    "    {" <> key <> "," <> StringRepeat[" ", pad] <> val <> "},"
  ]
] /@ $sortedNames;

(* LargeOp LaTeX values to populate kLargeOps *)
$largeOpLatex = DeleteDuplicates @
  Select[
    Values @ Map[#["latex"] &,
      Select[$finalMap, #["large"] &]],
    StringLength[#] > 0 &];
(* Also add manually known multi-integral forms *)
$largeOpLatex = DeleteDuplicates @ Join[$largeOpLatex,
  {"\\sum", "\\prod", "\\int", "\\oint", "\\iint", "\\iiint", "\\iiiint",
   "\\bigcup", "\\bigcap", "\\bigoplus", "\\bigotimes",
   "\\bigodot", "\\bigsqcup", "\\coprod", "\\biguplus"}];
largeOpLines = ("    " <> toCpp[#] <> ",") & /@ Sort[$largeOpLatex];

$now = DateString[{"Year","-","Month","-","Day"}];
$header = "// =============================================================\n\
// special_chars.cpp  —  Wolfbook WL named-char \[RightArrow] LaTeX\n\
//\n\
// AUTO-GENERATED by renderer/GenerateCharTable.wls on " <> $now <> "\n\
// Do NOT edit by hand.  Regenerate with:\n\
//   wolframscript -file renderer/GenerateCharTable.wls\n\
//\n\
// Table initialised once at startup; lookups are O(1) average\n\
// via std::unordered_map.  Keys are the literal strings that\n\
// appear inside a WL InputForm string, e.g. \"\\\\[Alpha]\".\n\
// =============================================================\n\
#include \"special_chars.h\"\n\
\n\
#include <unordered_map>\n\
#include <unordered_set>\n\
#include <string>\n\
\n\
namespace wolfbook {\n\
namespace {\n\
\n\
// ----------------------------------------------------------\n\
// Main lookup table: WL token \[RightArrow] LaTeX command\n\
// ----------------------------------------------------------\n\
const std::unordered_map<std::string_view, std::string_view> kTable {\n";

$footer = "}; // end kTable\n\
\n\
// ----------------------------------------------------------\n\
// Large-operator set (LaTeX side)\n\
// These operators take limits as subscript/superscript in\n\
// display math rather than using \\\\underset/\\\\overset.\n\
// ----------------------------------------------------------\n\
const std::unordered_set<std::string_view> kLargeOps {\n" <>
  StringJoin[largeOpLines ~~ "\n"] <>
"};\n\
\n\
} // anonymous namespace\n\
\n\
// ----------------------------------------------------------\n\
// Public API\n\
// ----------------------------------------------------------\n\
std::string_view wlCharToLatex(std::string_view wlToken) {\n\
    auto it = kTable.find(wlToken);\n\
    if (it == kTable.end()) return {};\n\
    return it->second;\n\
}\n\
\n\
bool isLargeOperator(std::string_view latexCmd) {\n\
    return kLargeOps.count(latexCmd) != 0;\n\
}\n\
\n\
} // namespace wolfbook\n";

$cppContent = $header <>
  StringJoin[tableLines ~~ "\n"] <> "\n" <>
  $footer;

Export[$cppOut, $cppContent, "Text", CharacterEncoding -> "UTF-8"];
Print["  Wrote ", $cppOut];
Print["  Lines: ", Length[StringSplit[$cppContent, "\n"]]];

(* ================================================================
   §5b  Write  renderer/SpecialChars.wl  (WL reference parity)
   ================================================================ *)
wlMapLines = Function[name,
  Module[{info = $finalMap[name], key, val},
    key = "\"\\[" <> name <> "]\"";
    (* WL string: LaTeX with single backslashes *)
    val = "\"" <> StringReplace[info["latex"], "\"" -> "\\\""] <> "\"";
    "  " <> key <> " -> " <> val
  ]
] /@ $sortedNames;

$wlContent =
"(* =========================================================\n\
   SpecialChars.wl  \[LongDash]  Wolfbook Mathematica \[RightArrow] LaTeX renderer\n\
   Mapping from Wolfram named characters to LaTeX commands.\n\
   Loaded by BoxToLatex.wl; exports only $WLtoLaTeX.\n\
\n\
   AUTO-GENERATED by renderer/GenerateCharTable.wls on " <> $now <> "\n\
   ========================================================= *)\n\
\n\
BeginPackage[\"Wolfbook`SpecialChars`\"]\n\
\n\
$WLtoLaTeX::usage = \"Association mapping Wolfram named-character strings to LaTeX command strings.\"\n\
\n\
Begin[\"`Private`\"]\n\
\n\
$WLtoLaTeX = <|\n" <>
  StringJoin[Riffle[wlMapLines, ",\n"]] <> "\n|>\n\
\n\
End[]  (* `Private` *)\n\
\n\
EndPackage[]\n";

Export[$wlOut, $wlContent, "Text", CharacterEncoding -> "UTF-8"];
Print["  Wrote ", $wlOut];

Print[""];
Print["Done. Run  ./build.sh native  to recompile."];
