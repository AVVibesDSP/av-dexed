// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

// The layer mode of DexedAudioProcessor (X816 mode): MIDI routing by
// channel and key range, the stereo mix, and what the editor changes.

#include "PluginProcessor.h"
#include "midi/MidiCCHandler.h"
#include "parameter/Model.h"
#include "core/AVDexedMicroTuning.h"
#include <cmath>

namespace {
float leftBalance(float pan) { return pan <= 0.5f ? 1.0f : 2.0f * (1.0f - pan); }
float rightBalance(float pan) { return pan >= 0.5f ? 1.0f : 2.0f * pan; }
/** Cents to the engine's pitch unit (an octave is 1 << 24). */
int32_t centsToPitch(float cents) { return int32_t(std::lround(double(cents) * double(1 << 24) / 1200.0)); }
/** Layer `layer` of a stack of `count`: where its tuning sits between the
    lowest (-1) and the highest (1), and where its notes start (free phase, in
    turns of the fundamental). Found by searching, for each size, the tunings
    and starts whose sum stays nearest one layer's level over the first
    harmonics (and the half ones of 0.50 ratios) and seconds: in step and evenly tuned, the copies add up at the
    attack and then cancel out, over and over (a "pluck"); evenly spread
    starts cancel at once. The tunings stay at least half a step apart. */
struct StackPlace { float tuning, phase; };
StackPlace stackPlace(int layer, int count) {
    static constexpr StackPlace places[7][8] = {
        { { -1.0f, 0.0f }, { 1.0f, 0.0f } },
        { { -1.0f, 0.0f }, { -0.0018f, 0.025f }, { 1.0f, 0.5271f } },
        { { -1.0f, 0.0f }, { -0.3353f, 0.1266f }, { 0.332f, 0.1131f }, { 1.0f, 0.7716f } },
        { { -1.0f, 0.0f }, { -0.3837f, 0.5636f }, { -0.0098f, 0.5383f }, { 0.6169f, 0.5547f }, { 1.0f, 0.2764f } },
        { { -1.0f, 0.0f }, { -0.6063f, 0.3844f }, { -0.3072f, 0.5974f }, { 0.2935f, 0.795f }, { 0.5926f, 0.5207f }, { 1.0f, 0.4954f } },
        { { -1.0f, 0.0f }, { -0.5727f, 0.713f }, { -0.3562f, 0.0452f }, { 0.1131f, 0.8885f }, { 0.3647f, 0.8666f }, { 0.614f, 0.3814f }, { 1.0f, 0.495f } },
        { { -1.0f, 0.0f }, { -0.6827f, 0.9696f }, { -0.4666f, 0.6507f }, { -0.0737f, 0.6194f }, { 0.147f, 0.4869f }, { 0.3513f, 0.091f }, { 0.7535f, 0.8629f }, { 1.0f, 0.8235f } },
    };
    if (count < 2 || layer < 0 || layer >= count || count > 8) return { 0.0f, 0.0f };
    return places[count - 2][layer];
}
}

void DexedAudioProcessor::renderAVLayers(float* left, float* right, int numSamples, MidiBufferIterator& it, MidiBufferIterator& end) {
    // Once per host block: the instrument's DX7 function settings and each
    // layer's own tuning, and where its volume and pan are heading.
    for (int layer = 0; layer < AVLayerCount; ++layer) {
        const auto& settings = avLayerSettings[size_t(layer)];
        syncAVLayer(layer);
        const float pan = juce::jlimit(0.0f, 1.0f, settings.pan.load(std::memory_order_relaxed));
        const float volume = settings.enabled.load(std::memory_order_relaxed)
            ? juce::jlimit(0.0f, 1.0f, settings.volume.load(std::memory_order_relaxed)) : 0.0f;
        avLayerGainLeft[size_t(layer)].setTargetValue(volume * leftBalance(pan));
        avLayerGainRight[size_t(layer)].setTargetValue(volume * rightBalance(pan));
    }

    // The single path's bookkeeping: the engine renders blocks of N samples,
    // what goes past the host block waits for the next one.
    int i;
    for (i = 0; i < numSamples && i < avLayerExtraSize; i++) {
        left[i] = avLayerExtraLeft[i];
        if (right != nullptr) right[i] = avLayerExtraRight[i];
    }
    if (avLayerExtraSize > numSamples) {
        for (int j = 0; j < avLayerExtraSize - numSamples; j++) {
            avLayerExtraLeft[j] = avLayerExtraLeft[j + numSamples];
            avLayerExtraRight[j] = avLayerExtraRight[j + numSamples];
        }
        avLayerExtraSize -= numSamples;
        processMidiMessages(it, end, numSamples);
        return;
    }
    for (; i < numSamples; i += N) {
        processMidiMessages(it, end, i);
        float mixLeft[N] {}, mixRight[N] {}, block[N];
        for (int layer = 0; layer < AVLayerCount; ++layer) {
            auto& gainLeft = avLayerGainLeft[size_t(layer)];
            auto& gainRight = avLayerGainRight[size_t(layer)];
            // Off and faded out: nothing to hear; its notes stop now.
            if (! avLayerSettings[size_t(layer)].enabled.load(std::memory_order_relaxed)
                && ! gainLeft.isSmoothing() && ! gainRight.isSmoothing()) {
                const auto bit = 1u << layer;
                if (avLayerFadeStopMask.fetch_and(~bit, std::memory_order_acq_rel) & bit) {
                    avLayers[size_t(layer)].panic();
                    clearAVLayerHeld(layer);
                }
                continue;
            }
            avLayers[size_t(layer)].renderBlock(block);
            for (int j = 0; j < N; ++j) {
                mixLeft[j] += block[j] * gainLeft.getNextValue();
                mixRight[j] += block[j] * gainRight.getNextValue();
            }
        }
        const int jmax = numSamples - i;
        for (int j = 0; j < N; ++j) {
            if (j < jmax) {
                left[i + j] = mixLeft[j];
                if (right != nullptr) right[i + j] = mixRight[j];
            } else {
                avLayerExtraLeft[j - jmax] = mixLeft[j];
                avLayerExtraRight[j - jmax] = mixRight[j];
            }
        }
    }
    avLayerExtraSize = i - numSamples;
}

void DexedAudioProcessor::syncAVLayer(int layer) {
    const auto& settings = avLayerSettings[size_t(layer)];
    const float cents = juce::jlimit(-AVLayerMaxDetune, AVLayerMaxDetune, settings.detune.load(std::memory_order_relaxed));
    const int mono = settings.mono.load(std::memory_order_relaxed);
    avLayers[size_t(layer)].followFunction(controllers, centsToPitch(cents), mono < 0 ? monoMode : mono == 1, normalizeDxVelocity);
    avLayers[size_t(layer)].setVoiceExtras(juce::jlimit(0.0f, AVLayerMaxDetune, settings.unison.load(std::memory_order_relaxed)),
                                           juce::jlimit(0.0f, 100.0f, settings.randomPitch.load(std::memory_order_relaxed)));
    avLayers[size_t(layer)].setFreePhase(settings.phase.load(std::memory_order_relaxed));
}

void DexedAudioProcessor::processAVLayerMidi(const MidiMessage& message) {
    const uint8* buf = message.getRawData();
    const int status = buf[0] & 0xF0;
    const int channel = juce::jlimit(1, 16, message.getChannel());
    const auto listens = [this, channel](int layer) {
        const auto& settings = avLayerSettings[size_t(layer)];
        const int wanted = settings.channel.load(std::memory_order_relaxed);
        return settings.enabled.load(std::memory_order_relaxed) && (wanted == 0 || wanted == channel);
    };
    // Each layer's own voice transpose (its "middle C"), not the selected one's.
    const auto pitchFor = [this](int layer, int note) {
        const int voiceTranspose = avLayers[size_t(layer)].getProgram().getRawData()[IDs::transpose.pos];
        return juce::jlimit(0, 127, note + avLayerSettings[size_t(layer)].transpose.load(std::memory_order_relaxed)
                                        + tuningTranspositionShift(voiceTranspose));
    };
    const int key = buf[1] & 0x7F;   // a data byte, whatever the host passed
    const auto inRange = [this](int layer, int note) {
        const auto& settings = avLayerSettings[size_t(layer)];
        return note >= settings.lowNote.load(std::memory_order_relaxed) && note <= settings.highNote.load(std::memory_order_relaxed);
    };

    switch (status) {
        case 0x90:
            if (buf[2] > 0) {
                // As the single path: MTS-ESP may filter the note.
                if (synthTuningState->is_standard_tuning() && MTS_HasMaster(mtsClient)
                    && MTS_ShouldFilterNote(mtsClient, key, channel - 1))
                    return;
                const auto play = [&](int layer) {
                    const int pitch = pitchFor(layer, key);
                    const auto slot = size_t((channel - 1) * 128 + key);
                    avLayerHeld[size_t(layer)][slot] = int8_t(pitch);
                    auto& count = avLayerHeldCount[size_t(layer)][slot];
                    if (count < 255) ++count;
                    avLayers[size_t(layer)].keydown(channel, pitch, buf[2] & 0x7F);
                };
                // The layers of an alternate group take turns (one of them
                // plays each note); every other layer plays.
                std::array<std::array<int, AVLayerCount>, AVLayerCount> groups {};
                std::array<int, AVLayerCount> groupSize {};
                for (int layer = 0; layer < AVLayerCount; ++layer) {
                    if (! listens(layer) || ! inRange(layer, key)) continue;
                    const int group = avLayerSettings[size_t(layer)].alternateGroup.load(std::memory_order_relaxed);
                    if (group >= 0 && group < AVLayerCount) groups[size_t(group)][size_t(groupSize[size_t(group)]++)] = layer;
                    else play(layer);
                }
                for (int group = 0; group < AVLayerCount; ++group)
                    if (const int size = groupSize[size_t(group)]; size > 0)
                        play(groups[size_t(group)][size_t(avAlternateTurn[size_t(group)]++ % unsigned(size))]);
                return;
            }
            [[fallthrough]];
        case 0x80:
            for (int layer = 0; layer < AVLayerCount; ++layer) {
                // The pitch it was played at, one note-on at a time: a key
                // played twice before its note-off is released as the single
                // path releases it, one voice per note-off. A layer that never
                // got the note (another channel, range or alternate member)
                // has nothing to release.
                const auto slot = size_t((channel - 1) * 128 + key);
                auto& count = avLayerHeldCount[size_t(layer)][slot];
                if (count == 0) continue;
                avLayers[size_t(layer)].keyup(channel, avLayerHeld[size_t(layer)][slot]);
                if (--count == 0) avLayerHeld[size_t(layer)][slot] = -1;
            }
            return;

        case 0xB0: {
            const int ctrl = buf[1], value = buf[2];
            midiCCMapper->processControlChange(ctrl, value);
            if (! avEditorOpen.load(std::memory_order_acquire))
                if (const int steps = consumeAVPresetSteps(); steps != 0) {
                    setCurrentProgram(((currentProgram + steps) % 32 + 32) % 32);
                    avHostProgramDirty.store(true, std::memory_order_release);
                    applySoundChangeNow();
                }
            switch (ctrl) {
                case 1:
                    controllers.modwheel_cc = value;
                    avKeyboardMod.store(value, std::memory_order_relaxed);
                    controllers.refresh();
                    break;
                case 5: controllers.portamento_cc = value; break;          // portamento: instrument-wide
                case 65: controllers.portamento_enable_cc = value >= 64; break;
                default: break;
            }
            for (int layer = 0; layer < AVLayerCount; ++layer) {
                if (! listens(layer)) continue;
                avLayers[size_t(layer)].controlChange(ctrl, value);
                if (ctrl == 120 || ctrl == 123)
                    clearAVLayerHeld(layer);
            }
            return;
        }

        case 0xC0:
            setCurrentProgram(buf[1] & 0x7F);   // into the selected layer
            applySoundChangeNow();
            return;

        case 0xD0:
            controllers.aftertouch_cc = buf[1];
            controllers.refresh();
            for (int layer = 0; layer < AVLayerCount; ++layer)
                if (listens(layer)) avLayers[size_t(layer)].aftertouch(buf[1]);
            return;

        case 0xE0: {
            const int value = buf[1] | (buf[2] << 7);
            controllers.pitch_cc = value;
            avKeyboardPitch.store(value, std::memory_order_relaxed);
            for (int layer = 0; layer < AVLayerCount; ++layer)
                if (listens(layer)) avLayers[size_t(layer)].pitchBend(value);
            return;
        }
        default:
            return;
    }
}

void DexedAudioProcessor::applySoundChangeNow() {
    // Render, in the middle of a block: the notes after a program change in
    // the same block play the new voice (the single path silences and loads
    // at once too); otherwise the next block's panic would cut them.
    const int selected = avSelectedLayer.load(std::memory_order_acquire);
    const auto bit = 1u << selected;
    if (avLayerPanicMask.fetch_and(~bit, std::memory_order_acq_rel) & bit) {
        avLayers[size_t(selected)].panic();
        clearAVLayerHeld(selected);
    }
    avLayers[size_t(selected)].setProgram(activeProgram);
    syncAVLayer(selected);   // the new voice's mono, unison and random pitch too
}

void DexedAudioProcessor::panicForSoundChange() {
    if (avLayerMode.load(std::memory_order_acquire))
        avLayerPanicMask.fetch_or(1u << avSelectedLayer.load(std::memory_order_acquire), std::memory_order_acq_rel);
    else
        panic();
}

void DexedAudioProcessor::setAVLayerMode(bool on) {
    if (on == avLayerMode.load(std::memory_order_acquire)) return;
    {
        const juce::ScopedLock lock(getCallbackLock());
        if (on) {
            const int selected = avSelectedLayer.load(std::memory_order_relaxed);
            avLayers[size_t(selected)].setProgram(activeProgram);
            bool anyEnabled = false;
            for (const auto& settings : avLayerSettings) anyEnabled |= settings.enabled.load(std::memory_order_relaxed);
            if (! anyEnabled) avLayerSettings[size_t(selected)].enabled.store(true, std::memory_order_relaxed);
        }
        avLayerMode.store(on, std::memory_order_release);
    }
    programChanged.set(true);
}

void DexedAudioProcessor::loadSelectedLayerIntoEditor(const Program& voice) {
    // applyProgram without silencing the instrument: the other layers play on.
    avSoundFromBankSlot.store(false, std::memory_order_relaxed);
    if (midiCCMapper != nullptr) midiCCMapper->forgetAVCCWrites();
    activeProgram = voice;
    activeProgram.pushToParameters(parameters);
    refreshVoice.set(true);
    programChanged.set(true);
    avPresetContextCurrent.store(false, std::memory_order_relaxed);
    avKnownPresetOrigin.store(false, std::memory_order_relaxed);
    captureAVPresetBaseline();
}

void DexedAudioProcessor::selectAVLayer(int layer) {
    layer = juce::jlimit(0, AVLayerCount - 1, layer);
    const int previous = avSelectedLayer.load(std::memory_order_acquire);
    if (layer == previous) return;
    Program voice;
    {
        const juce::ScopedLock lock(getCallbackLock());
        avLayers[size_t(previous)].setProgram(activeProgram);   // its edits stay with it
        refreshVoice.set(false);                                // already given to that layer
        avSelectedLayer.store(layer, std::memory_order_release);
        voice = avLayers[size_t(layer)].getProgram();
        activeProgram = voice;
    }
    loadSelectedLayerIntoEditor(voice);
}

Program DexedAudioProcessor::getAVLayerProgram(int layer) const {
    layer = juce::jlimit(0, AVLayerCount - 1, layer);
    if (layer == avSelectedLayer.load(std::memory_order_acquire))
        return activeProgram;
    return avLayers[size_t(layer)].getProgram();
}

void DexedAudioProcessor::setAVLayerProgram(int layer, const Program& voice) {
    layer = juce::jlimit(0, AVLayerCount - 1, layer);
    const bool selected = layer == avSelectedLayer.load(std::memory_order_acquire);
    {
        const juce::ScopedLock lock(getCallbackLock());
        avLayers[size_t(layer)].setProgram(voice);
        if (selected) {
            refreshVoice.set(false);
            activeProgram = voice;
        }
    }
    if (selected)
        loadSelectedLayerIntoEditor(voice);
}

void DexedAudioProcessor::setAVLayerEnabled(int layer, bool enabled) {
    layer = juce::jlimit(0, AVLayerCount - 1, layer);
    // Off: its volume fades out, then its notes stop (they never come back
    // when it is turned on again). On again before that: it just plays on.
    if (enabled) avLayerFadeStopMask.fetch_and(~(1u << layer), std::memory_order_acq_rel);
    else avLayerFadeStopMask.fetch_or(1u << layer, std::memory_order_acq_rel);
    avLayerSettings[size_t(layer)].enabled.store(enabled, std::memory_order_release);
}

void DexedAudioProcessor::setAVLayerVoiceExtras(int layer, const uint8_t* amem35) {
    layer = juce::jlimit(0, AVLayerCount - 1, layer);
    AVDexedPerformance::ResolvedLayer extras;
    if (amem35 != nullptr) AVDexedPerformance::applyVoiceExtras(extras, amem35);
    auto& settings = avLayerSettings[size_t(layer)];
    settings.unison.store(extras.unison, std::memory_order_relaxed);
    settings.randomPitch.store(extras.randomPitch, std::memory_order_relaxed);
    settings.mono.store(extras.mono, std::memory_order_relaxed);
}

void DexedAudioProcessor::setAVLayerMicroTuning(int layer, int table, int key) {
    layer = juce::jlimit(0, AVLayerCount - 1, layer);
    table = table >= 1 && table < AVDexedMicroTuning::presetCount ? table : -1;
    key = ((key % 12) + 12) % 12;
    auto& settings = avLayerSettings[size_t(layer)];
    if (settings.microTable.load() == table && settings.microKey.load() == key && (table < 0) == (avLayerTuning[size_t(layer)] == nullptr))
        return;
    auto tuning = table > 0 ? AVDexedMicroTuning::makePresetTuning(table, key) : nullptr;   // allocated here, never in render
    const juce::ScopedLock lock(getCallbackLock());
    avLayerTuning[size_t(layer)] = tuning;
    avLayers[size_t(layer)].setTuning(tuningForAVLayer(layer));
    settings.microTable.store(table, std::memory_order_relaxed);
    settings.microKey.store(key, std::memory_order_relaxed);
}

void DexedAudioProcessor::setAVLayerFreePhase(int layer, bool on) {
    layer = juce::jlimit(0, AVLayerCount - 1, layer);
    avLayerSettings[size_t(layer)].phase.store(on ? stackPlace(layer, AVLayerCount).phase : -1.0f, std::memory_order_relaxed);
}

void DexedAudioProcessor::setAVLayerChannel(int layer, int channel) {
    layer = juce::jlimit(0, AVLayerCount - 1, layer);
    channel = juce::jlimit(0, 16, channel);
    if (avLayerSettings[size_t(layer)].channel.exchange(channel, std::memory_order_acq_rel) != channel)
        avLayerReleaseMask.fetch_or(1u << layer, std::memory_order_acq_rel);
}

void DexedAudioProcessor::stackAVLayers(int count, float cents) {
    count = juce::jlimit(1, AVLayerCount, count);
    cents = juce::jlimit(0.0f, AVLayerMaxDetune, cents);
    const auto voice = activeProgram;
    {
        const juce::ScopedLock lock(getCallbackLock());
        avLayers[size_t(avSelectedLayer.load(std::memory_order_relaxed))].setProgram(activeProgram);
        for (int layer = 0; layer < AVLayerCount; ++layer) {
            auto& settings = avLayerSettings[size_t(layer)];
            const bool used = layer < count;
            if (used) avLayers[size_t(layer)].setProgram(voice);
            // From the lowest tuning on the left to the highest on the right,
            // as the modules of an eight-module rack spread over a stereo mix; together as
            // loud as one layer.
            const float position = count > 1 ? 2.0f * float(layer) / float(count - 1) - 1.0f : 0.0f;
            const auto place = used ? stackPlace(layer, count) : StackPlace { 0.0f, 0.0f };
            settings.enabled.store(used, std::memory_order_relaxed);
            settings.alternateGroup.store(-1, std::memory_order_relaxed);
            settings.unison.store(0.0f, std::memory_order_relaxed);
            settings.randomPitch.store(0.0f, std::memory_order_relaxed);
            settings.mono.store(-1, std::memory_order_relaxed);
            settings.channel.store(0, std::memory_order_relaxed);
            settings.lowNote.store(0, std::memory_order_relaxed);
            settings.highNote.store(127, std::memory_order_relaxed);
            settings.transpose.store(0, std::memory_order_relaxed);
            settings.detune.store(used ? std::round(cents * place.tuning * 10.0f) / 10.0f : 0.0f, std::memory_order_relaxed);
            settings.pan.store(used ? 0.5f + 0.4f * position : 0.5f, std::memory_order_relaxed);
            // Not in step, the layers add up as separate modules do: as
            // loud together as one layer.
            settings.volume.store(used ? 1.0f / std::sqrt(float(count)) : 1.0f, std::memory_order_relaxed);
            settings.phase.store(used && count > 1 ? place.phase : -1.0f, std::memory_order_relaxed);
        }
        for (int layer = 0; layer < AVLayerCount; ++layer) setAVLayerMicroTuning(layer, -1, 0);
        avSelectedLayer.store(0, std::memory_order_release);
        // New layers: "Guardar" saves them as new ones, never over the last set's file.
        avLayerSetFile = juce::File();
        avLayerSetId = {};
        activeProgram = voice;
        avLayerPanicMask.store((1u << AVLayerCount) - 1, std::memory_order_release);
        avLayerReleaseMask.store((1u << AVLayerCount) - 1, std::memory_order_release);
    }
    if (! avLayerMode.load(std::memory_order_acquire))
        setAVLayerMode(true);
    loadSelectedLayerIntoEditor(voice);
}

void DexedAudioProcessor::resetAVLayers() {
    const int selected = avSelectedLayer.load(std::memory_order_acquire);
    for (int layer = 0; layer < AVLayerCount; ++layer) {
        auto& settings = avLayerSettings[size_t(layer)];
        settings.enabled.store(layer == selected, std::memory_order_relaxed);
        settings.alternateGroup.store(-1, std::memory_order_relaxed);
            settings.unison.store(0.0f, std::memory_order_relaxed);
            settings.randomPitch.store(0.0f, std::memory_order_relaxed);
            settings.mono.store(-1, std::memory_order_relaxed);
        settings.channel.store(0, std::memory_order_relaxed);
        settings.lowNote.store(0, std::memory_order_relaxed);
        settings.highNote.store(127, std::memory_order_relaxed);
        settings.transpose.store(0, std::memory_order_relaxed);
        settings.detune.store(0.0f, std::memory_order_relaxed);
        settings.volume.store(1.0f, std::memory_order_relaxed);
        settings.pan.store(0.5f, std::memory_order_relaxed);
        settings.phase.store(-1.0f, std::memory_order_relaxed);
    }
    avLayerPanicMask.store((1u << AVLayerCount) - 1, std::memory_order_release);
    avLayerReleaseMask.store((1u << AVLayerCount) - 1, std::memory_order_release);   // channels changed: controllers at rest
    for (int layer = 0; layer < AVLayerCount; ++layer) setAVLayerMicroTuning(layer, -1, 0);
    setAVLayerSetContext({}, {});   // new layers, as a stack
}

juce::ValueTree DexedAudioProcessor::createAVLayersState() const {
    juce::ValueTree layers("avLayers");
    layers.setProperty("mode", avLayerMode.load(std::memory_order_acquire), nullptr);
    layers.setProperty("selected", avSelectedLayer.load(std::memory_order_acquire), nullptr);
    if (avLayerSetFile != juce::File()) {
        layers.setProperty("setPath", avLayerSetFile.getFullPathName(), nullptr);
        layers.setProperty("setId", avLayerSetId, nullptr);
    }
    for (int index = 0; index < AVLayerCount; ++index) {
        const auto& settings = avLayerSettings[size_t(index)];
        juce::ValueTree layer("layer");
        layer.setProperty("enabled", settings.enabled.load(std::memory_order_relaxed), nullptr);
        layer.setProperty("channel", settings.channel.load(std::memory_order_relaxed), nullptr);
        layer.setProperty("lowNote", settings.lowNote.load(std::memory_order_relaxed), nullptr);
        layer.setProperty("highNote", settings.highNote.load(std::memory_order_relaxed), nullptr);
        layer.setProperty("transpose", settings.transpose.load(std::memory_order_relaxed), nullptr);
        layer.setProperty("detune", settings.detune.load(std::memory_order_relaxed), nullptr);
        layer.setProperty("volume", settings.volume.load(std::memory_order_relaxed), nullptr);
        layer.setProperty("pan", settings.pan.load(std::memory_order_relaxed), nullptr);
        layer.setProperty("alternateGroup", settings.alternateGroup.load(std::memory_order_relaxed), nullptr);
        layer.setProperty("unison", settings.unison.load(std::memory_order_relaxed), nullptr);
        layer.setProperty("randomPitch", settings.randomPitch.load(std::memory_order_relaxed), nullptr);
        layer.setProperty("mono", settings.mono.load(std::memory_order_relaxed), nullptr);
        layer.setProperty("microTable", settings.microTable.load(std::memory_order_relaxed), nullptr);
        layer.setProperty("microKey", settings.microKey.load(std::memory_order_relaxed), nullptr);
        layer.setProperty("phase", settings.phase.load(std::memory_order_relaxed), nullptr);
        const auto voice = getAVLayerProgram(index);
        layer.setProperty("voice", juce::MemoryBlock(voice.getRawData(), Program::PROGRAM_SIZE + 1).toBase64Encoding(), nullptr);
        layers.addChild(layer, -1, nullptr);
    }
    return layers;
}

namespace {
/** One layer of a session or a layers file; a missing one has the defaults
    (only the first layer on). */
AVDexedPerformance::ResolvedLayer readLayerTree(const juce::ValueTree& layer, int index) {
    AVDexedPerformance::ResolvedLayer read;
    auto& plan = read.plan;
    const bool valid = layer.isValid() && layer.hasType("layer");
    plan.used = true;
    plan.enabled = valid ? bool(layer.getProperty("enabled", false)) : index == 0;
    if (! valid) return read;
    plan.channel = juce::jlimit(0, 16, int(layer.getProperty("channel", 0)));
    const int low = juce::jlimit(0, 127, int(layer.getProperty("lowNote", 0)));
    const int high = juce::jlimit(0, 127, int(layer.getProperty("highNote", 127)));
    plan.lowNote = juce::jmin(low, high);
    plan.highNote = juce::jmax(low, high);
    plan.transpose = juce::jlimit(-24, 24, int(layer.getProperty("transpose", 0)));
    plan.detune = juce::jlimit(-DexedAudioProcessor::AVLayerMaxDetune, DexedAudioProcessor::AVLayerMaxDetune, float(layer.getProperty("detune", 0.0f)));
    plan.volume = juce::jlimit(0.0f, 1.0f, float(layer.getProperty("volume", 1.0f)));
    plan.pan = juce::jlimit(0.0f, 1.0f, float(layer.getProperty("pan", 0.5f)));
    plan.alternateGroup = juce::jlimit(-1, DexedAudioProcessor::AVLayerCount - 1, int(layer.getProperty("alternateGroup", -1)));
    read.unison = juce::jlimit(0.0f, DexedAudioProcessor::AVLayerMaxDetune, float(layer.getProperty("unison", 0.0f)));
    read.randomPitch = juce::jlimit(0.0f, 100.0f, float(layer.getProperty("randomPitch", 0.0f)));
    read.mono = juce::jlimit(-1, 1, int(layer.getProperty("mono", -1)));
    plan.microTable = juce::jlimit(-1, 10, int(layer.getProperty("microTable", -1)));
    plan.microKey = juce::jlimit(0, 11, int(layer.getProperty("microKey", 0)));
    plan.phase = juce::jlimit(-1.0f, 1.0f, float(layer.getProperty("phase", -1.0f)));
    juce::MemoryBlock data;
    if (data.fromBase64Encoding(layer.getProperty("voice").toString()) && data.getSize() >= size_t(Program::PROGRAM_SIZE)) {
        read.voice = Program(static_cast<const uint8_t*>(data.getData()));
        if (data.getSize() > size_t(Program::PROGRAM_SIZE))
            read.voice.setOperatorSwitches(static_cast<const uint8_t*>(data.getData())[Program::PROGRAM_SIZE]);
        Cartridge::applyBankRules(read.voice);
        read.voiceFound = true;
    }
    return read;
}
}

void DexedAudioProcessor::restoreAVLayersState(const juce::ValueTree& layers) {
    const juce::ScopedLock lock(getCallbackLock());
    for (int index = 0; index < AVLayerCount; ++index) {
        const auto read = readLayerTree(layers.isValid() ? layers.getChild(index) : juce::ValueTree(), index);
        const auto& plan = read.plan;
        auto& settings = avLayerSettings[size_t(index)];
        settings.enabled.store(plan.enabled, std::memory_order_relaxed);
        settings.channel.store(plan.channel, std::memory_order_relaxed);
        settings.lowNote.store(plan.lowNote, std::memory_order_relaxed);
        settings.highNote.store(plan.highNote, std::memory_order_relaxed);
        settings.transpose.store(plan.transpose, std::memory_order_relaxed);
        settings.detune.store(plan.detune, std::memory_order_relaxed);
        settings.volume.store(plan.volume, std::memory_order_relaxed);
        settings.pan.store(plan.pan, std::memory_order_relaxed);
        settings.alternateGroup.store(plan.alternateGroup, std::memory_order_relaxed);
        settings.unison.store(read.unison, std::memory_order_relaxed);
        settings.randomPitch.store(read.randomPitch, std::memory_order_relaxed);
        settings.mono.store(read.mono, std::memory_order_relaxed);
        settings.phase.store(plan.phase, std::memory_order_relaxed);
        setAVLayerMicroTuning(index, plan.microTable, plan.microKey);
        avLayers[size_t(index)].setProgram(read.voice);
    }
    const int selected = juce::jlimit(0, AVLayerCount - 1, layers.isValid() ? int(layers.getProperty("selected", 0)) : 0);
    avSelectedLayer.store(selected, std::memory_order_release);
    // A path from another OS ("/Users/..." on Windows) is no file here.
    const auto setPath = layers.getProperty("setPath").toString();
    avLayerSetFile = juce::File::isAbsolutePath(setPath) ? juce::File(setPath) : juce::File();
    avLayerSetId = avLayerSetFile != juce::File() ? layers.getProperty("setId").toString() : juce::String();
    // The session's sound is the selected layer's voice.
    avLayers[size_t(selected)].setProgram(activeProgram);
    avLayerMode.store(layers.isValid() && bool(layers.getProperty("mode", false)), std::memory_order_release);
    avLayerPanicMask.store((1u << AVLayerCount) - 1, std::memory_order_release);
    avLayerReleaseMask.store((1u << AVLayerCount) - 1, std::memory_order_release);
}

bool DexedAudioProcessor::loadAVLayersFile(const juce::String& text) {
    const auto xml = juce::parseXML(text);
    if (xml == nullptr || ! xml->hasTagName("avDexedLayers")) return false;
    const auto file = juce::ValueTree::fromXml(*xml);
    const auto layers = file.getChildWithName("avLayers");
    if (int(file.getProperty("formatVersion", 0)) != 1 || ! layers.isValid()) return false;
    applyAVLayersTree(layers);
    return true;
}

void DexedAudioProcessor::applyAVLayersTree(const juce::ValueTree& layers) {
    std::array<AVDexedPerformance::ResolvedLayer, AVLayerCount> read {};
    for (int index = 0; index < AVLayerCount; ++index)
        read[size_t(index)] = readLayerTree(layers.getChild(index), index);
    applyAVLayerSetup(read);
    selectAVLayer(juce::jlimit(0, AVLayerCount - 1, int(layers.getProperty("selected", 0))));
}

juce::File DexedAudioProcessor::getAVLayerSetFile() const {
    const juce::ScopedLock lock(getCallbackLock());
    return avLayerSetFile;
}

juce::String DexedAudioProcessor::getAVLayerSetId() const {
    const juce::ScopedLock lock(getCallbackLock());
    return avLayerSetId;
}

void DexedAudioProcessor::setAVLayerSetContext(const juce::File& file, const juce::String& id) {
    const juce::ScopedLock lock(getCallbackLock());
    avLayerSetFile = file;
    avLayerSetId = id;
}

void DexedAudioProcessor::applyAVLayerSetup(const std::array<AVDexedPerformance::ResolvedLayer, AVLayerCount>& layers) {
    int first = -1, firstUsed = -1;
    for (int index = 0; index < AVLayerCount; ++index) {
        const auto& plan = layers[size_t(index)].plan;
        if (plan.used && firstUsed < 0) firstUsed = index;
        if (plan.used && plan.enabled && first < 0) first = index;
    }
    if (first < 0) first = juce::jmax(0, firstUsed);
    const auto selectedVoice = layers[size_t(first)].plan.used ? layers[size_t(first)].voice : activeProgram;
    {
        const juce::ScopedLock lock(getCallbackLock());
        for (int index = 0; index < AVLayerCount; ++index) {
            const auto& layer = layers[size_t(index)];
            const auto& plan = layer.plan;
            auto& settings = avLayerSettings[size_t(index)];
            settings.enabled.store(plan.used && plan.enabled, std::memory_order_relaxed);
            settings.alternateGroup.store(plan.used ? juce::jlimit(-1, AVLayerCount - 1, plan.alternateGroup) : -1, std::memory_order_relaxed);
            settings.unison.store(plan.used ? juce::jlimit(0.0f, AVLayerMaxDetune, layer.unison) : 0.0f, std::memory_order_relaxed);
            settings.randomPitch.store(plan.used ? juce::jlimit(0.0f, 100.0f, layer.randomPitch) : 0.0f, std::memory_order_relaxed);
            settings.mono.store(plan.used ? juce::jlimit(-1, 1, layer.mono) : -1, std::memory_order_relaxed);
            settings.channel.store(plan.used ? juce::jlimit(0, 16, plan.channel) : 0, std::memory_order_relaxed);
            settings.lowNote.store(plan.used ? juce::jlimit(0, 127, plan.lowNote) : 0, std::memory_order_relaxed);
            settings.highNote.store(plan.used ? juce::jlimit(0, 127, plan.highNote) : 127, std::memory_order_relaxed);
            settings.transpose.store(plan.used ? juce::jlimit(-24, 24, plan.transpose) : 0, std::memory_order_relaxed);
            settings.detune.store(plan.used ? juce::jlimit(-AVLayerMaxDetune, AVLayerMaxDetune, plan.detune) : 0.0f, std::memory_order_relaxed);
            settings.volume.store(plan.used ? juce::jlimit(0.0f, 1.0f, plan.volume) : 1.0f, std::memory_order_relaxed);
            settings.pan.store(plan.used ? juce::jlimit(0.0f, 1.0f, plan.pan) : 0.5f, std::memory_order_relaxed);
            settings.phase.store(plan.used ? plan.phase : -1.0f, std::memory_order_relaxed);
            avLayers[size_t(index)].setProgram(plan.used ? layer.voice : Program::initialProgram());
            setAVLayerMicroTuning(index, plan.used ? plan.microTable : -1, plan.microKey);
        }
        avSelectedLayer.store(first, std::memory_order_release);
        // Another set of layers: "Guardar capas" no longer writes to the last one's file.
        avLayerSetFile = juce::File();
        avLayerSetId = {};
        refreshVoice.set(false);
        activeProgram = selectedVoice;
        avLayerPanicMask.store((1u << AVLayerCount) - 1, std::memory_order_release);
        avLayerReleaseMask.store((1u << AVLayerCount) - 1, std::memory_order_release);
    }
    if (! avLayerMode.load(std::memory_order_acquire)) {
        const juce::ScopedLock lock(getCallbackLock());
        avLayerMode.store(true, std::memory_order_release);
    }
    loadSelectedLayerIntoEditor(selectedVoice);
}
