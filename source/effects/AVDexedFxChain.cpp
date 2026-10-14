// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedFxChain.h"

#include <cmath>

AVDexedFxChain::AVDexedFxChain() : delay(std::make_unique<AVDexedDelay>())
{
    reverb.setDry(0.0f);
    reverb.setWet(1.0f);
    delay->setDry(0.0f);
    delay->setWet(1.0f);
    chorus.setMix(0.0f);
}

void AVDexedFxChain::prepare(double sampleRate, int maximumBlockSize) noexcept
{
    const int safeBlockSize = juce::jmax(1, maximumBlockSize);
    preparedSampleRate = std::isfinite(sampleRate) && sampleRate > 0.0 ? sampleRate : 44100.0;
    sendBuffer.setSize(2, safeBlockSize, false, true, true);
    effectBuffer.setSize(2, safeBlockSize, false, true, true);
    reverb.prepare(preparedSampleRate, safeBlockSize);
    delay->prepare(preparedSampleRate, safeBlockSize);
    chorus.setMix(avChorusBypass.load(std::memory_order_relaxed)
                      ? 0.0f : chorusAmount.load(std::memory_order_relaxed));
    chorus.prepare(preparedSampleRate, safeBlockSize);
    avDelaySendSmoother.reset(preparedSampleRate, 0.02);
    avReverbSendSmoother.reset(preparedSampleRate, 0.02);
    avDelaySendSmoother.setCurrentAndTargetValue(avDelayBypass.load(std::memory_order_relaxed)
        ? 0.0f : delayAmount.load(std::memory_order_relaxed));
    avReverbSendSmoother.setCurrentAndTargetValue(avReverbBypass.load(std::memory_order_relaxed)
        ? 0.0f : reverbAmount.load(std::memory_order_relaxed));
    delayTailSamplesRemaining.store(0, std::memory_order_relaxed);
    reverbTailSamplesRemaining.store(0, std::memory_order_relaxed);
}

void AVDexedFxChain::reset() noexcept
{
    reverb.reset();
    delay->reset();
    chorus.reset();
    sendBuffer.clear();
    effectBuffer.clear();
    delayTailSamplesRemaining.store(0, std::memory_order_relaxed);
    reverbTailSamplesRemaining.store(0, std::memory_order_relaxed);
}

void AVDexedFxChain::setReverbAmount(float value) noexcept
{
    reverbAmount.store(std::isfinite(value) ? juce::jlimit(0.0f, 1.0f, value) : 0.0f,
                       std::memory_order_relaxed);
}

void AVDexedFxChain::setDelayAmount(float value) noexcept
{
    delayAmount.store(std::isfinite(value) ? juce::jlimit(0.0f, 1.0f, value) : 0.0f,
                      std::memory_order_relaxed);
}

void AVDexedFxChain::setDelayFeedback(float value) noexcept
{
    const float feedback = std::isfinite(value) ? juce::jlimit(0.0f, 0.90f, value) : 0.35f;
    delayFeedbackValue.store(feedback, std::memory_order_relaxed);
    delay->setFeedback(feedback);
}

void AVDexedFxChain::setChorusAmount(float value) noexcept
{
    const float amount = std::isfinite(value) ? juce::jlimit(0.0f, 1.0f, value) : 0.0f;
    chorusAmount.store(amount, std::memory_order_relaxed);
    chorus.setMix(avChorusBypass.load(std::memory_order_relaxed) ? 0.0f : amount);
}

void AVDexedFxChain::setChorusMode(int value) noexcept
{
    chorus.setMode(static_cast<AVDexedChorus::Mode>(juce::jlimit(0, 2, value) + 1));
}

void AVDexedFxChain::process(juce::AudioBuffer<float>& buffer, int numSamples, double hostBpm) noexcept
{
    if (numSamples <= 0 || buffer.getNumChannels() < 2 || sendBuffer.getNumSamples() <= 0)
        return;

    const int sampleCount = juce::jmin(numSamples, buffer.getNumSamples());
    chorus.setMix(avChorusBypass.load(std::memory_order_relaxed)
                      ? 0.0f : chorusAmount.load(std::memory_order_relaxed));
    chorus.process(buffer, 0, sampleCount);

    const float delaySend = avDelayBypass.load(std::memory_order_relaxed)
        ? 0.0f : delayAmount.load(std::memory_order_relaxed);
    const float reverbSend = avReverbBypass.load(std::memory_order_relaxed)
        ? 0.0f : reverbAmount.load(std::memory_order_relaxed);
    avDelaySendSmoother.setTargetValue(delaySend);
    avReverbSendSmoother.setTargetValue(reverbSend);
    const bool processDelay = delaySend > 1.0e-5f
        || avDelaySendSmoother.isSmoothing()
        || delayTailSamplesRemaining.load(std::memory_order_relaxed) > 0;
    const bool processReverb = reverbSend > 1.0e-5f
        || avReverbSendSmoother.isSmoothing()
        || reverbTailSamplesRemaining.load(std::memory_order_relaxed) > 0;
    if (!processDelay && !processReverb)
        return;

    if (delaySend > 1.0e-5f) {
        const double tailSeconds = delayFeedbackValue.load(std::memory_order_relaxed) > 1.0e-5f
            ? 50.0 : 1.5;
        delayTailSamplesRemaining.store(static_cast<int64_t>(preparedSampleRate * tailSeconds),
                                        std::memory_order_relaxed);
    }
    if (reverbSend > 1.0e-5f)
        reverbTailSamplesRemaining.store(static_cast<int64_t>(preparedSampleRate * 18.0),
                                         std::memory_order_relaxed);

    int processed = 0;
    while (processed < sampleCount) {
        const int chunk = juce::jmin(sampleCount - processed, sendBuffer.getNumSamples());
        for (int channel = 0; channel < 2; ++channel) {
            const auto* input = buffer.getReadPointer(channel, processed);
            auto* send = sendBuffer.getWritePointer(channel);
            for (int i = 0; i < chunk; ++i)
                send[i] = input[i];
        }

        auto* outL = buffer.getWritePointer(0, processed);
        auto* outR = buffer.getWritePointer(1, processed);

        if (processDelay) {
            for (int sample = 0; sample < chunk; ++sample) {
                const float send = avDelaySendSmoother.getNextValue();
                for (int channel = 0; channel < 2; ++channel)
                    effectBuffer.setSample(channel, sample, sendBuffer.getSample(channel, sample) * send);
            }
            delay->process(effectBuffer, 0, chunk, hostBpm);
            const auto* wetL = effectBuffer.getReadPointer(0);
            const auto* wetR = effectBuffer.getReadPointer(1);
            for (int i = 0; i < chunk; ++i) {
                outL[i] += wetL[i];
                outR[i] += wetR[i];
            }
        }

        if (processReverb) {
            for (int sample = 0; sample < chunk; ++sample) {
                const float send = avReverbSendSmoother.getNextValue();
                for (int channel = 0; channel < 2; ++channel)
                    effectBuffer.setSample(channel, sample, sendBuffer.getSample(channel, sample) * send);
            }
            reverb.process(effectBuffer, 0, chunk);
            const auto* wetL = effectBuffer.getReadPointer(0);
            const auto* wetR = effectBuffer.getReadPointer(1);
            for (int i = 0; i < chunk; ++i) {
                outL[i] += wetL[i];
                outR[i] += wetR[i];
            }
        }

        if (delaySend <= 1.0e-5f) {
            const auto remaining = delayTailSamplesRemaining.load(std::memory_order_relaxed);
            delayTailSamplesRemaining.store(juce::jmax<int64_t>(0, remaining - chunk),
                                            std::memory_order_relaxed);
        }
        if (reverbSend <= 1.0e-5f) {
            const auto remaining = reverbTailSamplesRemaining.load(std::memory_order_relaxed);
            reverbTailSamplesRemaining.store(juce::jmax<int64_t>(0, remaining - chunk),
                                             std::memory_order_relaxed);
        }
        processed += chunk;
    }
}

bool AVDexedFxChain::hasDelayTail() const noexcept
{
    return (!avDelayBypass.load(std::memory_order_relaxed) && delayAmount.load(std::memory_order_relaxed) > 1.0e-5f)
        || delayTailSamplesRemaining.load(std::memory_order_relaxed) > 0;
}

bool AVDexedFxChain::hasReverbTail() const noexcept
{
    return (!avReverbBypass.load(std::memory_order_relaxed) && reverbAmount.load(std::memory_order_relaxed) > 1.0e-5f)
        || reverbTailSamplesRemaining.load(std::memory_order_relaxed) > 0;
}
