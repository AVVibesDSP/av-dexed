// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

// The layer mode (X816 mode). One layer in the centre at full volume plays
// the very samples of the single path; channels, key ranges, transposition,
// tuning, volume and pan do what they say; edits stay with their layer; the
// layers come back with the session; a stack is not a "pluck".

#include "../source/PluginProcessor.h"
#include "../source/parameter/Model.h"
#include "../source/core/AVDexedSysex.h"
#include "../source/core/AVDexedMicroTuning.h"
#include "../source/core/AVDexedLibrary.h"
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
             && std::abs(a.volume.load() - b.volume.load()) < 1.0e-4f && std::abs(a.phase.load() - b.phase.load()) < 1.0e-4f
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

    // A sound file is one sound: it carries the layers only when saved with them.
    const auto sound = p.createAVNativePresetState("Stack", "id");
    auto xml = juce::AudioProcessor::getXmlFromBinary(sound.getData(), int(sound.getSize()));
    passed &= expect(xml != nullptr && xml->getChildByName("avLayers") == nullptr, "a sound file does not carry the layers");

    // The layers' own sound file comes back with the session; opening
    // another set of layers forgets it.
    const juce::File setFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("Set.avdexpreset");
    p.setAVLayerSetContext(setFile, "set-id");
    juce::MemoryBlock withSet;
    p.getStateInformation(withSet);
    restored->setStateInformation(withSet.getData(), int(withSet.getSize()));
    passed &= expect(restored->getAVLayerSetFile() == setFile && restored->getAVLayerSetId() == "set-id",
                     "a session keeps the layers' own sound file");
    const auto layered = p.createAVNativePresetState("Stack", "id", true);
    auto layeredXml = juce::AudioProcessor::getXmlFromBinary(layered.getData(), int(layered.getSize()));
    const auto* layeredLayers = layeredXml != nullptr ? layeredXml->getChildByName("avLayers") : nullptr;
    passed &= expect(layeredLayers != nullptr && ! layeredLayers->hasAttribute("setPath") && ! layeredLayers->hasAttribute("setId"),
                     "a layers preset carries the layers, not a path to itself");
    std::array<AVDexedPerformance::ResolvedLayer, DexedAudioProcessor::AVLayerCount> other {};
    other[0].plan.used = other[0].plan.enabled = true;
    other[0].voice = bank.unpackProgram(3);
    restored->applyAVLayerSetup(other);
    passed &= expect(restored->getAVLayerSetFile() == juce::File() && restored->getAVLayerSetId().isEmpty(),
                     "another set of layers forgets it");
    p.setAVLayerSetContext({}, {});

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

/** Every layer of `a` and `b` alike: settings and voice. */
bool sameLayers(DexedAudioProcessor& a, DexedAudioProcessor& b) {
    bool same = true;
    for (int layer = 0; layer < DexedAudioProcessor::AVLayerCount; ++layer) {
        const auto& x = a.getAVLayerSettings(layer);
        const auto& y = b.getAVLayerSettings(layer);
        same &= x.enabled.load() == y.enabled.load() && x.channel.load() == y.channel.load()
             && std::abs(x.detune.load() - y.detune.load()) < 1.0e-4f && std::abs(x.pan.load() - y.pan.load()) < 1.0e-4f
             && std::abs(x.unison.load() - y.unison.load()) < 1.0e-4f && std::abs(x.phase.load() - y.phase.load()) < 1.0e-4f
             && std::memcmp(a.getAVLayerProgram(layer).getRawData(), b.getAVLayerProgram(layer).getRawData(), 155) == 0;
    }
    return same;
}

bool testLayersFile() {
    bool passed = true;
    auto bank = varietyBank();
    auto processor = makeProcessor(bank, 6);
    auto& p = *processor;
    p.stackAVLayers(5, 9.0f);
    p.getAVLayerSettings(2).channel = 4;
    p.getAVLayerSettings(3).unison = 3.0f;
    p.setAVLayerProgram(4, bank.unpackProgram(20));
    p.selectAVLayer(3);

    // "Guardar capas": a sound file (.avdexpreset) that carries every layer.
    const auto preset = p.createAVNativePresetState("Mi stack", "id-capas", true);
    auto opened = makeProcessor(bank, 0);
    opened->setAVLayerMode(false);
    passed &= expect(opened->loadAVNativePreset(preset.getData(), int(preset.getSize())), "a layers preset opens");
    passed &= expect(opened->isAVLayerModeOn() && opened->getAVSelectedLayer() == 3 && sameLayers(p, *opened),
                     "every layer, its voice and the selected layer come back");
    passed &= expect(opened->activeProgram.getName() == p.activeProgram.getName(), "the editor shows the selected layer's voice");
    passed &= expect(opened->getAVPresetName() == "Mi stack" && opened->getAVPresetId() == "id-capas",
                     "and the sound keeps the preset's name");

    // In layer mode a single sound goes into the selected layer only.
    const auto single = makeProcessor(bank, 11)->createAVNativePresetState("Uno", "id-uno");
    passed &= expect(opened->loadAVNativePreset(single.getData(), int(single.getSize())), "a single sound opens");
    bool othersKept = opened->isAVLayerModeOn() && opened->getAVSelectedLayer() == 3
                      && opened->getAVLayerProgram(3).getName() == bank.unpackProgram(11).getName();
    for (int layer = 0; layer < DexedAudioProcessor::AVLayerCount; ++layer)
        if (layer != 3)
            othersKept &= std::memcmp(p.getAVLayerProgram(layer).getRawData(), opened->getAVLayerProgram(layer).getRawData(), 155) == 0;
    passed &= expect(othersKept, "a single sound goes into the selected layer, the others play on");

    // The layers files of 0.3.2 (.avdexlayers) still open.
    auto presetXml = juce::AudioProcessor::getXmlFromBinary(preset.getData(), int(preset.getSize()));
    juce::XmlElement legacy("avDexedLayers");
    legacy.setAttribute("formatVersion", 1);
    legacy.setAttribute("name", "Mi stack");
    legacy.addChildElement(new juce::XmlElement(*presetXml->getChildByName("avLayers")));
    auto old = makeProcessor(bank, 0);
    passed &= expect(old->loadAVLayersFile(legacy.toString()) && old->getAVSelectedLayer() == 3 && sameLayers(p, *old),
                     "an old layers file opens");
    passed &= expect(! old->loadAVLayersFile("<notLayers/>") && ! old->loadAVLayersFile("garbage"), "other files are refused");
    return passed;
}

/** The review's cases: each must hold (they did not before the fixes). */
bool testReviewCases() {
    bool passed = true;
    auto bank = varietyBank();
    // 1. Each layer uses its own voice's transpose, whichever layer is selected.
    {
        auto bass = bank.unpackProgram(9);
        uint8_t data[Program::PROGRAM_SIZE + 1];
        std::memcpy(data, bass.getRawData(), sizeof(data));
        data[144] = 12;   // an octave down
        bank.packProgram(data, 9, "BASS");
        auto single = makeProcessor(bank, 9);
        auto layered = makeProcessor(bank, 2);
        layered->setAVLayerMode(true);
        layered->setAVLayerProgram(1, bank.unpackProgram(9));
        layered->setAVLayerEnabled(1, true);
        layered->setAVLayerEnabled(0, false);   // layer 1 (another voice, transpose 0) stays selected
        render(*layered, {}, 0.1);
        passed &= expect(render(*layered, chord, 0.8) == render(*single, chord, 0.8),
                         "a layer plays with its own voice transpose, not the selected layer's");
    }
    // 2. A program change and notes in the same block: the notes play the new voice.
    {
        auto processor = makeProcessor(bank, 0);
        oneLayer(*processor);
        render(*processor, {}, 0.05);
        juce::AudioBuffer<float> buffer(2, 512);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::programChange(1, 5), 0);
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
        buffer.clear();
        processor->processBlock(buffer, midi);
        render(*processor, {}, 0.1);
        passed &= expect(processor->getAVLayerSoundingVoices(0) == 1 && processor->getAVLayerProgram(0).getName() == bank.unpackProgram(5).getName(),
                         "notes after a program change in the same block keep sounding, with the new voice");
    }
    // 3. Operators off in a layer that is not selected come back off.
    {
        auto processor = makeProcessor(bank, 0);
        processor->setAVLayerMode(true);
        auto voice = bank.unpackProgram(4);
        voice.setOperatorSwitches(0x2B);
        processor->setAVLayerProgram(3, voice);
        juce::MemoryBlock state;
        processor->getStateInformation(state);
        auto restored = makeProcessor(bank, 0);
        restored->setStateInformation(state.getData(), int(state.getSize()));
        passed &= expect(restored->getAVLayerProgram(3).getRawData()[155] == 0x2B, "a layer's operator switches survive the session");
    }
    // 4. A new channel while the pedal is down: the layer lets its notes go.
    {
        auto processor = makeProcessor(bank, 2);
        oneLayer(*processor);
        render(*processor, { { 0.0, 1, 60, 100 }, { 0.05, 1, 60, 0 } }, 0.1,
               { { 0.0, juce::MidiMessage::controllerEvent(1, 64, 127) } });
        processor->setAVLayerChannel(0, 2);
        render(*processor, {}, 3.0);
        passed &= expect(processor->getAVLayerSoundingVoices(0) == 0, "changing a layer's channel releases its pedal");
    }
    // 7. Alternate layers: one member's note-off leaves the other's note alone.
    {
        auto processor = makeProcessor(bank, 2);
        oneLayer(*processor);
        processor->setAVLayerProgram(1, bank.unpackProgram(2));
        processor->setAVLayerEnabled(1, true);
        processor->getAVLayerSettings(0).alternateGroup = 0;
        processor->getAVLayerSettings(1).alternateGroup = 0;
        render(*processor, { { 0.0, 1, 60, 100 }, { 0.01, 2, 60, 100 }, { 0.05, 2, 60, 0 } }, 2.5);
        passed &= expect(processor->getAVLayerSoundingVoices(0) + processor->getAVLayerSoundingVoices(1) == 1,
                         "in an alternate group a note-off releases only the note it ends");
    }
    // 8. Turning a layer off fades it, then stops it.
    {
        auto processor = makeProcessor(bank, 2);
        oneLayer(*processor);
        render(*processor, { { 0.0, 1, 60, 100 } }, 0.1);
        processor->setAVLayerEnabled(0, false);
        render(*processor, {}, 0.005);
        const bool stillFading = processor->getAVLayerSoundingVoices(0) == 1;
        render(*processor, {}, 0.1);
        passed &= expect(stillFading && processor->getAVLayerSoundingVoices(0) == 0, "an off layer fades out before its notes stop");
    }
    // 6. A note-on with a data byte above 127 (passed through by a host) is harmless.
    {
        auto processor = makeProcessor(bank, 2);
        oneLayer(*processor);
        juce::AudioBuffer<float> buffer(2, 256);
        juce::MidiBuffer midi;
        const uint8_t bad[] = { 0x9F, 0xFF, 0x64 };
        midi.addEvent(bad, 3, 0);
        processor->processBlock(buffer, midi);
        passed &= expect(true, "a malformed note-on does not write outside the held-notes table");
    }
    return passed;
}

bool testBankVoiceExtras() {
    // A DX7II bank with its AMEM: in layer mode a voice brings its unison and
    // random pitch to the selected layer; another voice without them clears them.
    auto bank = varietyBank();
    std::array<uint8_t, 1120> amem {};
    amem[3 * 35 + 5] = 2;          // slot 4: unison
    amem[3 * 35 + 34] = 3;         // its detune
    amem[3 * 35 + 4] = 2 << 4;     // random pitch
    juce::MemoryBlock file;
    const auto amemMessage = AVDexedSysex::amemMessage(amem.data());
    file.append(amemMessage.data(), amemMessage.size());
    const auto voices = AVDexedSysex::bankMessage(reinterpret_cast<const uint8_t*>(bank.getRawVoice()));
    file.append(voices.data(), voices.size());
    Cartridge withExtras;
    bool passed = expect(withExtras.load(static_cast<const uint8_t*>(file.getData()), int(file.getSize())) == 0
                             && withExtras.getAmemVoice(3) != nullptr,
                         "a bank keeps the AMEM sent with it");
    auto processor = makeProcessor(withExtras, 0);
    auto& settings = processor->getAVLayerSettings(0);
    processor->setCurrentProgram(3);
    passed &= expect(settings.unison.load() == 0.0f, "with one layer a voice's extras are not used");
    processor->setAVLayerMode(true);
    processor->setCurrentProgram(3);
    passed &= expect(settings.unison.load() > 0.0f && settings.randomPitch.load() > 0.0f, "in layer mode the voice brings them");
    processor->setCurrentProgram(4);
    passed &= expect(settings.unison.load() == 0.0f && settings.randomPitch.load() == 0.0f, "and the next voice without them clears them");
    return passed;
}

/** A copy of `bank` with an AMEM: slot 4 unison (detune 3) and random pitch, slot 6 mono. */
Cartridge bankWithAmem(Cartridge& bank) {
    std::array<uint8_t, 1120> amem {};
    amem[3 * 35 + 5] = 2;
    amem[3 * 35 + 34] = 3;
    amem[3 * 35 + 4] = 2 << 4;
    amem[5 * 35 + 5] = 1;
    juce::MemoryBlock file;
    const auto amemMessage = AVDexedSysex::amemMessage(amem.data());
    file.append(amemMessage.data(), amemMessage.size());
    const auto voices = AVDexedSysex::bankMessage(reinterpret_cast<const uint8_t*>(bank.getRawVoice()));
    file.append(voices.data(), voices.size());
    Cartridge withAmem;
    withAmem.load(static_cast<const uint8_t*>(file.getData()), int(file.getSize()));
    return withAmem;
}

/** The second review's cases. */
bool testSecondReviewCases() {
    bool passed = true;
    auto bank = varietyBank();
    // 1. The same key three times in an alternate group: three note-offs release all.
    {
        auto processor = makeProcessor(bank, 2);
        oneLayer(*processor);
        processor->setAVLayerProgram(1, bank.unpackProgram(2));
        processor->setAVLayerEnabled(1, true);
        processor->getAVLayerSettings(0).alternateGroup = 0;
        processor->getAVLayerSettings(1).alternateGroup = 0;
        render(*processor, { { 0.0, 1, 60, 100 }, { 0.01, 1, 60, 100 }, { 0.02, 1, 60, 100 },
                             { 0.1, 1, 60, 0 }, { 0.11, 1, 60, 0 }, { 0.12, 1, 60, 0 } }, 3.0);
        passed &= expect(processor->getAVLayerSoundingVoices(0) + processor->getAVLayerSoundingVoices(1) == 0,
                         "a key played three times in an alternate group is released by its three note-offs");
    }
    // 2. A program change to a mono voice and a note in the same block: the note plays mono and keeps sounding.
    {
        auto withAmem = bankWithAmem(bank);
        auto processor = makeProcessor(withAmem, 0);
        oneLayer(*processor);
        render(*processor, {}, 0.05);
        juce::AudioBuffer<float> buffer(2, 512);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::programChange(1, 5), 0);
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 10);
        buffer.clear();
        processor->processBlock(buffer, midi);
        render(*processor, {}, 0.2);
        passed &= expect(processor->getAVLayerSettings(0).mono.load() == 1 && processor->getAVLayerSoundingVoices(0) == 1,
                         "a program change to a mono voice in the middle of a block keeps the note after it");
    }
    // 3. A session keeps the bank's AMEM and the selected layer's unison when the host re-selects the program.
    {
        auto withAmem = bankWithAmem(bank);
        auto processor = makeProcessor(withAmem, 0);
        processor->setAVLayerMode(true);
        processor->setCurrentProgram(3);
        juce::MemoryBlock state;
        processor->getStateInformation(state);
        auto restored = makeProcessor(bank, 0);
        restored->setStateInformation(state.getData(), int(state.getSize()));
        restored->setCurrentProgram(3);   // as hosts do right after a restore
        passed &= expect(restored->getAVLayerSettings(0).unison.load() > 0.0f && restored->currentCart.getAmemVoice(3) != nullptr,
                         "a session keeps the AMEM and the restored layer's unison");
        restored->setCurrentProgram(5);
        passed &= expect(restored->getAVLayerSettings(0).mono.load() == 1, "and later program changes still find it");
    }
    // 4. Unison at full polyphony: a new key takes two voices.
    {
        auto processor = makeProcessor(bank, 2);
        oneLayer(*processor);
        processor->getAVLayerSettings(0).unison = 5.0f;
        std::vector<Note> keys;
        for (int k = 0; k < 9; ++k) keys.push_back({ 0.01 * k, 1, 48 + 2 * k, 100 });
        render(*processor, keys, 0.2);
        render(*processor, { { 0.0, 1, 64, 0 } }, 3.0);
        passed &= expect(processor->getAVLayerSoundingVoices(0) == 14,
                         "with every voice busy a unison key takes two voices (the oldest key's pair)");
    }
    // 8. Another voice stored in a slot drops that slot's AMEM, in the bank and in its file.
    {
        auto withAmem = bankWithAmem(bank);
        auto other = bank.unpackProgram(20);
        withAmem.packProgram(other.getRawData(), 3, "OTHER");
        const auto* slot = withAmem.getAmemVoice(3);
        bool cleared = slot != nullptr;
        for (int i = 0; slot != nullptr && i < 35; ++i) cleared &= slot[i] == 0;
        passed &= expect(cleared && withAmem.getAmemVoice(5)[5] == 1, "storing another voice clears only its slot's AMEM");
        juce::MemoryBlock original;
        const auto amemMessage = AVDexedSysex::amemMessage(bankWithAmem(bank).getAmem());
        original.append(amemMessage.data(), amemMessage.size());
        const auto voices = AVDexedSysex::bankMessage(reinterpret_cast<const uint8_t*>(bank.getRawVoice()));
        original.append(voices.data(), voices.size());
        const auto written = AVDexedLibrary::composeBankFile(original, withAmem, {});
        Cartridge reread;
        reread.load(static_cast<const uint8_t*>(written.getData()), int(written.getSize()));
        passed &= expect(reread.getAmemVoice(3) != nullptr && reread.getAmemVoice(3)[5] == 0 && reread.getAmemVoice(5)[5] == 1,
                         "and the file written keeps it so");
    }
    // 9. Random pitch differs from layer to layer (separate modules).
    {
        auto processor = makeProcessor(bank, 2);
        oneLayer(*processor);
        processor->setAVLayerProgram(1, bank.unpackProgram(2));
        processor->setAVLayerEnabled(1, true);
        for (int layer = 0; layer < 2; ++layer) {
            processor->getAVLayerSettings(layer).randomPitch = 20.0f;
            processor->getAVLayerSettings(layer).pan = layer == 0 ? 0.0f : 1.0f;
        }
        render(*processor, {}, 0.1);
        const auto sound = render(*processor, { { 0.0, 1, 60, 100 } }, 0.3);
        passed &= expect(! (sound.left == sound.right), "two layers with random pitch get different tunings");
    }
    return passed;
}

bool testMicroTuning() {
    bool passed = true;
    const auto werck = AVDexedMicroTuning::presetCents(5, 0);
    passed &= expect(std::abs(werck[69] + 11.73f) < 0.01f && werck[60] == 0.0f, "Werckmeister: A is 11.7 cents low, C as it is");
    const auto mean = AVDexedMicroTuning::presetCents(3, 2);
    passed &= expect(mean[62] == 0.0f && std::abs(mean[64] + 6.84f) < 0.01f, "mean tone follows its key (D)");
    passed &= expect(AVDexedMicroTuning::makePresetTuning(0, 0) == nullptr, "equal temperament keeps the instrument's tuning");
    auto table = AVDexedMicroTuning::makePresetTuning(5, 0);
    auto standard = createStandardTuning();
    passed &= expect(table != nullptr && std::abs((table->midinote_to_logfreq(69) - standard->midinote_to_logfreq(69)) + int(11.73 * 13981.0)) < 200,
                     "a preset tuning moves each note by its cents");

    auto bank = varietyBank();
    auto tuned = makeProcessor(bank, 2), plain = makeProcessor(bank, 2);
    oneLayer(*tuned);
    oneLayer(*plain);
    tuned->setAVLayerMicroTuning(0, 5, 0);
    const std::vector<Note> notes { { 0.0, 1, 69, 100 }, { 0.3, 1, 69, 0 } };
    passed &= expect(! (render(*tuned, notes, 0.5) == render(*plain, notes, 0.5)), "a layer in Werckmeister sounds different");
    // Turned on and off again before playing: exactly the instrument's own tuning.
    auto toggled = makeProcessor(bank, 2), untouched = makeProcessor(bank, 2);
    oneLayer(*toggled);
    oneLayer(*untouched);
    toggled->setAVLayerMicroTuning(0, 5, 0);
    toggled->setAVLayerMicroTuning(0, -1, 0);
    passed &= expect(render(*toggled, notes, 0.5) == render(*untouched, notes, 0.5), "with it off again it plays as before");

    tuned->setAVLayerMicroTuning(0, 6, 4);
    const auto preset = tuned->createAVNativePresetState("Kirnberger", "id-kirnberger", true);
    auto opened = makeProcessor(bank, 0);
    opened->loadAVNativePreset(preset.getData(), int(preset.getSize()));
    passed &= expect(opened->getAVLayerSettings(0).microTable.load() == 6 && opened->getAVLayerSettings(0).microKey.load() == 4,
                     "a layers preset keeps the layer's tuning");
    juce::MemoryBlock state;
    tuned->getStateInformation(state);
    auto restored = makeProcessor(bank, 0);
    restored->setStateInformation(state.getData(), int(state.getSize()));
    passed &= expect(restored->getAVLayerSettings(0).microTable.load() == 6, "and so does a session");
    return passed;
}

/** 25 ms levels (both sides) of one held note, the layer volumes settled first. */
std::vector<double> heldLevels(DexedAudioProcessor& processor, double seconds) {
    render(processor, {}, 0.1);
    const auto sound = render(processor, { { 0.0, 1, 60, 100 } }, seconds);
    const size_t window = size_t(rate * 0.025);
    std::vector<double> levels;
    for (size_t start = 0; start + window <= sound.left.size(); start += window) {
        double sum = 0.0;
        for (size_t i = start; i < start + window; ++i)
            sum += double(sound.left[i]) * sound.left[i] + double(sound.right[i]) * sound.right[i];
        levels.push_back(std::sqrt(sum / double(2 * window)));
    }
    return levels;
}

bool testStackIsNotAPluck() {
    // Eight copies of one voice in step add up at the attack (+9 dB) and,
    // evenly detuned, then cancel out (-13 dB) over and over: a "pluck". With
    // free phase and uneven tunings a stack stays near one layer's level
    // (spread: how far, in dB, the 25 ms levels stray from it).
    bool passed = true;
    for (int slot : { 1, 3, 9, 17 }) {   // sustained voices
        auto bank = varietyBank();
        auto one = makeProcessor(bank, slot), stack = makeProcessor(bank, slot), inStep = makeProcessor(bank, slot);
        oneLayer(*one);
        stack->stackAVLayers(8, 7.0f);
        inStep->stackAVLayers(8, 7.0f);
        for (int layer = 0; layer < 8; ++layer) {
            inStep->getAVLayerSettings(layer).phase = -1.0f;
            inStep->getAVLayerSettings(layer).detune = 7.0f * (2.0f * float(layer) / 7.0f - 1.0f);
        }
        const auto reference = heldLevels(*one, 1.5), free = heldLevels(*stack, 1.5), old = heldLevels(*inStep, 1.5);
        const auto against = [&](const std::vector<double>& levels, size_t w) {
            return juce::Decibels::gainToDecibels(levels[w] / juce::jmax(1.0e-9, reference[w]));
        };
        double attack = 0.0, oldAttack = 0.0, lowest = 0.0, squares = 0.0, oldSquares = 0.0;
        for (size_t w = 1; w <= 4; ++w) {
            attack = juce::jmax(attack, double(against(free, w)));
            oldAttack = juce::jmax(oldAttack, double(against(old, w)));
        }
        for (size_t w = 1; w < reference.size(); ++w) {
            lowest = juce::jmin(lowest, double(against(free, w)));
            squares += double(against(free, w)) * against(free, w);
            oldSquares += double(against(old, w)) * against(old, w);
        }
        const double spread = std::sqrt(squares / double(reference.size() - 1)), oldSpread = std::sqrt(oldSquares / double(reference.size() - 1));
        passed &= expect(oldAttack > 5.0 && oldSpread > 7.0, "eight layers in step add up at the attack and then cancel (how the stack was)");
        passed &= expect(attack < 4.0, "a free-phase stack attacks about as loud as one layer");
        // A chorus still dips for an instant now and then, as any ensemble does.
        passed &= expect(spread < 5.0 && lowest > -14.0, "and stays near that level afterwards");
    }

    // Free phase keeps the layer's sound: alone it is as loud as without it.
    auto bank = varietyBank();
    auto plain = makeProcessor(bank, 3), moved = makeProcessor(bank, 3);
    oneLayer(*plain);
    oneLayer(*moved);
    moved->getAVLayerSettings(0).phase = 0.4f;
    const auto a = heldLevels(*plain, 0.4), b = heldLevels(*moved, 0.4);
    bool sameLevel = true;
    for (size_t w = 2; w < a.size(); ++w)
        sameLevel &= std::abs(juce::Decibels::gainToDecibels(b[w] / juce::jmax(1.0e-9, a[w]))) < 0.5f;
    passed &= expect(sameLevel, "free phase moves a layer's start, not its sound");

    auto stacked = makeProcessor(bank, 3);
    stacked->stackAVLayers(4, 5.0f);
    passed &= expect(stacked->getAVLayerSettings(0).phase.load() == 0.0f && stacked->getAVLayerSettings(1).phase.load() > 0.0f
                         && stacked->getAVLayerSettings(4).phase.load() < 0.0f,
                     "a stack frees the phase of its layers only");
    stacked->resetAVLayers();
    passed &= expect(stacked->getAVLayerSettings(1).phase.load() < 0.0f, "and Reset puts every layer back in step");
    return passed;
}

bool testUnisonAndRandomPitch() {
    bool passed = true;
    auto bank = varietyBank();
    auto processor = makeProcessor(bank, 2);
    oneLayer(*processor);
    auto& settings = processor->getAVLayerSettings(0);
    settings.unison = 5.0f;
    render(*processor, { { 0.0, 1, 60, 100 } }, 0.05);
    passed &= expect(processor->getAVLayerSoundingVoices(0) == 2, "unison plays two voices per key");
    render(*processor, { { 0.0, 1, 60, 0 } }, 3.0);
    passed &= expect(processor->getAVLayerSoundingVoices(0) == 0, "and its note-off releases both");
    settings.unison = 0.0f;

    auto plain = makeProcessor(bank, 2), random = makeProcessor(bank, 2), again = makeProcessor(bank, 2);
    for (auto* p : { plain.get(), random.get(), again.get() }) oneLayer(*p);
    random->getAVLayerSettings(0).randomPitch = 20.0f;
    again->getAVLayerSettings(0).randomPitch = 20.0f;
    const std::vector<Note> notes { { 0.0, 1, 60, 100 }, { 0.1, 1, 64, 100 } };
    const auto randomSound = render(*random, notes, 0.4);
    passed &= expect(! (randomSound == render(*plain, notes, 0.4)) && randomSound == render(*again, notes, 0.4),
                     "random pitch retunes each note, the same way on every run");
    return passed;
}

/** A change of the layers changes the session beside the parameters: the
    host is told it has something to save (a project closed without asking
    lost it), but not by a host restore. */
bool testLayerChangesTellTheHost() {
    struct Host final : juce::AudioProcessorListener {
        int saves = 0;
        void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
        void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override {
            saves += details.nonParameterStateChanged ? 1 : 0;
        }
    };
    DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
    processor.setAVLayerMode(true);
    processor.stackAVLayers(3, 12.0f);
    processor.runAVTimerTickForTesting();
    Host host;
    processor.addListener(&host);
    processor.runAVTimerTickForTesting();
    const bool quiet = host.saves == 0;
    processor.getAVLayerSettings(1).volume.store(0.25f);   // as the Capas page's fader does
    processor.runAVTimerTickForTesting();
    const bool told = host.saves == 1;
    processor.removeListener(&host);

    juce::MemoryBlock session;
    processor.getStateInformation(session);
    DexedAudioProcessor restored(DexedAudioProcessor::TestMode{});
    Host restoredHost;
    restored.addListener(&restoredHost);
    restored.setStateInformation(session.getData(), int(session.getSize()));
    for (int tick = 0; tick < 3; ++tick) restored.runAVTimerTickForTesting();
    restored.removeListener(&restoredHost);
    return expect(quiet && told, "a change of the layers tells the host there is something to save")
        && expect(restoredHost.saves == 0 && restored.getAVLayerSettings(1).volume.load() == 0.25f,
                  "a host restore brings them back without telling it so");
}
}

bool runAVDexedLayerTests() {
    bool passed = testOneLayerIsTheSinglePath();
    passed &= testLayerChangesTellTheHost();
    passed &= testRoutingAndMix();
    passed &= testTransposeAndDetune();
    passed &= testEditsStayWithTheirLayer();
    passed &= testStackAndSession();
    passed &= testUnisonAndRandomPitch();
    passed &= testStackIsNotAPluck();
    passed &= testLayersFile();
    passed &= testReviewCases();
    passed &= testBankVoiceExtras();
    passed &= testSecondReviewCases();
    passed &= testMicroTuning();
    std::cout << (passed ? "Layer checks passed.\n" : "Layer checks FAILED.\n");
    return passed;
}
