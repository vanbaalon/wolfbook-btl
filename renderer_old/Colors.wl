(* =========================================================
   Colors.wl  —  Wolfbook: Mathematica colour → #rrggbb hex
   ========================================================= *)

BeginPackage["Wolfbook`Colors`"]

colorToHex::usage  = "colorToHex[col] converts a Mathematica colour to a lowercase 6-digit hex string \"#rrggbb\"."
colorToHex::unknown = "Unrecognised colour directive `1`; defaulting to #000000."

Begin["`Private`"]

channelHex[v_] := IntegerString[Clip[Round[255 v], {0, 255}], 16, 2]

(* RGBColor *)
colorToHex[RGBColor[r_, g_, b_]]    := "#" <> channelHex[r] <> channelHex[g] <> channelHex[b]
colorToHex[RGBColor[r_, g_, b_, _]] := colorToHex[RGBColor[r, g, b]]

(* GrayLevel *)
colorToHex[GrayLevel[g_]]    := With[{h = channelHex[g]}, "#" <> h <> h <> h]
colorToHex[GrayLevel[g_, _]] := colorToHex[GrayLevel[g]]

(* Hue / CMYK via ColorConvert *)
colorToHex[col : Hue[__]]      := colorToHex[ColorConvert[col, "RGB"]]
colorToHex[col : CMYKColor[__]]:= colorToHex[ColorConvert[col, "RGB"]]

(* Named colours *)
colorToHex[Red]      = "#ff0000"; colorToHex[Green]      = "#008000";
colorToHex[Blue]     = "#0000ff"; colorToHex[Yellow]     = "#ffff00";
colorToHex[Cyan]     = "#00ffff"; colorToHex[Magenta]    = "#ff00ff";
colorToHex[Black]    = "#000000"; colorToHex[White]      = "#ffffff";
colorToHex[Gray]     = "#808080"; colorToHex[Orange]     = "#ff8800";
colorToHex[Purple]   = "#800080"; colorToHex[Brown]      = "#a52a2a";
colorToHex[Pink]     = "#ffc0cb"; colorToHex[LightBlue]  = "#add8e6";
colorToHex[LightGreen]= "#90ee90";colorToHex[LightGray]  = "#d3d3d3";
colorToHex[DarkBlue] = "#00008b"; colorToHex[DarkGreen]  = "#006400";
colorToHex[DarkRed]  = "#8b0000"; colorToHex[DarkGray]   = "#404040";

colorToHex[Lighter[c_, ___]] := colorToHex[c]
colorToHex[Darker[c_, ___]]  := colorToHex[c]

(* Fallback via ColorConvert *)
colorToHex[col_] :=
  Module[{rgb = Quiet[ColorConvert[col, "RGB"]]},
    If[MatchQ[rgb, RGBColor[__]], colorToHex[rgb],
      (Message[colorToHex::unknown, col]; "#000000")]
  ]

End[]

EndPackage[]
