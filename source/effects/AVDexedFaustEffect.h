// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include "AVDexedFaustArch.h"

#include <cstring>
#include <memory>

// Wrapper comun para efectos generados por Faust en AV Dexed.
//
// Contrato (mismo estilo que AVDexedReverb):
//  - El constructor crea la instancia generada en HEAP (unique_ptr): las
//    lineas de delay de Faust son arrays miembro de tamano fijo y pueden
//    pesar varios MB; en el stack (tests) o dentro de synth engine por valor
//    reventarian el tamano del objeto. Constructor = hilo de carga/UI,
//    nunca el audio thread.
//  - prepare() solo desde prepareToPlay (re-inicializa constantes de SR y
//    limpia colas). findZone() SOLO en prepare: cachear punteros de zona
//    esta permitido; buscarlos por string en audio esta prohibido.
//  - reset() limpia colas sin tocar parametros (instanceClear).
//  - process() es RT-safe: sin allocs, sin locks, in-place (los .dsp se
//    generan con -inpl).
template <typename FaustClass>
class AVDexedFaustEffect
{
public:
    AVDexedFaustEffect() : fx (std::make_unique<FaustClass>())
    {
        fx->init (44100);   // estado valido aunque un host defectuoso no llame prepareToPlay
    }

    void prepare (double sampleRate, int /*maximumBlockSize*/) noexcept
    {
        const auto sr = (sampleRate > 0.0 && std::isfinite (sampleRate))
                            ? static_cast<int> (sampleRate) : 44100;
        fx->init (sr);      // instanceConstants + reset de zonas + clear de colas
    }

    void reset() noexcept
    {
        fx->instanceClear();
    }

    // Busca la zona de un slider por etiqueta exacta. LLAMAR SOLO EN
    // prepare()/constructor (recorre buildUserInterface con strcmp).
    // Devuelve nullptr si el .dsp ya no expone esa etiqueta (fail-soft:
    // el llamador debe tolerar nullptr y el parametro queda inerte).
    float* findZone (const char* label) noexcept
    {
        ZoneFinder finder { label, nullptr };
        fx->buildUserInterface (&finder);
        return finder.found;
    }

    void process (juce::AudioBuffer<float>& buffer, int startSample, int numSamples) noexcept
    {
        if (numSamples <= 0 || buffer.getNumChannels() < 2)
            return;   // fail-soft, mismo criterio que AVDexedReverb::process

        float* channels[2] = { buffer.getWritePointer (0, startSample),
                               buffer.getWritePointer (1, startSample) };
        fx->compute (numSamples, channels, channels);
    }

private:
    struct ZoneFinder final : public UI
    {
        const char* wanted;
        float* found;

        ZoneFinder (const char* wantedIn, float* foundIn) : wanted (wantedIn), found (foundIn) {}

        void addHorizontalSlider (const char* label, FAUSTFLOAT* zone, FAUSTFLOAT,
                                  FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) override { match (label, zone); }
        void addVerticalSlider (const char* label, FAUSTFLOAT* zone, FAUSTFLOAT,
                                FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) override { match (label, zone); }
        void addNumEntry (const char* label, FAUSTFLOAT* zone, FAUSTFLOAT,
                          FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) override { match (label, zone); }
        void addCheckButton (const char* label, FAUSTFLOAT* zone) override { match (label, zone); }
        void addButton (const char* label, FAUSTFLOAT* zone) override { match (label, zone); }

        void match (const char* label, FAUSTFLOAT* zone) noexcept
        {
            if (found == nullptr && std::strcmp (label, wanted) == 0)
                found = zone;
        }
    };

    std::unique_ptr<FaustClass> fx;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AVDexedFaustEffect)
};
