/*
 * Copyright 2016-2017 Pascal Gauthier.
 * Copyright 2012 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Modified by AV Vibes, 2026 (AV Dexed): carrier detection used for voice
 * state and the operator glow.
 */

#ifndef SYNTH_DX7NOTE_H_
#define SYNTH_DX7NOTE_H_

// This is the logic to put together a note from the MIDI description
// and run the low-level modules.

// It will continue to evolve a bit, as note-stealing logic, scaling,
// and real-time control of parameters live here.

#include "env.h"
#include "pitchenv.h"
#include "fm_core.h"
#include "tuning.h"
#include "porta.h"
#include "libMTSClient.h"
#include <memory>

struct VoiceStatus {
    uint32_t amp[6];
    char ampStep[6];
    char pitchStep;
};

class Dx7Note {
public:
    Dx7Note(std::shared_ptr<TuningState> ts, MTSClient *mtsc);
    void init(const uint8_t patch[156], int midinote, int velocity, int channel, const Controllers *ctrls);
    void initPortamento(const Dx7Note &srcNote);

    // Note: this _adds_ to the buffer. Interesting question whether it's
    // worth it...
    void compute(int32_t *buf, int32_t lfo_val, int32_t lfo_delay,
                 const Controllers *ctrls);
    /** compute in its two steps, so several voices' engine work can run
        together (AVRenderVoiceBlock): prepare sets this block's operator
        parameters (pitch, envelopes, amplitude modulation)... */
    void prepare(int32_t lfo_val, int32_t lfo_delay, const Controllers *ctrls);
    /** ...and render runs the engine on them, adding to `buf`. */
    void render(int32_t *buf, FmCore *core);
    // What the engine reads and keeps for this voice (EngineMkI::renderFour).
    FmOpParams *operatorParams() { return params_; }
    int32_t *feedbackMemory() { return fb_buf_; }
    int algorithmIndex() const { return algorithm_; }
    int32_t feedbackShift() const { return fb_shift_; }
    
    void keyup();
    
    bool isPlaying();
    /** True when at least one carrier is still at or above minimumAmp (same
        units as VoiceStatus::amp). */
    bool hasAudibleCarrier(uint32_t minimumAmp);
    
    // PG:add the update
    void update(const uint8_t patch[156], int midinote, int velocity, int channel);
    void updateBasePitches();
    void peekVoiceStatus(VoiceStatus &status);
    void transferState(Dx7Note& src);
    void transferSignal(Dx7Note &src);
    void transferPhase(Dx7Note &src);
    void oscSync();
    /** What compute (with no LFO) does to the operators' phases, and nothing
        else: the block's pitch (pitch envelope, bend, master tune), each
        switched-on operator's frequency (portamento moving on) and one block's
        turn of every phase, as each engine turns it. The free phase donor
        (AVDexedLayer::startFreePhase) only needs its phases. */
    void advancePhases(const Controllers *ctrls);
#if defined(AVDEXED_TESTING)
    int32_t testPhase(int op) const { return params_[op].phase; }
#endif

    // We should put this as a function and not a DX7Note method
    //int32_t osc_freq(int midinote, int mode, int coarse, int fine, int detune);

    std::shared_ptr<TuningState> tuning_state_;

    int mpePitchBend = 8192;
    int mpePressure = 0;
    int mpeTimbre = 0;
    
private:
    bool initialised_;
    Env env_[6];
    FmOpParams params_[6];
    PitchEnv pitchenv_;
    int32_t basepitch_[6];
    // AV Dexed: zero until the first sample (left unset in Dexed: the first
    // note of a new voice read whatever the memory held).
    int32_t fb_buf_[2] = { 0, 0 };
    int32_t fb_shift_;
    int32_t ampmodsens_[6];
    int32_t opMode[6];
    uint8_t opSwitch_;

    uint8_t playingMidiNote; // We need this for scale aware pitch bend
    uint8_t midiChannel;
    
    int ampmoddepth_;
    int algorithm_;
    int pitchmoddepth_;
    int pitchmodsens_;
    
    const uint8_t *currentPatch;
    
    int32_t porta_curpitch_[6];

    //int32_t noteLogFreq;
    double mtsFreq;
    static const int32_t mtsLogFreqToNoteLogFreq;
    MTSClient *mtsClient;
    int32_t osc_freq(int midinote, int mode, int coarse, int fine, int detune, int channel);
    /** The block's pitch modulation (pitch_base included); `pitch_base` is the bend and master tune. */
    int32_t pitchOfBlock(int32_t lfo_val, int32_t lfo_delay, const Controllers *ctrls, int32_t &pitch_base);
    /** Each switched-on operator's frequency for the block (portamento moving on). */
    void setFrequencies(int32_t pitch_base, int32_t pitch_mod, const Controllers *ctrls);
};

#endif  // SYNTH_DX7NOTE_H_
