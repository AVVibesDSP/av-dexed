// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include "AVDexedFaustEffect.h"
#include "AVDexedDelayFaust.h"

#include <atomic>

// Delay ping-pong adaptado para AV Dexed: tiempos L/R independientes,
// feedback universal y Dry/Wet de envio
// en vez de Mix).
//
// DSP: faust/AVDexedDelay.dsp -> AVDexedDelayFaust.h (Faust
// 2.85.9, commiteado). Topologia: entrada wet mono (promedio L+R), semilla
// en la linea L (timeMsL), feedback universal cruzado L->R->L con tope
// 0.90, one-pole de damp (DC unitario) solo en el lazo, de.sdelay para
// cambios de tiempo sin click/Doppler.
//
// Dry/Wet de ENVIO: out = dry*in + wet*soloEcos. El camino wet no contiene
// senal directa (primer tap en timeMsL), asi que dry=0/wet=1 es "solo el
// fx" y dry=1/wet=0 (defaults) es passthrough bit-exact -> arranque neutral
// (dry/wet en unidad conserva una ruta seca transparente).
//
// Este wrapper aporta lo que el .dsp no debe saber: sync a tempo (switch
// GLOBAL, division INDEPENDIENTE por lado), fallback determinista de BPM
// (invalido -> 120) y clamp del tiempo resultante a [20, 1500] ms.
//
// RT rules (mismas que AVDexedReverb): prepare()/reset() fuera del audio thread
// en vivo; process() sin allocs/locks/IO. Parametros por atomics desde UI;
// el audio thread copia atomics -> zonas Faust cacheadas en prepare().
class AVDexedDelay
{
public:
    AVDexedDelay();
    static constexpr size_t AVStorageBytes = sizeof(AVDexedDelayFaust);

    // Rango real del tiempo en ms (compartido con la UI, una sola verdad).
    static constexpr float minTimeMs = 20.0f;
    static constexpr float maxTimeMs = 1500.0f;

    // Divisiones de tiempo. El orden es el del selector de la UI y es
    // ESTABLE para persistencia (no reordenar sin migracion).
    enum class Division : int
    {
        sixteenth = 0,   // 1/16
        eighth,          // 1/8
        eighthDotted,    // 1/8D
        eighthTriplet,   // 1/8T
        quarter,         // 1/4
        quarterDotted,   // 1/4D
        quarterTriplet,  // 1/4T
        half             // 1/2
    };
    static constexpr int numDivisions = 8;

    /** Nombre corto de la division ("1/4", "1/8D", ...) para UI y popups.
        Unica fuente de verdad de los textos; indice fuera de rango se
        clampa (fail-soft). */
    static const char* divisionLabel (int divisionIndex) noexcept;

    void prepare (double sampleRate, int maximumBlockSize) noexcept;
    void reset() noexcept;

    // Parametros en vivo (hilo de UI). Clamp + NaN/Inf -> default, mismo
    // contrato que AVDexedReverb.
    void setSync (bool shouldSync) noexcept;         // global (un solo switch)
    void setDivisionL (int divisionIndex) noexcept;  // indice Division, clamped
    void setDivisionR (int divisionIndex) noexcept;
    void setTimeMsL (float ms) noexcept;             // modo libre (sync off)
    void setTimeMsR (float ms) noexcept;
    void setFeedback (float value) noexcept;         // 0..0.90, universal
    void setDamp (float value) noexcept;             // 0..1
    void setDry (float value) noexcept;              // 0..1, default 1 (senal intacta)
    void setWet (float value) noexcept;              // 0..1, default 0 (neutral)

    bool  getSync() const noexcept       { return sync.load (std::memory_order_relaxed); }
    int   getDivisionL() const noexcept  { return divisionL.load (std::memory_order_relaxed); }
    int   getDivisionR() const noexcept  { return divisionR.load (std::memory_order_relaxed); }
    float getTimeMsL() const noexcept    { return timeMsL.load (std::memory_order_relaxed); }
    float getTimeMsR() const noexcept    { return timeMsR.load (std::memory_order_relaxed); }
    float getFeedback() const noexcept   { return feedback.load (std::memory_order_relaxed); }
    float getDamp() const noexcept       { return damp.load (std::memory_order_relaxed); }
    float getDry() const noexcept        { return dry.load (std::memory_order_relaxed); }
    float getWet() const noexcept        { return wet.load (std::memory_order_relaxed); }

    // Tiempo efectivo por lado que aplicaria process() con este BPM
    // (para UI/tests).
    float effectiveTimeMsL (double hostBpm) const noexcept;
    float effectiveTimeMsR (double hostBpm) const noexcept;

    //==========================================================================
    // Audio thread. hostBpm puede venir invalido (NaN/0/negativo o host sin
    // tempo): fallback determinista a 120 BPM. Buffer < 2 canales: no-op.
    void process (juce::AudioBuffer<float>& buffer, int startSample, int numSamples,
                  double hostBpm) noexcept;

private:
    static double sanitizeBpm (double bpm) noexcept;
    static float divisionBeats (int divisionIndex) noexcept;
    float effectiveTimeMs (double hostBpm, int divisionIndex, float freeMs) const noexcept;
    void cacheZones() noexcept;

    AVDexedFaustEffect<AVDexedDelayFaust> fx;

    std::atomic<bool>  sync      { true };
    std::atomic<int>   divisionL { static_cast<int> (Division::quarter) };
    std::atomic<int>   divisionR { static_cast<int> (Division::quarter) };
    std::atomic<float> timeMsL   { 500.0f };
    std::atomic<float> timeMsR   { 500.0f };
    std::atomic<float> feedback  { 0.35f };
    std::atomic<float> damp      { 0.30f };
    std::atomic<float> dry       { 1.0f };   // senal original intacta por defecto
    std::atomic<float> wet       { 0.0f };   // neutral por defecto

    // Zonas Faust cacheadas en prepare() (nullptr = fail-soft inerte).
    float* timeMsLZone  = nullptr;
    float* timeMsRZone  = nullptr;
    float* feedbackZone = nullptr;
    float* dampZone     = nullptr;
    float* dryZone      = nullptr;
    float* wetZone      = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AVDexedDelay)
};
