// Generated from controls.json by generate.ps1.
#pragma once
namespace vst_base {
struct KnobStyle {
  double gainDiameter = 40.0;
  double delayDiameter = 54.0;
  double radiusInset = 3.0;
  double bodyInset = 8.0;
  double startDegrees = 135.0;
  double sweepDegrees = 270.0;
  double trackWidth = 3.5;
  double activeWidth = 2.5;
  double borderWidth = 1.0;
  double pointerWidth = 2.5;
  double pointerStartRatio = 0.32;
  double pointerEndInset = 2.0;
  double gainMin = -20.0;
  double gainMax = 20.0;
  double gainDefault = 0.0;
  double gainDragPixels = 160.0;
  double gainWheelNormalized = 0.0125;
  double delayMin = 0.0;
  double delayMax = 1000.0;
  double delayDefault = 0.0;
  double delayDragPixels = 200.0;
  double delayWheelNormalized = 0.01;
  double typedDelayMax = 10000.0;
  double inheritedZoomFactor = 1.5;
};
inline constexpr KnobStyle kKnobStyle {};
struct ButtonStyle {
  double cornerRadius = 2.0;
  double borderWidth = 1.0;
  double textMargin = 0.0;
  double fontSize = 8.0;
  double resetWidth = 34.0;
  double resetHeight = 13.0;
  double muteWidth = 46.0;
  double muteHeight = 12.0;
  double refreshWidth = 50.0;
  double refreshHeight = 14.0;
};
inline constexpr ButtonStyle kButtonStyle {};
struct SliderStyle {
  double width = 168.0;
  double height = 18.0;
  double trackWidth = 2.0;
  double trackEndInset = 2.5;
  double handleDiameter = 5.0;
  double handleOffsetY = 6.5;
  double travel = 163.0;
  double helperResetWidth = 44.0;
  double helperResetHeight = 20.0;
  double helperResetCornerRadius = 3.0;
  double helperResetTextMargin = 1.0;
};
inline constexpr SliderStyle kSliderStyle {};
struct DevicemenuStyle {
  double width = 280.0;
  double height = 26.0;
  double textInset = 8.0;
  double arrowAreaWidth = 24.0;
  double arrowHalfWidth = 4.0;
  double arrowHalfHeight = 2.0;
  double arrowLineWidth = 1.5;
};
inline constexpr DevicemenuStyle kDevicemenuStyle {};
struct MeterStyle {
  double width = 280.0;
  double height = 6.0;
  double innerInset = 1.0;
  double minimumDb = -60.0;
  double peakDecay = 0.86;
  double clipPeak = 1.0;
};
inline constexpr MeterStyle kMeterStyle {};
struct EditStyle {
  double gainWidth = 48.0;
  double gainHeight = 12.0;
  double gainFontSize = 8.0;
  double delayWidth = 72.0;
  double delayHeight = 18.0;
  double delayFontSize = 10.0;
  double delayInset = 4.0;
};
inline constexpr EditStyle kEditStyle {};
struct LinkStyle {
  double width = 38.0;
  double height = 39.0;
  double capsuleWidth = 20.0;
  double capsuleHeight = 10.0;
  double capsuleTop = 13.0;
  double capsuleRadius = 5.0;
  double dotDiameter = 6.0;
  double dotOffset = 5.0;
  double chainLineWidth = 1.3;
};
inline constexpr LinkStyle kLinkStyle {};
struct TypographyStyle {
  double regular = 14.0;
  double smallFontSize = 10.0;
  double heading = 12.0;
  double title = 17.0;
  double compact = 8.0;
};
inline constexpr TypographyStyle kTypographyStyle {};
} // namespace vst_base
