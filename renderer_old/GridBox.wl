(* =========================================================
   GridBox.wl  —  Wolfbook: GridBox → LaTeX matrix environments
   
   Loaded at the end of BoxToLatex.wl (after boxToLatex is
   fully defined) so that gridBoxToLatex can call boxToLatex
   for cell-level translation.

   Exported: gridBoxToLatex
   ========================================================= *)

BeginPackage["Wolfbook`GridBox`", {"Wolfbook`BoxToLatex`"}]

gridBoxToLatex::usage = "gridBoxToLatex[GridBox[rows, opts], env] converts a GridBox \
to a LaTeX tabular/matrix environment. env is optional; if omitted, the environment \
is inferred from GridBox options (default: pmatrix)."

gridBoxToLatex::badarg = "gridBoxToLatex received unexpected argument: `1`"

Begin["`Private`"]

(* ---- Build a single row ---- *)
rowToLatex[cells_List] :=
  StringRiffle[boxToLatex /@ cells, " & "]

(* ---- Detect ColumnAlignments → aligned env ---- *)
isAligned[_, opts___] :=
  Module[{ca = ColumnAlignments /. {opts} /. ColumnAlignments -> Automatic},
    !MatchQ[ca, Automatic | None]
  ]

(* ---- Main: with explicit env ---- *)
gridBoxToLatex[GridBox[rows_, ___], env_String] :=
  Module[{body = StringRiffle[rowToLatex /@ rows, "\\\\"]},
    "\\begin{" <> env <> "}" <> body <> "\\end{" <> env <> "}"
  ]

(* ---- Main: auto-detect env from options ---- *)
gridBoxToLatex[GridBox[rows_, opts___]] :=
  Module[{env = If[isAligned[rows, opts], "aligned", "pmatrix"]},
    gridBoxToLatex[GridBox[rows, opts], env]
  ]

(* ---- Grid tag: frame borders only ---- *)
(* Handles TagBox[GridBox[rows, opts], "Grid"] *)
gridTagBoxToLatex[GridBox[rows_, opts___]] :=
  Module[
    {frameRows, frameCols, frameOpts, colCount, colSpec, body},

    (* Parse GridBoxFrame *)
    frameOpts = GridBoxFrame /. {opts} /. GridBoxFrame -> None;
    frameCols = False; frameRows = False;
    If[frameOpts =!= None,
      With[{rv = "Rows"    /. frameOpts /. "Rows"    -> None,
            cv = "Columns" /. frameOpts /. "Columns" -> None},
        If[rv =!= None && MemberQ[Flatten[rv], True], frameRows = True];
        If[cv =!= None && MemberQ[Flatten[cv], True], frameCols = True];
      ]
    ];

    (* Column spec *)
    colCount = If[Length[rows] > 0, Length[rows[[1]]], 0];
    colSpec = If[frameCols,
      "|" <> StringJoin[Table["c|", colCount]],
      StringJoin[Table["c", colCount]]
    ];

    (* Build body *)
    body = StringRiffle[
      Table[
        StringRiffle[Table[boxToLatex[rows[[r, c]]], {c, colCount}], " & "],
        {r, Length[rows]}
      ],
      If[frameRows, "\\\\\\" <> "hline", "\\\\"]
    ];

    "\\begin{array}{" <> colSpec <> "}" <>
    If[frameRows, "\\hline", ""] <>
    body <>
    If[frameRows, "\\\\\\" <> "hline", ""] <>
    "\\end{array}"
  ]

(* ---- Fallback ---- *)
gridBoxToLatex[other_] :=
  (Message[gridBoxToLatex::badarg, other]; "")

End[]

EndPackage[]
