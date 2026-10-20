// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

// Audio fingerprint of the instrument as it plays today: every voice of the
// given banks, in the three engines, through the whole processor (voices,
// LFO, filter, effects, volume and pan), with a short performance of chords,
// pitch bend, the mod wheel, aftertouch and the sustain pedal. One hash per
// rendering, over the exact bits of both output channels.
//
//   --golden write <list.txt> <bank.syx>...   writes the fingerprints
//   --golden check <list.txt> <bank.syx>...   renders again and compares
//
// A change that must not alter the sound (a reader, the layers around the
// engine) is checked by writing the list before it and checking it after,
// with the same build settings. The fingerprints are bit-exact, so a list is
// only comparable with one made by the same compiler and build type.

#include "../source/PluginProcessor.h"
#include "../source/parameter/Model.h"
#include <iostream>

namespace {
constexpr double goldenRate = 48000.0;

void setParameter(DexedAudioProcessor& processor, const juce::String& id, float value) {
    if (auto* parameter = processor.parameters.getParameter(id))
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

/** FNV-1a over the bits of the samples. */
struct Fingerprint {
    uint64_t value = 1469598103934665603ull;
    void add(const float* samples, int count) {
        for (int i = 0; i < count; ++i) {
            uint32_t bits;
            std::memcpy(&bits, samples + i, sizeof(bits));
            for (int byte = 0; byte < 4; ++byte) {
                value ^= (bits >> (8 * byte)) & 0xFF;
                value *= 1099511628211ull;
            }
        }
    }
};

/** About 1.6 s: a chord, pitch bend, mod wheel, aftertouch, sustain pedal, a
    held note across the pedal release, then the release tail. Uneven block
    sizes on purpose (the engine renders in blocks of 64). */
uint64_t renderPerformance(DexedAudioProcessor& processor) {
    struct Event { int sample; juce::MidiMessage message; };
    const auto at = [](double seconds) { return int(seconds * goldenRate); };
    const std::vector<Event> events {
        { at(0.00), juce::MidiMessage::noteOn(1, 48, (juce::uint8) 100) },
        { at(0.00), juce::MidiMessage::noteOn(1, 60, (juce::uint8) 90) },
        { at(0.01), juce::MidiMessage::noteOn(1, 64, (juce::uint8) 70) },
        { at(0.02), juce::MidiMessage::noteOn(1, 67, (juce::uint8) 120) },
        { at(0.20), juce::MidiMessage::pitchWheel(1, 12000) },
        { at(0.30), juce::MidiMessage::controllerEvent(1, 1, 80) },
        { at(0.40), juce::MidiMessage::channelPressureChange(1, 90) },
        { at(0.50), juce::MidiMessage::controllerEvent(1, 64, 127) },
        { at(0.60), juce::MidiMessage::noteOff(1, 48) },
        { at(0.60), juce::MidiMessage::noteOff(1, 60) },
        { at(0.61), juce::MidiMessage::noteOff(1, 64) },
        { at(0.62), juce::MidiMessage::noteOff(1, 67) },
        { at(0.70), juce::MidiMessage::noteOn(1, 72, (juce::uint8) 127) },
        { at(0.75), juce::MidiMessage::noteOn(1, 36, (juce::uint8) 50) },
        { at(0.80), juce::MidiMessage::controllerEvent(1, 64, 0) },
        { at(0.90), juce::MidiMessage::pitchWheel(1, 8192) },
        { at(0.95), juce::MidiMessage::controllerEvent(1, 1, 0) },
        { at(1.00), juce::MidiMessage::noteOff(1, 72) },
        { at(1.05), juce::MidiMessage::noteOff(1, 36) },
        { at(1.06), juce::MidiMessage::channelPressureChange(1, 0) },
    };
    const int total = at(1.6);
    const int blockSizes[] = { 512, 300, 64, 1000, 37 };
    juce::AudioBuffer<float> buffer(2, 1024);
    Fingerprint print;
    size_t next = 0;
    int position = 0, block = 0;
    while (position < total) {
        const int size = juce::jmin(blockSizes[block++ % 5], total - position);
        juce::MidiBuffer midi;
        while (next < events.size() && events[next].sample < position + size) {
            midi.addEvent(events[next].message, juce::jmax(0, events[next].sample - position));
            ++next;
        }
        buffer.setSize(2, size, false, false, true);
        buffer.clear();
        processor.processBlock(buffer, midi);
        print.add(buffer.getReadPointer(0), size);
        print.add(buffer.getReadPointer(1), size);
        position += size;
    }
    return print.value;
}

/** Each rendering starts from a fresh processor, so nothing leaks from one to the next. */
std::unique_ptr<DexedAudioProcessor> makeProcessor(const Cartridge& bank, int slot, int engine) {
    auto processor = std::make_unique<DexedAudioProcessor>(DexedAudioProcessor::TestMode{});
    processor->setRateAndBufferSizeDetails(goldenRate, 1024);
    processor->prepareToPlay(goldenRate, 1024);
    processor->setActiveBank(bank, juce::File());
    processor->setCurrentProgram(slot);
    setParameter(*processor, IDs::engineType.name, float(engine));
    return processor;
}

/** The variations rendered for every voice, and a few more on slot 1 of each
    bank: mono with portamento, the filter, the effects, volume and pan. */
std::vector<std::pair<juce::String, uint64_t>> renderBank(const juce::File& file) {
    std::vector<std::pair<juce::String, uint64_t>> lines;
    Cartridge bank;
    if (bank.load(file) < 0 || ! bank.containsData())
        return lines;
    const auto bankName = file.getFileNameWithoutExtension();
    for (int engine = 0; engine < 3; ++engine)
        for (int slot = 0; slot < 32; ++slot) {
            auto processor = makeProcessor(bank, slot, engine);
            lines.emplace_back(bankName + " " + juce::String(slot + 1) + " engine" + juce::String(engine),
                               renderPerformance(*processor));
        }
    struct Variation { const char* name; std::vector<std::pair<juce::String, float>> values; };
    const std::vector<Variation> variations {
        { "mono+porta", { { IDs::monoMode.name, 1.0f }, { IDs::portamentoTm.name, 40.0f } } },
        { "gliss", { { IDs::portamentoTm.name, 60.0f }, { IDs::glissando.name, 1.0f } } },
        { "filter", { { IDs::cutoff.name, 0.3f }, { IDs::resonance.name, 0.6f } } },
        { "effects", { { IDs::reverbAmount.name, 0.5f }, { IDs::delayAmount.name, 0.4f }, { IDs::chorusAmount.name, 0.5f } } },
        { "volume+pan", { { IDs::masterVolume.name, 0.5f }, { IDs::pan.name, 0.2f } } },
        { "tune+bend", { { IDs::masterTuneAdj.name, 0.7f }, { IDs::pitchBendUp.name, 12.0f }, { IDs::pitchBendStep.name, 2.0f } } },
        { "wheel", { { IDs::modWheel.name, 99.0f }, { IDs::modWheelPitch.name, 1.0f }, { IDs::modWheelAmp.name, 1.0f } } },
    };
    for (const auto& variation : variations) {
        auto processor = makeProcessor(bank, 0, 1);
        for (const auto& [id, value] : variation.values)
            setParameter(*processor, id, value);
        lines.emplace_back(bankName + " 1 " + variation.name, renderPerformance(*processor));
    }
    return lines;
}
}

int AVRunGolden(const juce::StringArray& arguments) {
    if (arguments.size() < 3 || (arguments[0] != "write" && arguments[0] != "check")) {
        std::cerr << "--golden write|check <list.txt> <bank.syx>...\n";
        return 2;
    }
    const bool writing = arguments[0] == "write";
    const juce::File list(arguments[1]);
    juce::StringArray rendered;
    for (int i = 2; i < arguments.size(); ++i) {
        const juce::File bank(arguments[i]);
        const auto lines = renderBank(bank);
        if (lines.empty()) {
            std::cerr << "Cannot read " << bank.getFullPathName() << '\n';
            return 2;
        }
        for (const auto& [name, hash] : lines)
            rendered.add(name + " " + juce::String::toHexString((juce::int64) hash));
    }
    if (writing) {
        if (! list.replaceWithText(rendered.joinIntoString("\n") + "\n"))
            return 2;
        std::cout << rendered.size() << " fingerprints written to " << list.getFullPathName() << '\n';
        return 0;
    }
    juce::StringArray expected;
    expected.addLines(list.loadFileAsString());
    expected.removeEmptyStrings();
    int differences = 0;
    for (int i = 0; i < juce::jmax(expected.size(), rendered.size()); ++i)
        if (expected[i] != rendered[i]) {
            if (++differences <= 20)
                std::cerr << "DIFFERENT: expected \"" << expected[i] << "\" got \"" << rendered[i] << "\"\n";
        }
    std::cout << rendered.size() << " renderings, " << differences << " different\n";
    return differences == 0 ? 0 : 1;
}
