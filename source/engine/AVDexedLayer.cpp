// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedLayer.h"
#include "../msfa/aligned_buf.h"
#include "../parameter/Model.h"
#include "libMTSClient.h"
#include <algorithm>
#include <cmath>
#include <cstring>

AVDexedLayer::AVDexedLayer() {
    // Controllers has a constructor that leaves most fields unset: every one
    // is given the value the instrument starts with.
    controllers.pitch_cc = 0x2000;
    controllers.pitch_range_up = 3;
    controllers.pitch_range_dn = 3;
    controllers.pitch_step = 0;
    controllers.aftertouch_cc = 0;
    controllers.breath_cc = 0;
    controllers.foot_cc = 0;
    controllers.modwheel_cc = 0;
    controllers.portamento_enable_cc = false;
    controllers.portamento_cc = 0;
    controllers.portamento_gliss_cc = false;
    controllers.masterTune = 0;
    controllers.mpeEnabled = false;
    controllers.core = nullptr;
    controllers.refresh();
}

AVDexedLayer::~AVDexedLayer() {
    release();
}

void AVDexedLayer::prepare(std::shared_ptr<TuningState> tuning, MTSClient* mts, double sampleRate) {
    tuningState = tuning;
    rate = sampleRate > 0.0 ? sampleRate : 44100.0;
    mtsClient = mts;
    for (auto& voice : voices) {
        delete voice.dx7_note;
        voice = Voice {};
        voice.dx7_note = new Dx7Note(tuning, mts);
    }
    delete phaseDonor;
    phaseDonor = new Dx7Note(tuning, mts);
    phaseNotes = 0;
    lfoOffset = false;
    currentNote = 0;
    nextKeydownSeq = 0;
    lastActiveVoice = 0;
    sustain = false;
    controllers.pitch_cc = 0x2000;
    controllers.modwheel_cc = 0;
    controllers.foot_cc = 0;
    controllers.breath_cc = 0;
    controllers.aftertouch_cc = 0;
    controllers.refresh();
    lfo.reset(program.getParameters(IDs::lfoRate.pos));
    prepared = true;
}

void AVDexedLayer::setTuning(std::shared_ptr<TuningState> tuning) {
    tuningState = tuning;
    if (phaseDonor != nullptr)
        phaseDonor->tuning_state_ = tuning;
    for (auto& voice : voices)
        if (voice.dx7_note != nullptr)
            voice.dx7_note->tuning_state_ = tuning;
}

void AVDexedLayer::release() {
    for (auto& voice : voices) {
        delete voice.dx7_note;
        voice = Voice {};
    }
    delete phaseDonor;
    phaseDonor = nullptr;
    prepared = false;
}

void AVDexedLayer::setProgram(const Program& newProgram) {
    program = newProgram;
    if (! prepared) return;
    // As the instrument's refreshVoice: the sounding voices follow the new data.
    for (auto& voice : voices)
        if (voice.live)
            voice.dx7_note->update(program.getRawData(), voice.midi_note, voice.velocity, voice.channel);
    lfo.reset(program.getParameters(IDs::lfoRate.pos));
}

void AVDexedLayer::followFunction(const Controllers& instrument, int32_t tuneOffset, bool mono, bool normalizeVelocity) {
    controllers.pitch_range_up = instrument.pitch_range_up;
    controllers.pitch_range_dn = instrument.pitch_range_dn;
    controllers.pitch_step = instrument.pitch_step;
    controllers.wheel = instrument.wheel;
    controllers.foot = instrument.foot;
    controllers.breath = instrument.breath;
    controllers.at = instrument.at;
    controllers.portamento_enable_cc = instrument.portamento_enable_cc;
    controllers.portamento_cc = instrument.portamento_cc;
    controllers.portamento_gliss_cc = instrument.portamento_gliss_cc;
    controllers.transpose12AsScale = instrument.transpose12AsScale;
    controllers.masterTune = instrument.masterTune + tuneOffset;
    controllers.mpeEnabled = false;
    controllers.core = instrument.core;
    controllers.refresh();
    if (mono != monoMode) {
        allNotesOff();
        monoMode = mono;
    }
    normalizeDxVelocity = normalizeVelocity;
}

// DexedAudioProcessor::keydown, for one layer (no MPE: a layer listens to
// one channel or to all of them as one instrument).
void AVDexedLayer::keydown(int channel, int pitch, int velo) {
    if (! prepared || controllers.core == nullptr) return;
    if (velo == 0) {
        keyup(channel, pitch);
        return;
    }
    pitch = std::clamp(pitch, 0, 127);
    if (normalizeDxVelocity)
        velo = int(float(velo) * 0.7874015f);

    const auto cents = [](float value) { return int32_t(std::lround(double(value) * double(1 << 24) / 1200.0)); };
    int32_t randomOffset = 0;
    if (randomPitch > 0.0f) {   // the DX7II's random pitch: a new tuning for each key
        randomState = randomState * 1664525u + 1013904223u;
        const float unit = float(randomState >> 8) / float(1 << 24) * 2.0f - 1.0f;
        randomOffset = cents(unit * randomPitch);
    }
    if (unison > 0.0f && ! monoMode) {
        const int first = startVoice(channel, pitch, velo, randomOffset + cents(unison), true);
        startVoice(channel, pitch, velo, randomOffset - cents(unison), true, first);
    } else {
        startVoice(channel, pitch, velo, randomOffset, false);
    }
}

int AVDexedLayer::startVoice(int channel, int pitch, int velo, int32_t pitchOffset, bool unisonPair, int avoid) {
    bool triggerLfo = true;
    int bestNote = currentNote == avoid ? (currentNote + 1) % voiceCount : currentNote;
    int bestScore = -1;
    int note = currentNote;
    for (int i = 0; i < voiceCount; i++) {
        const Voice& v = voices[size_t(note)];
        if (v.keydown)
            triggerLfo = false;
        if (note == avoid) {   // the first half of this unison key
            note = (note + 1) % voiceCount;
            continue;
        }
        int score = 0;
        if (! v.dx7_note->isPlaying()) score += 4;
        if (! v.keydown) score += 2;
        if (v.midi_note == pitch) score += 1;
        if (score > bestScore || (score == bestScore && v.keydown_seq < voices[size_t(bestNote)].keydown_seq)) {
            bestNote = note;
            bestScore = score;
        }
        note = (note + 1) % voiceCount;
    }

    // Free phase: this layer's LFO runs from its own point (a sync LFO
    // restarts at each first key anyway, as on any module).
    if (freePhase && ! lfoOffset) {
        for (int i = 0; i < int(phaseCycles * 911.0f); ++i) lfo.getsample();
        lfoOffset = true;
    }
    if (triggerLfo)
        lfo.keydown();

    note = bestNote;
    auto& voice = voices[size_t(note)];
    currentNote = (note + 1) % voiceCount;
    voice.channel = channel;
    voice.midi_note = pitch;
    voice.velocity = velo;
    voice.sustained = sustain;
    voice.keydown = true;
    voice.keydown_seq = nextKeydownSeq++;
    voice.pitchOffset = pitchOffset;
    voice.unisonPair = unisonPair;
    const bool voiceSteal = voice.dx7_note->isPlaying();
    voice.dx7_note->init(program.getRawData(), pitch, velo, channel, &controllers);
    if (program.getRawData()[IDs::oscKeySync.pos] && ! voiceSteal)
        voice.dx7_note->oscSync();
    if (freePhase && ! voiceSteal)
        startFreePhase(*voice.dx7_note, channel, pitch);
    if ((voices[size_t(lastActiveVoice)].midi_note != -1 && controllers.portamento_enable_cc) && controllers.portamento_cc > 0)
        voice.dx7_note->initPortamento(*voices[size_t(lastActiveVoice)].dx7_note);

    if (monoMode) {
        for (int i = 0; i < voiceCount; i++) {
            auto& other = voices[size_t(i)];
            if (other.live) {
                if (! other.keydown) {
                    other.live = false;
                    voice.dx7_note->transferSignal(*other.dx7_note);
                    break;
                }
                if (other.midi_note < pitch) {
                    other.live = false;
                    voice.dx7_note->transferState(*other.dx7_note);
                    break;
                }
                return note;
            }
        }
    } else if (! program.getRawData()[IDs::oscKeySync.pos]) {
        for (int i = 0; i < voiceCount; i++) {
            if (i != note && voices[size_t(i)].dx7_note->isPlaying() && voices[size_t(i)].midi_note == pitch) {
                voice.dx7_note->transferPhase(*voices[size_t(i)].dx7_note);
                break;
            }
        }
    }

    voice.live = true;
    lastActiveVoice = note;
    return note;
}

// DexedAudioProcessor::keyup, for one layer.
void AVDexedLayer::keyup(int channel, int pitch) {
    juce::ignoreUnused(channel);
    if (! prepared) return;
    pitch = std::clamp(pitch, 0, 127);
    int note;
    for (note = 0; note < voiceCount; ++note) {
        if (voices[size_t(note)].midi_note == pitch && voices[size_t(note)].keydown) {
            voices[size_t(note)].keydown = false;
            break;
        }
    }
    if (note >= voiceCount)
        return;
    const bool pair = voices[size_t(note)].unisonPair;
    releaseVoice(note);
    // The other voice of a unison key goes with it.
    if (pair)
        for (int other = 0; other < voiceCount; ++other)
            if (voices[size_t(other)].midi_note == pitch && voices[size_t(other)].keydown && voices[size_t(other)].unisonPair) {
                voices[size_t(other)].keydown = false;
                releaseVoice(other);
                break;
            }
}

void AVDexedLayer::releaseVoice(int note) {
    if (monoMode) {
        int highNote = -1;
        int target = 0;
        for (int i = 0; i < voiceCount; i++) {
            if (voices[size_t(i)].keydown && voices[size_t(i)].midi_note > highNote) {
                target = i;
                highNote = voices[size_t(i)].midi_note;
            }
        }
        if (highNote != -1 && voices[size_t(note)].live) {
            voices[size_t(note)].live = false;
            voices[size_t(target)].live = true;
            voices[size_t(target)].dx7_note->transferState(*voices[size_t(note)].dx7_note);
        }
    }

    if (sustain)
        voices[size_t(note)].sustained = true;
    else
        voices[size_t(note)].dx7_note->keyup();
}

void AVDexedLayer::controlChange(int controller, int value) {
    switch (controller) {
        case 1: controllers.modwheel_cc = value; controllers.refresh(); break;
        case 2: controllers.breath_cc = value; controllers.refresh(); break;
        case 4: controllers.foot_cc = value; controllers.refresh(); break;
        case 64:
            sustain = value > 63;
            if (! sustain)
                for (auto& voice : voices)
                    if (voice.sustained && ! voice.keydown) {
                        voice.dx7_note->keyup();
                        voice.sustained = false;
                    }
            break;
        case 120: panic(); break;
        case 123: allNotesOff(); break;
        default: break;
    }
}

void AVDexedLayer::pitchBend(int value) {
    controllers.pitch_cc = value;
}

void AVDexedLayer::aftertouch(int value) {
    controllers.aftertouch_cc = value;
    controllers.refresh();
}

void AVDexedLayer::startFreePhase(Dx7Note& note, int channel, int pitch) {
    const bool keySync = program.getRawData()[IDs::oscKeySync.pos] != 0;
    float cycles = phaseCycles;
    if (! keySync) {   // free-running: somewhere new each note, by the golden ratio
        const float moved = float(phaseNotes) * 0.618034f;
        cycles += moved - std::floor(moved);
        cycles -= std::floor(cycles);
    }
    ++phaseNotes;
    // Below 0.02 turns: the instrument's own start (and the engine's
    // frequency table stays in range below).
    if (cycles < 0.02f) cycles = 0.0f;
    if (keySync && cycles == 0.0f) return;
    if (phaseDonor == nullptr || tuningState == nullptr) return;

    // The donor is this note begun a few engine blocks before, tuned so that
    // its fundamental turns `cycles` times in them: every operator, fixed
    // frequency ones too, turns as its frequency, so it is a moment of the same
    // note. No bend, modulation or portamento on the way. As many blocks as
    // keep that tuning near the note (at most 16, for the lowest keys), so no
    // operator goes past what the engine's frequency table holds.
    Controllers still = controllers;
    still.pitch_cc = 0x2000;
    still.modwheel_cc = 0;
    still.foot_cc = 0;
    still.breath_cc = 0;
    still.aftertouch_cc = 0;
    still.portamento_enable_cc = false;
    still.refresh();
    phaseDonor->init(program.getRawData(), pitch, 100, channel, &still);
    phaseDonor->oscSync();
    if (cycles > 0.0f) {
        // logfreq is log2(Hz) << 24.
        const int32_t noteLogfreq = tuningState->midinote_to_logfreq(pitch);
        const double noteHz = std::exp2(double(noteLogfreq) / double(1 << 24));
        const int blocks = std::clamp(int(std::lround(double(cycles) * rate / (double(N) * noteHz))), 1, 16);
        const double hz = double(cycles) * rate / (double(N) * double(blocks));
        still.masterTune = int32_t(std::lround(std::log2(hz) * double(1 << 24))) - noteLogfreq;
        AlignedBuf<int32_t, N> scratch;
        for (int block = 0; block < blocks; ++block) {
            for (int j = 0; j < N; ++j) scratch.get()[j] = 0;
            phaseDonor->compute(scratch.get(), 0, 0, &still);
        }
    }
    note.transferPhase(*phaseDonor);
}

void AVDexedLayer::releaseControllers() {
    controlChange(64, 0);
    controllers.pitch_cc = 0x2000;
    controllers.modwheel_cc = 0;
    controllers.breath_cc = 0;
    controllers.foot_cc = 0;
    controllers.aftertouch_cc = 0;
    controllers.refresh();
}

void AVDexedLayer::allNotesOff() {
    if (! prepared) return;
    for (auto& voice : voices)
        if (voice.keydown)
            keyup(voice.channel, voice.midi_note);
}

void AVDexedLayer::panic() {
    sustain = false;
    for (auto& voice : voices) {
        voice.midi_note = -1;
        voice.keydown = false;
        voice.sustained = false;
        voice.live = false;
        if (voice.dx7_note != nullptr)
            voice.dx7_note->oscSync();
    }
}

// The instrument's render loop for one engine block (DexedAudioProcessor::processBlock).
void AVDexedLayer::renderBlock(float* out) {
    for (int j = 0; j < N; ++j) out[j] = 0;
    if (! prepared || controllers.core == nullptr) return;
    AlignedBuf<int32_t, N> audiobuf;
    for (int j = 0; j < N; ++j) audiobuf.get()[j] = 0;
    const int32_t lfovalue = lfo.getsample();
    const int32_t lfodelay = lfo.getdelay();
    const bool checkMTSESPRetuning = tuningState != nullptr && tuningState->is_standard_tuning() && MTS_HasMaster(mtsClient);
    for (auto& voice : voices) {
        if (! voice.live) continue;
        if (checkMTSESPRetuning)
            voice.dx7_note->updateBasePitches();
        if (voice.pitchOffset != 0) {   // unison / random pitch: this voice's own tuning
            Controllers tuned = controllers;
            tuned.masterTune += voice.pitchOffset;
            voice.dx7_note->compute(audiobuf.get(), lfovalue, lfodelay, &tuned);
        } else {
            voice.dx7_note->compute(audiobuf.get(), lfovalue, lfodelay, &controllers);
        }
        for (int j = 0; j < N; ++j) {
            int32_t val = audiobuf.get()[j];
            val = val >> 4;
            const int clip_val = val < -(1 << 24) ? 0x8000 : val >= (1 << 24) ? 0x7fff : val >> 9;
            float f = float(clip_val) / float(0x8000);
            if (f > 1) f = 1;
            if (f < -1) f = -1;
            out[j] += f;
            audiobuf.get()[j] = 0;
        }
    }
}

int AVDexedLayer::soundingVoices() const {
    int playing = 0;
    for (const auto& voice : voices)
        if (voice.dx7_note != nullptr && voice.live && voice.dx7_note->isPlaying()) ++playing;
    return playing;
}

bool AVDexedLayer::peekVoiceStatus(VoiceStatus& status) {
    constexpr uint32_t audibleCarrierFloor = 1u << 18;
    VoiceStatus combined {}, one {};
    int32_t newest = 0;
    bool any = false;
    for (auto& voice : voices) {
        if (voice.dx7_note == nullptr || ! voice.live || ! voice.dx7_note->isPlaying()
            || ! voice.dx7_note->hasAudibleCarrier(audibleCarrierFloor))
            continue;
        voice.dx7_note->peekVoiceStatus(one);
        for (int op = 0; op < 6; ++op)
            combined.amp[op] = std::max(combined.amp[op], one.amp[op]);
        if (! any || voice.keydown_seq > newest) {
            newest = voice.keydown_seq;
            std::memcpy(combined.ampStep, one.ampStep, sizeof(combined.ampStep));
            combined.pitchStep = one.pitchStep;
        }
        any = true;
    }
    if (! any) {
        std::memset(status.amp, 0, sizeof(status.amp));
        return false;
    }
    status = combined;
    return true;
}
