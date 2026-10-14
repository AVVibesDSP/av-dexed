// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <array>

#include "AVDexedTheme.h"
#include "AVDexedLanguage.h"

#include <cmath>

namespace AVDexedMeters
{

/** Balistica visual comun para los peak meters de AV Vibes.

    La entrada es pico lineal (1.0 = 0 dBFS). La barra se dibuja en una
    escala -60..0 dBFS, con ataque instantaneo, hold corto y release constante
    en dB/segundo. Solo vive en el message thread. */
class AVDexedPeakMeterState
{
public:
    static constexpr float floorDb = -60.0f;
    static constexpr float ceilingDb = 0.0f;
    static constexpr float releaseDbPerSecond = 24.0f;
    static constexpr float peakHoldSeconds = 0.10f;
    static constexpr float clipHoldSeconds = 1.0f;
    static constexpr float clipFadeSeconds = 0.50f;

    void reset() noexcept
    {
        displayedDb = floorDb;
        peakHoldRemaining = 0.0f;
        clipHoldRemaining = 0.0f;
        clipAlpha = 0.0f;
    }

    void update (float linearPeak, float elapsedSeconds) noexcept
    {
        update (linearPeak, linearPeak, elapsedSeconds);
    }

    /** La barra puede representar un punto concreto mientras el LED vigila
        otro. Desde jul 2026 el TopBar usa la version de un solo pico (rojo =
        solo la salida real, convencion Kontakt/DAW); este overload queda
        para meters que quieran separar nivel y deteccion de clip. */
    void update (float linearPeak, float clipDetectionPeak, float elapsedSeconds) noexcept
    {
        const auto safePeak = std::isfinite (linearPeak) && linearPeak > 0.0f
                            ? linearPeak : 0.0f;
        const auto safeClipPeak = std::isfinite (clipDetectionPeak) && clipDetectionPeak > 0.0f
                                ? clipDetectionPeak : 0.0f;
        const auto dt = juce::jlimit (0.001f, 0.25f, elapsedSeconds);
        const auto incomingDb = juce::Decibels::gainToDecibels (safePeak, floorDb);

        if (incomingDb >= displayedDb)
        {
            displayedDb = incomingDb;
            peakHoldRemaining = peakHoldSeconds;
        }
        else if (peakHoldRemaining > 0.0f)
        {
            peakHoldRemaining = juce::jmax (0.0f, peakHoldRemaining - dt);
        }
        else
        {
            displayedDb = juce::jmax (incomingDb,
                                      displayedDb - releaseDbPerSecond * dt);
        }

        // El limitador deja pasar 1.0 exacto; el LED denuncia overs reales.
        if (safeClipPeak > 1.0f)
        {
            clipAlpha = 1.0f;
            clipHoldRemaining = clipHoldSeconds;
        }
        else if (clipHoldRemaining > 0.0f)
        {
            clipHoldRemaining = juce::jmax (0.0f, clipHoldRemaining - dt);
        }
        else
        {
            clipAlpha = juce::jmax (0.0f, clipAlpha - dt / clipFadeSeconds);
        }
    }

    float proportion() const noexcept
    {
        return juce::jmap (juce::jlimit (floorDb, ceilingDb, displayedDb),
                           floorDb, ceilingDb, 0.0f, 1.0f);
    }

    float displayedDecibels() const noexcept { return displayedDb; }
    float clipIndicatorAlpha() const noexcept { return clipAlpha; }

    static float linearToProportion (float linearPeak) noexcept
    {
        AVDexedPeakMeterState meter;
        meter.update (linearPeak, 1.0f / 30.0f);
        return meter.proportion();
    }

private:
    float displayedDb = floorDb;
    float peakHoldRemaining = 0.0f;
    float clipHoldRemaining = 0.0f;
    float clipAlpha = 0.0f;
};

} // namespace AVDexedMeters

// Compact L/R capsules adapted from AV Vibes' AVTopBarV2. Output peaks are
// measured after effects, balance and master, including a mono bus adapter.
class AVDexedPeakMeter final : public juce::Component, public juce::SettableTooltipClient {
public:
    float AVDisplayedGain() const noexcept {
        return juce::Decibels::decibelsToGain(juce::jmax(AVLeft.displayedDecibels(), AVRight.displayedDecibels()), -60.0f);
    }
    std::array<float, 4> AVShown { -1.0f, -1.0f, -1.0f, -1.0f };
    void AVUpdate(float left, float right, float elapsed) {
        AVLeft.update(left, elapsed);
        AVRight.update(right, elapsed);
        // Silence must not repaint 50 times a second: only a visible change does.
        const std::array<float, 4> shown { AVLeft.proportion(), AVRight.proportion(),
                                           AVLeft.clipIndicatorAlpha(), AVRight.clipIndicatorAlpha() };
        bool changed = false;
        for (size_t i = 0; i < shown.size(); ++i)
            changed |= std::abs(shown[i] - AVShown[i]) > 0.002f;
        if (!changed) return;
        AVShown = shown;
        const auto level = [](float db) { return db <= -60.0f ? juce::String::fromUTF8("−∞") : juce::String(db, 1); };
        setTooltip(AVDexedLanguageService::getInstance().text("outputPeak") +
                   " · L " + level(AVLeft.displayedDecibels()) + " / R " + level(AVRight.displayedDecibels()) + " dBFS");
        setTitle(getTooltip());
        repaint();
    }
    void paint(juce::Graphics& g) override {
        using namespace juce;
        const auto area = getLocalBounds().toFloat();
        const float scale = jmin(area.getWidth() / 15.0f, area.getHeight() / 30.0f);
        const auto pair = Rectangle<float>(15.0f * scale, 30.0f * scale).withCentre(area.getCentre());
        for (int channel = 0; channel < 2; ++channel) {
            const auto& state = channel == 0 ? AVLeft : AVRight;
            const auto track = Rectangle<float>(pair.getX() + float(channel) * 9.0f * scale,
                                                pair.getY(), 6.0f * scale, pair.getHeight());
            g.setGradientFill(ColourGradient(AVDexedTheme::silverTop, 0, track.getY(),
                                             AVDexedTheme::silverBottom, 0, track.getBottom(), false));
            g.fillRoundedRectangle(track, track.getWidth() * 0.5f);
            const float height = track.getHeight() * state.proportion();
            if (height > 0.5f) {
                const auto fill = track.withTop(track.getBottom() - jmax(height, track.getWidth()));
                g.setGradientFill(ColourGradient(AVDexedTheme::oceanTop, 0, fill.getY(),
                                                 AVDexedTheme::oceanBottom, 0, fill.getBottom(), false));
                g.fillRoundedRectangle(fill, fill.getWidth() * 0.5f);
            }
            if (isMouseOver()) {
                g.setColour(AVDexedTheme::oceanHighlight.withAlpha(0.22f));
                g.fillRoundedRectangle(track, track.getWidth() * 0.5f);
            }
            if (state.clipIndicatorAlpha() > 0.0f) {
                g.setColour(AVDexedTheme::dangerTop.withAlpha(state.clipIndicatorAlpha()));
                g.fillRoundedRectangle(track.withHeight(3.0f * scale), 1.5f * scale);
            }
        }
    }
    void mouseEnter(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }
private:
    AVDexedMeters::AVDexedPeakMeterState AVLeft, AVRight;
};
