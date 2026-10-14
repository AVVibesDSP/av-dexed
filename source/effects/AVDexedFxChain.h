// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "AVDexedChorus.h"
#include "AVDexedDelay.h"
#include "AVDexedReverb.h"

#include <memory>

/** Stereo post-synth effects. The dry path is kept once; delay and reverb
    process copies as wet-only sends, while chorus is an insert. */
class AVDexedFxChain final {
public:
    AVDexedFxChain();

    void prepare(double sampleRate, int maximumBlockSize) noexcept;
    void reset() noexcept;
    void process(juce::AudioBuffer<float>& stereoBuffer, int numSamples, double hostBpm) noexcept;

    void setReverbAmount(float value) noexcept;
    void setReverbSize(float value) noexcept { reverb.setDecay(value); }
    void setReverbPreDelay(float value) noexcept { reverb.setPredelay(value); }
    void setReverbDamping(float value) noexcept { reverb.setDamp(value); }

    void setDelayAmount(float value) noexcept;
    void setDelaySync(bool value) noexcept { delay->setSync(value); }
    void setDelayDivisionLeft(int value) noexcept { delay->setDivisionL(value); }
    void setDelayDivisionRight(int value) noexcept { delay->setDivisionR(value); }
    void setDelayTimeLeft(float value) noexcept { delay->setTimeMsL(value); }
    void setDelayTimeRight(float value) noexcept { delay->setTimeMsR(value); }
    void setDelayFeedback(float value) noexcept;
    void setDelayDamping(float value) noexcept { delay->setDamp(value); }

    void setChorusAmount(float value) noexcept;
    void setChorusMode(int value) noexcept;
    void setChorusRate(float value) noexcept { chorus.setRateHz(value); }
    void setChorusDepth(float value) noexcept { chorus.setDepthMs(value); }
    void setChorusSpread(float value) noexcept { chorus.setSpread(value); }
    void setReverbBypass(bool value) noexcept { avReverbBypass.store(value, std::memory_order_relaxed); }
    void setDelayBypass(bool value) noexcept { avDelayBypass.store(value, std::memory_order_relaxed); }
    void setChorusBypass(bool value) noexcept { avChorusBypass.store(value, std::memory_order_relaxed); }

    bool hasDelayTail() const noexcept;
    bool hasReverbTail() const noexcept;

private:
    AVDexedReverb reverb;
    AVDexedChorus chorus;
    std::unique_ptr<AVDexedDelay> delay;
    juce::AudioBuffer<float> sendBuffer;
    juce::AudioBuffer<float> effectBuffer;
    std::atomic<float> reverbAmount { 0.0f };
    std::atomic<float> delayAmount { 0.0f };
    std::atomic<float> chorusAmount { 0.0f };
    std::atomic<bool> avReverbBypass { false }, avDelayBypass { false }, avChorusBypass { false };
    juce::SmoothedValue<float> avDelaySendSmoother, avReverbSendSmoother;
    std::atomic<float> delayFeedbackValue { 0.35f };
    std::atomic<int64_t> delayTailSamplesRemaining { 0 };
    std::atomic<int64_t> reverbTailSamplesRemaining { 0 };
    double preparedSampleRate = 44100.0;
};
