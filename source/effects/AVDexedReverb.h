// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <vector>

// Reverb de AV Dexed (5 parametros: Dry/Wet/Predelay/Decay/Damp).
//
// Tanque: juce::dsp::Reverb, wrapper de juce::Reverb -- el algoritmo clasico
// "Freeverb" (8 combs + 4 allpass por canal, tunings de Jezar/Dreampoint,
// dominio publico, ver juce_audio_basics/utilities/
// juce_Reverb.h de JUCE). Investigacion previa (comparacion Freeverb vs Dattorro/
// plate, jul 2026): Freeverb es la opcion mas liviana de CPU con buena
// calidad sonora, y ya viene linkeada en este proyecto (juce::juce_dsp, ver
// CMakeLists.txt) -- cero dependencias nuevas. Dattorro/plate suena mas lujoso pero exige una red
// de allpass modulados hecha a mano (mas superficie de bug en tiempo real);
// se descarta aca por prudencia, no por desconocerla.
//
// This implementation uses the JUCE DSP module already included by the build.
//
// Predelay: juce::dsp::Reverb no trae una linea de predelay propia, asi que
// se implementa aca con un ring buffer estereo de tamano fijo reservado en
// prepare(). El predelay SOLO
// retrasa lo que entra al tanque -- el dry sigue el camino inmediato sin
// pasar por el ring -- que es el comportamiento estandar de un control de
// predelay (separar el ataque seco de la cola reverberada).
//
// Reglas de tiempo real: prepare()/reset() son las unicas
// funciones que tocan memoria o limpian buffers; process() nunca asigna,
// nunca bloquea, nunca toca archivos -- solo aritmetica y accesos contiguos
// a buffers ya reservados.
class AVDexedReverb
{
public:
    AVDexedReverb() = default;

    // Tope real del predelay en ms (0..1 del knob -> 0..maxPredelayMs).
    // Publico para que la UI (la burbuja de valor del knob) pueda mostrar el valor real en ms sin duplicar la
    // constante -- una sola fuente de verdad para el rango del parametro.
    static constexpr float maxPredelayMs = 250.0f;

    // AV Dexed default: pre-delay 18 % (= 45 ms),
    // caída 21 % y absorción 12 %; los mismos números son el fallback por NaN.
    static constexpr float defaultPredelay = 0.18f;
    static constexpr float defaultDecay    = 0.21f;
    static constexpr float defaultDamp     = 0.12f;

    /** Reserva los buffers (predelay + scratch del tanque) para esta
        sampleRate/tamano de bloque. Llamar solo desde prepareToPlay, nunca desde el audio thread en vivo. */
    void prepare (double sampleRate, int maximumBlockSize) noexcept;

    /** Limpia colas/estado sin reasignar memoria (instrument swap / All
        Sound Off). */
    void reset() noexcept;

    // Parametros en vivo, todos 0..1 (mismo rango que los sliders/knobs de la
    // UI, que pasan su valor directo sin reescalar). Atomics: seguros de
    // llamar desde el hilo de UI mientras el audio thread corre process().
    void setDry (float value) noexcept;
    void setWet (float value) noexcept;
    void setPredelay (float value) noexcept;
    void setDecay (float value) noexcept;
    void setDamp (float value) noexcept;

    float getDry() const noexcept      { return dry.load (std::memory_order_relaxed); }
    float getWet() const noexcept      { return wet.load (std::memory_order_relaxed); }
    float getPredelay() const noexcept { return predelay.load (std::memory_order_relaxed); }
    float getDecay() const noexcept    { return decay.load (std::memory_order_relaxed); }
    float getDamp() const noexcept     { return damp.load (std::memory_order_relaxed); }

    //==========================================================================
    // Audio thread. buffer debe tener al menos 2 canales; si no, no hace nada
    // (fail-soft).
    void process (juce::AudioBuffer<float>& buffer, int startSample, int numSamples) noexcept;

private:
    juce::dsp::Reverb tank;

    std::atomic<float> dry      { 0.55f };
    std::atomic<float> wet      { 0.55f };
    std::atomic<float> predelay { defaultPredelay };
    std::atomic<float> decay    { defaultDecay };
    std::atomic<float> damp     { defaultDamp };

    // Predelay: ring buffer estereo propio, tamano fijo = maxPredelayMs a la
    // sampleRate actual. Solo prepare()/reset() lo tocan fuera de process().
    std::vector<float> predelayBufferL, predelayBufferR;
    int predelaySizeSamples = 1;
    int predelayWriteIndex  = 0;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedPredelaySamples;

    // Dry externo (el dryLevel interno de juce::dsp::Reverb se deja en 0 --
    // ver .cpp -- porque el predelay no debe demorar el dry). wetLevel/
    // roomSize/damping SI usan el suavizado interno de juce::Reverb
    // (SmoothedValue de 10ms ya incluido en setParameters/setSampleRate), asi
    // que no se duplica aca.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedDry;

    // Scratch del tanque: buffer estereo preasignado en prepare() para poder
    // predelayar/reverberar sin tocar el buffer real (que debe conservar el
    // dry intacto). El host deberia respetar samplesPerBlock siempre, pero
    // process() trocea en capacityChunks igual por si acaso -- mas barato que
    // arriesgar un desborde de buffer.
    juce::AudioBuffer<float> wetScratch;
    int scratchCapacity = 0;

    double engineSampleRate = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AVDexedReverb)
};
