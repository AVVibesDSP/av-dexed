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
#include "../source/core/AVDexedPerformance.h"
#include <iostream>
#include <map>

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

/** --loader-report <folder> <out.txt>: how every .syx under `folder` loads,
    one line each (path, Cartridge::load result, readBank, bank fingerprint).
    Comparing two reports shows what a change to the reader accepts anew and
    proves that every file that loaded before still loads the same bank. */
int AVRunLoaderReport(const juce::File& folder, const juce::File& output) {
    juce::StringArray lines;
    for (const auto& entry : juce::RangedDirectoryIterator(folder, true, "*", juce::File::findFiles)) {
        const auto file = entry.getFile();
        if (! file.getFileExtension().equalsIgnoreCase(".syx") || file.getSize() > 4 * 1024 * 1024)
            continue;
        Cartridge bank;
        const int rc = bank.load(file);
        Cartridge viaLibrary;
        const bool readable = AVDexedLibrary::readBank(file, viaLibrary);
        lines.add(file.getRelativePathFrom(folder) + "\t" + juce::String(rc) + "\t" + (readable ? "bank" : "-")
                  + "\t" + (readable ? AVDexedLibrary::bankFingerprint(viaLibrary) : juce::String("-")));
    }
    lines.sort(true);
    if (! output.replaceWithText(lines.joinIntoString("\n") + "\n"))
        return 2;
    std::cout << lines.size() << " files reported\n";
    return 0;
}

/** --import <library folder> <file or folder>...: imports as the editor does
    (single voices of one folder gathered into banks) and lists what came out. */
int AVRunImport(const juce::StringArray& arguments) {
    if (arguments.size() < 2) return 2;
    const juce::File folder(arguments[0]);
    juce::Array<juce::File> files;
    for (int i = 1; i < arguments.size(); ++i) {
        const juce::File item(arguments[i]);
        if (item.isDirectory())
            for (const auto& entry : juce::RangedDirectoryIterator(item, false, "*.syx;*.SYX", juce::File::findFiles))
                files.add(entry.getFile());
        else
            files.add(item);
    }
    std::map<juce::String, juce::Array<juce::File>> singles;
    int sounds = 0;
    const auto report = [&sounds](const AVDexedLibrary::ImportResult& result, const juce::String& name) {
        sounds += result.sounds;
        std::cout << name << " -> status " << int(result.status) << ", " << result.sounds << " sounds:";
        for (const auto& file : result.files) {
            Cartridge bank;
            const bool ok = AVDexedLibrary::readBank(file, bank);
            std::cout << " [" << file.getFileName() << (ok ? "" : " UNREADABLE") << " " << (ok ? bank.getProgramName(0).trim() : "") << "]";
        }
        std::cout << '\n';
    };
    for (const auto& file : files) {
        if (AVDexedLibrary::isSingleVoiceFile(file)) { singles[file.getParentDirectory().getFullPathName()].add(file); continue; }
        report(AVDexedLibrary::importBank(file, folder), file.getFileName());
    }
    for (const auto& [directory, voices] : singles)
        report(voices.size() < 2 ? AVDexedLibrary::importBank(voices.getFirst(), folder)
                                 : AVDexedLibrary::importVoices(voices, folder, juce::File(directory).getFileName()),
               juce::File(directory).getFileName() + " (" + juce::String(voices.size()) + " single voices)");
    std::cout << sounds << " sounds added\n";
    return 0;
}

/** --performances <file>: the performances in a file, with their voices
    resolved from it or from the files next to it. */
int AVRunPerformanceReport(const juce::File& file) {
    juce::MemoryBlock bytes;
    if (! file.loadFileAsData(bytes)) return 2;
    const auto contents = AVDexedSysex::scan(bytes);
    const auto performances = AVDexedPerformance::readPerformances(contents);
    const AVDexedPerformance::VoicePool pool(file, contents);
    std::cout << performances.size() << " performances; voices from: " << pool.sources().joinIntoString(", ") << '\n';
    for (const auto& performance : performances) {
        const auto layers = AVDexedPerformance::resolve(performance, pool);
        std::cout << juce::String(performance.number).paddedLeft('0', 2) << " " << performance.name
                  << (performance.notes.isEmpty() ? "" : "  [" + performance.notes.joinIntoString(",") + "]") << '\n';
        for (int i = 0; i < AVDexedPerformance::layerCount; ++i) {
            const auto& layer = layers[size_t(i)];
            if (! layer.plan.used) continue;
            std::cout << "   " << (i + 1) << ": " << layer.plan.voice.label() << " " << layer.voice.getName().trim()
                      << (layer.voiceFound ? "" : " (missing)") << (layer.plan.enabled ? "" : " off")
                      << " ch " << layer.plan.channel << " keys " << layer.plan.lowNote << "-" << layer.plan.highNote
                      << " tr " << layer.plan.transpose << " det " << layer.plan.detune << " vol " << layer.plan.volume
                      << " pan " << layer.plan.pan << (layer.plan.alternateGroup >= 0 ? " alt" + juce::String(layer.plan.alternateGroup + 1) : juce::String())
                      << (layer.plan.microTable > 0 ? " tuning" + juce::String(layer.plan.microTable) + "/" + juce::String(layer.plan.microKey) : juce::String()) << '\n';
        }
    }
    return 0;
}

/** --layers-cpu <bank.syx> <slot>: the share of one core the whole instrument
    takes for 10 s of 16 held notes, with one layer and with eight stacked. */
int AVRunLayersCpu(const juce::File& bankFile, int slot) {
    Cartridge bank;
    if (bank.load(bankFile) < 0) return 2;
    for (int layers : { 1, 8 }) {
        auto processor = makeProcessor(bank, juce::jlimit(0, 31, slot - 1), 1);
        if (layers > 1) processor->stackAVLayers(layers, 7.0f);
        juce::AudioBuffer<float> buffer(2, 256);
        juce::MidiBuffer start;
        for (int n = 0; n < 16; ++n) start.addEvent(juce::MidiMessage::noteOn(1, 36 + n * 3, (juce::uint8) 100), 0);
        const int blocks = int(10.0 * goldenRate / 256);
        const auto t0 = juce::Time::getHighResolutionTicks();
        for (int b = 0; b < blocks; ++b) {
            juce::MidiBuffer midi;
            if (b == 0) midi = start;
            buffer.clear();
            processor->processBlock(buffer, midi);
        }
        const double seconds = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - t0);
        std::cout << layers << " layer(s), " << processor->getAVActiveVoices() << " voices: "
                  << juce::String(100.0 * seconds / 10.0, 2) << " % of one core\n";
    }
    return 0;
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
