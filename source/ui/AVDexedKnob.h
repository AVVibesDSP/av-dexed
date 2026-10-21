// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "AVDexedTheme.h"
#include "AVDexedPixelCache.h"

/** A knob's parts that do not move (the track, the face and its sheen), kept
    as an image at the screen's pixels in the slider's properties: a paint
    then draws one image and the parts that move (the value arc, the hover
    shade, the dot, the focus ring), instead of six gradient fills. The face
    never reaches the arc's ring, so the order of the two does not matter. */
struct AVKnobStill final : public juce::ReferenceCountedObject {
    AVDexedPixelCache image;
    static AVDexedPixelCache& of(juce::Slider& slider) {
        static const juce::Identifier key { "AVKnobStill" };
        auto& properties = slider.getProperties();
        auto* still = dynamic_cast<AVKnobStill*>(properties[key].getObject());
        if (still == nullptr) {
            still = new AVKnobStill();
            properties.set(key, juce::var(still));
        }
        return still->image;
    }
};

// Adapted with permission from AV Vibes' AVFastKnobV2. Vector geometry and
// hover/sheen are retained; the instrument's own ocean gradient replaces amber.
inline void AVDrawKnob(juce::Graphics& g, juce::Rectangle<float> area,
                       float value, juce::Slider& slider) {
    using namespace juce;
    const float scale = jmax(0.01f, jmin(area.getWidth(), area.getHeight()) / 94.26f);
    const auto centre = area.getCentre();
    const float arcRadius = 42.63f * scale;
    const float start = MathConstants<float>::pi + 0.7645f;
    const float end = 3.0f * MathConstants<float>::pi - 0.7645f;
    const float middle = 2.0f * MathConstants<float>::pi;
    const float angle = start + (end - start) * value;
    const auto arcBox = Rectangle<float>(arcRadius * 2, arcRadius * 2).withCentre(centre);
    const auto gradient = [](Colour top, Colour bottom, Rectangle<float> box) {
        return ColourGradient(top, box.getX(), box.getY(), bottom, box.getX(), box.getBottom(), false);
    };
    const auto face = Rectangle<float>(63.74f * scale, 63.74f * scale).withCentre(centre);
    const PathStrokeType stroke(3.0f * scale, PathStrokeType::curved, PathStrokeType::rounded);
    AVKnobStill::of(slider).draw(g, slider, arcBox.expanded(4.0f * scale).getUnion(face), [&](Graphics& sg) {
        Path track;
        track.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0, start, end, true);
        sg.setGradientFill(gradient(AVDexedTheme::knobTop, AVDexedTheme::knobBottom, arcBox));
        sg.strokePath(track, PathStrokeType(6.0f * scale, PathStrokeType::curved, PathStrokeType::rounded));
        sg.setGradientFill(gradient(AVDexedTheme::silverTop, AVDexedTheme::silverBottom, arcBox));
        sg.strokePath(track, stroke);
        sg.setGradientFill(gradient(AVDexedTheme::knobTop, AVDexedTheme::knobBottom, face));
        sg.fillEllipse(face);
        auto sheen = gradient(Colours::white.withAlpha(0.15f), Colours::white.withAlpha(0.15f), face);
        sheen.addColour(0.5, Colours::transparentWhite);
        sg.setGradientFill(sheen);
        sg.fillEllipse(face);
    });
    const bool bipolar = static_cast<bool>(slider.getProperties()["AVBipolar"]);
    const float origin = bipolar ? middle : start;
    if (std::abs(value - (bipolar ? 0.5f : 0.0f)) > 0.005f) {
        Path active;
        active.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0,
                             jmin(origin, angle), jmax(origin, angle), true);
        g.setGradientFill(gradient(AVDexedTheme::oceanTop, AVDexedTheme::oceanBottom, arcBox));
        g.strokePath(active, stroke);
    }
    if (slider.isMouseOverOrDragging()) {
        g.setColour(Colours::white.withAlpha(0.04f));
        g.fillEllipse(face);
    }
    const Point<float> dot(centre.x + 26.3f * scale * std::sin(angle),
                           centre.y - 26.3f * scale * std::cos(angle));
    const auto dotBox = Rectangle<float>(4.0f * scale, 4.0f * scale).withCentre(dot);
    g.setGradientFill(gradient(AVDexedTheme::dotTop, AVDexedTheme::dotBottom, dotBox));
    g.fillEllipse(dotBox);
    if (AVDexedTheme::showsFocus(slider)) {
        g.setColour(AVDexedTheme::oceanHighlight);
        g.drawEllipse(arcBox.expanded(4.0f * scale), 1.2f);
    }
}
