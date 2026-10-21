// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../source/effects/AVDexedFxChain.h"

#include <cmath>
#include <iostream>

bool runAVDexedStateTests();
bool runAVDexedLibraryTests();
bool AVRenderUIEvidence(const juce::File& directory);
bool AVRunLibraryQA(const juce::File& outputDirectory);
bool AVRenderBankVoice(const juce::File& bankFile, int slot);
bool AVScanBankVoices(const juce::File& folder);
bool AVAuditVoices(const juce::File& folder, const juce::File& output);
bool runAVDexedVoiceTests();
bool runAVDexedDropTests();
bool runAVDexedSysexTests();
bool runAVDexedLayerTests();
bool runAVDexedPerformanceTests();
int AVRunGolden(const juce::StringArray& arguments);
int AVRunImport(const juce::StringArray& arguments);
int AVRunPerformanceReport(const juce::File& file);
int AVRunLayersCpu(const juce::File& bankFile, int slot);
int AVRunUICpu(const juce::File& bankFile, int slot);
int AVRunUIResize(const juce::File& bankFile, int slot, const juce::File& outputDirectory);
int AVRunStackEnvelope(const juce::File& bankFile, int slot, bool inStep);
int AVRunLoaderReport(const juce::File& folder, const juce::File& output);

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
    if (argc == 3 && juce::String(argv[1]) == "--qa-library")
        return AVRunLibraryQA(juce::File(argv[2])) ? 0 : 1;
    if (argc == 4 && juce::String(argv[1]) == "--audit-voices")
        return AVAuditVoices(juce::File(argv[2]), juce::File(argv[3])) ? 0 : 1;
    if (argc == 3 && juce::String(argv[1]) == "--scan-voices")
        return AVScanBankVoices(juce::File(argv[2])) ? 0 : 1;
    if (argc == 4 && juce::String(argv[1]) == "--render-voice")
        return AVRenderBankVoice(juce::File(argv[2]), juce::String(argv[3]).getIntValue()) ? 0 : 1;
    if (argc == 4 && juce::String(argv[1]) == "--loader-report")
        return AVRunLoaderReport(juce::File(juce::String::fromUTF8(argv[2])), juce::File(juce::String::fromUTF8(argv[3])));
    if ((argc == 4 || argc == 5) && juce::String(argv[1]) == "--stack-envelope")
        return AVRunStackEnvelope(juce::File(juce::String::fromUTF8(argv[2])), juce::String(argv[3]).getIntValue(),
                                  argc == 5 && juce::String(argv[4]) == "in-step");
    if (argc == 4 && juce::String(argv[1]) == "--ui-cpu")
        return AVRunUICpu(juce::File(juce::String::fromUTF8(argv[2])), juce::String(argv[3]).getIntValue());
    if (argc == 5 && juce::String(argv[1]) == "--ui-resize")
        return AVRunUIResize(juce::File(juce::String::fromUTF8(argv[2])), juce::String(argv[3]).getIntValue(),
                             juce::File(juce::String::fromUTF8(argv[4])));
    if (argc == 4 && juce::String(argv[1]) == "--layers-cpu")
        return AVRunLayersCpu(juce::File(juce::String::fromUTF8(argv[2])), juce::String(argv[3]).getIntValue());
    if (argc == 3 && juce::String(argv[1]) == "--performances")
        return AVRunPerformanceReport(juce::File(juce::String::fromUTF8(argv[2])));
    if (argc >= 4 && juce::String(argv[1]) == "--import") {
        juce::StringArray arguments;
        for (int i = 2; i < argc; ++i) arguments.add(juce::String::fromUTF8(argv[i]));
        return AVRunImport(arguments);
    }
    if (argc >= 2 && juce::String(argv[1]) == "--golden") {
        juce::StringArray arguments;
        for (int i = 2; i < argc; ++i) arguments.add(juce::String::fromUTF8(argv[i]));
        return AVRunGolden(arguments);
    }
    // Every suite runs even after a failure, so one run lists every problem.
    bool passed = testDryPathAndZeroSamples();
    passed &= testDelayTailAfterSendIsRemoved();
    passed &= runAVDexedStateTests();
    passed &= runAVDexedLibraryTests();
    passed &= runAVDexedVoiceTests();
    passed &= runAVDexedDropTests();
    passed &= runAVDexedSysexTests();
    passed &= runAVDexedLayerTests();
    passed &= runAVDexedPerformanceTests();
    std::cout << (passed ? "AV Dexed regression tests passed.\n" : "AV Dexed regression tests FAILED.\n");
    return passed ? 0 : 1;
}
