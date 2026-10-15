// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "AVDexedTheme.h"
#include <juce_gui_basics/juce_gui_basics.h>

/** Pitch / mod wheel drawn like GOZO's: grey capsule track, ocean fill (mod
    from the bottom, pitch from the centre toward the thumb), twelve soft
    grooves, a grey thumb over a soft shadow band. It is fully vector: the
    earlier SVG carried its own thumb and grooves, so they appeared twice.
    Dragging is relative (a click never makes the value jump) and the pitch
    wheel springs back to centre on release. */
class AVDexedWheel final : public juce::Slider, private juce::Timer {
public:
    AVDexedWheel() {
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
        setSliderSnapsToMousePosition(false);
    }

    void paint(juce::Graphics& g) override {
        using namespace juce;
        const float s = float(getHeight()) / 60.0f;   // GOZO wheel: 18 × 60
        const auto body = Rectangle<float>(18.0f * s, float(getHeight())).withCentre(getLocalBounds().toFloat().getCentre());
        Path track;
        track.addRoundedRectangle(body, 3.0f * s);
        g.setGradientFill(ColourGradient(AVDexedTheme::silverTop, 0.0f, body.getY(),
                                         AVDexedTheme::silverBottom, 0.0f, body.getBottom(), false));
        g.fillPath(track);

        {
        Graphics::ScopedSaveState clip(g);
        g.reduceClipRegion(track);
        const float value = float(valueToProportionOfLength(getValue()));
        const float thumbH = 8.0f * s, margin = 2.0f * s;
        const float travel = body.getHeight() - 4.0f * s - thumbH;
        const float thumbY = body.getY() + margin + (1.0f - value) * travel;
        const float thumbCentre = thumbY + thumbH * 0.5f;

        if (isPitch()) {
            const float origin = body.getCentreY();
            if (std::abs(thumbCentre - origin) > 0.5f) {
                // The bright end of the gradient always sits at the thumb.
                const bool above = thumbCentre < origin;
                const auto fill = Rectangle<float>(body.getX(), jmin(origin, thumbCentre), body.getWidth(), std::abs(origin - thumbCentre));
                g.setGradientFill(ColourGradient(above ? AVDexedTheme::oceanTop : AVDexedTheme::oceanBottom, 0.0f, fill.getY(),
                                                 above ? AVDexedTheme::oceanBottom : AVDexedTheme::oceanTop, 0.0f, fill.getBottom(), false));
                g.fillRect(fill);
            }
        } else {
            const auto fill = body.withTop(thumbCentre);
            g.setGradientFill(ColourGradient(AVDexedTheme::oceanTop, 0.0f, fill.getY(),
                                             AVDexedTheme::oceanBottom, 0.0f, fill.getBottom(), false));
            g.fillRoundedRectangle(fill, 3.0f * s);
        }

        const float physical = 1.0f / jmax(0.01f, g.getInternalContext().getPhysicalPixelScaleFactor());
        for (int i = 0; i < 12; ++i) {
            const auto groove = Rectangle<float>(body.getX(), body.getY() + (2.0f + 5.0f * float(i)) * s,
                                                 body.getWidth(), jmax(1.0f * s, physical));
            ColourGradient shade(Colours::transparentBlack, groove.getX(), 0.0f, Colours::transparentBlack, groove.getRight(), 0.0f, false);
            shade.addColour(0.495, Colours::black.withAlpha(0.18f));
            g.setGradientFill(shade);
            g.fillRect(groove);
        }

        const auto band = Rectangle<float>(body.getX(), thumbCentre - 7.0f * s, body.getWidth(), 14.0f * s);
        ColourGradient shadow(Colour(0x00121212), 0.0f, band.getY(), Colour(0x00242424), 0.0f, band.getBottom(), false);
        shadow.addColour(2.5 / 14.0, Colour(0xff121212));
        shadow.addColour(11.5 / 14.0, Colour(0xff242424));
        g.setGradientFill(shadow);
        g.fillRect(band);

        const auto thumb = Rectangle<float>(body.getX(), thumbY, body.getWidth(), thumbH);
        g.setGradientFill(ColourGradient(AVDexedTheme::textPrimary, 0.0f, thumb.getY(),
                                         AVDexedTheme::textSecondary, 0.0f, thumb.getBottom(), false));
        g.fillRect(thumb);
        }
        if (AVDexedTheme::showsFocus(*this)) {
            g.setColour(AVDexedTheme::oceanHighlight);
            g.drawRoundedRectangle(body.expanded(2.0f), 4.0f * s, 1.0f);
        }
    }

    void mouseDown(const juce::MouseEvent& event) override {
        if (!isEnabled() || event.mods.isPopupMenu()) return;
        stopTimer();
        AVDragStart = float(valueToProportionOfLength(getValue()));
        AVDragging = true;
    }
    void mouseDrag(const juce::MouseEvent& event) override {
        if (!AVDragging) return;
        const float next = juce::jlimit(0.0f, 1.0f, AVDragStart - float(event.getDistanceFromDragStartY()) / (float(getHeight()) * 0.8f));
        setValue(proportionOfLengthToValue(next), juce::sendNotificationSync);
    }
    void mouseUp(const juce::MouseEvent&) override {
        AVDragging = false;
        if (isPitch()) startTimerHz(60);
    }
    void mouseDoubleClick(const juce::MouseEvent&) override {
        if (isDoubleClickReturnEnabled()) setValue(getDoubleClickReturnValue(), juce::sendNotificationSync);
    }
    bool isBeingDragged() const noexcept { return AVDragging || isTimerRunning(); }
    /** The wheels answer only to the hand: a key step would leave the pitch bent
        (nothing springs it back). Up / Down go on to the editor (sounds). */
    bool keyPressed(const juce::KeyPress&) override { return false; }

private:
    bool isPitch() const { return bool(getProperties()["AVPitch"]); }
    void timerCallback() override {
        // Spring back to centre: 35% of the remaining distance per frame.
        const float value = float(valueToProportionOfLength(getValue()));
        const float next = value + (0.5f - value) * 0.35f;
        if (std::abs(next - 0.5f) < 0.004f) {
            setValue(proportionOfLengthToValue(0.5), juce::sendNotificationSync);
            stopTimer();
            return;
        }
        setValue(proportionOfLengthToValue(next), juce::sendNotificationSync);
    }
    float AVDragStart = 0.0f;
    bool AVDragging = false;
};
