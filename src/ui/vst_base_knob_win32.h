#pragma once
#include <algorithm>
#include <cmath>
#include <gdiplus.h>
#include "vst_base_controls.h"
#include "vst_base_palette.h"

namespace vst_base {
// Audio-Bridge RotaryKnob::draw geometry, rendered with GDI+.
inline void DrawGainKnob(Gdiplus::Graphics& graphics, float cx, float cy,
                         float normalized, const Palette& colors) {
    const auto color = [](Color c) { return Gdiplus::Color(255, c.r, c.g, c.b); };
    const auto& k = kKnobStyle;
    const float half = static_cast<float>(k.gainDiameter / 2);
    const float radius = half - static_cast<float>(k.radiusInset);
    normalized = std::clamp(normalized, 0.0f, 1.0f);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen track(color(colors.track), static_cast<float>(k.trackWidth));
    Gdiplus::Pen active(color(colors.accent), static_cast<float>(k.activeWidth));
    for (auto* pen : {&track, &active}) {
        pen->SetStartCap(Gdiplus::LineCapRound);
        pen->SetEndCap(Gdiplus::LineCapRound);
    }
    graphics.DrawArc(&track, cx-radius, cy-radius, radius*2, radius*2,
                     static_cast<float>(k.startDegrees), static_cast<float>(k.sweepDegrees));
    if (normalized > 0)
        graphics.DrawArc(&active, cx-radius, cy-radius, radius*2, radius*2,
                         static_cast<float>(k.startDegrees),
                         static_cast<float>(k.sweepDegrees)*normalized);
    const float bodyRadius = half - static_cast<float>(k.bodyInset);
    const Gdiplus::RectF body(cx-bodyRadius, cy-bodyRadius, bodyRadius*2, bodyRadius*2);
    Gdiplus::LinearGradientBrush face(body, color(colors.knobGradientTop),
        color(colors.knobGradientBottom), Gdiplus::LinearGradientModeVertical);
    graphics.FillEllipse(&face, body);
    Gdiplus::Pen edge(color(colors.buttonBorder), static_cast<float>(k.borderWidth));
    graphics.DrawEllipse(&edge, body);
    const double angle = (k.startDegrees + k.sweepDegrees*normalized) *
                         3.14159265358979323846 / 180.0;
    const float inner = radius * static_cast<float>(k.pointerStartRatio);
    const float outer = radius - static_cast<float>(k.pointerEndInset);
    Gdiplus::Pen pointer(color(colors.text), static_cast<float>(k.pointerWidth));
    pointer.SetStartCap(Gdiplus::LineCapRound);
    pointer.SetEndCap(Gdiplus::LineCapRound);
    graphics.DrawLine(&pointer,
        cx+inner*static_cast<float>(std::cos(angle)), cy+inner*static_cast<float>(std::sin(angle)),
        cx+outer*static_cast<float>(std::cos(angle)), cy+outer*static_cast<float>(std::sin(angle)));
}
} // namespace vst_base
