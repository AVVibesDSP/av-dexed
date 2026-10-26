// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

// Banks turned into sound files (AVDexedConversion): the names, the folders,
// repeated and empty voices, the order, the backup of the originals, nothing
// left behind when it fails, and that every converted sound plays bit for bit
// as its bank voice does. --convert-check runs that last check on every bank
// of a library (a copy of one), as its conversion would make them.

#include "../source/PluginProcessor.h"
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
    passed &= expect(presetsIn(importados.getChildFile("Solo")).size() == 2, "single voices become a sound each");

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
    const auto locked = root.getChildFile("Bloqueado");
    locked.createDirectory();
    locked.setReadOnly(true, false);
    AVDexedConversion::Request refused = request;
    refused.destination = locked.getChildFile("Dentro");
    const auto failed = DexedAudioProcessor::convertAVBanks(refused);
    locked.setReadOnly(false, false);
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
}

bool runAVDexedConversionTests() {
    bool passed = testVoiceNames();
    passed &= testConvertFolder();
    passed &= testFactoryBanks();
    passed &= testKeepsTheInstrument();
    passed &= testExtrasInLayers();
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
