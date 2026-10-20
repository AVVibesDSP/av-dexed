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
#include <memory>

/** One DX7 of the layer mode, as one TF1 module of a TX816: its own 16
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

    void prepare(std::shared_ptr<TuningState> tuning, MTSClient* mts);
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

    void keydown(int channel, int pitch, int velocity);
    void keyup(int channel, int pitch);
    void controlChange(int controller, int value);
    void pitchBend(int value);
    void aftertouch(int value);
    void allNotesOff();
    void panic();

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
    };
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
