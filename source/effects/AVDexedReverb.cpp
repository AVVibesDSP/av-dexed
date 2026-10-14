// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedReverb.h"

#include <algorithm>
#include <cmath>

namespace
{
    // Rampa de 20 ms: suficiente para no hacer zipper al arrastrar un knob, sin notarse como
    // "lag" del control.
    constexpr double predelayRampSeconds = 0.020;
    constexpr double dryRampSeconds      = 0.020;
}

void AVDexedReverb::prepare (double sampleRateIn, int maximumBlockSize) noexcept
{
    engineSampleRate = sampleRateIn > 0.0 && std::isfinite (sampleRateIn) ? sampleRateIn : 44100.0;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = engineSampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (juce::jmax (1, maximumBlockSize));
    spec.numChannels      = 2;
    tank.prepare (spec);

    scratchCapacity = juce::jmax (1, maximumBlockSize);
    wetScratch.setSize (2, scratchCapacity, false, true, true);

    predelaySizeSamples = juce::jmax (1, juce::roundToInt (engineSampleRate * (maxPredelayMs / 1000.0)));
    predelayBufferL.assign (static_cast<size_t> (predelaySizeSamples), 0.0f);
    predelayBufferR.assign (static_cast<size_t> (predelaySizeSamples), 0.0f);
    predelayWriteIndex = 0;

    smoothedPredelaySamples.reset (engineSampleRate, predelayRampSeconds);
    smoothedDry.reset (engineSampleRate, dryRampSeconds);
    smoothedPredelaySamples.setCurrentAndTargetValue (
        predelay.load (std::memory_order_relaxed) * maxPredelayMs * 0.001f * static_cast<float> (engineSampleRate));
    smoothedDry.setCurrentAndTargetValue (dry.load (std::memory_order_relaxed));

    reset();
}

void AVDexedReverb::reset() noexcept
{
    tank.reset();
    std::fill (predelayBufferL.begin(), predelayBufferL.end(), 0.0f);
    std::fill (predelayBufferR.begin(), predelayBufferR.end(), 0.0f);
    predelayWriteIndex = 0;
}

void AVDexedReverb::setDry (float value) noexcept
{
    dry.store (std::isfinite (value) ? juce::jlimit (0.0f, 1.0f, value) : 0.55f, std::memory_order_relaxed);
}

void AVDexedReverb::setWet (float value) noexcept
{
    wet.store (std::isfinite (value) ? juce::jlimit (0.0f, 1.0f, value) : 0.55f, std::memory_order_relaxed);
}

void AVDexedReverb::setPredelay (float value) noexcept
{
    predelay.store (std::isfinite (value) ? juce::jlimit (0.0f, 1.0f, value) : defaultPredelay, std::memory_order_relaxed);
}

void AVDexedReverb::setDecay (float value) noexcept
{
    decay.store (std::isfinite (value) ? juce::jlimit (0.0f, 1.0f, value) : defaultDecay, std::memory_order_relaxed);
}

void AVDexedReverb::setDamp (float value) noexcept
{
    damp.store (std::isfinite (value) ? juce::jlimit (0.0f, 1.0f, value) : defaultDamp, std::memory_order_relaxed);
}

void AVDexedReverb::process (juce::AudioBuffer<float>& buffer, int startSample, int numSamples) noexcept
{
    // prepare() siempre deja scratchCapacity/predelaySizeSamples > 0; esta
    // guarda es fail-soft si un host defectuoso llama processBlock antes de
    // prepareToPlay.
    if (numSamples <= 0 || buffer.getNumChannels() < 2 || scratchCapacity <= 0)
        return;

    juce::dsp::Reverb::Parameters params;
    params.roomSize   = juce::jlimit (0.0f, 1.0f, decay.load (std::memory_order_relaxed));
    params.damping    = juce::jlimit (0.0f, 1.0f, damp.load (std::memory_order_relaxed));
    params.wetLevel   = juce::jlimit (0.0f, 1.0f, wet.load (std::memory_order_relaxed));
    params.dryLevel   = 0.0f;   // el dry va por fuera (ver header) -- el predelay no debe demorarlo
    params.width      = 1.0f;   // ancho estereo propio del tanque, fijo (no es uno de los 5 parametros)
    params.freezeMode = 0.0f;   // nunca expuesto: freeze sostendria la cola para siempre
    tank.setParameters (params);

    smoothedDry.setTargetValue (juce::jlimit (0.0f, 1.0f, dry.load (std::memory_order_relaxed)));

    const auto predelaySamplesTarget = predelay.load (std::memory_order_relaxed) * maxPredelayMs
                                      * 0.001f * static_cast<float> (engineSampleRate);
    smoothedPredelaySamples.setTargetValue (predelaySamplesTarget);

    auto* left  = buffer.getWritePointer (0, startSample);
    auto* right = buffer.getWritePointer (1, startSample);

    int processed = 0;

    while (processed < numSamples)
    {
        const int chunk = juce::jmin (numSamples - processed, scratchCapacity);

        auto* wetL = wetScratch.getWritePointer (0);
        auto* wetR = wetScratch.getWritePointer (1);

        // Predelay: escribe el sample ACTUAL en el ring antes de leer, asi
        // delaySamples==0 lee lo que se acaba de escribir (cero delay real,
        // no una vuelta entera del buffer). wetL/wetR quedan con la copia
        // demorada que alimenta al tanque; left/right (el buffer real)
        // quedan intactos para el mezclado dry de mas abajo.
        for (int i = 0; i < chunk; ++i)
        {
            predelayBufferL[static_cast<size_t> (predelayWriteIndex)] = left[processed + i];
            predelayBufferR[static_cast<size_t> (predelayWriteIndex)] = right[processed + i];

            const int delaySamples = juce::jlimit (0, predelaySizeSamples - 1,
                                                    juce::roundToInt (smoothedPredelaySamples.getNextValue()));
            int readIndex = predelayWriteIndex - delaySamples;
            while (readIndex < 0)
                readIndex += predelaySizeSamples;

            wetL[i] = predelayBufferL[static_cast<size_t> (readIndex)];
            wetR[i] = predelayBufferR[static_cast<size_t> (readIndex)];

            if (++predelayWriteIndex >= predelaySizeSamples)
                predelayWriteIndex = 0;
        }

        juce::dsp::AudioBlock<float> block (wetScratch);
        auto subBlock = block.getSubBlock (0, static_cast<size_t> (chunk));
        juce::dsp::ProcessContextReplacing<float> context (subBlock);
        tank.process (context);

        for (int i = 0; i < chunk; ++i)
        {
            const int idx = processed + i;
            const float dryGain = smoothedDry.getNextValue();
            left[idx]  = dryGain * left[idx]  + wetL[i];
            right[idx] = dryGain * right[idx] + wetR[i];
        }

        processed += chunk;
    }
}
