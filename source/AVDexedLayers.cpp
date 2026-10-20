// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

// The layer mode of DexedAudioProcessor (TX816 style): MIDI routing by
// channel and key range, the stereo mix, and what the editor changes.

#include "PluginProcessor.h"
#include "midi/MidiCCHandler.h"
#include "parameter/Model.h"

namespace {
float leftBalance(float pan) { return pan <= 0.5f ? 1.0f : 2.0f * (1.0f - pan); }
float rightBalance(float pan) { return pan >= 0.5f ? 1.0f : 2.0f * pan; }
/** Cents to the engine's pitch unit (an octave is 1 << 24). */
int32_t centsToPitch(float cents) { return int32_t(std::lround(double(cents) * double(1 << 24) / 1200.0)); }
}

void DexedAudioProcessor::renderAVLayers(float* left, float* right, int numSamples, MidiBufferIterator& it, MidiBufferIterator& end) {
    // Once per host block: the instrument's DX7 function settings and each
    // layer's own tuning, and where its volume and pan are heading.
    for (int layer = 0; layer < AVLayerCount; ++layer) {
        const auto& settings = avLayerSettings[size_t(layer)];
        const float cents = juce::jlimit(-AVLayerMaxDetune, AVLayerMaxDetune, settings.detune.load(std::memory_order_relaxed));
        avLayers[size_t(layer)].followFunction(controllers, centsToPitch(cents), monoMode, normalizeDxVelocity);
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
            // Off and faded out: nothing to hear (its voices were stopped).
            if (! avLayerSettings[size_t(layer)].enabled.load(std::memory_order_relaxed)
                && ! gainLeft.isSmoothing() && ! gainRight.isSmoothing())
                continue;
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

void DexedAudioProcessor::processAVLayerMidi(const MidiMessage& message) {
    const uint8* buf = message.getRawData();
    const int status = buf[0] & 0xF0;
    const int channel = juce::jlimit(1, 16, message.getChannel());
    const auto listens = [this, channel](int layer) {
        const auto& settings = avLayerSettings[size_t(layer)];
        const int wanted = settings.channel.load(std::memory_order_relaxed);
        return settings.enabled.load(std::memory_order_relaxed) && (wanted == 0 || wanted == channel);
    };
    const auto pitchFor = [this](int layer, int note) {
        return juce::jlimit(0, 127, note + avLayerSettings[size_t(layer)].transpose.load(std::memory_order_relaxed)
                                        + tuningTranspositionShift());
    };
    const auto inRange = [this](int layer, int note) {
        const auto& settings = avLayerSettings[size_t(layer)];
        return note >= settings.lowNote.load(std::memory_order_relaxed) && note <= settings.highNote.load(std::memory_order_relaxed);
    };

    switch (status) {
        case 0x90:
            if (buf[2] > 0) {
                // As the single path: MTS-ESP may filter the note.
                if (synthTuningState->is_standard_tuning() && MTS_HasMaster(mtsClient)
                    && MTS_ShouldFilterNote(mtsClient, buf[1], channel - 1))
                    return;
                for (int layer = 0; layer < AVLayerCount; ++layer) {
                    if (! listens(layer) || ! inRange(layer, buf[1])) continue;
                    const int pitch = pitchFor(layer, buf[1]);
                    avLayerHeld[size_t(layer)][size_t((channel - 1) * 128 + buf[1])] = int8_t(pitch);
                    avLayers[size_t(layer)].keydown(channel, pitch, buf[2]);
                }
                return;
            }
            [[fallthrough]];
        case 0x80:
            for (int layer = 0; layer < AVLayerCount; ++layer) {
                auto& held = avLayerHeld[size_t(layer)][size_t((channel - 1) * 128 + buf[1])];
                // The pitch it was played at; a note-on repeated without its
                // note-off is released as the single path releases it.
                if (held >= 0)
                    avLayers[size_t(layer)].keyup(channel, held);
                else if (listens(layer) && inRange(layer, buf[1]))
                    avLayers[size_t(layer)].keyup(channel, pitchFor(layer, buf[1]));
                held = -1;
            }
            return;

        case 0xB0: {
            const int ctrl = buf[1], value = buf[2];
            midiCCMapper->processControlChange(ctrl, value);
            if (! avEditorOpen.load(std::memory_order_acquire))
                if (const int steps = consumeAVPresetSteps(); steps != 0) {
                    setCurrentProgram(((currentProgram + steps) % 32 + 32) % 32);
                    avHostProgramDirty.store(true, std::memory_order_release);
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
                    avLayerHeld[size_t(layer)].fill(-1);
            }
            return;
        }

        case 0xC0:
            setCurrentProgram(buf[1]);   // into the selected layer
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
    avLayerSettings[size_t(layer)].enabled.store(enabled, std::memory_order_release);
    if (! enabled)   // its notes stop; they never come back when it is turned on again
        avLayerPanicMask.fetch_or(1u << layer, std::memory_order_acq_rel);
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
            // as the modules of a TX816 spread over a stereo mix; together as
            // loud as one layer.
            const float position = count > 1 ? 2.0f * float(layer) / float(count - 1) - 1.0f : 0.0f;
            settings.enabled.store(used, std::memory_order_relaxed);
            settings.channel.store(0, std::memory_order_relaxed);
            settings.lowNote.store(0, std::memory_order_relaxed);
            settings.highNote.store(127, std::memory_order_relaxed);
            settings.transpose.store(0, std::memory_order_relaxed);
            settings.detune.store(used ? std::round(cents * position * 10.0f) / 10.0f : 0.0f, std::memory_order_relaxed);
            settings.pan.store(used ? 0.5f + 0.4f * position : 0.5f, std::memory_order_relaxed);
            settings.volume.store(used ? 1.0f / std::sqrt(float(count)) : 1.0f, std::memory_order_relaxed);
        }
        avSelectedLayer.store(0, std::memory_order_release);
        activeProgram = voice;
        avLayerPanicMask.store((1u << AVLayerCount) - 1, std::memory_order_release);
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
        settings.channel.store(0, std::memory_order_relaxed);
        settings.lowNote.store(0, std::memory_order_relaxed);
        settings.highNote.store(127, std::memory_order_relaxed);
        settings.transpose.store(0, std::memory_order_relaxed);
        settings.detune.store(0.0f, std::memory_order_relaxed);
        settings.volume.store(1.0f, std::memory_order_relaxed);
        settings.pan.store(0.5f, std::memory_order_relaxed);
    }
    avLayerPanicMask.store((1u << AVLayerCount) - 1, std::memory_order_release);
}

juce::ValueTree DexedAudioProcessor::createAVLayersState() const {
    juce::ValueTree layers("avLayers");
    layers.setProperty("mode", avLayerMode.load(std::memory_order_acquire), nullptr);
    layers.setProperty("selected", avSelectedLayer.load(std::memory_order_acquire), nullptr);
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
        const auto voice = getAVLayerProgram(index);
        layer.setProperty("voice", juce::MemoryBlock(voice.getRawData(), Program::PROGRAM_SIZE + 1).toBase64Encoding(), nullptr);
        layers.addChild(layer, -1, nullptr);
    }
    return layers;
}

void DexedAudioProcessor::restoreAVLayersState(const juce::ValueTree& layers) {
    const juce::ScopedLock lock(getCallbackLock());
    for (int index = 0; index < AVLayerCount; ++index) {
        const auto layer = layers.isValid() ? layers.getChild(index) : juce::ValueTree();
        auto& settings = avLayerSettings[size_t(index)];
        const bool valid = layer.isValid() && layer.hasType("layer");
        settings.enabled.store(valid ? bool(layer.getProperty("enabled", false)) : index == 0, std::memory_order_relaxed);
        settings.channel.store(juce::jlimit(0, 16, valid ? int(layer.getProperty("channel", 0)) : 0), std::memory_order_relaxed);
        const int low = juce::jlimit(0, 127, valid ? int(layer.getProperty("lowNote", 0)) : 0);
        const int high = juce::jlimit(0, 127, valid ? int(layer.getProperty("highNote", 127)) : 127);
        settings.lowNote.store(juce::jmin(low, high), std::memory_order_relaxed);
        settings.highNote.store(juce::jmax(low, high), std::memory_order_relaxed);
        settings.transpose.store(juce::jlimit(-24, 24, valid ? int(layer.getProperty("transpose", 0)) : 0), std::memory_order_relaxed);
        settings.detune.store(juce::jlimit(-AVLayerMaxDetune, AVLayerMaxDetune, valid ? float(layer.getProperty("detune", 0.0f)) : 0.0f), std::memory_order_relaxed);
        settings.volume.store(juce::jlimit(0.0f, 1.0f, valid ? float(layer.getProperty("volume", 1.0f)) : 1.0f), std::memory_order_relaxed);
        settings.pan.store(juce::jlimit(0.0f, 1.0f, valid ? float(layer.getProperty("pan", 0.5f)) : 0.5f), std::memory_order_relaxed);
        Program voice = Program::initialProgram();
        juce::MemoryBlock data;
        if (valid && data.fromBase64Encoding(layer.getProperty("voice").toString()) && data.getSize() >= size_t(Program::PROGRAM_SIZE)) {
            voice = Program(static_cast<const uint8_t*>(data.getData()));
            Cartridge::applyBankRules(voice);
        }
        avLayers[size_t(index)].setProgram(voice);
    }
    const int selected = juce::jlimit(0, AVLayerCount - 1, layers.isValid() ? int(layers.getProperty("selected", 0)) : 0);
    avSelectedLayer.store(selected, std::memory_order_release);
    // The session's sound is the selected layer's voice.
    avLayers[size_t(selected)].setProgram(activeProgram);
    avLayerMode.store(layers.isValid() && bool(layers.getProperty("mode", false)), std::memory_order_release);
    avLayerPanicMask.store((1u << AVLayerCount) - 1, std::memory_order_release);
}
