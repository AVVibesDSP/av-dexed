// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

// DX7II and TX802 performances read into layers, their voices found in the
// file or next to it, and installed in the instrument.

#include "../source/PluginProcessor.h"
#include "../source/core/AVDexedPerformance.h"
#include <iostream>

namespace {
bool expect(bool condition, const char* message) {
    if (! condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool near(float a, float b) { return std::abs(a - b) < 1.0e-4f; }

std::array<uint8_t, 51> dx7iiPerformance(int mode, int voiceA, int voiceB, const char* name) {
    std::array<uint8_t, 51> p {};
    p[0] = uint8_t(mode);
    p[1] = uint8_t(voiceA);
    p[2] = uint8_t(voiceB);
    p[6] = 3;      // dual detune
    p[7] = 60;     // split point
    p[13] = 26;    // A: +2
    p[14] = 12;    // B: -12 (split)
    p[15] = 25;    // balance towards A
    p[16] = 99;    // total volume
    for (int i = 0; i < 20; ++i) p[size_t(31 + i)] = uint8_t(name[i] ? name[i] : ' ');
    return p;
}

bool testDx7ii() {
    bool passed = true;
    const auto dualBytes = dx7iiPerformance(1, 5, 40, "Dual Strings        ");
    const auto dual = AVDexedPerformance::fromDx7ii(dualBytes.data(), 1);
    const auto& a = dual.layers[0];
    const auto& b = dual.layers[1];
    passed &= expect(dual.name == "Dual Strings" && a.used && b.used && ! dual.layers[2].used, "a dual performance is two layers");
    passed &= expect(a.voice.source == AVDexedPerformance::VoiceRef::Source::internal && a.voice.number == 5 && b.voice.number == 40,
                     "the voices are internal numbers");
    passed &= expect(near(a.detune, 3.6f) && near(b.detune, -3.6f) && a.transpose == 2 && b.transpose == 2,
                     "dual detune splits both ways; the note shift of A applies to both");
    passed &= expect(near(a.volume, 1.0f) && near(b.volume, 0.5f), "the balance lowers the far side");
    passed &= expect(a.lowNote == 0 && a.highNote == 127 && b.lowNote == 0 && b.highNote == 127, "dual layers share the keyboard");

    const auto splitBytes = dx7iiPerformance(2, 5, 70, "Split               ");
    const auto split = AVDexedPerformance::fromDx7ii(splitBytes.data(), 2);
    passed &= expect(split.layers[0].highNote == 59 && split.layers[1].lowNote == 60, "split: A below the split point, B from it up");
    passed &= expect(split.layers[1].transpose == -12 && split.layers[1].voice.source == AVDexedPerformance::VoiceRef::Source::cartridge
                         && split.layers[1].voice.number == 6,
                     "split: B has its own note shift; 64-127 are cartridge voices");
    auto tunedBytes = dx7iiPerformance(1, 5, 40, "Tuned               ");
    tunedBytes[3] = 5;   // Werckmeister
    tunedBytes[4] = 2;
    tunedBytes[5] = 1;   // on voice A only
    const auto tuned = AVDexedPerformance::fromDx7ii(tunedBytes.data(), 4);
    passed &= expect(tuned.layers[0].microTable == 5 && tuned.layers[0].microKey == 2 && tuned.layers[1].microTable == -1
                         && tuned.notes.isEmpty(),
                     "a preset micro tuning goes to the voice it is switched on for");
    tunedBytes[3] = 12;  // a user table: not played, said
    passed &= expect(AVDexedPerformance::fromDx7ii(tunedBytes.data(), 5).notes.contains("microTuning"), "a user table is noted");
    const auto singleBytes = dx7iiPerformance(0, 9, 0, "Single              ");
    passed &= expect(! AVDexedPerformance::fromDx7ii(singleBytes.data(), 3).layers[1].used, "a single performance is one layer");
    return passed;
}

bool testTx802() {
    bool passed = true;
    std::array<uint8_t, 84> p {};
    // TG1-2 linked (offset 0), TG3 alone, TG4 alternate with TG3, TG5-8 off/various.
    const int offsets[8] = { 0, 0, 2, 3, 4, 5, 6, 7 };
    const int channels[8] = { 0, 0, 16, 16, 1, 1, 1, 1 };
    const int voices[8] = { 3, 3, 130, 200, 70, 0, 0, 0 };
    const int outs[8] = { 3, 3, 1, 2, 0, 3, 3, 3 };
    const int detunes[8] = { 7, 7, 9, 5, 7, 7, 7, 7 };
    for (int tg = 0; tg < 8; ++tg) {
        p[size_t(tg)] = uint8_t((offsets[tg] << 5) | channels[tg]);
        p[size_t(8 + tg)] = uint8_t(voices[tg]);
        p[size_t(24 + tg)] = 99;
        p[size_t(32 + tg)] = uint8_t((detunes[tg] << 3) | ((tg == 3) << 2) | outs[tg]);
        p[size_t(40 + tg)] = tg == 2 ? 36 : 0;
        p[size_t(48 + tg)] = 127;
        p[size_t(56 + tg)] = uint8_t(tg == 2 ? 36 : 24);
    }
    for (int i = 0; i < 20; ++i) p[size_t(64 + i)] = uint8_t("TX Test             "[i]);
    const auto performance = AVDexedPerformance::fromTx802(p.data(), 1);
    const auto& l = performance.layers;
    passed &= expect(performance.name == "TX Test" && l[0].used && ! l[1].used && l[2].used && l[3].used,
                     "a tone generator linked to another is part of it");
    passed &= expect(l[0].channel == 1 && l[2].channel == 0 && l[4].channel == 2, "receive channels (16: every channel)");
    passed &= expect(l[2].voice.source == AVDexedPerformance::VoiceRef::Source::presetA && l[2].voice.number == 2
                         && l[3].voice.source == AVDexedPerformance::VoiceRef::Source::presetB && l[3].voice.number == 8
                         && l[4].voice.source == AVDexedPerformance::VoiceRef::Source::cartridge,
                     "voice numbers: internal, cartridge, preset A and B");
    passed &= expect(near(l[2].pan, 0.0f) && near(l[3].pan, 1.0f) && near(l[0].pan, 0.5f) && ! l[4].enabled,
                     "outputs I, II, both and off");
    passed &= expect(near(l[2].detune, 2.4f) && near(l[3].detune, -2.4f) && l[2].transpose == 12 && l[2].lowNote == 36,
                     "detune, note shift and note limits");
    passed &= expect(l[2].alternateGroup == 2 && l[3].alternateGroup == 2 && l[0].alternateGroup < 0,
                     "alternate assign joins the instrument to its left");
    return passed;
}

struct Scratch {
    juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("avdexed-performance-" + juce::Uuid().toString());
    Scratch() { root.createDirectory(); }
    ~Scratch() { if (root.getFullPathName().contains("avdexed-performance-")) root.deleteRecursively(); }
};

std::array<uint8_t, 4096> namedBank(const juce::String& tag) {
    Cartridge bank;
    const auto init = Program::initialProgram();
    for (int slot = 0; slot < 32; ++slot) bank.packProgram(init.getRawData(), slot, tag + juce::String(slot + 1));
    std::array<uint8_t, 4096> voices {};
    std::memcpy(voices.data(), bank.getRawVoice(), 4096);
    return voices;
}

void writeBank(const juce::File& file, const juce::String& tag) {
    const auto message = AVDexedSysex::bankMessage(namedBank(tag).data());
    file.replaceWithData(message.data(), message.size());
}

bool testVoicesNextToThePerformance() {
    bool passed = true;
    Scratch scratch;
    // A DX7II performance dump alone, its voices in two files next to it.
    juce::MemoryBlock dump;
    const uint8_t header[] = { 0xF0, 0x43, 0x00, 0x7E, 0x0C, 0x6A };
    dump.append(header, sizeof(header));
    std::vector<uint8_t> body;
    for (const char* c = "LM  8973PM"; *c; ++c) body.push_back(uint8_t(*c));
    for (int i = 0; i < 32; ++i) {
        const auto perf = dx7iiPerformance(1, i, 32 + i, "Perf                ");
        body.insert(body.end(), perf.begin(), perf.end());
    }
    dump.append(body.data(), body.size());
    const uint8_t tail[] = { AVDexedSysex::checksum(body.data(), body.size()), 0xF7 };
    dump.append(tail, 2);
    const auto performanceFile = scratch.root.getChildFile("FactoryPerf.syx");
    performanceFile.replaceWithData(dump.getData(), dump.getSize());
    writeBank(scratch.root.getChildFile("FactoryVoice32.syx"), "LOW");
    writeBank(scratch.root.getChildFile("FactoryVoice64.syx"), "HIGH");
    writeBank(scratch.root.getChildFile("Unrelated.syx"), "NO");

    juce::MemoryBlock bytes;
    performanceFile.loadFileAsData(bytes);
    const auto contents = AVDexedSysex::scan(bytes);
    const auto performances = AVDexedPerformance::readPerformances(contents);
    passed &= expect(performances.size() == 32, "the 32 performances of a DX7II dump are read");
    const AVDexedPerformance::VoicePool pool(performanceFile, contents);
    passed &= expect(pool.sources().joinIntoString("|") == "FactoryVoice32.syx|FactoryVoice64.syx",
                     "the voices come from the files named as the performances");
    const auto resolved = AVDexedPerformance::resolve(performances[4], pool);
    passed &= expect(resolved[0].voiceFound && resolved[0].voice.getName().trim() == "LOW5"
                         && resolved[1].voiceFound && resolved[1].voice.getName().trim() == "HIGH5",
                     "internal 1-32 and 33-64 are the two files, in name order");

    // The second set of a factory pair: "FactoryPerfB" plays the internal
    // memory of "FactoryVoice32/64" and the cartridge of "FactoryVoice32B/64B".
    {
        juce::MemoryBlock dumpB;
        dumpB.append(header, sizeof(header));
        std::vector<uint8_t> bodyB;
        for (const char* c = "LM  8973PM"; *c; ++c) bodyB.push_back(uint8_t(*c));
        for (int i = 0; i < 32; ++i) {
            const auto perf = dx7iiPerformance(1, 1, 64 + 36, "Pair                ");
            bodyB.insert(bodyB.end(), perf.begin(), perf.end());
        }
        dumpB.append(bodyB.data(), bodyB.size());
        const uint8_t tailB[] = { AVDexedSysex::checksum(bodyB.data(), bodyB.size()), 0xF7 };
        dumpB.append(tailB, 2);
        const auto fileB = scratch.root.getChildFile("FactoryPerfB.syx");
        fileB.replaceWithData(dumpB.getData(), dumpB.getSize());
        writeBank(scratch.root.getChildFile("FactoryVoice32B.syx"), "CA");
        writeBank(scratch.root.getChildFile("FactoryVoice64B.syx"), "CB");
        juce::MemoryBlock bytesB;
        fileB.loadFileAsData(bytesB);
        const auto contentsB = AVDexedSysex::scan(bytesB);
        const AVDexedPerformance::VoicePool poolB(fileB, contentsB);
        const auto pair = AVDexedPerformance::resolve(AVDexedPerformance::readPerformances(contentsB)[0], poolB);
        passed &= expect(pair[0].voice.getName().trim() == "LOW2" && pair[1].voice.getName().trim() == "CB5",
                         "a \"B\" performance set: internal from the first set, cartridge from its own files");
    }

    // A voice that is nowhere: the layer stays silent and says which it was.
    AVDexedPerformance::Performance missing = performances[0];
    missing.layers[1].voice.source = AVDexedPerformance::VoiceRef::Source::presetB;
    const auto silent = AVDexedPerformance::resolve(missing, pool);
    passed &= expect(! silent[1].voiceFound && ! silent[1].plan.enabled && silent[1].voice.getName().trim() == "? B33",
                     "a voice that is not found leaves its layer off, named after the number");
    return passed;
}

bool testInstall() {
    bool passed = true;
    std::array<AVDexedPerformance::ResolvedLayer, 8> layers {};
    auto bank = namedBank("V");
    Cartridge cart;
    const auto message = AVDexedSysex::bankMessage(bank.data());
    cart.load(message.data(), int(message.size()));
    for (int i : { 1, 2, 3 }) {
        auto& layer = layers[size_t(i)];
        layer.plan.used = layer.plan.enabled = true;
        layer.plan.alternateGroup = 1;
        layer.plan.pan = i == 2 ? 0.0f : 1.0f;
        layer.voice = cart.unpackProgram(i);
        layer.voiceFound = true;
    }
    layers[5].plan.used = layers[5].plan.enabled = true;
    layers[5].plan.channel = 3;
    layers[5].plan.transpose = -12;
    layers[5].voice = cart.unpackProgram(9);

    DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
    processor.setRateAndBufferSizeDetails(48000.0, 512);
    processor.prepareToPlay(48000.0, 512);
    processor.applyAVLayerSetup(layers);
    passed &= expect(processor.isAVLayerModeOn() && processor.getAVSelectedLayer() == 1
                         && processor.activeProgram.getName().trim() == "V2",
                     "a performance turns the layer mode on and selects its first layer");
    passed &= expect(! processor.getAVLayerSettings(0).enabled.load() && processor.getAVLayerSettings(5).channel.load() == 3
                         && processor.getAVLayerSettings(5).transpose.load() == -12,
                     "every layer takes its part");

    // Three layers in one alternate group: each note goes to the next one.
    juce::AudioBuffer<float> buffer(2, 512);
    const auto play = [&](int note, bool on) {
        juce::MidiBuffer midi;
        midi.addEvent(on ? juce::MidiMessage::noteOn(1, note, (juce::uint8) 100) : juce::MidiMessage::noteOff(1, note), 0);
        buffer.clear();
        processor.processBlock(buffer, midi);
    };
    play(60, true);
    play(64, true);
    play(67, true);
    passed &= expect(processor.getAVLayerSoundingVoices(1) == 1 && processor.getAVLayerSoundingVoices(2) == 1
                         && processor.getAVLayerSoundingVoices(3) == 1 && processor.getAVLayerSoundingVoices(5) == 0,
                     "the layers of an alternate group take turns; a layer on another channel stays quiet");
    return passed;
}

bool testVoiceExtras() {
    std::array<uint8_t, 35> amem {};
    amem[4] = 3 << 4;      // random pitch 3
    amem[5] = 2;           // unison poly
    amem[34] = 4;          // unison detune 4
    AVDexedPerformance::ResolvedLayer layer;
    AVDexedPerformance::applyVoiceExtras(layer, amem.data());
    bool passed = expect(near(layer.randomPitch, 15.0f) && near(layer.unison, 4.8f) && layer.mono == -1,
                         "AMEM: random pitch, unison and its detune");
    amem = {};
    amem[5] = 1;           // mono
    AVDexedPerformance::applyVoiceExtras(layer, amem.data());
    passed &= expect(layer.mono == 1 && layer.unison == 0.0f && layer.randomPitch == 0.0f, "AMEM: mono, nothing else");
    return passed;
}
}

bool runAVDexedPerformanceTests() {
    bool passed = testDx7ii();
    passed &= testTx802();
    passed &= testVoicesNextToThePerformance();
    passed &= testInstall();
    passed &= testVoiceExtras();
    std::cout << (passed ? "Performance checks passed.\n" : "Performance checks FAILED.\n");
    return passed;
}
