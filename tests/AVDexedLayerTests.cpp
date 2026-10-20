// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

// The layer mode (TX816 style). One layer in the centre at full volume plays
// the very samples of the single path; channels, key ranges, transposition,
// tuning, volume and pan do what they say; edits stay with their layer; the
// layers come back with the session.

#include "../source/PluginProcessor.h"
#include "../source/parameter/Model.h"
#include <iostream>

namespace {
bool expect(bool condition, const char* message) {
    if (! condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

constexpr double rate = 48000.0;

struct Stereo {
    std::vector<float> left, right;
    bool operator==(const Stereo& other) const { return left == other.left && right == other.right; }
    float peak(bool rightChannel) const {
        float p = 0;
        for (const float v : rightChannel ? right : left) p = std::max(p, std::abs(v));
        return p;
    }
};

void setParameter(DexedAudioProcessor& processor, const juce::String& id, float value) {
    if (auto* parameter = processor.parameters.getParameter(id))
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

/** A bank with some variety: the init voice with different algorithms,
    ratios, feedback and LFO settings, so every renderer path is used. */
Cartridge varietyBank() {
    Cartridge bank;
    for (int slot = 0; slot < 32; ++slot) {
        auto voice = Program::initialProgram();
        uint8_t data[Program::PROGRAM_SIZE + 1];
        std::memcpy(data, voice.getRawData(), sizeof(data));
        data[134] = uint8_t(slot);                      // algorithm
        data[135] = uint8_t(slot % 8);                  // feedback
        data[137] = uint8_t(20 + slot * 2);             // LFO speed
        data[139] = uint8_t(slot % 3 == 0 ? 30 : 0);    // pitch mod depth
        data[142] = uint8_t(slot % 6);                  // LFO wave
        data[143] = uint8_t(slot % 8);                  // pitch mod sensitivity
        for (int op = 0; op < 6; ++op) {
            data[op * 21 + 16] = uint8_t(op == 5 ? 99 : 60 + (slot + op) % 30);   // output level
            data[op * 21 + 18] = uint8_t(1 + (slot + op) % 4);                    // coarse
            data[op * 21 + 0] = uint8_t(60 + (op * 7 + slot) % 40);               // EG rate 1
            data[op * 21 + 6] = uint8_t(slot % 2 ? 80 : 0);                       // EG level 3
        }
        bank.packProgram(data, slot, "LAYER " + juce::String(slot));
    }
    return bank;
}

std::unique_ptr<DexedAudioProcessor> makeProcessor(const Cartridge& bank, int slot) {
    auto processor = std::make_unique<DexedAudioProcessor>(DexedAudioProcessor::TestMode{});
    processor->setRateAndBufferSizeDetails(rate, 1024);
    processor->prepareToPlay(rate, 1024);
    processor->setActiveBank(bank, juce::File());
    processor->setCurrentProgram(slot);
    return processor;
}

struct Note { double time; int channel, note, velocity; };   // velocity 0: note-off

/** Renders `seconds` with uneven host blocks; `notes` at their times. */
Stereo render(DexedAudioProcessor& processor, const std::vector<Note>& notes, double seconds,
              const std::vector<std::pair<double, juce::MidiMessage>>& extra = {}) {
    const int total = int(seconds * rate);
    const int sizes[] = { 512, 300, 64, 1000, 37 };
    juce::AudioBuffer<float> buffer(2, 1024);
    Stereo out;
    int position = 0, block = 0;
    while (position < total) {
        const int size = juce::jmin(sizes[block++ % 5], total - position);
        juce::MidiBuffer midi;
        for (const auto& note : notes) {
            const int at = int(note.time * rate);
            if (at >= position && at < position + size)
                midi.addEvent(note.velocity > 0 ? juce::MidiMessage::noteOn(note.channel, note.note, (juce::uint8) note.velocity)
                                                : juce::MidiMessage::noteOff(note.channel, note.note), at - position);
        }
        for (const auto& [time, message] : extra) {
            const int at = int(time * rate);
            if (at >= position && at < position + size) midi.addEvent(message, at - position);
        }
        buffer.setSize(2, size, false, false, true);
        buffer.clear();
        processor.processBlock(buffer, midi);
        out.left.insert(out.left.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + size);
        out.right.insert(out.right.end(), buffer.getReadPointer(1), buffer.getReadPointer(1) + size);
        position += size;
    }
    return out;
}

const std::vector<Note> chord {
    { 0.00, 1, 48, 100 }, { 0.00, 1, 60, 90 }, { 0.01, 1, 64, 70 }, { 0.02, 1, 67, 120 },
    { 0.40, 1, 48, 0 }, { 0.40, 1, 60, 0 }, { 0.41, 1, 64, 0 }, { 0.42, 1, 67, 0 },
    { 0.50, 1, 72, 127 }, { 0.80, 1, 72, 0 },
};
const std::vector<std::pair<double, juce::MidiMessage>> gestures {
    { 0.10, juce::MidiMessage::pitchWheel(1, 12000) }, { 0.20, juce::MidiMessage::controllerEvent(1, 1, 90) },
    { 0.25, juce::MidiMessage::channelPressureChange(1, 70) }, { 0.30, juce::MidiMessage::controllerEvent(1, 64, 127) },
    { 0.60, juce::MidiMessage::controllerEvent(1, 64, 0) }, { 0.70, juce::MidiMessage::pitchWheel(1, 8192) },
};

/** Layer mode with only layer 1, in the centre, at full volume. */
void oneLayer(DexedAudioProcessor& processor) {
    processor.setAVLayerMode(true);
    for (int layer = 1; layer < DexedAudioProcessor::AVLayerCount; ++layer)
        processor.setAVLayerEnabled(layer, false);
    processor.setAVLayerEnabled(0, true);
}

bool testOneLayerIsTheSinglePath() {
    auto bank = varietyBank();
    int different = 0;
    struct Variation { const char* name; std::vector<std::pair<juce::String, float>> values; };
    const std::vector<Variation> variations {
        { "plain", {} },
        { "mono+porta", { { IDs::monoMode.name, 1.0f }, { IDs::portamentoTm.name, 40.0f } } },
        { "filter", { { IDs::cutoff.name, 0.3f }, { IDs::resonance.name, 0.6f } } },
        { "reverb", { { IDs::reverbAmount.name, 0.5f } } },
        { "delay", { { IDs::delayAmount.name, 0.4f } } },
        { "chorus", { { IDs::chorusAmount.name, 0.5f } } },
        { "wheel+tune", { { IDs::modWheel.name, 99.0f }, { IDs::modWheelPitch.name, 1.0f }, { IDs::masterTuneAdj.name, 0.7f },
                          { IDs::pitchBendUp.name, 12.0f } } },
    };
    for (int engine = 0; engine < 3; ++engine)
        for (int slot = 0; slot < 32; slot += 3)
            for (const auto& variation : variations) {
                auto single = makeProcessor(bank, slot);
                auto layered = makeProcessor(bank, slot);
                for (auto* processor : { single.get(), layered.get() }) {
                    setParameter(*processor, IDs::engineType.name, float(engine));
                    for (const auto& [id, value] : variation.values) setParameter(*processor, id, value);
                }
                oneLayer(*layered);
                if (! (render(*single, chord, 1.2, gestures) == render(*layered, chord, 1.2, gestures))) {
                    if (++different <= 5)
                        std::cerr << "  layer differs: slot " << slot << " engine " << engine << " " << variation.name << '\n';
                }
            }
    return expect(different == 0, "one layer in the centre at full volume plays the single path's samples");
}

bool testRoutingAndMix() {
    bool passed = true;
    auto bank = varietyBank();
    auto processor = makeProcessor(bank, 4);
    auto& p = *processor;
    oneLayer(p);
    // Layer 2: channel 2 only, upper half of the keyboard.
    p.setAVLayerProgram(1, bank.unpackProgram(9));
    p.setAVLayerEnabled(1, true);
    auto& second = p.getAVLayerSettings(1);
    second.channel = 2;
    second.lowNote = 60;
    auto& first = p.getAVLayerSettings(0);
    first.highNote = 59;

    render(p, { { 0.0, 1, 48, 100 } }, 0.05);
    passed &= expect(p.getAVLayerSoundingVoices(0) == 1 && p.getAVLayerSoundingVoices(1) == 0,
                     "a low note on channel 1 reaches layer 1 only");
    render(p, { { 0.0, 1, 72, 100 } }, 0.05);
    passed &= expect(p.getAVLayerSoundingVoices(0) == 1 && p.getAVLayerSoundingVoices(1) == 0,
                     "a high note on channel 1 reaches no layer (layer 1 ends at 59, layer 2 listens to channel 2)");
    render(p, { { 0.0, 2, 72, 100 } }, 0.05);
    passed &= expect(p.getAVLayerSoundingVoices(1) == 1, "a high note on channel 2 reaches layer 2");
    render(p, { { 0.0, 1, 48, 0 }, { 0.0, 2, 72, 0 } }, 3.0);
    passed &= expect(p.getAVLayerSoundingVoices(0) == 0 && p.getAVLayerSoundingVoices(1) == 0, "their note-offs release them");

    // Pan and volume.
    first.highNote = 127;
    p.setAVLayerEnabled(1, false);
    first.pan = 0.0f;
    render(p, {}, 0.1);   // the pan glides there
    const auto hardLeft = render(p, { { 0.0, 1, 60, 100 }, { 0.3, 1, 60, 0 } }, 0.5);
    passed &= expect(hardLeft.peak(false) > 0.01f && hardLeft.peak(true) < 1.0e-6f, "a layer panned hard left is silent on the right");
    first.pan = 0.5f;
    render(p, {}, 0.1);
    // (Compared on a fresh instrument: the filter's DC blocker remembers the
    // hard-left notes on one side only.)
    auto fresh = makeProcessor(bank, 4);
    oneLayer(*fresh);
    const auto centre = render(*fresh, { { 0.0, 1, 60, 100 }, { 0.3, 1, 60, 0 } }, 0.5);
    passed &= expect(centre.left == centre.right && centre.peak(false) > 0.01f, "a layer in the centre is the same on both sides");
    first.volume = 0.0f;
    render(p, {}, 0.1);   // the volume glides to zero
    const auto muted = render(p, { { 0.0, 1, 60, 100 }, { 0.3, 1, 60, 0 } }, 0.5);
    passed &= expect(muted.peak(false) < 1.0e-6f, "a layer at zero volume is silent");
    first.volume = 1.0f;
    render(p, { { 0.0, 1, 60, 0 } }, 2.0);
    return passed;
}

bool testTransposeAndDetune() {
    bool passed = true;
    auto bank = varietyBank();
    auto shifted = makeProcessor(bank, 2), plain = makeProcessor(bank, 2);
    oneLayer(*shifted);
    oneLayer(*plain);
    shifted->getAVLayerSettings(0).transpose = 12;
    passed &= expect(render(*shifted, { { 0.0, 1, 60, 100 }, { 0.4, 1, 60, 0 } }, 0.8)
                         == render(*plain, { { 0.0, 1, 72, 100 }, { 0.4, 1, 72, 0 } }, 0.8),
                     "a layer transposed +12 plays note 60 as note 72");

    // A transposition changed while a key is held: its note-off still releases it.
    shifted->getAVLayerSettings(0).transpose = 0;
    render(*shifted, { { 0.0, 1, 60, 100 } }, 0.05);
    shifted->getAVLayerSettings(0).transpose = 7;
    render(*shifted, { { 0.0, 1, 60, 0 } }, 3.0);
    passed &= expect(shifted->getAVLayerSoundingVoices(0) == 0, "a note held across a transposition change is released");

    auto detuned = makeProcessor(bank, 2), reference = makeProcessor(bank, 2);
    oneLayer(*detuned);
    oneLayer(*reference);
    detuned->getAVLayerSettings(0).detune = 7.0f;
    passed &= expect(! (render(*detuned, { { 0.0, 1, 60, 100 } }, 0.3) == render(*reference, { { 0.0, 1, 60, 100 } }, 0.3)),
                     "a detuned layer sounds different");
    return passed;
}

bool testEditsStayWithTheirLayer() {
    bool passed = true;
    auto bank = varietyBank();
    auto processor = makeProcessor(bank, 0);
    auto& p = *processor;
    p.setAVLayerMode(true);
    p.setAVLayerProgram(1, bank.unpackProgram(5));
    setParameter(p, IDs::algorithm.name, 17.0f);           // edit layer 1's voice
    const auto edited = p.activeProgram.getRawData()[134];
    p.selectAVLayer(1);
    passed &= expect(p.activeProgram.getName() == bank.unpackProgram(5).getName(), "selecting layer 2 shows its voice");
    passed &= expect(p.getAVLayerProgram(0).getRawData()[134] == edited, "the edit stays with layer 1");
    p.selectAVLayer(0);
    passed &= expect(p.activeProgram.getRawData()[134] == edited, "and comes back when layer 1 is selected again");
    p.selectAVLayer(1);
    p.setCurrentProgram(12);                               // the browser loads into the selected layer
    passed &= expect(p.getAVLayerProgram(1).getName() == bank.unpackProgram(12).getName()
                         && p.getAVLayerProgram(0).getRawData()[134] == edited,
                     "a sound chosen in the browser goes into the selected layer only");
    return passed;
}

bool testStackAndSession() {
    bool passed = true;
    auto bank = varietyBank();
    auto processor = makeProcessor(bank, 7);
    auto& p = *processor;
    p.stackAVLayers(8, 7.0f);
    bool stacked = p.isAVLayerModeOn();
    for (int layer = 0; layer < 8; ++layer) {
        const auto& settings = p.getAVLayerSettings(layer);
        stacked &= settings.enabled.load() && p.getAVLayerProgram(layer).getName() == bank.unpackProgram(7).getName();
    }
    passed &= expect(stacked, "stacking puts the voice in all eight layers");
    passed &= expect(std::abs(p.getAVLayerSettings(0).detune.load() + 7.0f) < 1.0e-4f && std::abs(p.getAVLayerSettings(7).detune.load() - 7.0f) < 1.0e-4f
                         && p.getAVLayerSettings(0).pan.load() < 0.5f && p.getAVLayerSettings(7).pan.load() > 0.5f,
                     "stacked layers spread from low and left to high and right");
    const auto wide = render(p, { { 0.0, 1, 60, 100 } }, 0.3);
    passed &= expect(! (wide.left == wide.right), "a stack is stereo");

    // The session keeps everything.
    p.getAVLayerSettings(3).channel = 5;
    p.getAVLayerSettings(3).transpose = -12;
    p.selectAVLayer(2);
    juce::MemoryBlock state;
    p.getStateInformation(state);
    auto restored = makeProcessor(bank, 0), again = makeProcessor(bank, 0);
    restored->setStateInformation(state.getData(), int(state.getSize()));
    again->setStateInformation(state.getData(), int(state.getSize()));
    bool same = restored->isAVLayerModeOn() && restored->getAVSelectedLayer() == 2;
    for (int layer = 0; layer < 8; ++layer) {
        const auto& a = p.getAVLayerSettings(layer);
        const auto& b = restored->getAVLayerSettings(layer);
        same &= a.enabled.load() == b.enabled.load() && a.channel.load() == b.channel.load() && a.transpose.load() == b.transpose.load()
             && std::abs(a.detune.load() - b.detune.load()) < 1.0e-4f && std::abs(a.pan.load() - b.pan.load()) < 1.0e-4f
             && std::abs(a.volume.load() - b.volume.load()) < 1.0e-4f
             && std::memcmp(p.getAVLayerProgram(layer).getRawData(), restored->getAVLayerProgram(layer).getRawData(), 155) == 0;
    }
    passed &= expect(same, "a session restores every layer");
    const auto restoredSound = render(*restored, chord, 0.6);
    passed &= expect(restoredSound == render(*again, chord, 0.6) && restoredSound.peak(false) > 0.01f
                         && ! (restoredSound.left == restoredSound.right),
                     "and they play (the same each time, in stereo)");

    // A session saved without layers (0.2.x) plays one layer.
    juce::MemoryBlock old;
    auto plain = makeProcessor(bank, 0);
    plain->getStateInformation(old);
    restored->setStateInformation(old.getData(), int(old.getSize()));
    passed &= expect(! restored->isAVLayerModeOn(), "a session without layers turns the layer mode off");

    // A sound file is one sound: it never carries the layers.
    const auto sound = p.createAVNativePresetState("Stack", "id");
    auto xml = juce::AudioProcessor::getXmlFromBinary(sound.getData(), int(sound.getSize()));
    passed &= expect(xml != nullptr && xml->getChildByName("avLayers") == nullptr, "a sound file does not carry the layers");

    // Leaving the layer mode plays the selected voice the single way.
    render(p, { { 0.0, 1, 60, 0 } }, 2.0);
    p.setAVLayerMode(false);
    const auto after = render(p, { { 0.0, 1, 60, 100 } }, 0.3);
    bool layersQuiet = true;
    for (int layer = 0; layer < 8; ++layer) layersQuiet &= p.getAVLayerSoundingVoices(layer) == 0;
    passed &= expect(p.getAVActiveVoices() == 1 && layersQuiet && after.peak(false) > 0.01f && after.left == after.right,
                     "with the layer mode off the selected voice plays as one sound");
    return passed;
}
}

bool runAVDexedLayerTests() {
    bool passed = testOneLayerIsTheSinglePath();
    passed &= testRoutingAndMix();
    passed &= testTransposeAndDetune();
    passed &= testEditsStayWithTheirLayer();
    passed &= testStackAndSession();
    std::cout << (passed ? "Layer checks passed.\n" : "Layer checks FAILED.\n");
    return passed;
}
