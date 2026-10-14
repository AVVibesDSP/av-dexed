// Copyright (C) 2026 AV Vibes. Part of AV Dexed.
// SPDX-License-Identifier: MIT (full text in doc/THIRD_PARTY_NOTICES.md)

declare name "AVDexedDelay";
declare author "AV Vibes";
declare version "2.0";
declare license "MIT";

import("stdfaust.lib");

//---------------------------------------------------------------------------
// Delay ping-pong, adapted for AV Dexed:
//  - entrada wet MONO (promedio L+R), semilla en la linea IZQUIERDA
//  - tiempos INDEPENDIENTES por lado (timeMsL / timeMsR)
//  - feedback UNIVERSAL cruzado L->R->L, tope 0.90 (estable siempre)
//  - de.sdelay: cambios de tiempo sin click ni Doppler (crossfade interno)
//  - damp: one-pole con ganancia DC unitaria SOLO en el lazo de feedback
//  - dry/wet como ENVIO: out = dry*in + wet*soloEcos. El camino wet NO
//    contiene la senal directa (primer tap en timeMsL), asi que dry=0/wet=1
//    es "solo el fx" y dry=1/wet=0 es passthrough bit-exact (arranque
//    neutral).
// El sync/divisiones/BPM vive en C++ (AVDexedDelay), aqui solo llegan ms.
//
// Librerias importadas: delays.lib (de.sdelay), routes.lib (ro.cross),
// signals.lib (si.smoo), maths.lib (ma.SR) -- todas faustlibraries LGPL
// **con la excepcion especial de embebido** que permite distribuir el C++
// generado sin restricciones (ver cabecera de delays.lib).
//
// Regenerar con Faust 2.85.9 para mantener el diff reproducible. El -scn
// evita la ambiguedad con juce::dsp bajo `using namespace juce`:
//   faust -single -inpl -scn AVDexedFaustDspBase -cn AVDexedDelayFaust -o source/effects/AVDexedDelayFaust.h source/effects/faust/AVDexedDelay.dsp
//---------------------------------------------------------------------------

dtMsL = hslider("timeMsL [unit:ms]", 500.0, 20.0, 1500.0, 0.1);
dtMsR = hslider("timeMsR [unit:ms]", 500.0, 20.0, 1500.0, 0.1);
fb    = hslider("feedback", 0.35, 0.0, 0.90, 0.001) : si.smoo;
dampN = hslider("damp", 0.30, 0.0, 1.0, 0.001) : si.smoo;

// dry/wet SIN si.smoo a proposito (v2.1): un one-pole en float se estanca a
// ~ulp/(1-polo) ~ 5e-5 del objetivo, asi que dry=1 nunca seria passthrough
// bit-exact (arranque neutral) ni el fade-in post-prepare
// desaparece del todo. Ganancia por-bloque sin rampa = mismo precedente que
// el Master Gain del engine (un multiply por bloque, sin smoothing). El
// smoo se queda solo donde un salto produce artefactos reales dentro del
// lazo (feedback/damp).
dryN  = hslider("dry", 1.0, 0.0, 1.0, 0.001);
wetN  = hslider("wet", 0.0, 0.0, 1.0, 0.001);

// 2^19 samples cubre 1.5 s hasta 192 kHz (1.5*192000 = 288000 < 524288).
MAXN = 524288;
IT   = 1024;   // ventana de crossfade del sdelay (~23 ms @ 44.1k)

dtsL = dtMsL*0.001*ma.SR : max(1.0) : min(float(MAXN - 1));
dtsR = dtMsR*0.001*ma.SR : max(1.0) : min(float(MAXN - 1));

// One-pole pasabajos con DC unitario (estilo Freeverb): d=0 transparente.
dampLP = *(1.0 - d) : + ~ *(d) with { d = dampN*0.85; };

wetInMono = *(0.5), *(0.5) :> _;

pingpong(m) = ((+(m), _) : (de.sdelay(MAXN, IT, dtsL), de.sdelay(MAXN, IT, dtsR)))
              ~ (ro.cross(2) : par(i, 2, *(fb) : dampLP));

process = _,_ <: (_,_), (wetInMono : pingpong) : mixdown
with {
    mixdown(dl, dr, wl, wr) = dryN*dl + wetN*wl, dryN*dr + wetN*wr;
};
