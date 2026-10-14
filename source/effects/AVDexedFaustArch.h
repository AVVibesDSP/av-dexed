// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Arquitectura minima propia para el C++ generado por Faust en AV Dexed.
//
// El compilador Faust (version 2.85.9) emite una clase
// `class X : public dsp` que referencia tres interfaces: `dsp`, `UI` y
// `Meta`. Las versiones oficiales viven en los architecture files de Faust
// (faust/dsp/dsp.h, faust/gui/UI.h, faust/gui/meta.h, licencia LGPL con
// excepcion de embebido); este header es una reescritura minima y propia de
// esas interfaces -- solo lo que el codigo generado realmente usa -- para
// que AV Dexed no dependa de headers instalados de Faust en el build normal.
//
// El build normal consume los headers generados que ya estan en Git; solo
// regenerarlos (comando en faust/AVDexedDelay.dsp) necesita el compilador Faust.
//
// RT-safety: nada aqui aloca ni bloquea. El codigo generado tampoco: sus
// lineas de delay son arrays miembro de tamano fijo. OJO: por eso mismo las
// instancias generadas pueden pesar varios MB -- crearlas SIEMPRE en heap
// via AVDexedFaustEffect (unique_ptr en el constructor, nunca en el stack ni en
// el audio thread).

#ifndef FAUSTFLOAT
#define FAUSTFLOAT float
#endif

struct Soundfile;   // nunca usado por los DSP de DEXED (sin addSoundfile)

// Interfaz de metadatos (declare name/license/... del .dsp).
struct Meta
{
    virtual ~Meta() = default;
    virtual void declare (const char* key, const char* value) = 0;
};

// Interfaz de UI que el codigo generado recorre en buildUserInterface().
// AV Dexed no construye UI real con esto: AVDexedFaustEffect la usa una sola vez
// en prepare() para cachear los punteros a las zonas de parametros
// (prohibido buscar zonas por string en el audio thread).
struct UI
{
    virtual ~UI() = default;

    // layout
    virtual void openTabBox (const char* label)        { (void) label; }
    virtual void openHorizontalBox (const char* label) { (void) label; }
    virtual void openVerticalBox (const char* label)   { (void) label; }
    virtual void closeBox() {}

    // controles activos
    virtual void addButton (const char* label, FAUSTFLOAT* zone)      { (void) label; (void) zone; }
    virtual void addCheckButton (const char* label, FAUSTFLOAT* zone) { (void) label; (void) zone; }
    virtual void addVerticalSlider (const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init,
                                    FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step)
    { (void) label; (void) zone; (void) init; (void) min; (void) max; (void) step; }
    virtual void addHorizontalSlider (const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init,
                                      FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step)
    { (void) label; (void) zone; (void) init; (void) min; (void) max; (void) step; }
    virtual void addNumEntry (const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init,
                              FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step)
    { (void) label; (void) zone; (void) init; (void) min; (void) max; (void) step; }

    // controles pasivos (medidores)
    virtual void addHorizontalBargraph (const char* label, FAUSTFLOAT* zone,
                                        FAUSTFLOAT min, FAUSTFLOAT max)
    { (void) label; (void) zone; (void) min; (void) max; }
    virtual void addVerticalBargraph (const char* label, FAUSTFLOAT* zone,
                                      FAUSTFLOAT min, FAUSTFLOAT max)
    { (void) label; (void) zone; (void) min; (void) max; }

    // soundfiles (no usados en DEXED)
    virtual void addSoundfile (const char* label, const char* filename, Soundfile** sf_zone)
    { (void) label; (void) filename; (void) sf_zone; }

    // metadatos por-zona ([unit:ms], etc.)
    virtual void declare (FAUSTFLOAT* zone, const char* key, const char* value)
    { (void) zone; (void) key; (void) value; }
};

// Clase base de los DSP generados. Mismos metodos (nombres y firmas) que
// faust/dsp/dsp.h.
//
// NOMBRE: NO se llama `dsp` (el estandar de Faust) a proposito. El
// JuceHeader.h real del build trae `using namespace juce;` (linea ~39), y
// con el existe el namespace `juce::dsp`: en Clang/macOS `class X : public
// dsp` resuelve ambiguo entre ::dsp y juce::dsp y el build muere
// ("reference to 'dsp' is ambiguous"). GCC
// hace lookup solo-de-tipos en la base-specifier y NO lo detecta -- por eso
// el arnes Linux compilaba. Los headers generados se producen con
// `-scn AVDexedFaustDspBase` para heredar de este nombre sin ambiguedad.
class AVDexedFaustDspBase
{
public:
    AVDexedFaustDspBase() = default;
    virtual ~AVDexedFaustDspBase() = default;

    virtual int getNumInputs() = 0;
    virtual int getNumOutputs() = 0;
    virtual void buildUserInterface (UI* ui_interface) = 0;
    virtual int getSampleRate() = 0;

    virtual void init (int sample_rate) = 0;
    virtual void instanceInit (int sample_rate) = 0;
    virtual void instanceConstants (int sample_rate) = 0;
    virtual void instanceResetUserInterface() = 0;
    virtual void instanceClear() = 0;

    virtual AVDexedFaustDspBase* clone() = 0;   // el codigo generado lo define; DEXED no lo usa (haria new)
    virtual void metadata (Meta* m) = 0;

    virtual void compute (int count, FAUSTFLOAT** inputs, FAUSTFLOAT** outputs) = 0;
};
