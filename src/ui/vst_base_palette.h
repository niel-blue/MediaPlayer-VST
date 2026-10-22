// Generated from palette.json by generate.ps1. Do not edit colors here.
#pragma once
#include <cstdint>
namespace vst_base {
struct Color { std::uint8_t r, g, b; };
struct Palette {
  Color background;
  Color surface;
  Color header;
  Color selection;
  Color border;
  Color separator;
  Color track;
  Color dialTrack;
  Color buttonBorder;
  Color accent;
  Color action;
  Color accentText;
  Color text;
  Color mutedText;
  Color onActionText;
  Color bypassFill;
  Color bypassBorder;
  Color bypassText;
  Color successFill;
  Color success;
  Color danger;
  Color knobFace;
  Color edgeHighlight;
  Color edgeShadow;
  Color knobGradientTop;
  Color knobGradientBottom;
};
inline constexpr Palette kDark {
  {12, 22, 35}, // background #0C1623
  {18, 33, 48}, // surface #122130
  {12, 30, 51}, // header #0C1E33
  {20, 56, 86}, // selection #143856
  {39, 73, 97}, // border #274961
  {37, 74, 99}, // separator #254A63
  {28, 76, 108}, // track #1C4C6C
  {36, 68, 93}, // dialTrack #24445D
  {69, 111, 137}, // buttonBorder #456F89
  {6, 201, 255}, // accent #06C9FF
  {0, 178, 235}, // action #00B2EB
  {6, 201, 255}, // accentText #06C9FF
  {231, 244, 251}, // text #E7F4FB
  {119, 148, 168}, // mutedText #7794A8
  {12, 22, 35}, // onActionText #0C1623
  {37, 48, 58}, // bypassFill #25303A
  {110, 132, 145}, // bypassBorder #6E8491
  {180, 194, 202}, // bypassText #B4C2CA
  {17, 49, 40}, // successFill #113128
  {50, 205, 130}, // success #32CD82
  {255, 116, 116}, // danger #FF7474
  {20, 56, 86}, // knobFace #143856
  {69, 111, 137}, // edgeHighlight #456F89
  {8, 15, 24}, // edgeShadow #080F18
  {30, 55, 72}, // knobGradientTop #1E3748
  {12, 22, 35}, // knobGradientBottom #0C1623
};
inline constexpr Palette kLight {
  {243, 247, 250}, // background #F3F7FA
  {255, 255, 255}, // surface #FFFFFF
  {229, 238, 245}, // header #E5EEF5
  {216, 234, 245}, // selection #D8EAF5
  {182, 202, 216}, // border #B6CAD8
  {213, 225, 233}, // separator #D5E1E9
  {196, 215, 228}, // track #C4D7E4
  {196, 215, 228}, // dialTrack #C4D7E4
  {182, 202, 216}, // buttonBorder #B6CAD8
  {0, 125, 168}, // accent #007DA8
  {0, 141, 187}, // action #008DBB
  {0, 125, 168}, // accentText #007DA8
  {23, 50, 71}, // text #173247
  {83, 111, 130}, // mutedText #536F82
  {255, 255, 255}, // onActionText #FFFFFF
  {229, 238, 245}, // bypassFill #E5EEF5
  {182, 202, 216}, // bypassBorder #B6CAD8
  {83, 111, 130}, // bypassText #536F82
  {229, 243, 234}, // successFill #E5F3EA
  {21, 150, 90}, // success #15965A
  {170, 48, 48}, // danger #AA3030
  {228, 238, 244}, // knobFace #E4EEF4
  {255, 255, 255}, // edgeHighlight #FFFFFF
  {166, 190, 206}, // edgeShadow #A6BECE
  {241, 247, 250}, // knobGradientTop #F1F7FA
  {216, 229, 238}, // knobGradientBottom #D8E5EE
};
} // namespace vst_base
