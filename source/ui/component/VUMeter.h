/*
  ==============================================================================

    VUMeter.h
    Based on the original VUMeter by Fulop Nandor (created 29 Jun 2024)
    for Dexed. Rewritten for AV Dexed by AV Vibes, 2026.

  ==============================================================================
*/

#pragma once

#include "JuceHeader.h"

class VuMeterBase : public juce::Component {
public:
    static constexpr int totalBlocks = 46;
    static constexpr int blockGap = 1;

    explicit VuMeterBase(int maximumDb, bool useTricolor = false);
    void paint(juce::Graphics& g) override;
    /** Shows `level`; repaints only when the number of lit blocks changes
        (the meter moves in 1 dB steps, so most updates change nothing). */
    void setLevel(float level);

    float v = 0.0f;

private:
    int activeBlocksFor(float level) const;
    int maximumDb;
    int minimumDb;
    bool tricolor;
};

class VuMeter final : public VuMeterBase {
public:
    VuMeter() : VuMeterBase(0) {}
};

class VuMeterOutput final : public VuMeterBase {
public:
    static constexpr int numRedBlocks = 6;
    static constexpr int numYellowBlocks = 6;

    VuMeterOutput() : VuMeterBase(6, true) {}
    static double getDecayFactor(double sampleRate);
};
