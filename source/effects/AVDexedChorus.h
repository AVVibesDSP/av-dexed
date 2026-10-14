// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <vector>

/** Chorus estéreo de inspiración clásica Roland Juno (60/106).
    
    Características técnicas y sonoras:
    - Línea de retardo fraccional interpolada mediante polinomio de Hermite de 4 puntos.
    - LFO triangular compartido con inversión de fase L/R (0° y 180° por defecto cuando Spread = 1.0).
    - Modos Juno clásicos:
      - Mode I: LFO ligero; Rate y Depth siguen siendo controles reales.
      - Mode II: LFO más ancho/presente; Rate y Depth siguen siendo controles reales.
      - Mode I+II: doble LFO cruzado, gobernado por los mismos faders.
    - Mezcla estéreo estricta (Mix): Mix = 0.0 es passthrough seco bit-exacto sin latencia añadida.
    
    RT-safety garantizada: prepare()/reset() fuera del hilo de audio; process() no realiza
    asignaciones de memoria, bloqueos ni I/O. */
class AVDexedChorus
{
public:
    AVDexedChorus() noexcept;

    enum class Mode : int
    {
        manual     = 0, // legado de sesiones anteriores; setMode() lo migra a mode1
        mode1      = 1,
        mode2      = 2,
        mode1Plus2 = 3
    };

    static constexpr float minRateHz  = 0.05f;
    static constexpr float maxRateHz  = 5.0f;
    static constexpr float minDepthMs = 0.0f;
    static constexpr float maxDepthMs = 5.0f;
    static constexpr float minMix     = 0.0f;
    static constexpr float maxMix     = 1.0f;
    static constexpr float minSpread  = 0.0f;
    static constexpr float maxSpread  = 1.0f;

    static constexpr float defaultRateHz  = 0.5f;
    static constexpr float defaultDepthMs = 2.0f;
    static constexpr float defaultMix     = 0.5f;
    static constexpr float defaultSpread  = 1.0f;
    static constexpr float defaultDelayBaseMs = 7.0f;

    void prepare (double sampleRate, int maximumBlockSize) noexcept;
    void reset() noexcept;
    void process (juce::AudioBuffer<float>& buffer, int startSample, int numSamples) noexcept;

    // Setters atómicos y RT-safe (hilo de UI / host)
    void setMode (Mode mode) noexcept;
    void setRateHz (float rateHz) noexcept;
    void setDepthMs (float depthMs) noexcept;
    void setMix (float mix) noexcept;
    void setSpread (float spread) noexcept;

    Mode  getMode() const noexcept    { return static_cast<Mode> (mode.load (std::memory_order_relaxed)); }
    float getRateHz() const noexcept  { return rateHz.load (std::memory_order_relaxed); }
    float getDepthMs() const noexcept { return depthMs.load (std::memory_order_relaxed); }
    float getMix() const noexcept     { return mix.load (std::memory_order_relaxed); }
    float getSpread() const noexcept  { return spread.load (std::memory_order_relaxed); }

private:
    friend class AVDexedChorusTests;

    double currentSampleRate = 44100.0;
    std::atomic<int> mode { static_cast<int> (Mode::mode1) };
    std::atomic<float> rateHz { defaultRateHz };
    std::atomic<float> depthMs { defaultDepthMs };
    std::atomic<float> mix { defaultMix };
    std::atomic<float> spread { defaultSpread };

    std::vector<float> ringBufferL;
    std::vector<float> ringBufferR;
    int writePos = 0;
    int bufferSize = 8192;

    double phaseLFO1 = 0.0;
    double phaseLFO2 = 0.0;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rateSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> depthSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> spreadSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> modeRateScaleSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> modeDepthScaleSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> modeWaveBlendSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> dualLfoBlendSmoother;

    float getTriangleWave (double phase) const noexcept;
    float readInterpolatedSample (const std::vector<float>& buffer, double delaySamples) const noexcept;
};
