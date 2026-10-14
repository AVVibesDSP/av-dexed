// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedChorus.h"
#include <cmath>
#include <algorithm>

namespace
{
float getModeRateScale (AVDexedChorus::Mode mode) noexcept
{
    if (mode == AVDexedChorus::Mode::mode1)
        return 0.70f;
    if (mode == AVDexedChorus::Mode::mode2)
        return 1.35f;
    return 1.0f;
}

float getModeDepthScale (AVDexedChorus::Mode mode) noexcept
{
    return mode == AVDexedChorus::Mode::mode1 ? 0.65f : 1.0f;
}

float getModeWaveBlend (AVDexedChorus::Mode mode) noexcept
{
    return mode == AVDexedChorus::Mode::mode1 ? 0.0f : 1.0f;
}

float getDualLfoBlend (AVDexedChorus::Mode mode) noexcept
{
    return mode == AVDexedChorus::Mode::mode1Plus2 ? 0.40f : 0.0f;
}
}

AVDexedChorus::AVDexedChorus() noexcept
{
    ringBufferL.resize (static_cast<size_t> (bufferSize), 0.0f);
    ringBufferR.resize (static_cast<size_t> (bufferSize), 0.0f);
    reset();
}

void AVDexedChorus::prepare (double sampleRate, int maximumBlockSize) noexcept
{
    juce::ignoreUnused (maximumBlockSize);
    currentSampleRate = std::isfinite (sampleRate) && sampleRate > 0.0 ? sampleRate : 44100.0;
    
    const int targetSize = juce::jmax (8192, juce::nextPowerOfTwo (juce::roundToInt (currentSampleRate * 0.10)));
    bufferSize = targetSize;
    ringBufferL.assign (static_cast<size_t> (bufferSize), 0.0f);
    ringBufferR.assign (static_cast<size_t> (bufferSize), 0.0f);

    rateSmoother.reset (currentSampleRate, 0.005);
    depthSmoother.reset (currentSampleRate, 0.005);
    mixSmoother.reset (currentSampleRate, 0.005);
    spreadSmoother.reset (currentSampleRate, 0.005);
    modeRateScaleSmoother.reset (currentSampleRate, 0.005);
    modeDepthScaleSmoother.reset (currentSampleRate, 0.005);
    modeWaveBlendSmoother.reset (currentSampleRate, 0.005);
    dualLfoBlendSmoother.reset (currentSampleRate, 0.005);

    reset();
}

void AVDexedChorus::reset() noexcept
{
    std::fill (ringBufferL.begin(), ringBufferL.end(), 0.0f);
    std::fill (ringBufferR.begin(), ringBufferR.end(), 0.0f);
    writePos = 0;
    phaseLFO1 = 0.0;
    phaseLFO2 = 0.0;

    rateSmoother.setCurrentAndTargetValue (getRateHz());
    depthSmoother.setCurrentAndTargetValue (getDepthMs());
    mixSmoother.setCurrentAndTargetValue (getMix());
    spreadSmoother.setCurrentAndTargetValue (getSpread());

    const auto activeMode = getMode();
    modeRateScaleSmoother.setCurrentAndTargetValue (getModeRateScale (activeMode));
    modeDepthScaleSmoother.setCurrentAndTargetValue (getModeDepthScale (activeMode));
    modeWaveBlendSmoother.setCurrentAndTargetValue (getModeWaveBlend (activeMode));
    dualLfoBlendSmoother.setCurrentAndTargetValue (getDualLfoBlend (activeMode));
}

void AVDexedChorus::setMode (Mode newMode) noexcept
{
    const int m = static_cast<int> (newMode);
    // La UI publica únicamente I, II e I+II. El antiguo valor 0 (Manual) y
    // cualquier valor corrupto migran fail-soft a I al restaurar sesiones.
    mode.store (m >= static_cast<int> (Mode::mode1)
                     && m <= static_cast<int> (Mode::mode1Plus2)
                    ? m : static_cast<int> (Mode::mode1),
                std::memory_order_relaxed);
}

void AVDexedChorus::setRateHz (float newRate) noexcept
{
    const float safeRate = std::isfinite (newRate)
        ? juce::jlimit (minRateHz, maxRateHz, newRate) : defaultRateHz;
    rateHz.store (safeRate, std::memory_order_relaxed);
}

void AVDexedChorus::setDepthMs (float newDepth) noexcept
{
    const float safeDepth = std::isfinite (newDepth)
        ? juce::jlimit (minDepthMs, maxDepthMs, newDepth) : defaultDepthMs;
    depthMs.store (safeDepth, std::memory_order_relaxed);
}

void AVDexedChorus::setMix (float newMix) noexcept
{
    const float safeMix = std::isfinite (newMix)
        ? juce::jlimit (minMix, maxMix, newMix) : defaultMix;
    mix.store (safeMix, std::memory_order_relaxed);
}

void AVDexedChorus::setSpread (float newSpread) noexcept
{
    const float safeSpread = std::isfinite (newSpread)
        ? juce::jlimit (minSpread, maxSpread, newSpread) : defaultSpread;
    spread.store (safeSpread, std::memory_order_relaxed);
}

float AVDexedChorus::getTriangleWave (double phase) const noexcept
{
    if (! std::isfinite (phase))
        return 0.0f;
    const double p = phase - std::floor (phase);
    if (p < 0.5)
        return static_cast<float> (4.0 * p - 1.0);
    return static_cast<float> (3.0 - 4.0 * p);
}

float AVDexedChorus::readInterpolatedSample (const std::vector<float>& buffer, double delaySamples) const noexcept
{
    if (buffer.empty() || bufferSize < 5 || writePos < 0 || writePos >= bufferSize
        || ! std::isfinite (delaySamples))
        return 0.0f;

    const double clampedDelay = juce::jlimit (1.0, static_cast<double> (bufferSize - 4), delaySamples);
    const int i = static_cast<int> (std::floor (clampedDelay));
    const double f = clampedDelay - static_cast<double> (i);

    const int p1 = ((writePos - i) % bufferSize + bufferSize) % bufferSize;
    const int p0 = (p1 + 1) % bufferSize;
    const int p2 = ((p1 - 1) % bufferSize + bufferSize) % bufferSize;
    const int p3 = ((p1 - 2) % bufferSize + bufferSize) % bufferSize;

    const float s0 = buffer[static_cast<size_t> (p0)];
    const float s1 = buffer[static_cast<size_t> (p1)];
    const float s2 = buffer[static_cast<size_t> (p2)];
    const float s3 = buffer[static_cast<size_t> (p3)];

    const float c0 = s1;
    const float c1 = 0.5f * (s2 - s0);
    const float c2 = s0 - 2.5f * s1 + 2.0f * s2 - 0.5f * s3;
    const float c3 = 0.5f * (s3 - s0) + 1.5f * (s1 - s2);

    return ((c3 * static_cast<float> (f) + c2) * static_cast<float> (f) + c1) * static_cast<float> (f) + c0;
}

void AVDexedChorus::process (juce::AudioBuffer<float>& buffer, int startSample, int numSamples) noexcept
{
    if (numSamples <= 0 || buffer.getNumChannels() == 0 || bufferSize <= 0
        || startSample < 0 || startSample >= buffer.getNumSamples())
        return;

    numSamples = juce::jmin (numSamples, buffer.getNumSamples() - startSample);

    const float currentMixTarget = getMix();
    if (currentMixTarget < 1.0e-5f && mixSmoother.getCurrentValue() < 1.0e-5f)
        return; // dry bit-exact passthrough

    const Mode activeMode = getMode();
    // Rate y Depth son controles reales en los tres modos. El carácter del
    // modo cambia la apertura estéreo (I/II) o la topología (I+II), nunca
    // sustituye silenciosamente el valor visible de los faders.
    const float targetRate = getRateHz();
    const float targetDepth = getDepthMs();

    const auto requestedSpread = getSpread();
    const float safeRate   = std::isfinite (targetRate)       ? juce::jlimit (minRateHz,  maxRateHz,  targetRate)       : defaultRateHz;
    const float safeDepth  = std::isfinite (targetDepth)      ? juce::jlimit (minDepthMs, maxDepthMs, targetDepth)      : defaultDepthMs;
    const float safeMix    = std::isfinite (currentMixTarget) ? juce::jlimit (minMix,     maxMix,     currentMixTarget) : defaultMix;
    const float safeSpread = std::isfinite (requestedSpread)  ? juce::jlimit (minSpread,  maxSpread,  requestedSpread)  : defaultSpread;

    rateSmoother.setTargetValue (safeRate);
    depthSmoother.setTargetValue (safeDepth);
    mixSmoother.setTargetValue (safeMix);
    spreadSmoother.setTargetValue (safeSpread);
    modeRateScaleSmoother.setTargetValue (getModeRateScale (activeMode));
    modeDepthScaleSmoother.setTargetValue (getModeDepthScale (activeMode));
    modeWaveBlendSmoother.setTargetValue (getModeWaveBlend (activeMode));
    dualLfoBlendSmoother.setTargetValue (getDualLfoBlend (activeMode));

    auto* left = buffer.getWritePointer (0, startSample);
    auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1, startSample) : nullptr;

    for (int i = 0; i < numSamples; ++i)
    {
        const float r   = juce::jlimit (minRateHz,  maxRateHz,  rateSmoother.getNextValue());
        const float dep = juce::jlimit (minDepthMs, maxDepthMs, depthSmoother.getNextValue());
        const float mx  = juce::jlimit (minMix,     maxMix,     mixSmoother.getNextValue());
        const float sp  = juce::jlimit (minSpread,  maxSpread,  spreadSmoother.getNextValue());

        // Los faders siguen gobernando los tres modos, pero cada pad aplica
        // un caracter clasico deliberadamente audible. I es mas lento,
        // menos profundo y senoidal; II es mas rapido, profundo y triangular;
        // I+II conserva los valores base y suma el segundo LFO. El perfil se
        // interpola durante 5 ms para que conmutar pads no mueva el tap de golpe.
        const float modeRate = juce::jlimit (minRateHz, maxRateHz,
                                             r * modeRateScaleSmoother.getNextValue());
        const float modeDepth = dep * modeDepthScaleSmoother.getNextValue();
        const float waveBlend = modeWaveBlendSmoother.getNextValue();
        const float dualBlend = dualLfoBlendSmoother.getNextValue();

        if (! std::isfinite (phaseLFO1) || phaseLFO1 < 0.0 || phaseLFO1 >= 1000.0)
            phaseLFO1 = 0.0;
        phaseLFO1 += static_cast<double> (modeRate) / currentSampleRate;
        if (phaseLFO1 >= 1.0)
            phaseLFO1 -= std::floor (phaseLFO1);

        const auto getMode1Wave = [] (double phase) noexcept
        {
            const double p = phase - std::floor (phase);
            return static_cast<float> (-std::cos (juce::MathConstants<double>::twoPi * p));
        };
        const double stereoPhase = static_cast<double> (sp) * (0.25 + 0.25 * waveBlend);
        const float sineL = getMode1Wave (phaseLFO1);
        const float sineR = getMode1Wave (phaseLFO1 + stereoPhase);
        const float triangleL = getTriangleWave (phaseLFO1);
        const float triangleR = getTriangleWave (phaseLFO1 + stereoPhase);
        float lfoValL = sineL + waveBlend * (triangleL - sineL);
        float lfoValR = sineR + waveBlend * (triangleR - sineR);

        if (! std::isfinite (phaseLFO2) || phaseLFO2 < 0.0 || phaseLFO2 >= 1000.0)
            phaseLFO2 = 0.0;
        phaseLFO2 += static_cast<double> (modeRate * 1.65f) / currentSampleRate;
        if (phaseLFO2 >= 1.0)
            phaseLFO2 -= std::floor (phaseLFO2);

        const float l2L = getTriangleWave (phaseLFO2);
        const float l2R = getTriangleWave (phaseLFO2 + 0.25);
        lfoValL += dualBlend * (l2L - lfoValL);
        lfoValR += dualBlend * (l2R - lfoValR);

        // Depth es una excursión bipolar. Con base=7 ms y depth<=5 ms el
        // delay queda entre 2 y 12 ms, sin clamp abrupto ni índices negativos.
        const double effectiveBaseMs = static_cast<double> (defaultDelayBaseMs);
        const double delaySamplesL = (effectiveBaseMs + static_cast<double> (lfoValL * modeDepth)) * currentSampleRate / 1000.0;
        const double delaySamplesR = (effectiveBaseMs + static_cast<double> (lfoValR * modeDepth)) * currentSampleRate / 1000.0;

        const float inL = left[i];
        const float inR = right != nullptr ? right[i] : inL;

        ringBufferL[static_cast<size_t> (writePos)] = std::isfinite (inL) ? inL : 0.0f;
        ringBufferR[static_cast<size_t> (writePos)] = std::isfinite (inR) ? inR : 0.0f;

        const float wetL = readInterpolatedSample (ringBufferL, delaySamplesL);
        const float wetR = readInterpolatedSample (ringBufferR, delaySamplesR);

        writePos = (writePos + 1) % bufferSize;

        const float outL = (1.0f - mx) * inL + mx * wetL;
        const float outR = (1.0f - mx) * inR + mx * wetR;

        left[i] = std::isfinite (outL) ? outL : 0.0f;
        if (right != nullptr)
            right[i] = std::isfinite (outR) ? outR : 0.0f;
    }
}
