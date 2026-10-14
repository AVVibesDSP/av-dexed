// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../source/effects/AVDexedFxChain.h"

#include <cmath>
#include <iostream>

bool runAVDexedStateTests();
bool AVRenderUIEvidence(const juce::File& directory);

namespace {
bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool testDryPathAndZeroSamples()
{
    AVDexedFxChain chain;
    chain.prepare(48000.0, 64);
    juce::AudioBuffer<float> buffer(2, 257);

    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            buffer.setSample(channel, sample, 0.2f * std::sin(0.03f * static_cast<float>(sample + channel)));

    juce::AudioBuffer<float> original;
    original.makeCopyOf(buffer);
    chain.process(buffer, 0, 120.0);
    bool zeroLengthUnchanged = true;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            zeroLengthUnchanged &= buffer.getSample(channel, sample) == original.getSample(channel, sample);

    chain.process(buffer, buffer.getNumSamples(), 120.0);
    bool dryIsBitExact = true;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            dryIsBitExact &= buffer.getSample(channel, sample) == original.getSample(channel, sample);

    return expect(zeroLengthUnchanged, "zero-sample processing leaves audio untouched")
        && expect(dryIsBitExact, "default FX path is bit-exact dry, including a host block larger than prepare size");
}

bool testDelayTailAfterSendIsRemoved()
{
    AVDexedFxChain chain;
    chain.prepare(48000.0, 64);
    chain.setDelaySync(false);
    chain.setDelayTimeLeft(20.0f);
    chain.setDelayTimeRight(20.0f);
    chain.setDelayFeedback(0.4f);
    chain.setDelayAmount(1.0f);

    juce::AudioBuffer<float> buffer(2, 64);
    buffer.clear();
    buffer.setSample(0, 0, 0.8f);
    buffer.setSample(1, 0, 0.8f);
    chain.process(buffer, 64, 120.0);
    chain.setDelayAmount(0.0f);

    const bool tailReported = chain.hasDelayTail();
    bool echoSurvivesZeroSend = false;
    for (int block = 0; block < 32; ++block) {
        buffer.clear();
        chain.process(buffer, 64, 120.0);
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < 64; ++sample)
                echoSurvivesZeroSend |= std::abs(buffer.getSample(channel, sample)) > 1.0e-6f;
    }

    return expect(tailReported, "host tail remains reported after send automation reaches zero")
        && expect(echoSurvivesZeroSend, "delay keeps processing and emits its stored echo after send reaches zero");
}
}

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    if (argc == 3 && juce::String(argv[1]) == "--render-ui")
        return AVRenderUIEvidence(juce::File(argv[2])) ? 0 : 1;
    const bool passed = testDryPathAndZeroSamples()
        && testDelayTailAfterSendIsRemoved()
        && runAVDexedStateTests();
    if (passed)
        std::cout << "AV Dexed regression tests passed.\n";
    return passed ? 0 : 1;
}
