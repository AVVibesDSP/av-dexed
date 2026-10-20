// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

// The DX7 family reader: banks wherever they are in a file (DX7II and TX802
// dumps, archives, cartridges without header, librarian headers, a missing
// F7), single voices, the universal DX7II/TX802 dumps, and the import that
// turns all of them into standard banks. Everything in temporary folders.

#include "../source/core/AVDexedLibrary.h"
#include "../source/core/AVDexedSysex.h"
#include "../source/core/Program.h"

#include <iostream>

namespace {
bool expect(bool condition, const char* message) {
    if (! condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

struct Scratch {
    juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("avdexed-sysex-" + juce::Uuid().toString());
    Scratch() { root.createDirectory(); }
    ~Scratch() { if (root.getFullPathName().contains("avdexed-sysex-")) root.deleteRecursively(); }
};

/** 32 packed voices named "<tag> NN", with a little variety in the operators. */
std::array<uint8_t, 4096> makeVoices(const juce::String& tag) {
    Cartridge bank;
    auto program = Program::initialProgram();
    for (int slot = 0; slot < 32; ++slot)
        bank.packProgram(program.getRawData(), slot, tag + " " + juce::String(slot + 1).paddedLeft('0', 2));
    std::array<uint8_t, 4096> voices {};
    std::memcpy(voices.data(), bank.getRawVoice(), 4096);
    for (int slot = 0; slot < 32; ++slot)
        voices[size_t(slot * 128 + 5 * 17 + 14)] = uint8_t((slot * 3 + tag.hashCode()) & 0x3F) + 30;   // OP1 level
    return voices;
}

std::array<uint8_t, 1120> makeAmem(int seed) {
    std::array<uint8_t, 1120> amem {};
    for (size_t i = 0; i < amem.size(); ++i) amem[i] = uint8_t((i * 7 + size_t(seed)) & 0x3F);
    return amem;
}

void append(juce::MemoryBlock& block, std::initializer_list<int> bytes) {
    for (const int b : bytes) { const auto c = uint8_t(b); block.append(&c, 1); }
}

template <size_t N> void append(juce::MemoryBlock& block, const std::array<uint8_t, N>& bytes) {
    block.append(bytes.data(), bytes.size());
}

/** A universal entry group: count, "LM  " + id, data, checksum over id and data. */
void appendUniversalGroup(juce::MemoryBlock& block, const char* id, const std::vector<uint8_t>& data, int count) {
    append(block, { (count >> 7) & 0x7F, count & 0x7F });
    std::vector<uint8_t> body;
    for (const char* c = "LM  "; *c; ++c) body.push_back(uint8_t(*c));
    for (int i = 0; i < 6; ++i) body.push_back(uint8_t(id[i]));
    body.insert(body.end(), data.begin(), data.end());
    block.append(body.data(), body.size());
    append(block, { AVDexedSysex::checksum(body.data(), body.size()) });
}

/** A DX7II "all data" file as the DX7II sends it: performances, then the two
    banks, then their two AMEM, each after a bank-select message. */
juce::MemoryBlock makeDx7iiDump() {
    juce::MemoryBlock dump;
    append(dump, { 0xF0, 0x43, 0x00, 0x7E });
    appendUniversalGroup(dump, "8973PM", std::vector<uint8_t>(32 * 51, 0x10), 1642);
    append(dump, { 0xF7 });
    append(dump, { 0xF0, 0x43, 0x10, 0x19, 0x4D, 0x00, 0xF7 });
    append(dump, AVDexedSysex::bankMessage(makeVoices("A").data()));
    append(dump, { 0xF0, 0x43, 0x10, 0x19, 0x4D, 0x01, 0xF7 });
    append(dump, AVDexedSysex::bankMessage(makeVoices("B").data()));
    append(dump, AVDexedSysex::amemMessage(makeAmem(1).data()));
    append(dump, AVDexedSysex::amemMessage(makeAmem(2).data()));
    return dump;
}

std::array<uint8_t, 155> makeVced(const juce::String& name, int variant) {
    auto program = Program::initialProgram();
    std::array<uint8_t, 155> voice {};
    std::memcpy(voice.data(), program.getRawData(), 155);
    voice[5 * 21 + 16] = uint8_t(40 + variant % 50);
    for (int i = 0; i < 10; ++i) voice[size_t(145 + i)] = uint8_t(i < name.length() ? name[i] : ' ');
    return voice;
}

juce::MemoryBlock vcedFile(const std::array<uint8_t, 155>& voice) {
    juce::MemoryBlock file;
    append(file, { 0xF0, 0x43, 0x00, 0x00, 0x01, 0x1B });
    file.append(voice.data(), voice.size());
    append(file, { AVDexedSysex::checksum(voice.data(), voice.size()), 0xF7 });
    return file;
}

std::array<uint8_t, 4096> bankVoicesOf(const juce::File& file) {
    Cartridge bank;
    std::array<uint8_t, 4096> voices {};
    if (AVDexedLibrary::readBank(file, bank))
        std::memcpy(voices.data(), bank.getRawVoice(), 4096);
    return voices;
}

bool testScanFindsEveryBank() {
    bool passed = true;
    const auto dump = makeDx7iiDump();
    const auto contents = AVDexedSysex::scan(dump);
    passed &= expect(contents.banks.size() == 2, "a DX7II dump gives its two banks");
    passed &= expect(contents.banks.size() == 2 && contents.banks[0].voices == makeVoices("A") && contents.banks[1].voices == makeVoices("B"),
                     "the banks keep their order and voices");
    passed &= expect(contents.banks.size() == 2 && contents.banks[0].hasAmem && contents.banks[0].amem == makeAmem(1)
                         && contents.banks[1].hasAmem && contents.banks[1].amem == makeAmem(2),
                     "the two AMEM sent after the two banks pair in order");
    const auto performances = contents.universalNamed("8973PM");
    passed &= expect(performances.size() == 1 && performances[0]->data.size() == 32 * 51 && performances[0]->checksumOk,
                     "the DX7II performance dump is read with its checksum (byte count includes the ID)");

    Cartridge first;
    passed &= expect(first.load(static_cast<const uint8_t*>(dump.getData()), int(dump.getSize())) == 0
                         && std::memcmp(first.getRawVoice(), makeVoices("A").data(), 4096) == 0,
                     "Cartridge still loads the first bank of a dump as before");

    // TX802: 64 entries in one message, the count without the ID, ASCII nibbles.
    juce::MemoryBlock tx802;
    append(tx802, { 0xF0, 0x43, 0x00, 0x7E });
    for (int performance = 0; performance < 64; ++performance) {
        std::vector<uint8_t> text;
        for (int i = 0; i < 84; ++i) {
            const int value = (performance + i) & 0x7F;
            text.push_back(uint8_t("0123456789ABCDEF"[value >> 4]));
            text.push_back(uint8_t("0123456789ABCDEF"[value & 15]));
        }
        appendUniversalGroup(tx802, "8952PM", text, 168);
    }
    append(tx802, { 0xF7 });
    const auto txContents = AVDexedSysex::scan(tx802);   // the entries point into it
    const auto tx = txContents.universalNamed("8952PM");
    bool allOk = tx.size() == 64;
    for (const auto* entry : tx) allOk &= entry->checksumOk && entry->data.size() == 168;
    passed &= expect(allOk, "a TX802 performance dump gives its 64 entries (byte count without the ID)");
    passed &= expect(tx.size() == 64 && AVDexedSysex::decodeAsciiNibbles(tx[5]->data)[3] == 8,
                     "ASCII nibbles decode high nibble first");
    return passed;
}

bool testScanOddFiles() {
    bool passed = true;
    const auto voices = makeVoices("R");
    // A cartridge saved without its SysEx header.
    Cartridge raw;
    passed &= expect(AVDexedSysex::looksLikeRawCartridge(voices.data(), voices.size()), "a headerless cartridge is recognised");
    passed &= expect(raw.load(voices.data(), int(voices.size())) == 0 && std::memcmp(raw.getRawVoice(), voices.data(), 4096) == 0,
                     "a headerless cartridge loads as a bank");
    std::array<uint8_t, 4096> noise {};
    for (size_t i = 0; i < noise.size(); ++i) noise[i] = uint8_t((i * 37) & 0x7F);
    for (int voice = 0; voice < 32; ++voice) noise[size_t(voice * 128 + 120)] = 3;   // unreadable names
    passed &= expect(! AVDexedSysex::looksLikeRawCartridge(noise.data(), noise.size()), "4096 bytes of other data are not a cartridge");

    // A librarian's header before the bank, and the last F7 missing.
    juce::MemoryBlock wrapped;
    wrapped.append("Sysex Manager-sjk1995", 21);
    const auto message = AVDexedSysex::bankMessage(voices.data());
    wrapped.append(message.data(), message.size() - 1);
    Cartridge wrappedBank;
    passed &= expect(wrappedBank.load(static_cast<const uint8_t*>(wrapped.getData()), int(wrapped.getSize())) == 0
                         && std::memcmp(wrappedBank.getRawVoice(), voices.data(), 4096) == 0,
                     "a bank after a librarian header and without its last F7 loads");

    // A bank cut short, then a complete one: only the complete one counts.
    juce::MemoryBlock cut;
    cut.append(message.data(), 2000);
    const auto other = AVDexedSysex::bankMessage(makeVoices("C").data());
    cut.append(other.data(), other.size());
    const auto contents = AVDexedSysex::scan(cut);
    passed &= expect(contents.banks.size() == 1 && contents.banks[0].voices == makeVoices("C"), "a bank cut short is skipped");

    // A wrong checksum is reported, the voices still read.
    auto bad = message;
    bad[100] = uint8_t(bad[100] ^ 1);
    const auto badScan = AVDexedSysex::scan(bad.data(), bad.size());
    passed &= expect(badScan.banks.size() == 1 && ! badScan.banks[0].checksumOk, "a checksum mismatch is reported");
    return passed;
}

bool testPackVoiceMatchesBanks() {
    // Unpacking a bank voice and packing it again gives the same 128 bytes.
    const auto voices = makeVoices("P");
    Cartridge bank;
    bank.load(AVDexedSysex::bankMessage(voices.data()).data(), 4104);
    bool same = true;
    for (int slot = 0; slot < 32; ++slot) {
        const auto program = bank.unpackProgram(slot);
        const auto packed = AVDexedSysex::packVoice(program.getRawData());
        same &= std::memcmp(packed.data(), voices.data() + slot * 128, 128) == 0;
    }
    return expect(same, "packVoice packs as a 32-voice bulk stores a voice");
}

bool testImportSplitsAndConverts() {
    bool passed = true;
    Scratch scratch;
    const auto root = AVDexedLibrary::rootFor(scratch.root.getChildFile("Library"));
    root.createDirectory();
    const auto outside = scratch.root.getChildFile("Downloads");
    outside.createDirectory();
    const auto folder = root.getChildFile("DX7II");
    using Status = AVDexedLibrary::ImportResult::Status;

    const auto dumpFile = outside.getChildFile("Factory.syx");
    const auto dump = makeDx7iiDump();
    dumpFile.replaceWithData(dump.getData(), dump.getSize());
    const auto split = AVDexedLibrary::importBank(dumpFile, folder);
    passed &= expect(split.status == Status::copied && split.files.size() == 2 && split.sounds == 64,
                     "a 64-voice dump becomes two banks");
    passed &= expect(folder.getChildFile("Factory A.syx").existsAsFile() && folder.getChildFile("Factory B.syx").existsAsFile(),
                     "the two banks are named A and B");
    passed &= expect(bankVoicesOf(folder.getChildFile("Factory B.syx")) == makeVoices("B"), "bank B holds the second bank");
    juce::MemoryBlock bytesB;
    folder.getChildFile("Factory B.syx").loadFileAsData(bytesB);
    const auto scannedB = AVDexedSysex::scan(bytesB);
    passed &= expect(scannedB.banks.size() == 1 && scannedB.banks[0].hasAmem && scannedB.banks[0].amem == makeAmem(2),
                     "each bank keeps its DX7II additional data");
    const auto again = AVDexedLibrary::importBank(dumpFile, folder);
    passed &= expect(again.status == Status::alreadyThere && again.files.size() == 2 && again.sounds == 0,
                     "importing the same dump again reuses both banks");

    const auto rawFile = outside.getChildFile("Raw.syx");
    const auto voices = makeVoices("R");
    rawFile.replaceWithData(voices.data(), voices.size());
    const auto converted = AVDexedLibrary::importBank(rawFile, folder);
    passed &= expect(converted.status == Status::copied && converted.file == folder.getChildFile("Raw.syx")
                         && converted.file.getSize() == 4104 && bankVoicesOf(converted.file) == voices,
                     "a headerless cartridge is imported as a standard bank");

    const auto plain = outside.getChildFile("Plain.syx");
    const auto plainBytes = AVDexedSysex::bankMessage(makeVoices("Q").data());
    plain.replaceWithData(plainBytes.data(), plainBytes.size());
    const auto copied = AVDexedLibrary::importBank(plain, folder);
    juce::MemoryBlock copiedBytes;
    copied.file.loadFileAsData(copiedBytes);
    passed &= expect(copied.status == Status::copied && copied.sounds == 32
                         && copiedBytes == juce::MemoryBlock(plainBytes.data(), plainBytes.size()),
                     "a standard bank is copied byte for byte");

    // Single voices: 40 files of one folder become two banks.
    const auto singles = outside.getChildFile("Single Voice");
    singles.createDirectory();
    juce::Array<juce::File> voiceFiles;
    for (int i = 0; i < 40; ++i) {
        const auto file = singles.getChildFile("voice" + juce::String(i + 1) + ".syx");
        const auto bytes = vcedFile(makeVced("SOLO " + juce::String(i + 1), i));
        file.replaceWithData(bytes.getData(), bytes.getSize());
        voiceFiles.add(file);
    }
    passed &= expect(AVDexedLibrary::isSingleVoiceFile(voiceFiles[0]) && ! AVDexedLibrary::isSingleVoiceFile(plain),
                     "single-voice files are told apart from banks");
    const auto gathered = AVDexedLibrary::importVoices(voiceFiles, folder, "Single Voice");
    passed &= expect(gathered.status == Status::copied && gathered.files.size() == 2 && gathered.sounds == 40,
                     "40 single voices become two banks");
    Cartridge second;
    AVDexedLibrary::readBank(folder.getChildFile("Single Voice 2.syx"), second);
    passed &= expect(second.getProgramName(0).trim() == "SOLO 33" && second.getProgramName(7).trim() == "SOLO 40"
                         && second.getProgramName(8).trim() == "INIT VOICE",
                     "the voices keep their file-name order and the rest of the bank is Init");

    const auto lone = AVDexedLibrary::importBank(voiceFiles[4], folder);
    Cartridge loneBank;
    AVDexedLibrary::readBank(lone.file, loneBank);
    passed &= expect(lone.status == Status::copied && lone.sounds == 1 && loneBank.getProgramName(0).trim() == "SOLO 5",
                     "a lone single voice becomes a bank of its own");

    const auto junk = outside.getChildFile("notes.syx");
    junk.replaceWithText("not a bank at all");
    passed &= expect(AVDexedLibrary::importBank(junk, folder).status == Status::notABank, "other data is still refused");
    return passed;
}
}

bool runAVDexedSysexTests() {
    bool passed = testScanFindsEveryBank();
    passed &= testScanOddFiles();
    passed &= testPackVoiceMatchesBanks();
    passed &= testImportSplitsAndConverts();
    std::cout << (passed ? "DX7 family reader checks passed.\n" : "DX7 family reader checks FAILED.\n");
    return passed;
}
