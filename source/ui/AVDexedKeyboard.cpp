// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedKeyboard.h"
#include "AVDexedTheme.h"
#include "AVDexedPixelCache.h"

using namespace juce;

namespace {
// Bottom corners at 75 % of the largest possible rounding. The largest is half
// the key's width (beyond it the bottom is already a half circle).
constexpr float keyRounding = 0.75f;
float bottomRadius(Rectangle<float> key) { return key.getWidth() * 0.5f * keyRounding; }

Path bottomRoundedKey(Rectangle<float> area, float radius) {
    Path shape;
    shape.addRoundedRectangle(area.getX(), area.getY(), area.getWidth(), area.getHeight(),
                              radius, radius, false, false, true, true);
    return shape;
}

void drawBottomBlueOverlay(Graphics& g, const Path& shape, Rectangle<float> area,
                           float alphaTop, float alphaBottom) {
    ColourGradient pressed(AVDexedTheme::oceanTop.withAlpha(alphaTop), area.getCentreX(), area.getY(),
                           AVDexedTheme::oceanBottom.withAlpha(alphaBottom), area.getCentreX(), area.getBottom(), false);
    g.setGradientFill(pressed);
    g.fillPath(shape);
}
}

void AVDexedKeyboardComponent::releaseAVUINotes() {
    // JUCE releases only notes recorded as keyboard/mouse input by this component.
    MidiKeyboardComponent::focusLost(juce::Component::focusChangedDirectly);
    AVMouseHeld = false;
}

bool AVDexedKeyboardComponent::mouseDownOnKey(int, const MouseEvent&) {
    AVMouseHeld = true;
    return true;
}

void AVDexedKeyboardComponent::mouseUpOnKey(int, const MouseEvent&) { AVMouseHeld = false; }

void AVDexedKeyboardComponent::timerCallback() {
    MidiKeyboardComponent::timerCallback();
    if (AVMouseHeld && !Desktop::getInstance().getMainMouseSource().isDragging())
        releaseAVUINotes();
}

AVDexedKeyboardComponent::AVDexedKeyboardComponent(MidiKeyboardState& state)
    : MidiKeyboardComponent(state, MidiKeyboardComponent::horizontalKeyboard) {
    setColour(whiteNoteColourId, AVDexedTheme::canvas);
    setColour(blackNoteColourId, Colour(0xff000000));
    setColour(keySeparatorLineColourId, Colour(0x66000000));
    setColour(mouseOverKeyOverlayColourId, Colours::white.withAlpha(0.09f));
    setColour(keyDownOverlayColourId, AVDexedTheme::oceanTop.withAlpha(0.48f));
    setColour(textLabelColourId, Colour(0xff263438));
    setColour(shadowColourId, Colour(0x55000000));
}

void AVDexedKeyboardComponent::resized() {
    MidiKeyboardComponent::resized();
    AVBlackKeyShadows.clear();
}

void AVDexedKeyboardComponent::AVDrawBlackKeyShadow(Graphics& g, const Path& shape) {
    // As DropShadow::drawForPath, but the blur covers the whole key's shadow,
    // which is what a full repaint draws (a partial one blurred only the part
    // it reached, a few levels apart at its edge).
    constexpr int radius = 5;
    const Point<int> offset { 0, 2 };
    const auto bounds = shape.getBounds();
    const auto area = (bounds.getSmallestIntegerContainer() + offset).expanded(radius + 1);
    if (area.getWidth() <= 2 || area.getHeight() <= 2 || !g.clipRegionIntersects(area))
        return;
    auto& shadow = AVBlackKeyShadows[{ bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight() }];
    if (!shadow.isValid() || AVDexedPixelCache::isLost(shadow)) {
        const auto imageType = g.getInternalContext().getPreferredImageTypeForTemporaryImages();
        shadow = Image(Image::SingleChannel, area.getWidth(), area.getHeight(), true, *imageType);
        shadow.setBackupEnabled(false);   // kept on the GPU on Windows: never read back
        {
            Graphics shadowGraphics(shadow);
            shadowGraphics.setColour(Colours::white);
            shadowGraphics.fillPath(shape, AffineTransform::translation(float(offset.x - area.getX()),
                                                                        float(offset.y - area.getY())));
        }
        shadow.getPixelData()->applySingleChannelBoxBlurEffect(radius);
    }
    g.setColour(Colour(0x70000000));
    g.drawImageAt(shadow, area.getX(), area.getY(), true);
}

void AVDexedKeyboardComponent::drawWhiteNote(int midiNoteNumber, Graphics& g,
                                              Rectangle<float> area, bool isDown,
                                              bool isOver, Colour lineColour,
                                              Colour textColour) {
    // JUCE paints every key on every paint: one wholly outside the area being
    // painted (a pressed key's neighbours) would change no pixel.
    if (!g.clipRegionIntersects(area.expanded(1.0f).getSmallestIntegerContainer()))
        return;
    // A GOZO-like gap (about 2.4 px) lets the rounded bottoms read as shapes.
    const auto key = area.reduced(1.2f, 0.0f);
    const auto shape = bottomRoundedKey(key, bottomRadius(key));

    g.setGradientFill(ColourGradient(Colour(0xffd9e8e3), key.getCentreX(), key.getY(),
                                    Colour(0xff8d8d8d), key.getCentreX(), key.getBottom(), false));
    g.fillPath(shape);

    if (isOver) {
        g.setColour(Colours::white.withAlpha(0.09f));
        g.fillPath(shape);
    }
    if (isDown)
        drawBottomBlueOverlay(g, shape, key, 0.45f, 0.62f);

    g.setColour(lineColour.isTransparent() ? Colour(0x66000000) : lineColour);
    g.strokePath(shape, PathStrokeType(0.8f));

    if (midiNoteNumber % 12 == 0) {
        const auto diameter = jmin(key.getWidth() * 0.35f, 8.0f);
        const auto centre = Point<float>(key.getCentreX(), key.getY() + key.getHeight() * 0.83f);
        g.setColour(Colour(0xff070707));
        g.fillEllipse(Rectangle<float>(diameter, diameter).withCentre(centre));
        g.setColour(Colour(0xff172b36));
        g.fillEllipse(Rectangle<float>(diameter * 0.5f, diameter * 0.5f).withCentre(centre));
    }

    juce::ignoreUnused(textColour);
}

void AVDexedKeyboardComponent::drawBlackNote(int, Graphics& g, Rectangle<float> area,
                                              bool isDown, bool isOver, Colour) {
    const auto key = area.reduced(0.45f, 0.0f);
    const auto shape = bottomRoundedKey(key, bottomRadius(key));
    if (!AVDexedTheme::lowPower) AVDrawBlackKeyShadow(g, shape);
    if (!g.clipRegionIntersects(area.expanded(1.0f).getSmallestIntegerContainer()))
        return;

    g.setGradientFill(ColourGradient(Colour(0xff12222b), key.getCentreX(), key.getY(),
                                    Colour(0xff000000), key.getCentreX(), key.getBottom(), false));
    g.fillPath(shape);

    if (isOver) {
        g.setColour(Colours::white.withAlpha(0.09f));
        g.fillPath(shape);
    }
    if (isDown)
        drawBottomBlueOverlay(g, shape, key, 0.55f, 0.74f);

    g.setColour(Colour(0xff060606));
    g.strokePath(shape, PathStrokeType(0.9f));
}
