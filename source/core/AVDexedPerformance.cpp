// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedPerformance.h"
#include "Cartridge.h"
#include <algorithm>

namespace AVDexedPerformance {
namespace {
/** One step of the DX7II dual detune and the TX802 detune, in cents: the
    size of one step of the DX7 operator detune around middle C (an estimate:
    Yamaha documents the steps, not their size). */
constexpr float detuneStepCents = 1.2f;

juce::String asciiName(const uint8_t* data, int length) {
    juce::String name;
    for (int i = 0; i < length; ++i) {
        const auto c = data[i] & 0x7F;
        name += juce::String::charToString(juce::juce_wchar(c >= 32 && c < 127 ? c : ' '));
    }
    return name.trim();
}

VoiceRef dx7iiVoice(int number) {
    VoiceRef ref;
    ref.source = number < 64 ? VoiceRef::Source::internal : VoiceRef::Source::cartridge;
    ref.number = number % 64;
    return ref;
}

VoiceRef tx802Voice(int number) {
    VoiceRef ref;
    ref.source = number < 64 ? VoiceRef::Source::internal
               : number < 128 ? VoiceRef::Source::cartridge
               : number < 192 ? VoiceRef::Source::presetA
                              : VoiceRef::Source::presetB;
    ref.number = number % 64;
    return ref;
}

/** TX802 PCED (116 parameters, one per byte) in the PMEM layout (84 bytes). */
std::array<uint8_t, 84> tx802PcedToPmem(const std::vector<uint8_t>& pced) {
    std::array<uint8_t, 84> pmem {};
    if (pced.size() < 116) return pmem;
    for (int tg = 0; tg < 8; ++tg) {
        pmem[size_t(tg)] = uint8_t(((pced[size_t(tg)] & 7) << 5) | (pced[size_t(8 + tg)] & 31));
        pmem[size_t(8 + tg)] = pced[size_t(16 + tg)];
        pmem[size_t(16 + tg)] = pced[size_t(88 + tg)];
        pmem[size_t(24 + tg)] = pced[size_t(32 + tg)];
        pmem[size_t(32 + tg)] = uint8_t(((pced[size_t(24 + tg)] & 15) << 3) | ((pced[size_t(80 + tg)] & 1) << 2) | (pced[size_t(40 + tg)] & 3));
        pmem[size_t(40 + tg)] = pced[size_t(48 + tg)];
        pmem[size_t(48 + tg)] = pced[size_t(56 + tg)];
        pmem[size_t(56 + tg)] = uint8_t(((pced[size_t(72 + tg)] & 1) << 6) | (pced[size_t(64 + tg)] & 63));
    }
    for (int i = 0; i < 20; ++i) pmem[size_t(64 + i)] = pced[size_t(96 + i)];
    return pmem;
}

/** A file name reduced to what tells a set of files apart: lower case,
    letters and digits only, without the words that only say what a file is. */
juce::String normalisedName(const juce::File& file, bool voiceFile) {
    auto name = file.getFileNameWithoutExtension().toLowerCase().retainCharacters("abcdefghijklmnopqrstuvwxyz0123456789");
    for (const auto* word : { "performances", "performance", "perf", "voices", "voice", "banks", "bank" })
        name = name.replace(word, "");
    if (voiceFile)   // "Voice32"/"Voice64": the half of the internal memory, not part of the set's name
        name = name.replace("32", "").replace("64", "");
    return name;
}

int commonPrefix(const juce::String& a, const juce::String& b) {
    int n = 0;
    while (n < a.length() && n < b.length() && a[n] == b[n]) ++n;
    return n;
}
}

juce::String VoiceRef::label() const {
    const char* prefix = source == Source::internal ? "I" : source == Source::cartridge ? "C"
                       : source == Source::presetA ? "A" : source == Source::presetB ? "B" : "?";
    return prefix + juce::String(number + 1).paddedLeft('0', 2);
}

Performance fromDx7ii(const uint8_t* p, int number) {
    Performance performance;
    performance.kind = Performance::Kind::dx7ii;
    performance.number = number;
    performance.name = asciiName(p + 31, 20);
    const int mode = juce::jlimit(0, 2, int(p[0]));               // single / dual / split
    const float total = juce::jlimit(0, 99, int(p[16])) / 99.0f;
    const int balance = juce::jlimit(0, 100, int(p[15]));         // 0: only A ... 100: only B
    const float dualDetune = float(p[6] & 7) * detuneStepCents;
    const int splitPoint = juce::jlimit(0, 127, int(p[7]));
    const int shiftA = juce::jlimit(-24, 24, int(p[13]) - 24), shiftB = juce::jlimit(-24, 24, int(p[14]) - 24);

    auto& a = performance.layers[0];
    a.used = a.enabled = true;
    a.voice = dx7iiVoice(p[1] & 0x7F);
    a.transpose = shiftA;
    a.volume = mode == 0 ? total : total * juce::jmin(1.0f, float(100 - balance) / 50.0f);
    if (mode == 1) a.detune = dualDetune;
    if (mode == 2) a.highNote = juce::jmax(0, splitPoint - 1);   // A plays below the split point
    if (mode != 0) {
        auto& b = performance.layers[1];
        b.used = b.enabled = true;
        b.voice = dx7iiVoice(p[2] & 0x7F);
        b.transpose = mode == 2 ? shiftB : shiftA;
        b.volume = total * juce::jmin(1.0f, float(balance) / 50.0f);
        if (mode == 1 && dualDetune > 0.0f) b.detune = -dualDetune;
        if (mode == 2) b.lowNote = splitPoint;                       // B from the split point up
    }
    if ((p[20] & 3) != 0) performance.notes.add("pan");              // pan by LFO, velocity or key
    if ((p[5] & 3) != 0) performance.notes.add("microTuning");
    return performance;
}

Performance fromTx802(const uint8_t* p, int number) {
    Performance performance;
    performance.kind = Performance::Kind::tx802;
    performance.number = number;
    performance.name = asciiName(p + 64, 20);
    bool microTuning = false;
    int previousLeader = -1;
    for (int tg = 0; tg < layerCount; ++tg) {
        const int offset = p[tg] >> 5;
        // A tone generator linked to the one at its offset gives it notes: it
        // is that instrument, not one of its own.
        if (offset < tg) continue;
        auto& layer = performance.layers[size_t(tg)];
        const int channel = p[tg] & 31;
        const int out = p[32 + tg] & 3;
        layer.used = true;
        layer.voice = tx802Voice(p[8 + tg]);
        layer.channel = channel >= 16 ? 0 : channel + 1;
        layer.lowNote = juce::jlimit(0, 127, int(p[40 + tg]));
        layer.highNote = juce::jlimit(layer.lowNote, 127, int(p[48 + tg]));
        layer.transpose = juce::jlimit(-24, 24, (p[56 + tg] & 63) - 24);
        layer.detune = float(juce::jlimit(0, 14, (p[32 + tg] >> 3) & 15) - 7) * detuneStepCents;
        layer.volume = juce::jlimit(0, 99, int(p[24 + tg])) / 99.0f;
        // Output I is the left of a stereo pair, II the right, both the centre.
        layer.pan = out == 1 ? 0.0f : out == 2 ? 1.0f : 0.5f;
        layer.enabled = out != 0;
        // Alternate assign joins the instrument to its left: the TX802 shows
        // "135 <B02 <A15" for three that take turns.
        if (((p[32 + tg] >> 2) & 1) != 0 && previousLeader >= 0) {
            auto& left = performance.layers[size_t(previousLeader)];
            if (left.alternateGroup < 0) left.alternateGroup = previousLeader;
            layer.alternateGroup = left.alternateGroup;
        }
        previousLeader = tg;
        microTuning |= p[16 + tg] != 0;
    }
    if (microTuning) performance.notes.add("microTuning");
    return performance;
}

std::vector<Performance> readPerformances(const AVDexedSysex::Contents& contents) {
    std::vector<Performance> found;
    for (const auto& entry : contents.universal) {
        if (entry.id == "8973PM") {
            for (size_t i = 0; i + 51 <= entry.data.size() && i / 51 < 32; i += 51)
                found.push_back(fromDx7ii(entry.data.data() + i, int(i / 51) + 1));
        } else if (entry.id == "8973PE" && entry.data.size() >= 51) {
            found.push_back(fromDx7ii(entry.data.data(), 1));
        } else if (entry.id == "8952PM" && entry.data.size() >= 168) {
            const auto bytes = AVDexedSysex::decodeAsciiNibbles(entry.data);
            found.push_back(fromTx802(bytes.data(), int(found.size()) + 1));
        } else if (entry.id == "8952PE") {
            const auto pmem = tx802PcedToPmem(AVDexedSysex::decodeAsciiNibbles(entry.data));
            found.push_back(fromTx802(pmem.data(), 1));
        }
    }
    return found;
}

//==============================================================================
std::vector<VoicePool::Sibling> VoicePool::readSiblings(const juce::File& folder, const juce::File& except) {
    std::vector<Sibling> siblings;
    if (! folder.isDirectory()) return siblings;
    int read = 0;
    for (const auto& entry : juce::RangedDirectoryIterator(folder, false, "*", juce::File::findFiles)) {
        const auto file = entry.getFile();
        if (file == except || ! file.getFileExtension().equalsIgnoreCase(".syx") || file.getSize() > 4 * 1024 * 1024)
            continue;
        if (++read > 400) break;   // a folder of thousands of banks is not a set
        juce::MemoryBlock bytes;
        if (! file.loadFileAsData(bytes)) continue;
        auto scanned = AVDexedSysex::scan(bytes);
        if (scanned.banks.empty()) continue;
        siblings.push_back({ file, normalisedName(file, true), std::move(scanned.banks) });
    }
    std::sort(siblings.begin(), siblings.end(), [](const Sibling& a, const Sibling& b) {
        return a.file.getFileName().compareNatural(b.file.getFileName()) < 0;
    });
    return siblings;
}

VoicePool::VoicePool(const juce::File& performanceFile, const AVDexedSysex::Contents& contents) {
    const auto self = performanceFile.getFileName();
    // The performance file's own banks are its internal memory.
    if (! contents.banks.empty()) {
        for (size_t half = 0; half < std::min<size_t>(2, contents.banks.size()); ++half)
            setBank(VoiceRef::Source::internal, int(half), contents.banks[half], self);
    }
    const auto folder = performanceFile.getParentDirectory();
    if (! folder.isDirectory()) return;

    const auto siblings = readSiblings(folder, performanceFile);

    // The TX802's preset banks: "…A1", "…A2", "…B1", "…B2", next to the
    // performances or in the folder above them.
    const auto takePresets = [this](const std::vector<Sibling>& candidates) {
        for (const auto& sibling : candidates) {
            const auto& n = sibling.name;
            const auto source = n.endsWith("a1") || n.endsWith("a2") ? VoiceRef::Source::presetA
                              : n.endsWith("b1") || n.endsWith("b2") ? VoiceRef::Source::presetB : VoiceRef::Source::none;
            if (source != VoiceRef::Source::none)
                setBank(source, n.endsWith("2") ? 1 : 0, sibling.banks.front(), sibling.file.getFileName());
        }
    };
    takePresets(siblings);
    if (! banks[size_t(VoiceRef::Source::presetA)][0].present && folder.getParentDirectory() != folder)
        takePresets(readSiblings(folder.getParentDirectory(), performanceFile));

    if (! contents.banks.empty()) return;
    // The internal memory: the voice files named as the performance file
    // ("DX7IIFDPerf" → "DX7IIFDVoice32", "DX7IIFDVoice64"), else those sharing
    // the longest start of the name ("TenNoMakiPerfA" → "TenNoMakiVoice").
    const auto wanted = normalisedName(performanceFile, false);
    std::vector<const Sibling*> chosen;
    for (const auto& sibling : siblings)
        if (sibling.name == wanted) chosen.push_back(&sibling);
    if (chosen.empty()) {
        int best = 3;
        for (const auto& sibling : siblings) best = std::max(best, commonPrefix(sibling.name, wanted));
        if (best > 3)
            for (const auto& sibling : siblings)
                if (commonPrefix(sibling.name, wanted) == best) chosen.push_back(&sibling);
    }
    // Sets sold on cartridge (the DX7II's 64-127, "C01"...) came as these same
    // voice files: they stand for the cartridge too.
    int half = 0;
    for (const auto* sibling : chosen)
        for (const auto& bank : sibling->banks)
            if (half < 2) {
                setBank(VoiceRef::Source::internal, half, bank, sibling->file.getFileName());
                setBank(VoiceRef::Source::cartridge, half, bank, sibling->file.getFileName());
                ++half;
            }
}

void VoicePool::setBank(VoiceRef::Source source, int half, const AVDexedSysex::Bank& bank, const juce::String& from) {
    setBank(source, half, bank.voices, from);
    auto& slot = banks[size_t(source)][size_t(juce::jlimit(0, 1, half))];
    slot.hasAmem = bank.hasAmem;
    slot.amem = bank.amem;
}

void VoicePool::setBank(VoiceRef::Source source, int half, const std::array<uint8_t, AVDexedSysex::bankBytes>& voices,
                        const juce::String& from) {
    auto& slot = banks[size_t(source)][size_t(juce::jlimit(0, 1, half))];
    slot.present = true;
    slot.voices = voices;
    slot.hasAmem = false;
    slot.from = from;
}

std::optional<std::array<uint8_t, AVDexedSysex::amemVoiceBytes>> VoicePool::extrasFor(const VoiceRef& ref) const {
    if (ref.source == VoiceRef::Source::none) return std::nullopt;
    const auto& half = banks[size_t(ref.source)][size_t(juce::jlimit(0, 63, ref.number) / 32)];
    if (! half.present || ! half.hasAmem) return std::nullopt;
    std::array<uint8_t, AVDexedSysex::amemVoiceBytes> extras {};
    std::copy_n(half.amem.begin() + (ref.number % 32) * AVDexedSysex::amemVoiceBytes, AVDexedSysex::amemVoiceBytes, extras.begin());
    return extras;
}

void applyVoiceExtras(ResolvedLayer& layer, const uint8_t* amem) {
    // AMEM byte 4: random pitch in bits 6-4; byte 5: poly/mono (bit 0) and
    // unison (bit 1); byte 34: unison detune in bits 2-0. The cents are
    // estimates (Yamaha gives the steps): random pitch ±5 to ±41 cents as the
    // DX7II/DX7s manuals describe, unison detune in steps of the dual detune.
    static constexpr float randomCents[8] = { 0.0f, 5.0f, 10.0f, 15.0f, 21.0f, 27.0f, 34.0f, 41.0f };
    layer.randomPitch = randomCents[(amem[4] >> 4) & 7];
    const int mode = amem[5] & 3;
    layer.mono = (mode & 1) != 0 ? 1 : -1;
    const int unisonDetune = amem[34] & 7;
    // Unison without detune would only double the voice: left as one.
    layer.unison = (mode & 2) != 0 && unisonDetune > 0 ? float(unisonDetune) * 1.2f : 0.0f;
}

std::optional<Program> VoicePool::voiceFor(const VoiceRef& ref) const {
    if (ref.source == VoiceRef::Source::none) return std::nullopt;
    const auto& half = banks[size_t(ref.source)][size_t(juce::jlimit(0, 63, ref.number) / 32)];
    if (! half.present) return std::nullopt;
    Cartridge bank;
    const auto message = AVDexedSysex::bankMessage(half.voices.data());
    if (bank.load(message.data(), int(message.size())) > 1) return std::nullopt;
    return bank.unpackProgram(ref.number % 32);
}

juce::StringArray VoicePool::sources() const {
    juce::StringArray names;
    for (const auto& source : banks)
        for (const auto& half : source)
            if (half.present) names.addIfNotAlreadyThere(half.from);
    return names;
}

std::array<ResolvedLayer, layerCount> resolve(const Performance& performance, const VoicePool& pool) {
    std::array<ResolvedLayer, layerCount> layers {};
    for (int i = 0; i < layerCount; ++i) {
        auto& layer = layers[size_t(i)];
        layer.plan = performance.layers[size_t(i)];
        if (! layer.plan.used) continue;
        if (const auto voice = pool.voiceFor(layer.plan.voice)) {
            layer.voice = *voice;
            layer.voiceFound = true;
            if (const auto extras = pool.extrasFor(layer.plan.voice))
                applyVoiceExtras(layer, extras->data());
        } else {
            // Silent rather than a sine: the voice it names is not here.
            layer.voice = Program::initialProgram();
            layer.voice.setName("? " + layer.plan.voice.label());
            layer.plan.enabled = false;
        }
    }
    return layers;
}
}
