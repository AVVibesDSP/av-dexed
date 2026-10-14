// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedDelay.h"

#include <cmath>

namespace
{
    // Beats (negras) por division, orden EXACTO del enum Division:
    // 1/16, 1/8, 1/8D, 1/8T, 1/4, 1/4D, 1/4T, 1/2.
    constexpr float kDivisionBeats[AVDexedDelay::numDivisions] =
    {
        0.25f,
        0.5f,
        0.75f,
        1.0f / 3.0f,
        1.0f,
        1.5f,
        2.0f / 3.0f,
        2.0f
    };

    constexpr double kFallbackBpm = 120.0;   // standalone / host sin tempo valido
    constexpr double kMinBpm      = 20.0;
    constexpr double kMaxBpm      = 999.0;
}

AVDexedDelay::AVDexedDelay()
{
    // Las zonas tambien se cachean aqui (ademas de prepare) para que un
    // host defectuoso que procese sin prepareToPlay no encuentre nullptr.
    cacheZones();
}

void AVDexedDelay::cacheZones() noexcept
{
    timeMsLZone  = fx.findZone ("timeMsL");
    timeMsRZone  = fx.findZone ("timeMsR");
    feedbackZone = fx.findZone ("feedback");
    dampZone     = fx.findZone ("damp");
    dryZone      = fx.findZone ("dry");
    wetZone      = fx.findZone ("wet");
}

void AVDexedDelay::prepare (double sampleRate, int maximumBlockSize) noexcept
{
    fx.prepare (sampleRate, maximumBlockSize);

    // init() resetea las zonas a los defaults del .dsp; los punteros no
    // cambian (arrays miembro), pero se refrescan por si el generado cambia.
    cacheZones();

    // Asienta los si.smoo del .dsp (instanceClear los deja en CERO, no en
    // el valor del slider) procesando ~300 ms de silencio: sin esto cada
    // prepareToPlay producia un fade-in de ~50 ms del DRY sobre toda la
    // senal (defecto detectado por los tests de Pan/Width a 2 kHz). Con
    // entrada de silencio las lineas de delay permanecen en cero; solo
    // convergen los smoothers hacia los atomics actuales (~19 taus -> el
    // punto fijo en float, a <=1 ulp del target). prepare() NO es el audio
    // thread: procesar aqui es legal y cuesta <2 ms hasta a 192 kHz (mismo
    // criterio que AVDexedReverb, que asienta sus SmoothedValue en prepare()).
    const auto sr = (sampleRate > 0.0 && std::isfinite (sampleRate)) ? sampleRate : 44100.0;
    juce::AudioBuffer<float> scratch (2, 512);
    const int settleSamples = juce::roundToInt (sr * 0.3);
    for (int done = 0; done < settleSamples; done += scratch.getNumSamples())
    {
        scratch.clear();
        process (scratch, 0, scratch.getNumSamples(), 120.0);
    }
}

void AVDexedDelay::reset() noexcept
{
    fx.reset();
}

void AVDexedDelay::setSync (bool shouldSync) noexcept
{
    sync.store (shouldSync, std::memory_order_relaxed);
}

void AVDexedDelay::setDivisionL (int divisionIndex) noexcept
{
    divisionL.store (juce::jlimit (0, numDivisions - 1, divisionIndex), std::memory_order_relaxed);
}

void AVDexedDelay::setDivisionR (int divisionIndex) noexcept
{
    divisionR.store (juce::jlimit (0, numDivisions - 1, divisionIndex), std::memory_order_relaxed);
}

void AVDexedDelay::setTimeMsL (float ms) noexcept
{
    timeMsL.store (std::isfinite (ms) ? juce::jlimit (minTimeMs, maxTimeMs, ms) : 500.0f,
                   std::memory_order_relaxed);
}

void AVDexedDelay::setTimeMsR (float ms) noexcept
{
    timeMsR.store (std::isfinite (ms) ? juce::jlimit (minTimeMs, maxTimeMs, ms) : 500.0f,
                   std::memory_order_relaxed);
}

void AVDexedDelay::setFeedback (float value) noexcept
{
    feedback.store (std::isfinite (value) ? juce::jlimit (0.0f, 0.90f, value) : 0.35f,
                    std::memory_order_relaxed);
}

void AVDexedDelay::setDamp (float value) noexcept
{
    damp.store (std::isfinite (value) ? juce::jlimit (0.0f, 1.0f, value) : 0.30f,
                std::memory_order_relaxed);
}

void AVDexedDelay::setDry (float value) noexcept
{
    dry.store (std::isfinite (value) ? juce::jlimit (0.0f, 1.0f, value) : 1.0f,
               std::memory_order_relaxed);
}

void AVDexedDelay::setWet (float value) noexcept
{
    wet.store (std::isfinite (value) ? juce::jlimit (0.0f, 1.0f, value) : 0.0f,
               std::memory_order_relaxed);
}

const char* AVDexedDelay::divisionLabel (int divisionIndex) noexcept
{
    // Mismo orden EXACTO que el enum Division (estable para persistencia).
    static const char* const names[numDivisions] =
        { "1/16", "1/8", "1/8D", "1/8T", "1/4", "1/4D", "1/4T", "1/2" };

    return names[juce::jlimit (0, numDivisions - 1, divisionIndex)];
}

double AVDexedDelay::sanitizeBpm (double bpm) noexcept
{
    if (! std::isfinite (bpm) || bpm < kMinBpm || bpm > kMaxBpm)
        return kFallbackBpm;
    return bpm;
}

float AVDexedDelay::divisionBeats (int divisionIndex) noexcept
{
    return kDivisionBeats[juce::jlimit (0, numDivisions - 1, divisionIndex)];
}

float AVDexedDelay::effectiveTimeMs (double hostBpm, int divisionIndex, float freeMs) const noexcept
{
    if (! sync.load (std::memory_order_relaxed))
        return freeMs;

    const double bpm   = sanitizeBpm (hostBpm);
    const double beats = divisionBeats (divisionIndex);
    const double ms    = beats * 60000.0 / bpm;   // negra = 60000/bpm ms

    // Tope: divisiones largas con BPM lento no desbordan la linea.
    return juce::jlimit (minTimeMs, maxTimeMs, static_cast<float> (ms));
}

float AVDexedDelay::effectiveTimeMsL (double hostBpm) const noexcept
{
    return effectiveTimeMs (hostBpm,
                            divisionL.load (std::memory_order_relaxed),
                            timeMsL.load (std::memory_order_relaxed));
}

float AVDexedDelay::effectiveTimeMsR (double hostBpm) const noexcept
{
    return effectiveTimeMs (hostBpm,
                            divisionR.load (std::memory_order_relaxed),
                            timeMsR.load (std::memory_order_relaxed));
}

void AVDexedDelay::process (juce::AudioBuffer<float>& buffer, int startSample, int numSamples,
                       double hostBpm) noexcept
{
    if (numSamples <= 0 || buffer.getNumChannels() < 2)
        return;

    // Copia atomics -> zonas Faust (punteros cacheados; nunca busqueda por
    // string aqui). El .dsp suaviza fb/damp/dry/wet (si.smoo) y los tiempos
    // entran a de.sdelay, que hace el cambio con crossfade sin click ni
    // Doppler.
    if (timeMsLZone != nullptr)
        *timeMsLZone = effectiveTimeMsL (hostBpm);
    if (timeMsRZone != nullptr)
        *timeMsRZone = effectiveTimeMsR (hostBpm);
    if (feedbackZone != nullptr)
        *feedbackZone = feedback.load (std::memory_order_relaxed);
    if (dampZone != nullptr)
        *dampZone = damp.load (std::memory_order_relaxed);
    if (dryZone != nullptr)
        *dryZone = dry.load (std::memory_order_relaxed);
    if (wetZone != nullptr)
        *wetZone = wet.load (std::memory_order_relaxed);

    fx.process (buffer, startSample, numSamples);
}
