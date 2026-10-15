// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <BinaryData.h>
#include <cmath>
#include "AVDexedTheme.h"

/** Drag-over feedback for files dragged onto AV Dexed, as in GOZO: a static
    whole-window wash (oceanTop at 10%) with a 3 px oceanHighlight border, plus
    a centred card with the "+" and a hint saying what dropping will do (the
    card is GOZO's loading-overlay panel, so the hint reads over any view).

    The only motion is the fade: 83 ms in, 117 ms out (GOZO's dialog timing).
    The timer runs only while the alpha moves and stops when the fade ends, and
    nothing repaints while the overlay stands still. It never takes the mouse,
    so the drop still reaches the editor underneath. It owns its timer because
    the editor's timer is stopped and restarted by other windows. */
class AVDexedDropOverlay final : public juce::Component, private juce::Timer {
public:
    AVDexedDropOverlay() {
        setInterceptsMouseClicks(false, false);
        setVisible(false);
        if (auto svg = juce::XmlDocument::parse(juce::String::fromUTF8(BinaryData::AVDexed_PlusIcon_svg,
                                                                       BinaryData::AVDexed_PlusIcon_svgSize)))
            plusIcon = juce::Drawable::createFromSVG(*svg);
    }
    ~AVDexedDropOverlay() override { stopTimer(); }

    void setHint(const juce::String& text) {
        if (hint == text) return;
        hint = text;
        if (isVisible()) repaint();
    }

    void show() {
        target = 1.0f;
        if (! isVisible()) { alpha = 0.0f; setVisible(true); }
        toFront(false);
        startFade();
    }
    /** `instantly` is used when something else must be photographed right away
        (the blurred backdrop of an in-editor dialog). */
    void hide(bool instantly) {
        target = 0.0f;
        if (instantly || ! isVisible()) {
            stopTimer();
            alpha = 0.0f;
            setVisible(false);
            return;
        }
        startFade();
    }

    void paint(juce::Graphics& g) override {
        using namespace juce;
        if (alpha <= 0.0f) return;
        const auto area = getLocalBounds().toFloat();
        g.setColour(AVDexedTheme::oceanTop.withAlpha(0.10f * alpha));
        g.fillRect(area);
        // GOZO draws its border in window pixels, outside the zoom. This overlay
        // lives in the zoomed frame, so the frame's scale is undone for the stroke.
        g.setColour(AVDexedTheme::oceanHighlight.withAlpha(alpha));
        g.drawRect(area, 3.0f / jmax(0.25f, frameZoom()));

        if (hint.isEmpty()) return;
        // Centred like the in-editor dialogs: over the area above the 30 px help footer.
        const auto font = AVDexedTheme::font(22.0f, "SemiBold");
        const float textWidth = GlyphArrangement::getStringWidth(font, hint);
        const float cardWidth = jlimit(jmin(320.0f, area.getWidth() - 48.0f), jmax(1.0f, area.getWidth() - 48.0f), textWidth + 96.0f);
        const auto card = Rectangle<float>(cardWidth, 150.0f).withCentre(area.withTrimmedBottom(30.0f).getCentre());
        const bool fading = alpha < 1.0f;
        if (fading) g.beginTransparencyLayer(alpha);
        AVDexedTheme::panel(g, card, 18.0f);
        g.setColour(AVDexedTheme::oceanHighlight.withAlpha(0.22f));
        g.drawRoundedRectangle(card.reduced(0.5f), 18.0f, 1.0f);
        if (plusIcon != nullptr)
            plusIcon->drawWithin(g, Rectangle<float>(34.0f, 34.0f).withCentre({ card.getCentreX(), card.getY() + 50.0f }),
                                 RectanglePlacement::centred, 1.0f);
        AVDexedTheme::drawOceanText(g, hint, font, { card.getX() + 24.0f, card.getY() + 86.0f, card.getWidth() - 48.0f, 32.0f },
                                    Justification::centred);
        if (fading) g.endTransparencyLayer();
    }

private:
    static constexpr double fadeInMs = 83.0, fadeOutMs = 117.0;
    float alpha = 0.0f, target = 0.0f;
    double lastTick = 0.0;
    juce::String hint;
    std::unique_ptr<juce::Drawable> plusIcon;

    /** The editor's zoom: the scale of the frame this overlay lives in. The
        editor's own transform (a host's AudioProcessorEditor::setScaleFactor)
        is left applied, as it is to GOZO's border. */
    float frameZoom() const {
        auto transform = getTransform();
        if (auto* frame = getParentComponent())
            transform = transform.followedBy(frame->getTransform());
        return std::sqrt(std::abs(transform.getDeterminant()));
    }

    void startFade() {
        if (alpha == target) {
            stopTimer();
            // A hide() before the first fade-in step: nothing to fade out.
            if (target <= 0.0f) setVisible(false);
            return;
        }
        if (isTimerRunning()) return;
        lastTick = juce::Time::getMillisecondCounterHiRes();
        startTimerHz(60);
    }

    // Steps by elapsed time, so a busy message thread shortens the fade
    // instead of stretching it.
    void timerCallback() override {
        const double now = juce::Time::getMillisecondCounterHiRes();
        const bool fadingIn = target > alpha;
        const float step = float((now - lastTick) / (fadingIn ? fadeInMs : fadeOutMs));
        lastTick = now;
        const float next = fadingIn ? juce::jmin(target, alpha + step) : juce::jmax(target, alpha - step);
        if (next != alpha) { alpha = next; repaint(); }
        if (alpha != target) return;
        stopTimer();
        if (target <= 0.0f) setVisible(false);
    }
};
