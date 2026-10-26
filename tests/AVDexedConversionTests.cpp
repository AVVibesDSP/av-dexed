// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

// Banks turned into sound files (AVDexedConversion): the names, the folders,
// repeated and empty voices, the order, the backup of the originals, nothing
// left behind when it fails, and that every converted sound plays bit for bit
// as its bank voice does. --convert-check runs that last check on every bank
// of a library (a copy of one), as its conversion would make them.

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif
#include "../source/PluginProcessor.h"
#include "../source/ui/PluginEditor.h"
#include "../source/ui/AVDexedLanguage.h"
#include "../source/parameter/Model.h"
#include "../source/core/AVDexedSysex.h"
#include "../source/core/AVDexedConversion.h"
#include "../source/midi/MidiCCHandler.h"
#include <iostream>
#include <map>
#include <set>

namespace {
bool expect(bool condition, const juce::String& message) {
    if (! condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

constexpr double rate = 48000.0;

/** FNV-1a over the bits of about 0.9 s: a chord, pitch bend, mod wheel,
    aftertouch and the sustain pedal, then the release. Uneven blocks. */
uint64_t renderPrint(DexedAudioProcessor& processor) {
    struct Event { int sample; juce::MidiMessage message; };
    const auto at = [](double seconds) { return int(seconds * rate); };
    const std::vector<Event> events {
        { at(0.00), juce::MidiMessage::noteOn(1, 48, (juce::uint8) 100) },
        { at(0.00), juce::MidiMessage::noteOn(1, 60, (juce::uint8) 90) },
        { at(0.01), juce::MidiMessage::noteOn(1, 67, (juce::uint8) 120) },
        { at(0.15), juce::MidiMessage::pitchWheel(1, 12000) },
        { at(0.20), juce::MidiMessage::controllerEvent(1, 1, 80) },
        { at(0.25), juce::MidiMessage::channelPressureChange(1, 90) },
        { at(0.30), juce::MidiMessage::controllerEvent(1, 64, 127) },
        { at(0.40), juce::MidiMessage::noteOff(1, 48) },
        { at(0.40), juce::MidiMessage::noteOff(1, 60) },
        { at(0.41), juce::MidiMessage::noteOff(1, 67) },
        { at(0.50), juce::MidiMessage::noteOn(1, 72, (juce::uint8) 127) },
        { at(0.55), juce::MidiMessage::controllerEvent(1, 64, 0) },
        { at(0.60), juce::MidiMessage::noteOff(1, 72) },
    };
    const int total = at(0.9);
    const int blockSizes[] = { 512, 300, 64, 1000, 37 };
    juce::AudioBuffer<float> buffer(2, 1024);
    uint64_t print = 1469598103934665603ull;
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
        for (int channel = 0; channel < 2; ++channel)
            for (int i = 0; i < size; ++i) {
                uint32_t bits;
                std::memcpy(&bits, buffer.getReadPointer(channel) + i, sizeof(bits));
                for (int byte = 0; byte < 4; ++byte)
                    print = (print ^ ((bits >> (8 * byte)) & 0xFF)) * 1099511628211ull;
            }
        position += size;
    }
    return print;
}

std::unique_ptr<DexedAudioProcessor> freshProcessor() {
    auto processor = std::make_unique<DexedAudioProcessor>(DexedAudioProcessor::TestMode{});
    processor->setRateAndBufferSizeDetails(rate, 1024);
    processor->prepareToPlay(rate, 1024);
    return processor;
}

/** The bank voice as the browser plays it (its file gives its settings). */
uint64_t bankVoicePrint(const Cartridge& bank, const juce::File& file, int slot) {
    auto processor = freshProcessor();
    processor->setActiveBank(bank, file);
    processor->setCurrentProgram(slot);
    return renderPrint(*processor);
}

/** The sound file as the browser plays it; 0 when it does not load. */
uint64_t soundFilePrint(const juce::File& file) {
    juce::MemoryBlock data;
    auto processor = freshProcessor();
    if (! file.loadFileAsData(data) || ! processor->loadAVNativePreset(data.getData(), int(data.getSize())))
        return 0;
    return renderPrint(*processor);
}

std::unique_ptr<juce::XmlElement> readXml(const juce::File& file) {
    juce::MemoryBlock data;
    if (! file.loadFileAsData(data)) return {};
    return juce::AudioProcessor::getXmlFromBinary(data.getData(), int(data.getSize()));
}

juce::XmlElement* session(const std::unique_ptr<juce::XmlElement>& xml) {
    return xml != nullptr ? xml->getChildByName("session") : nullptr;
}

/** 32 different voices (the Init voice with other algorithms, ratios,
    levels and LFOs; `seed` makes another bank), named by `name`. */
Cartridge makeBank(const std::function<juce::String(int)>& name, int seed) {
    Cartridge bank;
    for (int slot = 0; slot < 32; ++slot) {
        auto voice = Program::initialProgram();
        uint8_t data[Program::PROGRAM_SIZE + 1];
        std::memcpy(data, voice.getRawData(), sizeof(data));
        const int v = slot + seed * 7;
        data[134] = uint8_t(v % 32);                   // algorithm
        data[135] = uint8_t(v % 8);                    // feedback
        data[137] = uint8_t(20 + (v * 3) % 70);        // LFO speed
        data[139] = uint8_t(v % 3 == 0 ? 30 : 0);      // pitch mod depth
        data[142] = uint8_t(v % 6);                    // LFO wave (sample & hold too)
        data[143] = uint8_t(v % 8);                    // pitch mod sensitivity
        data[144] = uint8_t(12 + seed % 25);           // transpose: another bank, other sounds
        for (int op = 0; op < 6; ++op) {
            data[op * 21 + 16] = uint8_t(op == 5 ? 99 : 60 + (v + op) % 30);
            data[op * 21 + 18] = uint8_t(1 + (v + op) % 4);
            data[op * 21 + 0] = uint8_t(60 + (op * 7 + v) % 40);
            data[op * 21 + 6] = uint8_t(v % 2 ? 80 : 0);
        }
        bank.packProgram(data, slot, name(slot));
    }
    return bank;
}

juce::MemoryBlock bankBytes(const Cartridge& bank) {
    const auto message = AVDexedSysex::bankMessage(reinterpret_cast<const uint8_t*>(Cartridge(bank).getRawVoice()));
    return juce::MemoryBlock(message.data(), message.size());
}

juce::Array<juce::File> presetsIn(const juce::File& folder) {
    juce::Array<juce::File> found;
    for (const auto& entry : juce::RangedDirectoryIterator(folder, false, "*.avdexpreset", juce::File::findFiles))
        found.add(entry.getFile());
    return found;
}

/** A scratch library under /tmp, removed at the end. */
struct Scratch {
    juce::File home = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("avdexed-conversion-" + juce::Uuid().toString());
    juce::File root = home.getChildFile("AV Dexed");
    juce::File outside = home.getChildFile("Downloads");
    Scratch() { root.createDirectory(); outside.createDirectory(); }
    ~Scratch() { home.deleteRecursively(); }
};

bool testVoiceNames() {
    bool passed = true;
    // Numbered from 0 by the bank (the AV Dexed expansions): the number goes.
    const auto numbered = makeBank([](int slot) { return juce::String(slot) + " BAS Ana"; }, 0);
    const auto names = AVDexedConversion::voiceNames(numbered, 32, "Bajos");
    passed &= expect(names.size() == 32 && names[0] == "BAS Ana" && names[26] == "BAS Ana" && names[31] == "BAS Ana",
                     "a bank that numbers its voices from 0 loses the numbers");
    // From 1 as well.
    const auto fromOne = makeBank([](int slot) { return juce::String(slot + 1).paddedLeft('0', 2) + " PNO"; }, 1);
    passed &= expect(AVDexedConversion::voiceNames(fromOne, 32, "x")[0] == "PNO"
                         && AVDexedConversion::voiceNames(fromOne, 32, "x")[9] == "PNO",
                     "a bank that numbers them from 1 too");
    // A few names that start with a number are names, not numbers.
    const auto someNumbers = makeBank([](int slot) {
        return slot == 1 ? juce::String("1 OF 3") : slot == 2 ? juce::String("2 PIANOS") : "VOICE " + juce::String(slot);
    }, 2);
    const auto kept = AVDexedConversion::voiceNames(someNumbers, 32, "x");
    passed &= expect(kept[1] == "1 OF 3" && kept[2] == "2 PIANOS" && kept[5] == "VOICE 5",
                     "names of their own that start with a number stay");
    // Nothing left: the bank's name and the slot.
    const auto blank = makeBank([](int) { return juce::String("          "); }, 3);
    passed &= expect(AVDexedConversion::voiceNames(blank, 32, "Rhodes")[4] == "Rhodes 5", "a voice without a name gets the bank's");
    return passed;
}

/** Every sound file `result` wrote plays as the voice it came from. */
bool playsAsItsBankVoice(const AVDexedConversion::Result& result,
                         const std::map<juce::String, std::pair<juce::File, Cartridge>>& banks, int& checked) {
    bool same = true;
    for (const auto& file : result.files) {
        const auto xml = readXml(file);
        const auto* s = session(xml);
        if (s == nullptr) return false;
        const auto bank = banks.find(s->getStringAttribute("avSourceBank"));
        if (bank == banks.end()) continue;
        const int slot = s->getIntAttribute("avSourceSlot");
        const auto expected = bankVoicePrint(bank->second.second, bank->second.first, slot);
        if (soundFilePrint(file) != expected) {
            std::cerr << "  not the same: " << file.getFileName() << " (" << bank->first << " #" << slot + 1 << ")\n";
            same = false;
        }
        ++checked;
    }
    return same;
}

bool testConvertFolder() {
    bool passed = true;
    Scratch scratch;
    const auto& root = scratch.root;
    const auto& outside = scratch.outside;

    // Rhodes: numbered from 0, slot 6 the Init voice, slot 8 the same sound as slot 7 under another name.
    auto rhodes = makeBank([](int slot) { return juce::String(slot) + " EP " + juce::String(slot); }, 0);
    rhodes.packProgram(Program::initialProgram().getRawData(), 5, "INIT VOICE");
    {
        const auto twin = rhodes.unpackProgram(6);
        rhodes.packProgram(twin.getRawData(), 7, "7 EP TWIN");
    }
    const auto rhodesFile = outside.getChildFile("Rhodes.syx");
    rhodesFile.replaceWithData(bankBytes(rhodes).getData(), bankBytes(rhodes).getSize());
    // Pads/Warm: its slot 4 with a reverb of its own, embedded in the file.
    const auto warm = makeBank([](int slot) { return "WARM " + juce::String(slot); }, 1);
    AVDexedLibrary::BankSettings settings;
    settings[3] = { AVDexedLibrary::voiceHash(warm, 3), { { IDs::reverbAmount.name, 0.5f } } };
    const auto warmFile = outside.getChildFile("Pads").getChildFile("Warm.syx");
    warmFile.getParentDirectory().createDirectory();
    const auto warmBytes = AVDexedLibrary::composeBankFile(bankBytes(warm), warm, settings);
    warmFile.replaceWithData(warmBytes.getData(), warmBytes.getSize());
    // An archive of three banks; a file of two single voices; a DX7II bank with its AMEM.
    juce::MemoryBlock archiveBytes;
    std::vector<Cartridge> archived;
    for (int i = 0; i < 3; ++i) {
        archived.push_back(makeBank([i](int slot) { return "ARC" + juce::String(i) + " " + juce::String(slot); }, 10 + i));
        archiveBytes.append(bankBytes(archived.back()).getData(), bankBytes(archived.back()).getSize());
    }
    const auto archiveFile = outside.getChildFile("Archive.syx");
    archiveFile.replaceWithData(archiveBytes.getData(), archiveBytes.getSize());
    juce::MemoryBlock soloBytes;
    for (int i = 0; i < 2; ++i) {
        std::array<uint8_t, AVDexedSysex::voiceBytes + 8> message {};
        const auto voice = makeBank([i](int) { return "SOLO " + juce::String(i); }, 20 + i).unpackProgram(i);
        const uint8_t head[] = { 0xF0, 0x43, 0x00, 0x00, 0x01, 0x1B };
        std::memcpy(message.data(), head, 6);
        std::memcpy(message.data() + 6, voice.getRawData(), AVDexedSysex::voiceBytes);
        message[6 + AVDexedSysex::voiceBytes] = AVDexedSysex::checksum(voice.getRawData(), AVDexedSysex::voiceBytes);
        message[7 + AVDexedSysex::voiceBytes] = 0xF7;
        soloBytes.append(message.data(), message.size());
    }
    const auto soloFile = outside.getChildFile("Solo.syx");
    soloFile.replaceWithData(soloBytes.getData(), soloBytes.getSize());
    auto dx7ii = makeBank([](int slot) { return "MK2 " + juce::String(slot); }, 30);
    std::array<uint8_t, 1120> amem {};
    amem[3 * 35 + 5] = 2;          // slot 4: unison
    amem[3 * 35 + 34] = 3;         // its detune
    amem[3 * 35 + 4] = 2 << 4;     // random pitch
    juce::MemoryBlock dx7iiBytes;
    const auto amemMessage = AVDexedSysex::amemMessage(amem.data());
    dx7iiBytes.append(amemMessage.data(), amemMessage.size());
    dx7iiBytes.append(bankBytes(dx7ii).getData(), bankBytes(dx7ii).getSize());
    const auto dx7iiFile = outside.getChildFile("DX7II.syx");
    dx7iiFile.replaceWithData(dx7iiBytes.getData(), dx7iiBytes.getSize());
    const auto notesFile = outside.getChildFile("notes.syx");
    notesFile.replaceWithText("not a bank");

    AVDexedConversion::Request request;
    request.sources = { rhodesFile, warmFile, archiveFile, soloFile, dx7iiFile, notesFile };
    request.root = root;
    request.destination = root.getChildFile("Importados");
    request.layout = AVDexedConversion::Layout::folderPerBank;
    request.sourceBase = outside;
    request.backupLabel = "Prueba";
    std::vector<float> steps;
    const auto result = DexedAudioProcessor::convertAVBanks(request, [&steps](float done) { steps.push_back(done); return true; });

    const auto importados = root.getChildFile("Importados");
    passed &= expect(result.ok && result.banks == 7 && result.unreadable == juce::StringArray { "notes.syx" },
                     "seven banks are read (one file of three), and the file that holds none is named");
    passed &= expect(result.empty == 1 && result.repeated == 1 && result.sounds == 30 + 32 + 96 + 2 + 32,
                     "the Init voice and the repeated sound are left out, every other voice is a sound file");
    passed &= expect(! steps.empty() && steps.back() == 1.0f, "the progress ends at 1");
    const auto rhodesFolder = importados.getChildFile("Rhodes");
    passed &= expect(presetsIn(rhodesFolder).size() == 30 && rhodesFolder.getChildFile("EP 0.avdexpreset").existsAsFile()
                         && rhodesFolder.getChildFile("EP 26.avdexpreset").existsAsFile()
                         && ! rhodesFolder.getChildFile("7 EP TWIN.avdexpreset").existsAsFile(),
                     "a bank becomes a folder of sounds named without the slot numbers");
    passed &= expect(importados.getChildFile("Pads").getChildFile("Warm").isDirectory()
                         && presetsIn(importados.getChildFile("Pads").getChildFile("Warm")).size() == 32,
                     "a dropped folder's structure is kept");
    passed &= expect(presetsIn(importados.getChildFile("Archive 1")).size() == 32 && presetsIn(importados.getChildFile("Archive 3")).size() == 32,
                     "the banks of an archive get a folder each");
    passed &= expect(importados.getChildFile("SOLO 0.avdexpreset").existsAsFile() && importados.getChildFile("SOLO 1.avdexpreset").existsAsFile()
                         && ! importados.getChildFile("Solo").exists(),
                     "single voices become a sound each, in their folder (not a folder of their own)");

    // What a converted sound file holds.
    {
        const auto first = readXml(rhodesFolder.getChildFile("EP 0.avdexpreset"));
        const auto* s = session(first);
        passed &= expect(first != nullptr && first->getChildByName("profile") == nullptr && first->getChildByName("tuning") == nullptr
                             && s != nullptr && ! s->hasAttribute("cartridge") && s->getStringAttribute("sourcePath").isEmpty()
                             && s->getStringAttribute("avPresetName") == "EP 0" && s->getStringAttribute("avSourceBank") == "Rhodes"
                             && s->getIntAttribute("avSourceSlot") == 0 && s->getStringAttribute("avSoundKey").isNotEmpty()
                             && first->getChildByName("avLayers") == nullptr,
                         "a converted sound keeps its origin and key, not the bank, the MIDI assignments or the tuning");
        bool ordered = true;
        int previousSlot = -1;
        for (int order = 0; order < 30; ++order) {
            const juce::XmlElement* match = nullptr;
            std::unique_ptr<juce::XmlElement> holder;
            for (const auto& file : presetsIn(rhodesFolder)) {
                holder = readXml(file);
                if (session(holder) != nullptr && session(holder)->getIntAttribute("avLibraryOrder", -1) == order) { match = session(holder); break; }
            }
            ordered &= match != nullptr && match->getIntAttribute("avSourceSlot") > previousSlot;
            if (match != nullptr) previousSlot = match->getIntAttribute("avSourceSlot");
        }
        passed &= expect(ordered, "the sounds keep the bank's order, one after another (0, 1, 2...)");
        const auto withAmem = readXml(importados.getChildFile("DX7II").getChildFile("MK2 3.avdexpreset"));
        passed &= expect(session(withAmem) != nullptr && session(withAmem)->getStringAttribute("voiceAmem").isNotEmpty()
                             && session(readXml(importados.getChildFile("Rhodes").getChildFile("EP 1.avdexpreset")))->getStringAttribute("voiceAmem").isEmpty(),
                         "a DX7II voice keeps its additional data");
    }

    // The originals: backed up as they were, where nothing lists them.
    const auto backups = root.getChildFile(AVDexedLibrary::syxBackupsFolderName);
    passed &= expect(result.backup.isAChildOf(backups) && result.backup.getFileName().endsWith(" Prueba")
                         && result.backup.getChildFile("Rhodes.syx").hasIdenticalContentTo(rhodesFile)
                         && result.backup.getChildFile("Pads").getChildFile("Warm.syx").hasIdenticalContentTo(warmFile)
                         && result.backup.getChildFile("Archive.syx").hasIdenticalContentTo(archiveFile)
                         && ! result.backup.getChildFile("notes.syx").exists(),
                     "the original files are kept in UserSYXsBackups, with their folders");
    bool listed = false;
    for (const auto& file : AVDexedLibrary::findSoundFiles(root))
        listed |= file.isAChildOf(backups) || file.getFullPathName().contains(".converting-");
    for (const auto& folder : AVDexedLibrary::listFolders(root, true))
        listed |= folder == backups || folder.isAChildOf(backups);
    passed &= expect(! listed && ! AVDexedLibrary::findSoundFiles(root).isEmpty(), "the library never lists the backups");
    bool staged = false;
    for (const auto& entry : juce::RangedDirectoryIterator(root, false, ".converting-*", juce::File::findDirectories))
        staged |= entry.getFile().isDirectory();
    passed &= expect(! staged && rhodesFile.existsAsFile(), "nothing is left half done, and the dropped files stay where they were");

    // Bit for bit: each converted sound plays as its bank voice.
    std::map<juce::String, std::pair<juce::File, Cartridge>> banks;
    for (const auto& file : request.sources)
        for (const auto& bank : AVDexedLibrary::readSourceBanks(file)) {
            Cartridge plain;
            const bool standard = AVDexedLibrary::readBank(file, plain) && AVDexedLibrary::readSourceBanks(file).size() == 1
                && bank.voices == 32;
            banks[bank.name] = { standard ? file : juce::File(), bank.bank };
        }
    int checked = 0;
    passed &= expect(playsAsItsBankVoice(result, banks, checked) && checked == result.sounds,
                     "every converted sound plays bit for bit as its bank voice (" + juce::String(checked) + ")");
    const auto reverb = readXml(importados.getChildFile("Pads").getChildFile("Warm").getChildFile("WARM 3.avdexpreset"));
    bool reverbCarried = false;
    if (reverb != nullptr)
        if (const auto* values = reverb->getChildByName("parameters"))
            for (const auto* parameter : values->getChildIterator())
                if (parameter->getStringAttribute("id") == IDs::reverbAmount.name)
                    reverbCarried = std::abs(parameter->getDoubleAttribute("value") - 0.5) < 1.0e-6;
    passed &= expect(reverbCarried, "the settings embedded in the bank go with their sound");

    // Again: everything is there already.
    const auto again = DexedAudioProcessor::convertAVBanks(request);
    passed &= expect(again.ok && again.sounds == 0 && again.repeated == result.sounds + result.repeated,
                     "converting the same banks again adds nothing");
    // Flat: every voice straight into the folder.
    AVDexedConversion::Request flat = request;
    flat.sources = { rhodesFile, warmFile };
    flat.destination = root.getChildFile("Plano");
    flat.layout = AVDexedConversion::Layout::flat;
    flat.sourceBase = juce::File();
    flat.backup = false;
    const auto flatResult = DexedAudioProcessor::convertAVBanks(flat);
    passed &= expect(flatResult.ok && presetsIn(root.getChildFile("Plano")).size() == 62 && ! flatResult.backup.exists(),
                     "the flat layout puts every voice in the folder itself");
    // More banks into the same folder later: after the sounds already there.
    AVDexedConversion::Request later = flat;
    later.sources = { soloFile };
    const auto laterResult = DexedAudioProcessor::convertAVBanks(later);
    bool after = laterResult.ok && laterResult.files.size() == 2;
    for (const auto& file : laterResult.files)
        after &= session(readXml(file)) != nullptr && session(readXml(file))->getIntAttribute("avLibraryOrder") >= 62;
    passed &= expect(after, "sounds added to a folder later come after the ones already there");

    // Cancelled, or a destination it cannot write: nothing changes, no backup is left.
    const int backupsBefore = backups.getNumberOfChildFiles(juce::File::findDirectories);
    AVDexedConversion::Request cancelled = request;
    cancelled.destination = root.getChildFile("Cancelado");
    const auto cancel = DexedAudioProcessor::convertAVBanks(cancelled, [](float done) { return done < 0.5f; });
    passed &= expect(! cancel.ok && ! root.getChildFile("Cancelado").exists()
                         && backups.getNumberOfChildFiles(juce::File::findDirectories) == backupsBefore,
                     "a cancelled conversion changes nothing and leaves no backup");
    // A file where a folder would go: a read-only folder still takes new files on Windows.
    const auto locked = root.getChildFile("Bloqueado");
    locked.replaceWithText("not a folder");
    AVDexedConversion::Request refused = request;
    refused.destination = locked.getChildFile("Dentro");
    const auto failed = DexedAudioProcessor::convertAVBanks(refused);
    passed &= expect(! failed.ok && ! locked.getChildFile("Dentro").exists()
                         && backups.getNumberOfChildFiles(juce::File::findDirectories) == backupsBefore,
                     "a destination that cannot be written changes nothing and leaves no backup");
    return passed;
}

/** The factory banks (the bundled zip) convert bit for bit too. */
bool testFactoryBanks() {
    Scratch scratch;
    if (AVDexedLibrary::installFactorySounds(scratch.root, BinaryData::builtin_pgm_zip, size_t(BinaryData::builtin_pgm_zipSize), false) <= 1)
        return expect(false, "the factory sounds install into a scratch library");
    juce::Array<juce::File> sources;
    sources.add(AVDexedLibrary::folderNamed(scratch.root, AVDexedLibrary::factoryFolderName).getChildFile("Dexed_01.syx"));
    auto synprez = AVDexedLibrary::folderNamed(scratch.root, AVDexedLibrary::synprezFolderName).findChildFiles(juce::File::findFiles, true, "*.syx");
    std::sort(synprez.begin(), synprez.end());
    for (int i = 0; i < juce::jmin(2, synprez.size()); ++i) sources.add(synprez[i]);
    AVDexedConversion::Request request;
    request.sources = sources;
    request.root = scratch.root;
    request.destination = scratch.root.getChildFile("Convertidos");
    request.backup = false;
    const auto result = DexedAudioProcessor::convertAVBanks(request);
    std::map<juce::String, std::pair<juce::File, Cartridge>> banks;
    for (const auto& file : sources)
        for (const auto& bank : AVDexedLibrary::readSourceBanks(file))
            banks[bank.name] = { file, bank.bank };
    int checked = 0;
    return expect(result.ok && result.sounds > 64 && playsAsItsBankVoice(result, banks, checked) && checked == result.sounds,
                  "the factory banks convert bit for bit (" + juce::String(checked) + " sounds)");
}

/** Loading a converted sound keeps what is the instrument's, as a bank voice does. */
bool testKeepsTheInstrument() {
    bool passed = true;
    Scratch scratch;
    const auto bank = makeBank([](int slot) { return "KEEP " + juce::String(slot); }, 40);
    const auto file = scratch.outside.getChildFile("Keep.syx");
    file.replaceWithData(bankBytes(bank).getData(), bankBytes(bank).getSize());
    AVDexedConversion::Request request;
    request.sources = { file };
    request.root = scratch.root;
    request.destination = scratch.root.getChildFile("Keep");
    request.backup = false;
    const auto result = DexedAudioProcessor::convertAVBanks(request);
    auto processor = freshProcessor();
    processor->midiCCMapper->removeMapping(1);
    processor->applySCLTuning(std::string("! test\n12 tone\n12\n!\n100.0\n200.0\n300.0\n400.0\n500.0\n600.0\n700.0\n800.0\n900.0\n1000.0\n1100.0\n2/1\n"));
    const auto scl = processor->currentSCLData;
    juce::MemoryBlock data;
    passed &= expect(result.ok && ! result.files.isEmpty() && result.files[0].loadFileAsData(data)
                         && processor->loadAVNativePreset(data.getData(), int(data.getSize())),
                     "a converted sound loads");
    passed &= expect(processor->midiCCMapper->getMapping(IDs::cutoff.name) == -1 && processor->currentSCLData == scl && ! scl.empty(),
                     "and keeps the instrument's MIDI assignments and tuning, as a bank voice does");

    // The instrument's own settings (pitch bend, mono, portamento, engine,
    // wheel) stay as they are, exactly as when the bank voice is loaded.
    const auto setUp = [](DexedAudioProcessor& player) {
        for (const auto& [id, value] : std::vector<std::pair<juce::String, float>> {
                 { IDs::pitchBendUp.name, 12.0f }, { IDs::monoMode.name, 1.0f }, { IDs::portamentoTm.name, 40.0f },
                 { IDs::engineType.name, 2.0f }, { IDs::modWheelPitch.name, 1.0f }, { IDs::cutoff.name, 0.3f } })
            if (auto* parameter = player.parameters.getParameter(id))
                parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    };
    auto fromSound = freshProcessor(), fromBank = freshProcessor();
    setUp(*fromSound);
    setUp(*fromBank);
    fromSound->loadAVNativePreset(data.getData(), int(data.getSize()));
    fromBank->setActiveBank(bank, file);
    fromBank->setCurrentProgram(0);   // result.files[0] is its slot 1
    bool same = true;
    for (auto* parameter : fromSound->getParameters())
        if (auto* other = fromBank->getParameters()[parameter->getParameterIndex()])
            same &= std::abs(parameter->getValue() - other->getValue()) < 1.0e-6f;
    const auto* bend = fromSound->parameters.getParameter(IDs::pitchBendUp.name);
    passed &= expect(same && bend != nullptr && std::abs(bend->convertFrom0to1(bend->getValue()) - 12.0f) < 0.01f,
                     "a converted sound keeps the instrument's pitch bend, mono, portamento and engine, as its bank voice does");
    passed &= expect(renderPrint(*fromSound) == renderPrint(*fromBank), "and plays bit for bit as the bank voice with them");
    return passed;
}

/** In layer mode a converted DX7II voice brings its unison and random pitch,
    as from its bank. */
bool testExtrasInLayers() {
    Scratch scratch;
    auto bank = makeBank([](int slot) { return "MK2 " + juce::String(slot); }, 50);
    std::array<uint8_t, 1120> amem {};
    amem[3 * 35 + 5] = 2;
    amem[3 * 35 + 34] = 3;
    amem[3 * 35 + 4] = 2 << 4;
    juce::MemoryBlock bytes;
    const auto amemMessage = AVDexedSysex::amemMessage(amem.data());
    bytes.append(amemMessage.data(), amemMessage.size());
    bytes.append(bankBytes(bank).getData(), bankBytes(bank).getSize());
    const auto file = scratch.outside.getChildFile("MK2.syx");
    file.replaceWithData(bytes.getData(), bytes.getSize());
    AVDexedConversion::Request request;
    request.sources = { file };
    request.root = scratch.root;
    request.destination = scratch.root.getChildFile("MK2");
    request.layout = AVDexedConversion::Layout::flat;
    request.backup = false;
    DexedAudioProcessor::convertAVBanks(request);
    Cartridge withAmem;
    withAmem.load(static_cast<const uint8_t*>(bytes.getData()), int(bytes.getSize()));
    auto fromBank = freshProcessor();
    fromBank->setActiveBank(withAmem, file);
    fromBank->setAVLayerMode(true);
    fromBank->setCurrentProgram(3);
    auto fromSound = freshProcessor();
    fromSound->setAVLayerMode(true);
    juce::MemoryBlock data;
    scratch.root.getChildFile("MK2").getChildFile("MK2 3.avdexpreset").loadFileAsData(data);
    fromSound->loadAVNativePreset(data.getData(), int(data.getSize()));
    const auto& a = fromBank->getAVLayerSettings(0);
    const auto& b = fromSound->getAVLayerSettings(0);
    return expect(a.unison.load() > 0.0f && a.unison.load() == b.unison.load() && a.randomPitch.load() == b.randomPitch.load()
                      && a.mono.load() == b.mono.load(),
                  "in layer mode a converted DX7II voice brings its unison and random pitch, as from its bank");
}

/** A TX802 performance becomes a layers preset that plays as the Capas page
    plays the performance, its voices from the bank next to its file. */
bool testPerformances() {
    Scratch scratch;
    std::vector<uint8_t> pmem(84, 0);
    for (int tg = 0; tg < 8; ++tg) {
        pmem[size_t(tg)] = uint8_t((tg << 5) | 16);       // every TG its own instrument, omni
        pmem[size_t(8 + tg)] = uint8_t(tg * 3);           // internal voices 1, 4, 7…
        pmem[size_t(24 + tg)] = uint8_t(80 + tg * 2);     // output volume
        pmem[size_t(32 + tg)] = uint8_t(((7 + (tg % 3) - 1) << 3) | 3);   // detune around the centre, outputs I+II
        pmem[size_t(48 + tg)] = 127;
        pmem[size_t(56 + tg)] = uint8_t(24 + (tg % 2) * 12);
    }
    const char* name = "Conv Performance    ";
    for (int i = 0; i < 20; ++i) pmem[size_t(64 + i)] = uint8_t(name[i]);
    std::vector<uint8_t> body;
    for (const char* c = "LM  8952PM"; *c; ++c) body.push_back(uint8_t(*c));
    for (const auto byte : pmem) {
        body.push_back(uint8_t("0123456789ABCDEF"[byte >> 4]));
        body.push_back(uint8_t("0123456789ABCDEF"[byte & 15]));
    }
    juce::MemoryBlock dump;
    const uint8_t head[] = { 0xF0, 0x43, 0x00, 0x7E, 0x01, 0x28 };
    dump.append(head, sizeof(head));
    dump.append(body.data(), body.size());
    const uint8_t tail[] = { AVDexedSysex::checksum(body.data(), body.size()), 0xF7 };
    dump.append(tail, 2);
    const auto set = scratch.outside.getChildFile("TX Set");
    set.createDirectory();
    const auto performanceFile = set.getChildFile("SetPerf.syx");
    performanceFile.replaceWithData(dump.getData(), dump.getSize());
    const auto voices = makeBank([](int slot) { return "TXV " + juce::String(slot + 1); }, 60);
    set.getChildFile("SetVoice32.syx").replaceWithData(bankBytes(voices).getData(), bankBytes(voices).getSize());

    AVDexedConversion::Request request;
    request.sources = { performanceFile };
    request.root = scratch.root;
    request.destination = scratch.root.getChildFile("Performances");
    request.layout = AVDexedConversion::Layout::automatic;
    request.backup = false;
    const auto result = DexedAudioProcessor::convertAVBanks(request);
    const auto preset = request.destination.getChildFile("Conv Performance.avdexpreset");
    bool passed = expect(result.ok && result.performances == 1 && result.sounds == 1 && preset.existsAsFile(),
                         "a performance file becomes a layers preset named after the performance");
    const auto xml = readXml(preset);
    passed &= expect(xml != nullptr && xml->getChildByName("avLayers") != nullptr && xml->getChildByName("profile") == nullptr,
                     "it carries the eight layers, not the MIDI assignments");

    juce::MemoryBlock bytes;
    performanceFile.loadFileAsData(bytes);
    const auto contents = AVDexedSysex::scan(bytes);
    const auto performances = AVDexedPerformance::readPerformances(contents);
    const AVDexedPerformance::VoicePool pool(performanceFile, contents);
    auto installed = freshProcessor();
    if (! performances.empty()) installed->applyAVLayerSetup(AVDexedPerformance::resolve(performances.front(), pool));
    const auto expected = renderPrint(*installed);
    passed &= expect(! performances.empty() && soundFilePrint(preset) == expected,
                     "the layers preset plays bit for bit as the performance installed in the Capas page");
    const auto again = DexedAudioProcessor::convertAVBanks(request);
    passed &= expect(again.ok && again.sounds == 0 && again.repeated == 1, "the same performance again adds nothing");
    return passed;
}
}

/** Every file below `folder` with its bytes, by path below it (what "as it was" means). */
std::map<juce::String, juce::MemoryBlock> snapshot(const juce::File& folder) {
    std::map<juce::String, juce::MemoryBlock> files;
    for (const auto& entry : juce::RangedDirectoryIterator(folder, true, "*", juce::File::findFiles)) {
        const auto path = entry.getFile().getRelativePathFrom(folder).replaceCharacter('\\', '/');
        if (path.startsWith(".cache") || path.startsWith(AVDexedLibrary::syxBackupsFolderName)) continue;
        juce::MemoryBlock bytes;
        entry.getFile().loadFileAsData(bytes);
        files[path] = bytes;
    }
    return files;
}

/** The library's own banks turned into sounds where they are (the one-time
    conversion of the library), their files moved into the backup, and all of
    it undone again. */
bool testLibraryConversion() {
    bool passed = true;
    Scratch scratch;
    const auto& root = scratch.root;
    const auto write = [](const juce::File& file, const juce::MemoryBlock& bytes) {
        file.getParentDirectory().createDirectory();
        return file.replaceWithData(bytes.getData(), bytes.getSize());
    };
    auto b1 = makeBank([](int slot) { return juce::String(slot) + " BAS " + juce::String(slot); }, 70);
    auto b2 = makeBank([](int slot) { return juce::String(slot) + " BAS X" + juce::String(slot); }, 71);
    b2.packProgram(b1.unpackProgram(5).getRawData(), 5, "5 BAS COPY");   // the same sound as B1's slot 6
    write(root.getChildFile("Basses/B1.syx"), bankBytes(b1));
    write(root.getChildFile("Basses/B2.syx"), bankBytes(b2));
    write(root.getChildFile("Mis cosas/Rhodes/R.syx"), bankBytes(makeBank([](int slot) { return "RHO " + juce::String(slot); }, 72)));
    write(root.getChildFile("Dexed Factory/Dexed_01.syx"), bankBytes(makeBank([](int slot) { return "FAC " + juce::String(slot); }, 73)));
    AVDexedLibrary::writePackManifest(root.getChildFile("Dexed Factory"), "factory", "Dexed Factory");
    write(root.getChildFile("Suelto.syx"), bankBytes(makeBank([](int slot) { return "LOOSE " + juce::String(slot); }, 74)));
    {
        DexedAudioProcessor player(DexedAudioProcessor::TestMode{});
        write(root.getChildFile("User Presets/Piano.avdexpreset"), player.createAVNativePresetState("Piano", "id-piano"));
    }
    const auto before = snapshot(root);

    AVDexedConversion::Request request;
    for (const auto& file : AVDexedLibrary::findSoundFiles(root))
        if (file.hasFileExtension("syx")) request.sources.add(file);
    request.root = root;
    request.layout = AVDexedConversion::Layout::besideSources;
    request.sourceBase = root;
    request.backupLabel = "Biblioteca";
    request.moveSources = true;
    const auto result = DexedAudioProcessor::convertAVBanks(request);
    bool noBanks = true;
    for (const auto& file : AVDexedLibrary::findSoundFiles(root)) noBanks &= ! file.hasFileExtension("syx");
    passed &= expect(result.ok && result.banks == 5 && result.repeated == 1 && result.sounds == 5 * 32 - 1 && noBanks,
                     "the library's banks become sounds and no bank is left in it");
    passed &= expect(presetsIn(root.getChildFile("Basses")).size() == 63 && presetsIn(root.getChildFile("Mis cosas/Rhodes")).size() == 32
                         && presetsIn(root.getChildFile("Dexed Factory")).size() == 32 && presetsIn(root).size() == 32
                         && root.getChildFile("Dexed Factory").getChildFile(AVDexedLibrary::packManifestName).existsAsFile(),
                     "each bank's sounds stay in its folder, at any depth, a pack still a pack");
    passed &= expect(root.getChildFile("User Presets/Piano.avdexpreset").hasIdenticalContentTo(root.getChildFile("User Presets/Piano.avdexpreset"))
                         && snapshot(root)["User Presets/Piano.avdexpreset"] == before.at("User Presets/Piano.avdexpreset"),
                     "a sound file of the library is left as it is");
    bool backedUp = result.backup.getChildFile(".conversion").existsAsFile();
    for (const auto* path : { "Basses/B1.syx", "Basses/B2.syx", "Mis cosas/Rhodes/R.syx", "Dexed Factory/Dexed_01.syx", "Suelto.syx" }) {
        juce::MemoryBlock bytes;
        backedUp &= result.backup.getChildFile(path).loadFileAsData(bytes) && bytes == before.at(path);
    }
    passed &= expect(backedUp, "the original files are in the backup as they were, with their folders and a record");

    // Where each bank voice went: sessions and favourites follow it.
    juce::String key;
    const auto voice = AVDexedConversion::convertedVoice(root, "bank:" + AVDexedLibrary::bankFingerprint(b1) + ":05", &key);
    juce::String repeatedKey;
    const auto repeated = AVDexedConversion::convertedVoice(root, "bank:" + AVDexedLibrary::bankFingerprint(b2) + ":05", &repeatedKey);
    passed &= expect(voice.existsAsFile() && voice.getParentDirectory() == root.getChildFile("Basses") && key.startsWith("user:")
                         && repeated == voice && repeatedKey == key,
                     "a bank voice finds the sound it became, a repeated one the sound kept");
    passed &= expect(AVDexedLibrary::readSoundIndex(root).count("Basses/" + voice.getFileName()) == 1,
                     "the browser's index already knows the new sounds");
    passed &= expect(AVDexedConversion::lastUndoableConversion(root) == result.backup, "the conversion can be undone");

    // Undone: the library is as it was.
    const auto undone = AVDexedConversion::undoConversion(root, result.backup);
    passed &= expect(undone.ok && undone.restored == 5 && undone.removed == result.sounds && undone.kept == 0
                         && int(undone.origins.size()) == 5 * 32,
                     "undoing it puts the five files back and removes every sound it made");
    passed &= expect(snapshot(root) == before, "and the library is exactly as it was");
    passed &= expect(! result.backup.exists() && ! AVDexedConversion::convertedVoice(root, "bank:" + AVDexedLibrary::bankFingerprint(b1) + ":05").exists()
                         && AVDexedConversion::lastUndoableConversion(root) == juce::File(),
                     "its backup goes, and the bank voices are banks again");

    // A sound saved over since the conversion stays when it is undone.
    const auto again = DexedAudioProcessor::convertAVBanks(request);
    const auto edited = presetsIn(root.getChildFile("Basses")).getFirst();
    {
        DexedAudioProcessor player(DexedAudioProcessor::TestMode{});
        write(edited, player.createAVNativePresetState("Editado", "id-editado"));
    }
    const auto undoneAgain = AVDexedConversion::undoConversion(root, again.backup);
    passed &= expect(again.ok && undoneAgain.ok && undoneAgain.kept == 1 && edited.existsAsFile()
                         && root.getChildFile("Basses/B1.syx").existsAsFile(),
                     "a sound saved over since stays when the conversion is undone");
    return passed;
}

/** A DAW session saved with a bank voice of the library reopens, once the
    library is converted, on the sound file that voice became: the same sound,
    unmodified, "Guardar" writing there. So does a host choosing the program
    again, and the bank an instance starts with. */
bool testSessionsFindTheirSound() {
    bool passed = true;
    Scratch scratch;
    const auto oldApp = DexedAudioProcessor::dexedAppDir;
    DexedAudioProcessor::dexedAppDir = scratch.root;
    const auto bankFile = scratch.root.getChildFile("Pianos/P.syx");
    bankFile.getParentDirectory().createDirectory();
    const auto bank = makeBank([](int slot) { return "PNO " + juce::String(slot); }, 80);
    bankFile.replaceWithData(bankBytes(bank).getData(), bankBytes(bank).getSize());
    juce::MemoryBlock session;
    {
        auto player = freshProcessor();
        Cartridge read;
        AVDexedLibrary::readBank(bankFile, read);
        player->setActiveBank(read, bankFile);
        player->setCurrentProgram(3);
        player->getStateInformation(session);
    }
    const auto reopen = [&session] {
        auto player = freshProcessor();
        player->setStateInformation(session.getData(), int(session.getSize()));
        return player;
    };
    const auto expected = renderPrint(*reopen());

    AVDexedConversion::Request request;
    request.sources = { bankFile };
    request.root = scratch.root;
    request.layout = AVDexedConversion::Layout::besideSources;
    request.sourceBase = scratch.root;
    request.moveSources = true;
    passed &= expect(DexedAudioProcessor::convertAVBanks(request).ok, "the library is converted");
    const auto sound = scratch.root.getChildFile("Pianos/PNO 3.avdexpreset");
    auto reopened = reopen();
    passed &= expect(sound.existsAsFile() && reopened->getAVPresetPath() == sound && reopened->getAVPresetName() == "PNO 3"
                         && ! reopened->isAVPresetModified() && reopened->hasAVKnownPresetOrigin(),
                     "a session of a converted bank voice reopens on its sound file, unmodified");
    passed &= expect(renderPrint(*reopened) == expected, "and plays bit for bit as before");
    auto selectedAgain = reopen();
    selectedAgain->setCurrentProgram(3);
    passed &= expect(selectedAgain->getAVPresetPath() == sound, "a host choosing the program again keeps it");
    auto starting = freshProcessor();
    starting->setActiveBank(bank, juce::File());
    starting->setCurrentProgram(0);
    passed &= expect(starting->getAVPresetPath() == scratch.root.getChildFile("Pianos/PNO 0.avdexpreset"),
                     "the bank an instance starts with plays its converted sound file");
    DexedAudioProcessor::dexedAppDir = oldApp;
    return passed;
}

/** MIDI program changes walk the folder of the sound playing, in the
    browser's order, at once and bit for bit as opening that sound; Bank
    Select counts in blocks of 128; a learned arrow steps through it with the
    window closed; a sound that is more than a voice is read whole; a bank
    voice still walks its bank. */
bool testProgramChanges() {
    bool passed = true;
    Scratch scratch;
    const auto oldApp = DexedAudioProcessor::dexedAppDir;
    DexedAudioProcessor::dexedAppDir = scratch.root;
    AVDexedConversion::Request request;
    for (int b = 0; b < 5; ++b) {
        const auto file = scratch.outside.getChildFile("B" + juce::String(b) + ".syx");
        const auto bank = makeBank([b](int slot) { return "PC" + juce::String(b) + " " + juce::String(slot); }, 90 + b);
        file.replaceWithData(bankBytes(bank).getData(), bankBytes(bank).getSize());
        request.sources.add(file);
    }
    const auto folder = scratch.root.getChildFile("Carpeta");
    request.root = scratch.root;
    request.destination = folder;
    request.layout = AVDexedConversion::Layout::flat;
    request.backup = false;
    const auto result = DexedAudioProcessor::convertAVBanks(request);
    {
        // A sound of the user's own after the converted ones.
        auto maker = freshProcessor();
        Cartridge bank = makeBank([](int) { return juce::String("ZETA"); }, 99);
        maker->setActiveBank(bank, juce::File());
        maker->setCurrentProgram(9);
        const auto own = maker->createAVNativePresetState("Zeta", "id-zeta");
        folder.getChildFile("Zeta.avdexpreset").replaceWithData(own.getData(), own.getSize());
    }
    std::vector<juce::File> byOrder(size_t(result.sounds));
    for (const auto& file : result.files)
        if (const auto xml = readXml(file); session(xml) != nullptr)
            byOrder[size_t(session(xml)->getIntAttribute("avLibraryOrder"))] = file;
    passed &= expect(result.ok && result.sounds == 160, "160 sounds to walk");

    const auto open = [](DexedAudioProcessor& player, const juce::File& file) {
        juce::MemoryBlock data;
        file.loadFileAsData(data);
        player.loadAVNativePreset(data.getData(), int(data.getSize()));
        player.setAVPresetContext(player.getAVPresetName(), session(readXml(file))->getStringAttribute("avPresetId"), file);
        player.captureAVPresetBaseline();
    };
    const auto settle = [](DexedAudioProcessor& player) {
        for (int i = 0; i < 1000 && ! player.isAVFolderReadForTesting(); ++i) {
            player.runAVTimerTickForTesting();
            juce::Thread::sleep(5);
        }
        return player.isAVFolderReadForTesting();
    };
    const auto send = [](DexedAudioProcessor& player, std::initializer_list<juce::MidiMessage> messages) {
        juce::AudioBuffer<float> audio(2, 64);
        audio.clear();
        juce::MidiBuffer midi;
        for (const auto& message : messages) midi.addEvent(message, 0);
        player.processBlock(audio, midi);
    };
    const auto voiceOf = [](const juce::File& file) {
        auto reference = freshProcessor();
        juce::MemoryBlock data;
        file.loadFileAsData(data);
        reference->loadAVNativePreset(data.getData(), int(data.getSize()));
        return reference->activeProgram;
    };
    auto player = freshProcessor();
    open(*player, byOrder[0]);
    passed &= expect(settle(*player), "the folder of the sound playing is read");
    send(*player, { juce::MidiMessage::programChange(1, 5) });
    passed &= expect(std::memcmp(player->activeProgram.getRawData(), voiceOf(byOrder[5]).getRawData(), Program::PROGRAM_SIZE + 1) == 0,
                     "a program change plays sound 6 of the folder at once, in render");
    player->runAVTimerTickForTesting();
    passed &= expect(player->getAVPresetPath() == byOrder[5] && ! player->isAVPresetModified(),
                     "and the sound playing is that sound file then");
    send(*player, { juce::MidiMessage::controllerEvent(1, 0, 0), juce::MidiMessage::controllerEvent(1, 32, 1),
                    juce::MidiMessage::programChange(1, 2) });
    player->runAVTimerTickForTesting();
    passed &= expect(player->getAVPresetPath() == byOrder[130], "Bank Select 1 and program 3: sound 131");
    send(*player, { juce::MidiMessage::controllerEvent(1, 32, 0), juce::MidiMessage::programChange(1, 0) });
    player->runAVTimerTickForTesting();
    passed &= expect(player->getAVPresetPath() == byOrder[0], "Bank Select 0 and program 1: the first sound");

    // Bit for bit: a program change in the block plays as opening the sound.
    {
        auto changed = freshProcessor(), opened = freshProcessor();
        open(*changed, byOrder[0]);
        settle(*changed);
        send(*changed, { juce::MidiMessage::programChange(1, 77) });
        open(*opened, byOrder[77]);
        send(*opened, {});
        passed &= expect(renderPrint(*changed) == renderPrint(*opened), "a program change plays bit for bit as opening that sound");
    }

    // A learned arrow with the window closed: the next sound of the folder.
    player->startAVPresetStepLearn(DexedAudioProcessor::AVPresetStep::next);
    player->midiCCMapper->processControlChange(20, 127);
    player->midiCCMapper->commitAVPresetStepLearn();
    player->consumeAVPresetStepLearned();
    send(*player, { juce::MidiMessage::controllerEvent(1, 20, 0) });
    send(*player, { juce::MidiMessage::controllerEvent(1, 20, 127) });
    player->runAVTimerTickForTesting();
    passed &= expect(player->getAVPresetPath() == byOrder[1], "a learned arrow steps to the next sound of the folder");

    // A sound of the user's own (more than a voice): read whole, a moment later.
    send(*player, { juce::MidiMessage::controllerEvent(1, 32, 1), juce::MidiMessage::programChange(1, 32) });
    player->runAVTimerTickForTesting();
    passed &= expect(player->getAVPresetPath() == folder.getChildFile("Zeta.avdexpreset"), "a sound of one's own is read whole");

    // A bank voice walks its bank, as before.
    auto bankPlayer = freshProcessor();
    bankPlayer->setActiveBank(makeBank([](int slot) { return "BNK " + juce::String(slot); }, 98), juce::File());
    bankPlayer->setCurrentProgram(0);
    for (int i = 0; i < 5; ++i) bankPlayer->runAVTimerTickForTesting();
    send(*bankPlayer, { juce::MidiMessage::programChange(1, 7) });
    passed &= expect(bankPlayer->getCurrentProgram() == 7, "a bank voice's program change walks its bank");
    DexedAudioProcessor::dexedAppDir = oldApp;
    return passed;
}

/** A folder of sounds exported as .syx banks and imported again: the same
    voices, settings and DX7II data, the same sounds bit for bit. */
bool testExportRoundTrip() {
    bool passed = true;
    Scratch scratch;
    // 40 voices: a bank with a reverb embedded on slot 4, and 8 voices of a DX7II bank with its AMEM.
    const auto first = makeBank([](int slot) { return juce::String(slot) + " EXP " + juce::String(slot); }, 110);
    AVDexedLibrary::BankSettings settings;
    settings[3] = { AVDexedLibrary::voiceHash(first, 3), { { IDs::reverbAmount.name, 0.5f }, { IDs::cutoff.name, 0.25f } } };
    const auto firstFile = scratch.outside.getChildFile("Uno.syx");
    const auto firstBytes = AVDexedLibrary::composeBankFile(bankBytes(first), first, settings);
    firstFile.replaceWithData(firstBytes.getData(), firstBytes.getSize());
    auto second = makeBank([](int slot) { return "MK2 " + juce::String(slot); }, 111);
    for (int slot = 8; slot < 32; ++slot) second.packProgram(Program::initialProgram().getRawData(), slot, "INIT VOICE");
    std::array<uint8_t, 1120> amem {};
    amem[2 * 35 + 5] = 2;
    amem[2 * 35 + 34] = 3;
    juce::MemoryBlock secondBytes;
    const auto amemMessage = AVDexedSysex::amemMessage(amem.data());
    secondBytes.append(amemMessage.data(), amemMessage.size());
    secondBytes.append(bankBytes(second).getData(), bankBytes(second).getSize());
    const auto secondFile = scratch.outside.getChildFile("Dos.syx");
    secondFile.replaceWithData(secondBytes.getData(), secondBytes.getSize());

    AVDexedConversion::Request request;
    request.sources = { firstFile, secondFile };
    request.root = scratch.root;
    request.destination = scratch.root.getChildFile("Ida");
    request.layout = AVDexedConversion::Layout::flat;
    request.backup = false;
    const auto there = DexedAudioProcessor::convertAVBanks(request);
    std::vector<juce::File> byOrder(size_t(there.sounds));
    for (const auto& file : there.files)
        if (const auto xml = readXml(file); session(xml) != nullptr)
            byOrder[size_t(session(xml)->getIntAttribute("avLibraryOrder"))] = file;
    juce::Array<juce::File> sounds;
    for (const auto& file : byOrder) sounds.add(file);
    const auto exported = DexedAudioProcessor::exportAVSoundsToBanks(sounds, scratch.outside.getChildFile("Exportados"), "Ida");
    passed &= expect(there.ok && there.sounds == 40 && exported.ok && exported.voices == 40 && exported.files.size() == 2
                         && exported.files[0].getFileName() == "Ida 1.syx" && exported.files[1].getFileName() == "Ida 2.syx",
                     "40 sounds export as two banks of the DX7");
    const auto back = AVDexedLibrary::readSourceBanks(exported.files[0]);
    const auto backSecond = AVDexedLibrary::readSourceBanks(exported.files[1]);
    bool sameVoices = back.size() == 1 && backSecond.size() == 1;
    for (int slot = 0; sameVoices && slot < 32; ++slot)
        sameVoices = std::memcmp(Cartridge(back[0].bank).unpackProgram(slot).getRawData(),
                                 Cartridge(first).unpackProgram(slot).getRawData(), 145) == 0;
    for (int slot = 0; sameVoices && slot < 8; ++slot)
        sameVoices = std::memcmp(Cartridge(backSecond[0].bank).unpackProgram(slot).getRawData(),
                                 Cartridge(second).unpackProgram(slot).getRawData(), 145) == 0;
    passed &= expect(sameVoices && Cartridge(back[0].bank).getProgramName(3).trim() == "EXP 3",
                     "the voices come back the same, named as the sounds (without the slot number)");
    passed &= expect(back[0].settings.count(3) == 1 && std::abs(back[0].settings.at(3).values.at(IDs::reverbAmount.name) - 0.5f) < 1.0e-4f
                         && backSecond[0].bank.getAmemVoice(2) != nullptr && backSecond[0].bank.getAmemVoice(2)[5] == 2,
                     "with their sound settings and DX7II additional data");

    // Imported again: the same sounds, bit for bit.
    AVDexedConversion::Request again = request;
    again.sources = exported.files;
    again.destination = scratch.root.getChildFile("Vuelta");
    const auto returned = DexedAudioProcessor::convertAVBanks(again);
    std::vector<juce::File> returnedByOrder(size_t(returned.sounds));
    for (const auto& file : returned.files)
        if (const auto xml = readXml(file); session(xml) != nullptr)
            returnedByOrder[size_t(session(xml)->getIntAttribute("avLibraryOrder"))] = file;
    bool sameSounds = returned.ok && returned.sounds == 40;
    for (size_t i = 0; sameSounds && i < byOrder.size(); ++i)
        sameSounds = soundFilePrint(byOrder[i]) == soundFilePrint(returnedByOrder[i]);
    passed &= expect(sameSounds, "imported again, every sound plays bit for bit as before");
    return passed;
}

/** A bank received by SysEx becomes sounds in "Recibido de DX7" (once). */
bool testReceivedBank() {
#if JUCE_MAC
    bool passed = true;
    Scratch scratch;
    const auto oldApp = DexedAudioProcessor::dexedAppDir;
    DexedAudioProcessor::dexedAppDir = scratch.root;
    const auto pump = [](int milliseconds) {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + milliseconds;
        while (juce::Time::getMillisecondCounterHiRes() < deadline)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
    };
    auto player = freshProcessor();
    player->keepAVReceivedBanksForTesting();
    const auto bank = makeBank([](int slot) { return "RX " + juce::String(slot); }, 120);
    const auto dump = bankBytes(bank);
    const auto receive = [&] {
        juce::AudioBuffer<float> audio(2, 64);
        audio.clear();
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage(dump.getData(), int(dump.getSize())), 0);
        player->processBlock(audio, midi);
        player->runAVTimerTickForTesting();
    };
    const auto folder = scratch.root.getChildFile(AVDexedLanguageService::getInstance().text("receivedFolderName"));
    receive();
    for (int i = 0; i < 400 && (presetsIn(folder).size() < 32 || player->isAVConverting()); ++i) pump(10);
    pump(100);
    passed &= expect(presetsIn(folder).size() == 32 && AVDexedLibrary::bankFingerprint(player->currentCart) == AVDexedLibrary::bankFingerprint(bank),
                     "a bank received by SysEx plays, and becomes 32 sounds of the library");
    passed &= expect(player->getAVPresetPath() == folder.getChildFile("RX 0.avdexpreset"), "the voice playing is its sound file then");
    receive();
    for (int i = 0; i < 400 && player->isAVConverting(); ++i) pump(10);
    pump(100);
    passed &= expect(presetsIn(folder).size() == 32, "the same bank again adds nothing");
    bool noDump = true;
    for (const auto& entry : juce::RangedDirectoryIterator(scratch.root.getChildFile(".cache"), false, "*.syx", juce::File::findFiles))
        noDump = ! entry.getFile().exists();
    passed &= expect(noDump, "and the received dump is not left behind");
    DexedAudioProcessor::dexedAppDir = oldApp;
    return passed;
#else
    return true;
#endif
}

bool runAVDexedConversionTests() {
    bool passed = testVoiceNames();
    passed &= testConvertFolder();
    passed &= testFactoryBanks();
    passed &= testKeepsTheInstrument();
    passed &= testExtrasInLayers();
    passed &= testPerformances();
    passed &= testLibraryConversion();
    passed &= testSessionsFindTheirSound();
    passed &= testProgramChanges();
    passed &= testExportRoundTrip();
    passed &= testReceivedBank();
    std::cout << (passed ? "Conversion checks passed.\n" : "Conversion checks FAILED.\n");
    return passed;
}

/** Every bank of `library` (a copy of one) converted as its migration would
    convert it (each first-level folder flat, its banks in order), and every
    sound checked bit for bit against its bank voice. Prints what it would
    write; returns 0 when every sound plays as its voice. */
int AVRunConvertCheck(const juce::File& library) {
    if (! library.isDirectory()) {
        std::cerr << "Not a folder: " << library.getFullPathName() << '\n';
        return 2;
    }
    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto out = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("avdexed-convert-check-" + juce::Uuid().toString());
    out.createDirectory();
    int banks = 0, sounds = 0, repeated = 0, empty = 0, different = 0, checked = 0, unchecked = 0;
    juce::int64 bytes = 0;
    juce::StringArray unreadable;
    for (const auto& folder : AVDexedLibrary::listFolders(library)) {
        auto files = folder.findChildFiles(juce::File::findFiles | juce::File::ignoreHiddenFiles, true, "*.syx;*.SYX");
        std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b) {
            return a.getFullPathName().compareNatural(b.getFullPathName(), false) < 0;
        });
        if (files.isEmpty()) continue;
        AVDexedConversion::Request request;
        request.sources = files;
        request.root = out;
        request.destination = out.getChildFile(folder.getFileName());
        request.layout = AVDexedConversion::Layout::flat;
        request.backup = false;
        const auto result = DexedAudioProcessor::convertAVBanks(request);
        if (! result.ok) {
            std::cerr << "FAILED to convert " << folder.getFileName() << '\n';
            return 1;
        }
        std::map<juce::String, std::pair<juce::File, Cartridge>> sources;
        std::set<juce::String> clashing;
        for (const auto& file : files) {
            const auto found = AVDexedLibrary::readSourceBanks(file);
            for (const auto& bank : found) {
                Cartridge plain;
                const bool standard = found.size() == 1 && bank.voices == 32 && AVDexedLibrary::readBank(file, plain);
                if (! sources.emplace(bank.name, std::make_pair(standard ? file : juce::File(), bank.bank)).second)
                    clashing.insert(bank.name);
            }
        }
        for (const auto& name : clashing) sources.erase(name);
        int folderChecked = 0;
        const bool same = playsAsItsBankVoice(result, sources, folderChecked);
        checked += folderChecked;
        unchecked += result.sounds - folderChecked;
        for (const auto& file : result.files) bytes += file.getSize();
        if (! same) ++different;
        banks += result.banks;
        sounds += result.sounds;
        repeated += result.repeated;
        empty += result.empty;
        unreadable.addArray(result.unreadable);
        std::cout << folder.getFileName() << ": " << result.banks << " banks, " << result.sounds << " sounds, "
                  << result.repeated << " repeated, " << result.empty << " empty" << (same ? "" : ", NOT ALL THE SAME") << '\n';
        out.getChildFile(folder.getFileName()).deleteRecursively();
    }
    out.deleteRecursively();
    std::cout << "Total: " << banks << " banks, " << sounds << " sounds (" << checked << " checked bit for bit, "
              << unchecked << " not checked), " << repeated << " repeated, " << empty << " empty, "
              << unreadable.size() << " unreadable; " << juce::String(double(bytes) / (1024.0 * 1024.0), 1) << " MB; "
              << juce::String((juce::Time::getMillisecondCounterHiRes() - started) / 1000.0, 1) << " s\n";
    if (different > 0) std::cout << different << " folders with sounds that do not play as their voice\n";
    return different == 0 ? 0 : 1;
}

/** Every first-level folder of `library` converted flat into `destination`
    (as the library's conversion will lay it out), kept there: a library of
    sound files to measure the browser with. */
int AVRunConvertLibrary(const juce::File& library, const juce::File& destination) {
    if (! library.isDirectory() || destination.exists()) {
        std::cerr << "Needs an existing library and a destination that does not exist yet\n";
        return 2;
    }
    destination.createDirectory();
    const auto started = juce::Time::getMillisecondCounterHiRes();
    int sounds = 0;
    for (const auto& folder : AVDexedLibrary::listFolders(library)) {
        auto files = folder.findChildFiles(juce::File::findFiles | juce::File::ignoreHiddenFiles, true, "*.syx;*.SYX");
        std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b) {
            return a.getFullPathName().compareNatural(b.getFullPathName(), false) < 0;
        });
        if (files.isEmpty()) continue;
        AVDexedConversion::Request request;
        request.sources = files;
        request.root = destination;
        request.destination = destination.getChildFile(folder.getFileName());
        request.layout = AVDexedConversion::Layout::flat;
        request.backup = false;
        const auto result = DexedAudioProcessor::convertAVBanks(request);
        if (! result.ok) return 1;
        sounds += result.sounds;
    }
    std::cout << sounds << " sounds in " << juce::String((juce::Time::getMillisecondCounterHiRes() - started) / 1000.0, 1)
              << " s (conversion only)\n";
    return 0;
}

int AVRunIndexBench(const juce::File& root, bool withoutIndexFirst);

/** How long the browser of a new window takes to list `root` (a library):
    once without its saved index, once with it. */
int AVRunIndexBench(const juce::File& root) { return AVRunIndexBench(root, true); }

int AVRunIndexBench(const juce::File& root, bool withoutIndexFirst) {
#if JUCE_MAC
    const auto pump = [](int milliseconds) {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + milliseconds;
        while (juce::Time::getMillisecondCounterHiRes() < deadline)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
    };
    const auto oldApp = DexedAudioProcessor::dexedAppDir, oldCart = DexedAudioProcessor::dexedCartDir;
    DexedAudioProcessor::dexedAppDir = root;
    DexedAudioProcessor::dexedCartDir = AVDexedLibrary::rootFor(root);
    if (withoutIndexFirst) root.getChildFile(".cache").getChildFile("library-index").deleteFile();
    int code = 0;
    for (const auto* run : { withoutIndexFirst ? "without its index" : "with the index as it is", "with its index" }) {
        DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
        auto editor = std::make_unique<DexedAudioProcessorEditor>(&processor);
        const auto started = juce::Time::getMillisecondCounterHiRes();
        while (editor->playView.libraryIndexing && juce::Time::getMillisecondCounterHiRes() - started < 300000)
            pump(5);
        const auto seconds = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
        std::cout << "Listed " << editor->playView.libraryEntries.size() << " sounds " << run << " in "
                  << juce::String(seconds, 2) << " s\n";
        if (editor->playView.libraryIndexing) code = 1;
        editor.reset();
        pump(200);
    }
    std::cout << "Index: " << root.getChildFile(".cache").getChildFile("library-index").getSize() / 1024 << " KB\n";
    DexedAudioProcessor::dexedAppDir = oldApp;
    DexedAudioProcessor::dexedCartDir = oldCart;
    return code;
#else
    juce::ignoreUnused(root);
    return 2;
#endif
}

/** The PNG files of `before` against those of `after` with the same names:
    how many pixels differ, and where (the --render-ui captures before and
    after a change). */
int AVRunCompareImages(const juce::File& before, const juce::File& after) {
    int changed = 0;
    for (const auto& entry : juce::RangedDirectoryIterator(before, false, "*.png", juce::File::findFiles)) {
        const auto other = after.getChildFile(entry.getFile().getFileName());
        const auto a = juce::ImageFileFormat::loadFrom(entry.getFile());
        const auto b = juce::ImageFileFormat::loadFrom(other);
        if (! b.isValid() || a.getBounds() != b.getBounds()) {
            std::cout << entry.getFile().getFileName() << ": missing or another size\n";
            ++changed;
            continue;
        }
        int different = 0;
        juce::Rectangle<int> area;
        const juce::Image::BitmapData da(a, juce::Image::BitmapData::readOnly), db(b, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < a.getHeight(); ++y)
            for (int x = 0; x < a.getWidth(); ++x)
                if (da.getPixelColour(x, y) != db.getPixelColour(x, y)) {
                    ++different;
                    area = area.isEmpty() ? juce::Rectangle<int>(x, y, 1, 1) : area.getUnion({ x, y, 1, 1 });
                }
        if (different > 0) {
            std::cout << entry.getFile().getFileName() << ": " << different << " px differ in " << area.toString() << "\n";
            ++changed;
        }
    }
    std::cout << changed << " images differ\n";
    return 0;
}

/** The one-time conversion of a library (a copy of one), checked: every bank
    voice plays bit for bit as the sound file it became, every favourite bank
    voice finds its sound, the first window lists it quickly, and undoing it
    leaves every file as it was. */
int AVRunMigrateCheck(const juce::File& library) {
    if (! library.isDirectory()) return 2;
    const auto oldApp = DexedAudioProcessor::dexedAppDir;
    DexedAudioProcessor::dexedAppDir = library;
    std::map<juce::String, juce::String> before;   // path → hash of the file
    for (const auto& entry : juce::RangedDirectoryIterator(library, true, "*", juce::File::findFiles)) {
        juce::MemoryBlock bytes;
        entry.getFile().loadFileAsData(bytes);
        before[entry.getFile().getRelativePathFrom(library)] = AVDexedLibrary::stableHash(bytes.getData(), bytes.getSize());
    }
    AVDexedConversion::Request request;
    for (const auto& file : AVDexedLibrary::findSoundFiles(library))
        if (file.hasFileExtension("syx")) request.sources.add(file);
    request.root = library;
    request.layout = AVDexedConversion::Layout::besideSources;
    request.sourceBase = library;
    request.backupLabel = "Biblioteca";
    request.moveSources = true;
    auto started = juce::Time::getMillisecondCounterHiRes();
    const auto result = DexedAudioProcessor::convertAVBanks(request);
    std::cout << "Converted " << result.banks << " banks into " << result.sounds << " sounds (" << result.repeated << " repeated, "
              << result.empty << " empty) in " << juce::String((juce::Time::getMillisecondCounterHiRes() - started) / 1000.0, 1) << " s\n";
    if (! result.ok) return 1;
    int code = 0;
    // Bit for bit, every voice of every original file against its sound file.
    int checked = 0, different = 0, missing = 0;
    for (const auto& original : request.sources) {
        const auto backup = result.backup.getChildFile(original.getRelativePathFrom(library));
        for (const auto& bank : AVDexedLibrary::readSourceBanks(backup)) {
            if (bank.singleVoices) continue;
            const auto fingerprint = AVDexedLibrary::bankFingerprint(bank.bank);
            Cartridge plain;
            const bool standard = AVDexedLibrary::readBank(backup, plain) && AVDexedLibrary::readSourceBanks(backup).size() == 1;
            for (int slot = 0; slot < 32; ++slot) {
                const auto sound = AVDexedConversion::convertedVoice(library, "bank:" + fingerprint + ":" + juce::String(slot).paddedLeft('0', 2));
                if (sound == juce::File()) {
                    // The Init voice is an empty slot: it has no sound.
                    if (std::memcmp(Cartridge(bank.bank).unpackProgram(slot).getRawData(), Program::initialProgram().getRawData(), 145) != 0) ++missing;
                    continue;
                }
                ++checked;
                if (soundFilePrint(sound) != bankVoicePrint(bank.bank, standard ? backup : juce::File(), slot)) {
                    if (++different <= 10) std::cerr << "  not the same: " << original.getFileName() << " #" << slot + 1 << '\n';
                }
            }
        }
    }
    std::cout << checked << " voices checked bit for bit, " << different << " different, " << missing << " without a sound\n";
    if (different > 0 || missing > 0) code = 1;
    // The favourites of the copy's preferences.
    juce::PropertiesFile preferences(library.getChildFile(DexedAudioProcessor::preferencesFileName), AVDexedLibrary::preferencesOptions());
    int favorites = 0, found = 0;
    for (const auto& key : juce::StringArray::fromLines(preferences.getValue("favoritePresetIds")))
        if (key.startsWith("bank:")) {
            ++favorites;
            found += AVDexedConversion::convertedVoice(library, key).existsAsFile() ? 1 : 0;
        }
    std::cout << found << " of " << favorites << " favourite bank voices find their sound file\n";
    juce::int64 bytes = 0;
    for (const auto& file : result.files) bytes += file.getSize();
    std::cout << juce::String(double(bytes) / (1024.0 * 1024.0), 1) << " MB of sound files\n";
    AVRunIndexBench(library, false);
    started = juce::Time::getMillisecondCounterHiRes();
    const auto undone = AVDexedConversion::undoConversion(library, result.backup);
    std::cout << "Undone: " << undone.restored << " files back, " << undone.removed << " sounds removed, " << undone.kept << " kept, in "
              << juce::String((juce::Time::getMillisecondCounterHiRes() - started) / 1000.0, 1) << " s\n";
    int changed = 0;
    for (const auto& entry : juce::RangedDirectoryIterator(library, true, "*", juce::File::findFiles)) {
        const auto path = entry.getFile().getRelativePathFrom(library);
        if (path.startsWith(".cache") || path.startsWith(AVDexedLibrary::syxBackupsFolderName)) continue;
        juce::MemoryBlock fileBytes;
        entry.getFile().loadFileAsData(fileBytes);
        const auto was = before.find(path);
        if (was == before.end() || was->second != AVDexedLibrary::stableHash(fileBytes.getData(), fileBytes.getSize())) {
            if (++changed <= 10) std::cerr << "  changed or new: " << path << '\n';
        }
    }
    int gone = 0;
    for (const auto& [path, hash] : before)
        if (! path.startsWith(".cache") && ! library.getChildFile(path).existsAsFile()) ++gone;
    std::cout << "After undoing: " << changed << " files changed or new, " << gone << " gone\n";
    if (! undone.ok || changed > 0 || gone > 0) code = 1;
    DexedAudioProcessor::dexedAppDir = oldApp;
    return code;
}
