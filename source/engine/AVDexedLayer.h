// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../msfa/controllers.h"
#include "../msfa/dx7note.h"
#include "../msfa/lfo.h"
#include "../msfa/synth.h"
#include "../msfa/tuning.h"
#include "../core/Program.h"
#include <array>
#include <atomic>
#include <cmath>
#include <memory>

/** One DX7 of the layer mode, as one module of an eight-module rack (X816 mode): its own 16
    voices, LFO and controller state, playing one voice. Nothing of the engine
    changes: the voices are the engine's Dx7Note, and the note logic (voice
    stealing, mono, portamento, sustain) is DexedAudioProcessor::keydown and
    keyup's, line for line, so a layer plays a voice exactly as the instrument
    does with one layer (tests/AVDexedLayerTests.cpp compares the samples).

    Called on the audio thread only, except prepare() and release(). */
class AVDexedLayer {
public:
    static constexpr int voiceCount = 16;

    AVDexedLayer();
    ~AVDexedLayer();

    void prepare(std::shared_ptr<TuningState> tuning, MTSClient* mts, double sampleRate);
    void release();
    /** A new tuning (Scala files), as DexedAudioProcessor::resetTuning gives its voices. */
    void setTuning(std::shared_ptr<TuningState> tuning);

    /** The voice this layer plays. Voices already sounding follow the new
        data (as the instrument does when its sound is edited). */
    void setProgram(const Program& program);
    const Program& getProgram() const noexcept { return program; }

    /** The instrument-wide DX7 function settings (pitch bend, controller
        ranges, portamento, engine) that every layer follows, plus this
        layer's own tuning offset; its live controller values are its own. */
    void followFunction(const Controllers& instrument, int32_t tuneOffset, bool mono, bool normalizeVelocity);

    /** The DX7II voice functions a layer adds around the engine: unison (two
        voices per key, tuned `unisonCents` up and down) and a random tuning
        of each note within ±`randomCents`. Zero leaves the layer as it was. */
    void setVoiceExtras(float unisonCents, float randomCents) noexcept { unison = unisonCents; randomPitch = randomCents; }
    /** Each layer its own random sequence (as separate modules would be), always the same one. */
    void setRandomSeed(uint32_t seed) noexcept { randomState = seed == 0 ? 1 : seed; }
    /** Free phase: the layer behaves as a separate module of a rack, whose
        oscillators never run in step with the other modules'. Its notes
        start their operators where they would be had the note begun
        `cycles` turns of its fundamental before (0-1): the same sound, only
        not in step. A voice without oscillator key sync also moves on each
        note, as free-running oscillators do (every layer by the same, so
        they stay apart). The LFO starts from its own point too. Negative:
        off, the instrument's behaviour exactly.
        Without it, stacked copies of one voice start in step: the attack adds
        up (+9 dB with 8) and the detuned copies then cancel out (a "pluck"). */
    void setFreePhase(float cycles) noexcept {
        freePhase = cycles >= 0.0f;
        phaseCycles = freePhase ? cycles - std::floor(cycles) : 0.0f;
    }

    void keydown(int channel, int pitch, int velocity);
    void keyup(int channel, int pitch);
    void controlChange(int controller, int value);
    void pitchBend(int value);
    void aftertouch(int value);
    void allNotesOff();
    void panic();
    /** Sustain pedal up, pitch bend centred, wheel, breath, foot and aftertouch at rest. */
    void releaseControllers();

    /** One engine block (N samples) of this layer's voices, mono, in the
        instrument's float scale. */
    void renderBlock(float* out);

    int soundingVoices() const;
    bool isSounding() const { return soundingVoices() > 0; }
    /** The editor's operator meters, as DexedAudioProcessor::peekVoiceStatus. */
    bool peekVoiceStatus(VoiceStatus& status);

private:
    struct Voice {
        int channel = 0;
        int midi_note = -1;
        int velocity = 0;
        bool keydown = false;
        bool sustained = false;
        bool live = false;
        int32_t keydown_seq = -1;
        Dx7Note* dx7_note = nullptr;
        int32_t pitchOffset = 0;      // unison / random tuning of this voice (engine pitch units)
        bool unisonPair = false;      // one of the two voices of a unison key
    };
    /** Starts one voice; never takes `avoid` (the other half of a unison key). Returns its index. */
    int startVoice(int channel, int pitch, int velo, int32_t pitchOffset, bool unisonPair, int avoid = -1);
    void releaseVoice(int note);
    float unison = 0.0f, randomPitch = 0.0f;
    uint32_t randomState = 1;
    bool freePhase = false;
    float phaseCycles = 0.0f;
    uint32_t phaseNotes = 0;
    bool lfoOffset = false;
    double rate = 44100.0;
    // A note that never sounds: it gives the phases a free-phase note starts at.
    Dx7Note* phaseDonor = nullptr;
    void startFreePhase(Dx7Note& note, int channel, int pitch);
    std::array<Voice, voiceCount> voices {};
    Controllers controllers {};
    Lfo lfo {};
    Program program = Program::initialProgram();
    int currentNote = 0;
    int32_t nextKeydownSeq = 0;
    int lastActiveVoice = 0;
    bool sustain = false;
    bool monoMode = false;
    bool normalizeDxVelocity = false;
    bool prepared = false;
    std::shared_ptr<TuningState> tuningState;
    MTSClient* mtsClient = nullptr;
};
