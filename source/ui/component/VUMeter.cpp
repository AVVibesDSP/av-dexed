/*
  ==============================================================================

    VUMeter.cpp
    Based on the original VUMeter by Fulop Nandor (created 29 Jun 2024)
    for Dexed. Rewritten for AV Dexed by AV Vibes, 2026.

  ==============================================================================
*/

#include "VUMeter.h"
#include "../AVDexedTheme.h"

#include <cmath>

VuMeterBase::VuMeterBase(int maximumDbToShow, bool useTricolor)
    : maximumDb(maximumDbToShow),
      minimumDb(maximumDbToShow - totalBlocks + 1),
      tricolor(useTricolor) {
}

int VuMeterBase::activeBlocksFor(float level) const {
    const float amplitude = std::abs(level);
    const float db = amplitude > 0.0f ? 20.0f * std::log10(amplitude) : static_cast<float>(minimumDb - 1);
    return juce::jlimit(0, totalBlocks, static_cast<int>(std::floor(db - static_cast<float>(minimumDb) + 1.0f)));
}

void VuMeterBase::setLevel(float level) {
    const bool changed = activeBlocksFor(level) != activeBlocksFor(v);
    v = level;
    if (changed) repaint();
}

void VuMeterBase::resized() {
    for (auto& layer : stateLayers) layer.invalidate();
}

void VuMeterBase::paint(juce::Graphics& g) {
    const int activeBlocks = activeBlocksFor(v);
    stateLayers[size_t(activeBlocks)].draw(g, *this, getLocalBounds().toFloat(),
                                           [this, activeBlocks](juce::Graphics& lg) { paintBlocks(lg, activeBlocks); });
}

void VuMeterBase::paintBlocks(juce::Graphics& g, int activeBlocks) const {
    auto bounds = getLocalBounds().toFloat();
    g.setColour(AVDexedTheme::controlBottom);
    g.fillRoundedRectangle(bounds, 2.0f);

    const float availableWidth = juce::jmax(0.0f, bounds.getWidth() - 2.0f);
    const float gap = juce::jmin(static_cast<float>(blockGap), availableWidth / static_cast<float>(totalBlocks));
    const float segmentWidth = (availableWidth - gap * static_cast<float>(totalBlocks - 1)) / static_cast<float>(totalBlocks);
    for (int i = 0; i < totalBlocks; ++i) {
        const float x = bounds.getX() + 1.0f + static_cast<float>(i) * (segmentWidth + gap);
        auto segment = juce::Rectangle<float>(x, bounds.getY() + 1.0f, segmentWidth,
                                               juce::jmax(0.0f, bounds.getHeight() - 2.0f));
        if (i >= activeBlocks) {
            g.setColour(AVDexedTheme::track);
        } else if (tricolor && i >= totalBlocks - VuMeterOutput::numRedBlocks) {
            g.setGradientFill(juce::ColourGradient(AVDexedTheme::dangerTop, segment.getX(), segment.getY(),
                                                    AVDexedTheme::dangerBottom, segment.getX(), segment.getBottom(), false));
        } else {
            g.setGradientFill(juce::ColourGradient(AVDexedTheme::oceanTop, segment.getX(), segment.getY(),
                                                    AVDexedTheme::oceanBottom, segment.getX(), segment.getBottom(), false));
        }
        g.fillRoundedRectangle(segment, 0.8f);
    }
}

double VuMeterOutput::getDecayFactor(double sampleRate) {
    constexpr double dBdrop = -40.0;
    constexpr double fallingTime = 0.3;
    return std::exp(dBdrop / 20.0 * std::log(10.0) / (fallingTime * sampleRate));
}
